#pragma once
// Control plane (Story 2.1, AD-2/AD-8/AD-23). The control thread is the ONLY writer of structural
// settings: it validates a ws.v1 command against the generated registry, builds a new immutable
// core::Snapshot generation, publishes it with one atomic pointer store (SnapshotExchange) and replies
// with the request_id and the new revision. An invalid command returns a closed-enum error and changes
// nothing (the revision does not move). Performance parameters never touch a snapshot: they become one
// timestamped UMP on the control->RT queue, and the RT side owns the value (LastValueTable).
//
// Threading: handle(), load_initial(), reclaim(), fill_state() and fill_live_values() are control-thread only (the
// ControlServer's thread, or the test thread when no server runs; never two threads at once). Only revision(),
// live_generations(), freed_generations() and commands_rejected() are safe from any thread (atomics).
// Generations are freed here, never on RT: reclaim() frees a generation once it is not current, RT does
// not hold it and the RT-side ledger reports no voice referencing it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "velora/core/snapshot.hpp"
#include "velora/host/midi_ingest.hpp"
#include "ws_v1.pb.h"

namespace velora::host {

struct ControlFail;  // validation failure: closed error code + message + details (control_plane.cpp)

using ControlEventQueue = HostEventQueue;  // single producer: the control thread; consumer: RenderDriver

// Scene commands (Story 4.4). The plane parses the ws.v1 commands; the work (build a parameter-only generation from the loaded Live Set's
// scenes) is the SceneController's, reached through this seam so the plane does not depend on it.
enum class SceneResult : std::uint8_t {
  kOk = 0,           // the scene was recalled (a new generation was published)
  kAtFirst,          // previous_scene() at the first scene: nothing changed
  kAtLast,           // next_scene() at the last scene: nothing changed
  kNoSuchScene,      // recall_scene(index) with index >= the Live Set's scene count
  kNoLiveSet,        // no Live Set loaded (or the current generation is not the loaded Live Set's)
  kLoadInProgress,   // a Live Set load is running: nothing changed
  kBusy,             // the snapshot ledger is full: nothing changed
  kFxBudgetExceeded, // the scene's effects would cost more than the FX budget: nothing changed
  kInternal,
};
const char* to_string(SceneResult r);
struct SceneOutcome {
  SceneResult result = SceneResult::kInternal;
  std::int32_t scene = -1;       // the current scene after the call (-1 = none)
  std::uint64_t revision = 0;    // the plane's revision after the call
  bool published = false;        // a new generation went out
  const char* busy_reason = "";  // kBusy / kLoadInProgress: "ledger_full" or "load_in_progress" (for the ws.v1 error details)
  bool ok() const { return result == SceneResult::kOk || result == SceneResult::kAtFirst || result == SceneResult::kAtLast; }
};
class SceneHandler {
 public:
  enum class Op : std::uint8_t { kRecall, kNext, kPrevious };
  virtual ~SceneHandler() = default;
  virtual SceneOutcome run(Op op, std::uint32_t index) = 0;
};

// Live Set switching (Story 4.5). The plane parses JumpToLiveSet / CancelLoad and puts the loading status into State; the work (catalog,
// background load, publication) is the LiveSetSwitcher's, reached through this seam so the plane does not depend on it. Control thread only.
struct SwitchOutcome {
  enum class Result : std::uint8_t { kAccepted = 0, kAlreadyLoaded, kAlreadyLoading, kUnknownLiveSet };
  Result result = Result::kUnknownLiveSet;
  std::string target_id, target_name;
  bool superseded_previous = false;  // a load in progress was cancelled to start this one
  bool cancelled = false;            // CancelLoad: a load was in progress and is cancelled
};
class SwitchHandler {
 public:
  virtual ~SwitchHandler() = default;
  virtual SwitchOutcome jump(const std::string& id) = 0;
  virtual SwitchOutcome cancel() = 0;
  // Fills State.loading, State.live_set_id and State.live_set_name.
  virtual void fill_status(ws::v1::State* out) const = 0;
};

class ControlPlane {
 public:
  using Clock = std::function<std::uint64_t()>;  // host time in ns, the same clock the MIDI adapter stamps with

  ControlPlane(core::SnapshotExchange& exchange, const core::LastValueTable& live, ControlEventQueue& rt_events,
               Clock now_ns);
  // Frees every remaining generation: destroy only after the RT side has stopped.
  ~ControlPlane();
  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;

  // Validates `config` (the dev configuration, or any State) on top of the registry defaults and publishes
  // it as generation 1. Call once, before audio runs. On failure nothing is published and `err` says why.
  bool load_initial(const ws::v1::State& config, std::string& err);

  // Live Set load (Story 4.3): publishes a complete, prepared snapshot (built by LiveSetLoader from a Live Set) as the next generation,
  // replacing the structural state. Unlike load_initial() it also works after the first generation. Control thread only. The same
  // FX budget rule as every structural command applies (a snapshot that raises the cost above the budget is refused).
  // can_publish() says, without changing anything, whether publish_snapshot() would succeed right now (it may free generations
  // nobody reads any more to make room): the loader calls it before it touches the voice arbiter, so a refused load leaves
  // everything as it was.
  enum class PublishStatus : std::uint8_t { kOk = 0, kFxBudgetExceeded, kBusy, kInternal };
  PublishStatus can_publish(const core::Snapshot& work);
  PublishStatus publish_snapshot(const core::Snapshot& work, std::string& detail);
  // Test hook: the next publish_snapshot() returns `status` without committing anything (one shot), to exercise a late failure.
  void inject_publish_fault_for_tests(PublishStatus status) noexcept { fault_ = status; fault_set_ = true; }

  // Scene commands (Story 4.4): the SceneController registers itself here (and unregisters in its destructor). Control thread only.
  void set_scene_handler(SceneHandler* h) noexcept { scene_handler_ = h; }
  // The generation RT is (or will be) playing; control thread only, valid until the next publish. nullptr before the first generation.
  const core::Snapshot* current_snapshot() const noexcept { return current_; }
  // A program_generation number that was never handed out before (Snapshot::program_generation, AD-2); control thread only.
  std::uint64_t next_program_generation() noexcept { return ++program_generation_; }
  // Live Set switching (Story 4.5): the LiveSetSwitcher registers itself here (and unregisters in its destructor). Control thread only.
  void set_switch_handler(SwitchHandler* h) noexcept { switch_handler_ = h; }
  // A reservation epoch that was never handed out before (Snapshot::reservation_epoch); control thread only.
  std::uint64_t next_reservation_epoch() noexcept { return ++reservation_epoch_; }
  // The generation RT announced it is reading (hazard pointer), nullptr before RT ran. Control thread only: it is the only thread that frees
  // generations, so the pointee stays valid for the duration of the call.
  const core::Snapshot* rt_held_snapshot() const noexcept { return exchange_.rt_held(); }

  // Executes one command (anything but Hello, which the server owns). `changed` is set when a new
  // generation was published. The reply carries request_id and the current revision.
  ws::v1::ServerMessage handle(const ws::v1::ClientMessage& msg, bool& changed);

  // Frees the generations nobody can read any more. Returns how many were freed.
  std::size_t reclaim();

  void fill_state(ws::v1::State* out) const;               // current structural state
  void fill_live_values(ws::v1::PerformanceValues* out) const;  // RT-owned values, read lock-free

  std::uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
  std::size_t live_generations() const noexcept { return live_count_.load(std::memory_order_acquire); }
  std::uint64_t freed_generations() const noexcept { return freed_.load(std::memory_order_acquire); }
  std::uint64_t commands_rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }

  // FX budget (Story 2.7, AR18): the most the insert effects, sends and master may cost, in arbiter cost units. It is the amount
  // reserved for FX FIRST by VoiceArbiter::reserve() (LiveSetConfig::fx_cost), so voices can never eat into it; a command that RAISES the
  // cost above it (the resulting snapshot costs more than the budget and more than the current one) is refused with ERROR_CODE_FX_BUDGET_EXCEEDED and changes nothing (nothing published,
  // the effect instances built for it are freed on the spot). Set it before load_initial(); the default (core::kMaxFxCost) is
  // the cost of every slot of every part holding the heaviest insert, i.e. nothing is ever refused.
  void set_fx_budget(std::uint64_t units) noexcept { fx_budget_.store(units, std::memory_order_release); }
  std::uint64_t fx_budget() const noexcept { return fx_budget_.load(std::memory_order_acquire); }
  // Declared cost of the effect instances of the current generation (fixed topology included); 0 before load_initial().
  std::uint64_t fx_cost() const noexcept { return fx_cost_.load(std::memory_order_acquire); }

  static constexpr std::uint32_t kProtocolVersion = 1;

 private:
  bool commit(const core::Snapshot& work, ControlFail& fail);
  PublishStatus fault_ = PublishStatus::kOk;
  bool fault_set_ = false;

  core::SnapshotExchange& exchange_;
  const core::LastValueTable& live_;
  ControlEventQueue& rt_events_;
  Clock clock_;
  const core::Snapshot* current_ = nullptr;
  SceneHandler* scene_handler_ = nullptr;
  SwitchHandler* switch_handler_ = nullptr;
  std::uint64_t program_generation_ = 0;
  std::uint64_t reservation_epoch_ = 0;
  std::vector<core::Snapshot*> generations_;  // everything not yet freed, oldest first
  std::uint32_t ledger_used_ = 0;             // bit i = ledger slot i in use
  std::atomic<std::uint64_t> revision_{0};
  std::atomic<std::size_t> live_count_{0};
  std::atomic<std::uint64_t> freed_{0}, rejected_{0};
  std::atomic<std::uint64_t> fx_budget_{core::kMaxFxCost}, fx_cost_{0};
};

}  // namespace velora::host

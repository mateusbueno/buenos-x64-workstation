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

  static constexpr std::uint32_t kProtocolVersion = 1;

 private:
  bool commit(const core::Snapshot& work, ControlFail& fail);

  core::SnapshotExchange& exchange_;
  const core::LastValueTable& live_;
  ControlEventQueue& rt_events_;
  Clock clock_;
  const core::Snapshot* current_ = nullptr;
  std::vector<core::Snapshot*> generations_;  // everything not yet freed, oldest first
  std::uint32_t ledger_used_ = 0;             // bit i = ledger slot i in use
  std::atomic<std::uint64_t> revision_{0};
  std::atomic<std::size_t> live_count_{0};
  std::atomic<std::uint64_t> freed_{0}, rejected_{0};
};

}  // namespace velora::host

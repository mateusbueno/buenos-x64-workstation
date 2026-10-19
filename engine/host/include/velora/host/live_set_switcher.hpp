#pragma once
// Seamless Live Set switching (Story 4.5, FR79, FR81, AR19, AR21, NFR16 partial). `jump_to(id)` returns at once; the target's heads are made
// resident on a BACKGROUND worker thread while the current generation keeps playing and answering the keys; the control thread publishes the
// new generation only when every head of the target is resident, and RT swaps to it at a block boundary.
//
// Who does what.
//   * The worker thread (non-RT; never the audio threads and never the control thread's command handling) reads the target's file and runs
//     LiveSetLoader::stage(): resolve, size the heads from the bundle indexes (early head-budget refusal), bring every instrument resident
//     (cooperative cancellation between chunks, samples and instruments), build the generation and the reservation config. It touches neither
//     the plane, the arbiter nor the scene controller.
//   * The control thread (the ControlServer thread in engine-host; the test or render thread otherwise) calls jump_to() / cancel_load() and
//     tick(). tick() harvests the finished stage, commits it with LiveSetLoader::commit_seamless() (prepare the reservation, publish the
//     generation carrying it) and refreshes the published loading status.
//   * RT adopts the generation at the next block and commits the reservation in the same serial phase (BlockRenderer::adopt_generation,
//     VoiceArbiter::commit_prepared). Notes held, sustained by the pedal or releasing at the swap finish on the old generation: the PartRouter
//     releases them with the pitch, part and engine they started on, voices keep their sample bank, and the old generation (snapshot, banks,
//     the bundle leases of its heads) is freed by ControlPlane::reclaim() only when RT reports no voice references it.
//
// Loading status (ws.v1 State.loading). jump_to() puts the status in LOADING with the target's name; tick() republishes it (progress ratio,
// rate based estimate of the remaining seconds, smoothed) at least every `publish_interval_s` (default 0.5 s, so at least once per second)
// while the load runs. The server's poll loop is the control tick (ControlServer::set_tick(), every <= 20 ms). tick() returns true when the
// published status or the generation changed, which is when the server pushes a State to the subscribers. The clock is injectable
// (SwitcherOptions::clock) and the heads come through the HeadProvider, whose progress callback feeds the ratio, so a throttled provider and
// a fake clock test the cadence without sleeping.
//
// Cancellation latency. The flag is checked before the file is read, after it is parsed, before every part and instrument, after the sizes are
// estimated, between chunks of samples and inside the provider's read loop; a worker inside a step that does not check (one file read, one
// resolve, one provider estimate_bytes call, one chunk) finishes that step, then stops at the next check, and the new job starts right after.
//
// One load at a time. A jump to another target while one loads CANCELS the first (the token is checked between chunks of samples, samples and
// instruments, and inside the provider's read loop; its partial heads are released before the new target starts) and loads the new one; the
// sound is not touched. A jump to the Live Set that already sounds is a no-op (kAlreadyLoaded; if a load is running it is cancelled, which
// is how the player goes back), a jump to the target already loading is a no-op too (kAlreadyLoading: the load is not restarted).
// Failures never touch the sound: a target over `head_budget_bytes` (refused early from the index sizes, guarded while loading), an unknown
// id, an unreadable or invalid file, an instrument that cannot be made resident, a voice-pool or FX-budget refusal all leave the current Live
// Set playing and publish a typed error in the status. A cancelled load is INFORMATIONAL (LOAD_CANCELLED), not an error for the UI.
//
// Deterministic mode for offline renders and tests (`background_thread = false`): stage() runs inside jump_to() on the caller's thread and
// `hold_ticks` N delays the commit to the (N+1)-th tick() after the jump, i.e. N ticks of simulated load time (blocks, offline), so a render is a pure function of its
// inputs. The code path is the same as with the worker.
//
// Head budget. `head_budget_bytes` bounds the TARGET's heads (decoded PCM). The generation that keeps playing during the load stays resident
// as well; counting both against one tier budget is Story 4.7 (preload within the head budget, board-accepted).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "velora/host/control_plane.hpp"
#include "velora/host/live_set_loader.hpp"

namespace velora::host {

struct CatalogEntry {
  std::string id;    // the Live Set's UUID
  std::string name;
  std::string path;  // the .liveset file (the LiveSetStore layout: <dir>/<id>.liveset)
};

// Lists the `*.liveset` files of `dir` (the LiveSetStore directory), reading each for its id and name; sorted by name, then id. Files that
// cannot be read or are invalid are skipped and described in `problems` (optional). Control or I/O thread.
std::vector<CatalogEntry> scan_live_set_dir(const std::string& dir, std::uint32_t part_count, std::vector<std::string>* problems = nullptr);
// The catalog entry of one Live Set file (reads it). False with `error` set when it cannot be read or is invalid.
bool catalog_entry_for_file(const std::string& path, std::uint32_t part_count, CatalogEntry& out, std::string& error);

struct SwitcherOptions {
  std::uint64_t head_budget_bytes = 64ull << 30;  // the Mac default is large; the board's tier budget comes from the tier profile (Story 4.1)
  std::function<double()> clock;                  // seconds, monotonic; default std::chrono::steady_clock
  double publish_interval_s = 0.5;                // the loading status is republished at least this often while loading (must stay <= 1 s)
  double assumed_bytes_per_second = 200e6;        // ETA before any rate was measured
  double rate_smoothing = 0.3;                    // weight of the newest window in the smoothed rate (0 < a <= 1)
  bool background_thread = true;                  // false: deterministic mode, stage() runs inside jump_to()
  std::uint32_t hold_ticks = 0;                   // simulated load time: the commit happens at the (N+1)-th tick() after the jump at the earliest
  double busy_retry_s = 5.0;                      // a commit refused with kBusy (ledger full) is retried on each tick for this long
};

enum class SwitchState : std::uint8_t { kIdle = 0, kLoading, kReady, kFailed };
enum class SwitchError : std::uint8_t { kNone = 0, kHeadBudgetExceeded, kLoadCancelled, kNoSuchLiveSet, kLoadFailed };
const char* to_string(SwitchState s);
const char* to_string(SwitchError e);

struct SwitchStatus {
  SwitchState state = SwitchState::kIdle;
  std::string target_id, target_name;
  float estimated_remaining_s = 0;  // finite, >= 0; 0 unless loading
  float progress = 0;               // 0..1
  std::uint64_t bytes_loaded = 0, bytes_total = 0;
  SwitchError last_error = SwitchError::kNone;
  std::string last_error_detail;    // for kLoadFailed the reason: read_failed, invalid_live_set, ...
  std::uint64_t seq = 0;            // increases with every publication
  std::uint32_t cancelled_loads = 0;
  bool loading() const { return state == SwitchState::kLoading; }
  bool informational() const { return last_error == SwitchError::kLoadCancelled; }
};

class LiveSetSwitcher final : public SwitchHandler {
 public:
  // Registers itself as `plane`'s switch handler; `loader` and `plane` must outlive it. The loader's HeadProvider is used by the worker
  // thread: do not call loader.load() while a jump is loading.
  LiveSetSwitcher(LiveSetLoader& loader, ControlPlane& plane, SwitcherOptions options = {});
  ~LiveSetSwitcher() override;  // cancels the load in progress, joins the worker
  LiveSetSwitcher(const LiveSetSwitcher&) = delete;
  LiveSetSwitcher& operator=(const LiveSetSwitcher&) = delete;

  // The Live Sets a jump can name. Control thread.
  void set_catalog(std::vector<CatalogEntry> entries);
  void add_to_catalog(CatalogEntry entry);  // replaces an entry with the same id
  const std::vector<CatalogEntry>& catalog() const { return catalog_; }

  // Control thread. See the header comment for the rules; returns at once.
  SwitchOutcome jump_to(const std::string& id);
  // Cancels the load in progress (informational LOAD_CANCELLED, state IDLE); the current Live Set is untouched.
  SwitchOutcome cancel_load();
  // Control thread, periodically (the server poll loop, or once per block offline). Returns true when the status or the generation changed.
  bool tick();

  const SwitchStatus& status() const { return status_; }
  bool loading() const { return active_; }
  // Loads the staged result of a finished job and the generation it published, observable by tests.
  std::uint64_t switches_published() const { return published_; }

  // SwitchHandler (ControlPlane seam).
  SwitchOutcome jump(const std::string& id) override { return jump_to(id); }
  SwitchOutcome cancel() override { return cancel_load(); }
  void fill_status(ws::v1::State* out) const override;

 private:
  struct Progress {
    std::atomic<std::uint64_t> done{0}, total{0};
    std::atomic<std::uint32_t> inst_done{0}, inst_total{0};
  };
  struct Job {
    std::uint64_t token = 0;
    CatalogEntry target;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<Progress> progress;
  };
  struct JobResult {
    std::uint64_t token = 0;
    LiveSetLoadResult result;
    std::unique_ptr<StagedLiveSet> staged;
  };

  std::unique_ptr<JobResult> run_job(const Job& job);
  void worker_main();
  double now() const;
  void publish();                          // status_.seq++, marks the status dirty
  void start_status(const CatalogEntry& target);
  void cancel_active(bool count_as_cancelled);
  void finish_failure(const LiveSetLoadResult& r);
  bool try_commit();
  void refresh_progress(double now_s);

  LiveSetLoader& loader_;
  ControlPlane& plane_;
  SwitcherOptions opt_;
  std::vector<CatalogEntry> catalog_;
  SwitchStatus status_;
  bool dirty_ = false;

  // Control-thread state of the load in progress.
  bool active_ = false;
  Job active_job_;
  std::uint64_t next_token_ = 0;
  std::uint32_t ticks_since_jump_ = 0;
  std::unique_ptr<JobResult> held_;  // the finished stage waiting for its commit
  double busy_since_ = -1;
  double last_publish_s_ = 0, window_start_s_ = 0;
  std::uint64_t window_start_done_ = 0;
  double rate_ = 0;  // smoothed bytes per second, 0 = not measured yet
  std::uint64_t published_ = 0;

  // Worker.
  std::thread worker_;
  std::mutex m_;
  std::condition_variable cv_;
  std::optional<Job> pending_;
  std::unique_ptr<JobResult> done_;
  bool stop_ = false;
};

}  // namespace velora::host

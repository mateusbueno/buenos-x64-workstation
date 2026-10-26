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
// Head budget. `head_budget_bytes` bounds the TARGET's heads (decoded PCM) of a user-initiated load (a jump): the generation that keeps playing during
// that load stays resident as well, and counting both is not asked of it (Story 4.5). The PRELOAD below shares the budget between both generations.
//
// Next Live Set preload (Story 4.7, FR80, AR21; the <10 ms command-to-swap figure is a dev-board measurement, deferred). The setlist controller names the
// NEXT Live Set of the loaded setlist with set_preload_target(current, next). Once `current` sounds and no user load is running, tick() STAGES `next`
// on the same worker (LiveSetLoader::stage(): heads resident, generation built; NOT published) and the status goes LOADING -> READY. Budget: the preload
// may hold budget - (heads of the sounding generation) bytes on top of it; an instrument both generations use (same library, bundle and instrument key)
// is NOT loaded again: the staged generation takes the resident instrument, so it is held once and counted once (StageControl::reuse). When current +
// next (deduped) do not fit, stage() refuses from the bundle indexes before any sample is read: SKIPPED_BUDGET, not ready, and a later "next" is the
// ordinary Story 4.5 background load, unchanged. Any other stage failure is FAILED (the same fallback; the 4.5 path then reports its own typed error).
// Nothing is published and the arbiter is not touched while the preload waits: the voice-pool reservation is PREPARED AT COMMIT by commit_seamless()
// (the same refusal path as 4.5), never reserved at preload, so the sounding Live Set can never be starved by a preload. A READY preload holds its
// heads (and so the bundle leases) until it is committed or discarded; discarding destroys the staged generation, which frees them.
//   * jump_to(next) with a READY preload commits it inside the call: no instrument is loaded at command time (only commit_seamless(): prepare the
//     reservation, publish), RT adopts it at the next block, and the old generation drains as in 4.5. The following preload starts once the setlist
//     controller names the new pair (it does so right after the step, and on every tick).
//   * jump_to(next) while the preload is still LOADING cancels it and runs the ordinary 4.5 load for that target (simplest cancel semantics: one
//     job at a time per kind, the worker stops at its next cancellation check).
//   * jump_to(anything else), reload_current(): the preload is discarded (cancelled/released) first; it is restaged for the (possibly new) pair once
//     the new current sounds. cancel_load() while LOADING drops the preload and does not restart it for the same pair.
//   * Invalidation: set_preload_target() with another pair, live_set_changed(id) (the target's file was written), invalidate_preload() (a library
//     change made heads stale), a catalog replacement/removal of the target, and a changed file signature (size, mtime) seen by the throttled check.
// Staleness. A verdict is made against the sounding generation's head bytes; when that changes (an edit or library commit, a reload) the preload is
// discarded and restaged, so READY / SKIPPED_BUDGET never describe a generation that no longer sounds. The staged Live Set's overlay (unsaved edits)
// is compared by LiveSetEditPort::overlay_revision() (a full compare only when the revisions differ or are unknown); its file by size + mtime of the
// Live Set file ONLY; its heads by the library's `current` link (LiveSetLoader::heads_still_current), at the status cadence and again at the jump. A
// mismatch at the jump falls back to the 4.5 load. engine-host has no library-change event yet, so these checks are how a replaced or removed library
// is noticed; invalidate_preload() is the call for one when it exists.
// A commit that is refused at the jump (arbiter, FX budget) leaves the preload READY and is reported as the ordinary typed failure; a busy ledger is
// retried like 4.5 (the preload is consumed then).
// Known limits. Generations that still drain (voices pinned on an old generation) are not counted in the budget: the real resident bytes can exceed it by
// up to the pinned-generation limit. FAILED and SKIPPED_BUDGET are terminal until the pair or the sounding Live Set changes (no retry or backoff yet). A
// cancelled preload can delay a user load until the worker's next cancellation check. LOADING's head_bytes_held counts only what the stage loads itself
// (shared heads are not in it), as does READY's.
// Deterministic mode: the preload stage runs inside tick() and READY is declared at the (hold_ticks+1)-th tick after it started; the clock is injectable.
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
#include "velora/host/edit_port.hpp"
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

enum class PreloadState : std::uint8_t { kNone = 0, kLoading, kReady, kSkippedBudget, kFailed };
const char* to_string(PreloadState s);

struct PreloadStatus {
  PreloadState state = PreloadState::kNone;
  std::string target_id, target_name;     // the next Live Set ("" = none)
  std::uint64_t head_bytes_held = 0;      // PCM the preload holds on top of the sounding generation (partial while loading)
  std::uint64_t head_bytes_current = 0;   // PCM of the sounding generation the verdict (LOADING / READY / SKIPPED_BUDGET / FAILED) was made against; 0 with NONE
  std::uint64_t head_bytes_needed = 0;    // SKIPPED_BUDGET: the PCM the next Live Set needs beyond the heads it shares with the current one
  std::string detail;                     // FAILED / SKIPPED_BUDGET: the reason, no numbers (they are in the typed fields)
  std::uint64_t seq = 0;
  bool ready() const { return state == PreloadState::kReady; }
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
  void remove_from_catalog(const std::string& id);  // Story 4.6: a deleted Live Set can no longer be a jump target
  const std::vector<CatalogEntry>& catalog() const { return catalog_; }
  // Story 4.6: the Live Set that sounds (last published by the loader, "" before the first) and the one stepping is relative to: the target of the
  // load in progress, else the Live Set that sounds. Control thread.
  const std::string& sounding_id() const { return loader_.loaded_id(); }
  const std::string& effective_id() const { return active_ ? active_job_.target.id : loader_.loaded_id(); }

  // Control thread. See the header comment for the rules; returns at once.
  SwitchOutcome jump_to(const std::string& id);
  // Story 4.9: rebuilds the Live Set that sounds from its in-memory edited state (the edit session's overlay) by the same background load and seamless
  // swap as a jump; the one case where the target is the Live Set that already sounds. kAccepted, or kUnknownLiveSet when nothing sounds.
  SwitchOutcome reload_current();
  // Story 4.9: with a port, a jump to a Live Set that has unsaved edits builds its generation from the edited state, not from the file. Control thread.
  void set_edit_port(LiveSetEditPort* port) { edits_ = port; }
  // Story 4.7. Control thread. `current_id` is the Live Set that must sound for the preload to start (the one `next_id` follows); `next_id` "" = nothing to
  // preload. Naming the pair that is already set changes nothing; a different pair discards the preload (see the header comment).
  void set_preload_target(const std::string& current_id, const std::string& next_id);
  void invalidate_preload();                       // discards the preload and lets it restage (library change, ...)
  void live_set_changed(const std::string& id);    // the Live Set's file was written: a preload of it is invalidated
  const PreloadStatus& preload_status() const { return pre_status_; }
  std::uint32_t preload_commits() const { return preload_commits_; }  // jumps served from a READY preload
  // Cancels the load in progress (informational LOAD_CANCELLED, state IDLE); the current Live Set is untouched.
  SwitchOutcome cancel_load();
  // Control thread, periodically (the server poll loop, or once per block offline). Returns true when the status or the generation changed.
  bool tick();

  const SwitchStatus& status() const { return status_; }
  bool loading() const { return active_; }
  // Story 4.9: the id of the load job in progress (SwitchOutcome::token of the call that started it), 0 when none.
  std::uint64_t active_token() const { return active_ ? active_job_.token : 0; }
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
    std::shared_ptr<const ::velora::liveset::v1::LiveSet> overlay;  // Story 4.9: the edited state to build from instead of the file
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<Progress> progress;
    // Story 4.7 (preload jobs only)
    bool preload = false;
    std::vector<HeadUse> reuse;       // the sounding generation's heads, taken over instead of loaded again
    std::uint64_t head_budget = 0;    // what the preload may add to them (0 = unlimited)
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
  void commit_succeeded();
  bool commit_refused(const LiveSetLoadResult& r);
  void refresh_progress(double now_s);

  SwitchOutcome jump_impl(const std::string& id, bool reload);

  // Story 4.7
  void pre_publish();
  void pre_set_target_fields();
  void discard_preload();
  void maybe_start_preload();
  void harvest_preload();
  void check_preload_file();
  bool overlay_unchanged(const ::velora::liveset::v1::LiveSet* now_overlay, std::uint64_t now_revision) const;
  bool take_preload(const CatalogEntry& entry, const ::velora::liveset::v1::LiveSet* overlay, SwitchOutcome& o);

  LiveSetLoader& loader_;
  ControlPlane& plane_;
  LiveSetEditPort* edits_ = nullptr;
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

  // Story 4.7: control-thread state of the preload.
  struct FileSig {
    bool valid = false;
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    bool operator==(const FileSig& o) const { return valid == o.valid && size == o.size && mtime == o.mtime; }
  };
  static FileSig sig_of(const std::string& path);
  PreloadStatus pre_status_;
  std::string pre_for_, pre_next_;      // the requested pair
  bool pre_blocked_ = false;            // cancelled by cancel_load(): not restarted for this pair
  bool pre_running_ = false;            // LOADING: a stage job is in flight (or finished and held for the simulated load time)
  Job pre_job_;
  CatalogEntry pre_entry_;
  std::unique_ptr<JobResult> pre_held_;  // the finished stage: LOADING (waiting) or READY
  std::uint32_t pre_ticks_ = 0;
  std::uint64_t pre_overlay_rev_ = 0;   // LiveSetEditPort::overlay_revision() when it was staged
  FileSig pre_sig_;
  double pre_check_s_ = 0, pre_pub_s_ = 0;
  std::uint32_t preload_commits_ = 0;

  // Worker.
  std::thread worker_;
  std::mutex m_;
  std::condition_variable cv_;
  std::optional<Job> pending_;
  std::optional<Job> pending_pre_;       // Story 4.7: a preload stage, run when no user job is waiting
  std::unique_ptr<JobResult> done_;
  std::unique_ptr<JobResult> done_pre_;
  bool stop_ = false;
};

}  // namespace velora::host

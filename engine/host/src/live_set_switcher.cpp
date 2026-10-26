#include "velora/host/live_set_switcher.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace velora::host {

namespace fs = std::filesystem;

const char* to_string(SwitchState s) {
  switch (s) {
    case SwitchState::kIdle: return "idle";
    case SwitchState::kLoading: return "loading";
    case SwitchState::kReady: return "ready";
    case SwitchState::kFailed: return "failed";
  }
  return "?";
}

const char* to_string(PreloadState s) {
  switch (s) {
    case PreloadState::kNone: return "none";
    case PreloadState::kLoading: return "loading";
    case PreloadState::kReady: return "ready";
    case PreloadState::kSkippedBudget: return "skipped_budget";
    case PreloadState::kFailed: return "failed";
  }
  return "?";
}

const char* to_string(SwitchError e) {
  switch (e) {
    case SwitchError::kNone: return "none";
    case SwitchError::kHeadBudgetExceeded: return "head budget exceeded";
    case SwitchError::kLoadCancelled: return "load cancelled";
    case SwitchError::kNoSuchLiveSet: return "no such live set";
    case SwitchError::kLoadFailed: return "load failed";
  }
  return "?";
}

namespace {

const char* reason_of(LiveSetLoadStatus s) {
  switch (s) {
    case LiveSetLoadStatus::kReadFailed: return "read_failed";
    case LiveSetLoadStatus::kInvalidLiveSet: return "invalid_live_set";
    case LiveSetLoadStatus::kPartCountUnsupported: return "part_count_unsupported";
    case LiveSetLoadStatus::kInstrumentLoadFailed: return "instrument_load_failed";
    case LiveSetLoadStatus::kArbiterRejected: return "arbiter_rejected";
    case LiveSetLoadStatus::kFxBudgetExceeded: return "fx_budget_exceeded";
    case LiveSetLoadStatus::kBusy: return "busy";
    default: return "internal";
  }
}

}  // namespace

bool catalog_entry_for_file(const std::string& path, std::uint32_t part_count, CatalogEntry& out, std::string& error) {
  liveset::ReadOptions ro;
  ro.part_count = part_count;
  liveset::LoadResult lr;
  const liveset::Status st = liveset::read_file(path, ro, lr);
  if (st != liveset::Status::kOk) {
    error = path + ": " + (st == liveset::Status::kInvalid ? "invalid Live Set" : "cannot be read as a Live Set");
    return false;
  }
  out.id = lr.live_set.id();
  out.name = lr.live_set.name();
  out.path = path;
  return true;
}

std::vector<CatalogEntry> scan_live_set_dir(const std::string& dir, std::uint32_t part_count, std::vector<std::string>* problems) {
  std::vector<CatalogEntry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path& p = it->path();
    if (p.extension() != ".liveset") continue;
    std::error_code sec;
    if (!it->is_regular_file(sec) || sec) continue;
    CatalogEntry e;
    std::string err;
    if (catalog_entry_for_file(p.string(), part_count, e, err)) out.push_back(std::move(e));
    else if (problems) problems->push_back(err);
  }
  if (ec && problems) problems->push_back(dir + ": " + ec.message());
  std::sort(out.begin(), out.end(), [](const CatalogEntry& a, const CatalogEntry& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
  return out;
}

LiveSetSwitcher::LiveSetSwitcher(LiveSetLoader& loader, ControlPlane& plane, SwitcherOptions options)
    : loader_(loader), plane_(plane), opt_(std::move(options)) {
  if (!(opt_.publish_interval_s > 0) || opt_.publish_interval_s > 1.0) opt_.publish_interval_s = 0.5;  // "at least once per second"
  if (!(opt_.rate_smoothing > 0) || opt_.rate_smoothing > 1.0) opt_.rate_smoothing = 0.3;
  if (!(opt_.assumed_bytes_per_second > 0)) opt_.assumed_bytes_per_second = 200e6;
  plane_.set_switch_handler(this);
  if (opt_.background_thread) worker_ = std::thread([this] { worker_main(); });
}

LiveSetSwitcher::~LiveSetSwitcher() {
  plane_.set_switch_handler(nullptr);
  if (active_job_.cancel) active_job_.cancel->store(true, std::memory_order_release);
  if (pre_job_.cancel) pre_job_.cancel->store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    if (pending_) pending_->cancel->store(true, std::memory_order_release);
    if (pending_pre_) pending_pre_->cancel->store(true, std::memory_order_release);
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

double LiveSetSwitcher::now() const {
  if (opt_.clock) return opt_.clock();
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LiveSetSwitcher::set_catalog(std::vector<CatalogEntry> entries) { catalog_ = std::move(entries); }

void LiveSetSwitcher::add_to_catalog(CatalogEntry entry) {
  const bool is_target = !pre_next_.empty() && entry.id == pre_next_;
  for (CatalogEntry& e : catalog_)
    if (e.id == entry.id) {
      const bool changed = e.path != entry.path || e.name != entry.name;
      e = std::move(entry);
      if (is_target && changed) invalidate_preload();  // the target's file or name moved
      return;
    }
  if (is_target) invalidate_preload();  // the target was missing from the catalog (FAILED): it can be staged now
  catalog_.push_back(std::move(entry));
}

void LiveSetSwitcher::remove_from_catalog(const std::string& id) {
  catalog_.erase(std::remove_if(catalog_.begin(), catalog_.end(), [&](const CatalogEntry& e) { return e.id == id; }), catalog_.end());
  if (!pre_next_.empty() && id == pre_next_) invalidate_preload();  // the staged Live Set no longer exists as a jump target
}

void LiveSetSwitcher::publish() {
  ++status_.seq;
  dirty_ = true;
}

void LiveSetSwitcher::start_status(const CatalogEntry& target) {
  status_.state = SwitchState::kLoading;
  status_.target_id = target.id;
  status_.target_name = target.name;
  status_.estimated_remaining_s = 0;
  status_.progress = 0;
  status_.bytes_loaded = status_.bytes_total = 0;
  status_.last_error = SwitchError::kNone;
  status_.last_error_detail.clear();
  rate_ = 0;
  window_start_done_ = 0;
  window_start_s_ = last_publish_s_ = now();
  publish();
}

// Stops the load in progress and frees whatever it made resident that is not already freed (a finished stage nobody committed is destroyed here;
// a stage still running notices the flag, releases its partial heads and returns).
void LiveSetSwitcher::cancel_active(bool count_as_cancelled) {
  if (!active_) return;
  active_job_.cancel->store(true, std::memory_order_release);
  std::unique_ptr<JobResult> trash;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (pending_ && pending_->token == active_job_.token) pending_.reset();
    trash = std::move(done_);
  }
  trash.reset();
  held_.reset();
  active_ = false;
  busy_since_ = -1;
  if (count_as_cancelled) ++status_.cancelled_loads;
}

SwitchOutcome LiveSetSwitcher::jump_to(const std::string& id) { return jump_impl(id, false); }

SwitchOutcome LiveSetSwitcher::reload_current() {
  if (loader_.loaded_id().empty()) {
    SwitchOutcome o;
    o.result = SwitchOutcome::Result::kUnknownLiveSet;
    return o;
  }
  return jump_impl(loader_.loaded_id(), true);
}

SwitchOutcome LiveSetSwitcher::jump_impl(const std::string& id, bool reload) {
  SwitchOutcome o;
  const CatalogEntry* target = nullptr;
  for (const CatalogEntry& e : catalog_)
    if (e.id == id) target = &e;
  CatalogEntry synthetic;
  if (!target && reload) {  // the Live Set that sounds need not be in the catalog (started from a file); the overlay is all a reload needs
    synthetic.id = id;
    synthetic.name = loader_.loaded_name();
    target = &synthetic;
  }
  if (!target) {
    o.result = SwitchOutcome::Result::kUnknownLiveSet;
    o.target_id = id;
    if (!active_) status_.state = SwitchState::kFailed;  // a load in progress keeps loading; the error is still published
    status_.last_error = SwitchError::kNoSuchLiveSet;
    status_.last_error_detail = id;
    publish();
    return o;
  }
  o.target_id = target->id;
  o.target_name = target->name;
  if (id == loader_.loaded_id() && !reload) {  // already sounding: a no-op; a load that is running is the player changing their mind
    o.result = SwitchOutcome::Result::kAlreadyLoaded;
    if (active_) {
      cancel_active(true);
      status_.state = SwitchState::kIdle;
      status_.estimated_remaining_s = 0;
      status_.progress = 0;
      status_.last_error = SwitchError::kLoadCancelled;
      status_.last_error_detail.clear();
      o.superseded_previous = true;
      publish();
    }
    return o;
  }
  if (active_ && id == active_job_.target.id) {  // already on its way: do not restart a long load
    o.result = SwitchOutcome::Result::kAlreadyLoading;
    return o;
  }
  const CatalogEntry entry = *target;  // cancel_active() does not touch the catalog, but keep a copy anyway
  std::shared_ptr<const ::velora::liveset::v1::LiveSet> overlay;
  if (edits_)  // a Live Set with unsaved edits is rebuilt from them; the copy is taken here, on the control thread, before the worker starts
    if (const ::velora::liveset::v1::LiveSet* ov = edits_->overlay_for(entry.id)) overlay = std::make_shared<const ::velora::liveset::v1::LiveSet>(*ov);
  if (reload && !overlay) {  // nothing to rebuild from
    o.result = SwitchOutcome::Result::kUnknownLiveSet;
    return o;
  }
  // Story 4.7: the next Live Set is already staged: commit it now, no load work (see the header comment).
  if (!reload && take_preload(entry, overlay.get(), o)) return o;
  discard_preload();  // any other jump (or a reload) supersedes the preload: its heads are released before the new target loads
  o.superseded_previous = active_;
  cancel_active(true);

  Job job;
  job.token = ++next_token_;
  job.target = entry;
  job.overlay = std::move(overlay);
  job.cancel = std::make_shared<std::atomic<bool>>(false);
  job.progress = std::make_shared<Progress>();
  active_job_ = job;
  active_ = true;
  ticks_since_jump_ = 0;
  busy_since_ = -1;
  start_status(entry);
  if (opt_.background_thread) {
    {
      std::lock_guard<std::mutex> lk(m_);
      pending_ = std::move(job);
    }
    cv_.notify_one();
  } else {
    std::unique_ptr<JobResult> r = run_job(job);  // deterministic mode: the stage runs here, the commit waits for hold_ticks
    std::lock_guard<std::mutex> lk(m_);
    done_ = std::move(r);
  }
  o.result = SwitchOutcome::Result::kAccepted;
  o.token = active_job_.token;
  return o;
}

SwitchOutcome LiveSetSwitcher::cancel_load() {
  SwitchOutcome o;
  o.result = SwitchOutcome::Result::kAlreadyLoaded;
  o.target_id = status_.target_id;
  o.target_name = status_.target_name;
  if (!active_) {
    if (pre_running_) {  // Story 4.7: a preload in flight is dropped (the current Live Set is untouched) and not restarted for this pair
      discard_preload();
      pre_blocked_ = true;
    }
    return o;
  }
  cancel_active(true);
  status_.state = SwitchState::kIdle;
  status_.estimated_remaining_s = 0;
  status_.progress = 0;
  status_.last_error = SwitchError::kLoadCancelled;
  status_.last_error_detail.clear();
  publish();
  o.cancelled = true;
  return o;
}

std::unique_ptr<LiveSetSwitcher::JobResult> LiveSetSwitcher::run_job(const Job& job) {
  auto jr = std::make_unique<JobResult>();
  jr->token = job.token;
  const auto cancelled = [&job] { return job.cancel->load(std::memory_order_acquire); };
  liveset::ReadOptions ro;
  ro.part_count = loader_.options().part_count;
  liveset::LoadResult lr;
  liveset::Status st = liveset::Status::kOk;
  if (job.overlay) lr.live_set = *job.overlay;  // the edited state (validated by stage() like any Live Set)
  else st = liveset::read_file(job.target.path, ro, lr);
  if (cancelled()) {
    jr->result.status = LiveSetLoadStatus::kCancelled;
    return jr;
  }
  if (st == liveset::Status::kInvalid) {
    jr->result.status = LiveSetLoadStatus::kInvalidLiveSet;
    jr->result.detail = job.target.path;
    jr->result.report = std::move(lr.report);
    return jr;
  }
  if (st != liveset::Status::kOk) {
    jr->result.status = LiveSetLoadStatus::kReadFailed;
    jr->result.read_status = st;
    jr->result.detail = job.target.path;
    return jr;
  }
  if (cancelled()) {  // cancelled while the file was read and parsed: do not start the (long) stage
    jr->result.status = LiveSetLoadStatus::kCancelled;
    return jr;
  }
  StageControl ctl;
  ctl.cancel = job.cancel.get();
  ctl.head_budget_bytes = job.preload ? job.head_budget : opt_.head_budget_bytes;
  if (job.preload) ctl.reuse = &job.reuse;
  Progress* prog = job.progress.get();
  ctl.progress = [prog](const StageProgress& p) {
    prog->total.store(p.bytes_total, std::memory_order_relaxed);
    prog->inst_total.store(p.instruments_total, std::memory_order_relaxed);
    prog->inst_done.store(p.instruments_done, std::memory_order_relaxed);
    prog->done.store(p.bytes_done, std::memory_order_release);
  };
  jr->result = loader_.stage(lr.live_set, ctl, jr->staged);
  return jr;
}

void LiveSetSwitcher::worker_main() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [this] { return stop_ || pending_.has_value() || pending_pre_.has_value(); });
      if (stop_) return;
      if (pending_) {  // a user-initiated load always goes before a preload
        job = std::move(*pending_);
        pending_.reset();
      } else {
        job = std::move(*pending_pre_);
        pending_pre_.reset();
      }
    }
    std::unique_ptr<JobResult> r;
    if (!job.cancel->load(std::memory_order_acquire)) r = run_job(job);
    if (!r) continue;
    std::lock_guard<std::mutex> lk(m_);
    if (!job.cancel->load(std::memory_order_acquire)) (job.preload ? done_pre_ : done_) = std::move(r);
    // else: cancelled while running; `r` (and any heads it still held) is destroyed here, on the worker, when the lock scope ends
  }
}

void LiveSetSwitcher::finish_failure(const LiveSetLoadResult& r) {
  active_ = false;
  busy_since_ = -1;
  status_.estimated_remaining_s = 0;
  if (r.status == LiveSetLoadStatus::kCancelled) {  // a stage that stopped on its own flag: informational
    ++status_.cancelled_loads;
    status_.state = SwitchState::kIdle;
    status_.last_error = SwitchError::kLoadCancelled;
    status_.last_error_detail.clear();
    publish();
    return;
  }
  status_.state = SwitchState::kFailed;
  if (r.status == LiveSetLoadStatus::kHeadBudgetExceeded) {
    status_.last_error = SwitchError::kHeadBudgetExceeded;
    status_.last_error_detail = "head_bytes=" + std::to_string(r.head_bytes) + " head_budget=" + std::to_string(r.head_budget);
  } else {
    status_.last_error = SwitchError::kLoadFailed;
    status_.last_error_detail = reason_of(r.status);
  }
  publish();
}

bool LiveSetSwitcher::try_commit() {
  const LiveSetLoadResult r = loader_.commit_seamless(*held_->staged);
  if (r.status == LiveSetLoadStatus::kOk) {
    commit_succeeded();
    return true;
  }
  return commit_refused(r);
}

void LiveSetSwitcher::commit_succeeded() {
  {
    held_.reset();
    active_ = false;
    busy_since_ = -1;
    status_.state = SwitchState::kReady;
    status_.estimated_remaining_s = 0;
    status_.progress = 1.0f;
    status_.bytes_loaded = status_.bytes_total = std::max(status_.bytes_total, status_.bytes_loaded);
    status_.last_error = SwitchError::kNone;
    status_.last_error_detail.clear();
    ++published_;
    publish();
    if (pre_status_.state == PreloadState::kSkippedBudget || pre_status_.state == PreloadState::kFailed) {
      pre_status_.state = PreloadState::kNone;
      pre_status_.head_bytes_current = pre_status_.head_bytes_needed = 0;  // the current Live Set changed: the verdict on its successor no longer holds
      pre_status_.detail.clear();
      pre_publish();
    }
  }
}

bool LiveSetSwitcher::commit_refused(const LiveSetLoadResult& r) {
  if (r.status == LiveSetLoadStatus::kBusy) {  // 32 generations pinned by sounding notes: wait for some to end
    const double t = now();
    if (busy_since_ < 0) busy_since_ = t;
    if (t - busy_since_ <= opt_.busy_retry_s) return false;
  }
  held_.reset();
  finish_failure(r);
  return false;
}

void LiveSetSwitcher::refresh_progress(double now_s) {
  if (now_s - last_publish_s_ < opt_.publish_interval_s) return;
  const std::uint64_t done = active_job_.progress->done.load(std::memory_order_acquire);
  const std::uint64_t total = active_job_.progress->total.load(std::memory_order_relaxed);
  const double dt = now_s - window_start_s_;
  if (dt > 0) {  // rate over the window since the last publication, smoothed
    const double inst = static_cast<double>(done >= window_start_done_ ? done - window_start_done_ : 0) / dt;
    rate_ = rate_ > 0 ? opt_.rate_smoothing * inst + (1.0 - opt_.rate_smoothing) * rate_ : inst;
  }
  window_start_s_ = now_s;
  window_start_done_ = done;
  const double remaining = total > done ? static_cast<double>(total - done) : 0.0;
  const double rate = rate_ > 0 ? rate_ : opt_.assumed_bytes_per_second;
  double eta = remaining / rate;
  if (!std::isfinite(eta) || eta < 0) eta = 0;
  status_.estimated_remaining_s = static_cast<float>(std::min(eta, 1e9));
  status_.progress = total ? static_cast<float>(std::min(1.0, static_cast<double>(done) / static_cast<double>(total))) : 0.0f;
  status_.bytes_loaded = done;
  status_.bytes_total = total;
  last_publish_s_ = now_s;
  publish();
}

bool LiveSetSwitcher::tick() {
  bool generation = false;
  if (active_) {
    ++ticks_since_jump_;
    if (!held_) {
      std::unique_ptr<JobResult> stale;
      std::lock_guard<std::mutex> lk(m_);
      if (done_ && done_->token == active_job_.token) held_ = std::move(done_);
      else stale = std::move(done_);
    }
    if (held_ && held_->result.status != LiveSetLoadStatus::kOk) {  // a refusal is reported at once, not after the simulated load time
      const LiveSetLoadResult r = held_->result;
      held_.reset();
      finish_failure(r);
    } else if (held_ && ticks_since_jump_ > opt_.hold_ticks) {
      // The totals are only known to the stage; keep the last progress it reported for the final status.
      status_.bytes_total = active_job_.progress->total.load(std::memory_order_relaxed);
      status_.bytes_loaded = active_job_.progress->done.load(std::memory_order_acquire);
      generation = try_commit();
    }
    // Still loading, held for the simulated load time, or retrying a busy commit: the status keeps its cadence (at least once per second).
    if (active_) refresh_progress(now());
  }
  harvest_preload();
  check_preload_file();
  maybe_start_preload();
  const bool changed = dirty_ || generation;
  dirty_ = false;
  return changed;
}

// ---- Story 4.7: next Live Set preload ------------------------------------------------------------------------------------------------------

LiveSetSwitcher::FileSig LiveSetSwitcher::sig_of(const std::string& path) {
  FileSig s;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return s;
  const auto mt = fs::last_write_time(path, ec);
  if (ec) return s;
  s.valid = true;
  s.size = size;
  s.mtime = static_cast<std::int64_t>(mt.time_since_epoch().count());
  return s;
}

void LiveSetSwitcher::pre_publish() {
  ++pre_status_.seq;
  dirty_ = true;
}

void LiveSetSwitcher::pre_set_target_fields() {
  std::string name;
  for (const CatalogEntry& e : catalog_)
    if (e.id == pre_next_) name = e.name;
  if (pre_status_.target_id == pre_next_ && pre_status_.target_name == name) return;
  pre_status_.target_id = pre_next_;
  pre_status_.target_name = name;
  pre_publish();
}

// Cancels the stage in flight and frees everything the preload holds (a finished stage is destroyed here: its heads and bundle leases go with it).
void LiveSetSwitcher::discard_preload() {
  if (pre_job_.cancel) pre_job_.cancel->store(true, std::memory_order_release);
  std::unique_ptr<JobResult> trash;
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_pre_.reset();
    trash = std::move(done_pre_);
  }
  trash.reset();
  pre_held_.reset();
  pre_running_ = false;
  pre_job_ = Job{};
  if (pre_status_.state != PreloadState::kNone || pre_status_.head_bytes_held != 0 || pre_status_.head_bytes_current != 0 || pre_status_.head_bytes_needed != 0 ||
      !pre_status_.detail.empty()) {
    pre_status_.state = PreloadState::kNone;
    pre_status_.head_bytes_held = 0;
    pre_status_.head_bytes_current = 0;
    pre_status_.head_bytes_needed = 0;
    pre_status_.detail.clear();
    pre_publish();
  }
}

void LiveSetSwitcher::set_preload_target(const std::string& current_id, const std::string& next_id) {
  if (current_id == pre_for_ && next_id == pre_next_) return;
  pre_for_ = current_id;
  pre_next_ = next_id;
  pre_blocked_ = false;
  discard_preload();
  pre_set_target_fields();
}

void LiveSetSwitcher::invalidate_preload() {
  pre_blocked_ = false;
  discard_preload();  // the state is NONE again: the next tick() restages it
}

void LiveSetSwitcher::live_set_changed(const std::string& id) {
  if (!pre_next_.empty() && id == pre_next_) invalidate_preload();
}

void LiveSetSwitcher::maybe_start_preload() {
  if (pre_running_ || pre_held_ || pre_blocked_ || pre_next_.empty() || active_) return;
  if (pre_status_.state != PreloadState::kNone) return;  // READY / SKIPPED_BUDGET / FAILED stand until the pair or the current Live Set changes
  const std::string& cur = loader_.loaded_id();
  if (cur.empty() || cur != pre_for_ || pre_next_ == cur) return;  // wait until the current Live Set sounds; its heads are what the budget is shared with
  const CatalogEntry* t = nullptr;
  for (const CatalogEntry& e : catalog_)
    if (e.id == pre_next_) t = &e;
  pre_set_target_fields();
  if (!t) {
    pre_status_.state = PreloadState::kFailed;
    pre_status_.detail = "missing";
    pre_status_.head_bytes_held = 0;
    pre_publish();
    return;
  }
  pre_entry_ = *t;
  Job job;
  job.token = ++next_token_;
  job.target = pre_entry_;
  if (edits_)
    if (const ::velora::liveset::v1::LiveSet* ov = edits_->overlay_for(pre_entry_.id)) job.overlay = std::make_shared<const ::velora::liveset::v1::LiveSet>(*ov);
  job.cancel = std::make_shared<std::atomic<bool>>(false);
  job.progress = std::make_shared<Progress>();
  job.preload = true;
  job.reuse = loader_.current_heads();
  const std::uint64_t cur_bytes = loader_.current_head_bytes();
  const std::uint64_t budget = opt_.head_budget_bytes;
  job.head_budget = budget == 0 ? 0 : (cur_bytes >= budget ? 1 : budget - cur_bytes);  // 0 would mean "unlimited": with nothing left, 1 byte admits only shared heads
  pre_sig_ = job.overlay ? FileSig{} : sig_of(pre_entry_.path);
  pre_overlay_rev_ = edits_ ? edits_->overlay_revision(pre_entry_.id) : 0;
  pre_check_s_ = pre_pub_s_ = now();
  pre_job_ = job;
  pre_ticks_ = 0;
  pre_running_ = true;
  pre_status_.state = PreloadState::kLoading;
  pre_status_.head_bytes_held = 0;
  pre_status_.head_bytes_current = cur_bytes;
  pre_status_.head_bytes_needed = 0;
  pre_status_.detail.clear();
  pre_publish();
  if (opt_.background_thread) {
    {
      std::lock_guard<std::mutex> lk(m_);
      pending_pre_ = std::move(job);
    }
    cv_.notify_one();
  } else {
    std::unique_ptr<JobResult> r = run_job(job);  // deterministic mode: staged here, declared READY after hold_ticks
    std::lock_guard<std::mutex> lk(m_);
    done_pre_ = std::move(r);
  }
}

void LiveSetSwitcher::harvest_preload() {
  if (!pre_running_) return;
  ++pre_ticks_;
  if (!pre_held_) {
    std::unique_ptr<JobResult> stale;
    std::lock_guard<std::mutex> lk(m_);
    if (done_pre_ && done_pre_->token == pre_job_.token) pre_held_ = std::move(done_pre_);
    else stale = std::move(done_pre_);
  }
  if (!pre_held_) {  // still staging: the bytes held so far, at the status cadence
    const double t = now();
    if (t - pre_pub_s_ >= opt_.publish_interval_s) {
      pre_pub_s_ = t;
      const std::uint64_t done = pre_job_.progress->done.load(std::memory_order_acquire);
      if (done != pre_status_.head_bytes_held) {
        pre_status_.head_bytes_held = done;
        pre_publish();
      }
    }
    return;
  }
  const LiveSetLoadResult& r = pre_held_->result;
  if (r.status != LiveSetLoadStatus::kOk) {  // a refusal is reported at once
    pre_running_ = false;
    pre_status_.head_bytes_held = 0;
    if (r.status == LiveSetLoadStatus::kHeadBudgetExceeded) {
      pre_status_.state = PreloadState::kSkippedBudget;
      pre_status_.detail = "head_budget_exceeded";  // the numbers are in head_bytes_needed / head_bytes_current / the budget
      pre_status_.head_bytes_needed = r.head_bytes;
    } else if (r.status == LiveSetLoadStatus::kCancelled) {
      pre_status_.state = PreloadState::kNone;
      pre_status_.detail.clear();
    } else {
      pre_status_.state = PreloadState::kFailed;
      pre_status_.detail = reason_of(r.status);
    }
    pre_held_.reset();
    pre_job_ = Job{};
    pre_publish();
    return;
  }
  if (pre_ticks_ > opt_.hold_ticks) {
    pre_running_ = false;
    pre_job_.reuse.clear();
    pre_status_.state = PreloadState::kReady;
    pre_status_.head_bytes_held = r.loaded_head_bytes;
    pre_status_.detail.clear();
    pre_publish();
  }
}

// True when the unsaved edits the preload was staged from are still the ones the session holds (none, none; or the same revision, or equal content).
bool LiveSetSwitcher::overlay_unchanged(const ::velora::liveset::v1::LiveSet* now_overlay, std::uint64_t now_revision) const {
  const bool had = pre_job_.overlay != nullptr;
  if (had != (now_overlay != nullptr)) return false;
  if (!had) return true;
  if (now_revision != LiveSetEditPort::kUnknownRevision && now_revision == pre_overlay_rev_) return true;  // cheap path: nothing was edited since
  return pre_job_.overlay->SerializeAsString() == now_overlay->SerializeAsString();                       // safe fallback: the revisions differ or are unknown
}

// What makes a staged or skipped preload stale: the sounding generation's head bytes (every tick, cheap), then at the status cadence the unsaved edits of the
// next Live Set, its file (size + mtime) and the library bundles of its heads.
void LiveSetSwitcher::check_preload_file() {
  if (pre_status_.state == PreloadState::kNone) return;
  if (loader_.current_head_bytes() != pre_status_.head_bytes_current && pre_status_.state != PreloadState::kFailed) {
    invalidate_preload();  // the budget verdict was made against another generation: restage against the one that sounds
    return;
  }
  if (!(pre_running_ || pre_held_)) return;
  const double t = now();
  if (t - pre_check_s_ < opt_.publish_interval_s) return;
  pre_check_s_ = t;
  if (edits_) {
    const std::uint64_t rev = edits_->overlay_revision(pre_entry_.id);
    const bool stale = rev != LiveSetEditPort::kUnknownRevision ? rev != pre_overlay_rev_ : !overlay_unchanged(edits_->overlay_for(pre_entry_.id), rev);
    if (stale) {
      invalidate_preload();
      return;
    }
  }
  if (!pre_job_.overlay && !(sig_of(pre_entry_.path) == pre_sig_)) {
    invalidate_preload();
    return;
  }
  if (pre_held_ && pre_held_->staged && !loader_.heads_still_current(pre_held_->staged->heads)) invalidate_preload();
}

bool LiveSetSwitcher::take_preload(const CatalogEntry& entry, const ::velora::liveset::v1::LiveSet* overlay, SwitchOutcome& o) {
  if (pre_status_.state != PreloadState::kReady || !pre_held_ || pre_running_ || active_ || pre_entry_.id != entry.id) return false;
  // Unsaved edits appeared, went away or changed since it was staged: the caller discards and loads. (Revision compare first; no serialization when equal.)
  if (!overlay_unchanged(overlay, edits_ ? edits_->overlay_revision(entry.id) : 0)) return false;
  if (!pre_job_.overlay && !(sig_of(entry.path) == pre_sig_)) return false;  // the file changed since it was staged
  if (!pre_held_->staged || !loader_.heads_still_current(pre_held_->staged->heads)) return false;  // a library was replaced or removed under the heads
  Job job;
  job.token = next_token_ + 1;
  job.target = entry;
  job.cancel = std::make_shared<std::atomic<bool>>(false);
  job.progress = std::make_shared<Progress>();
  // commit_seamless() only: the reservation is prepared and the generation published; no instrument is loaded here. The preload is consumed only by a
  // commit that went through (or is retried for a busy ledger); a refusal leaves it READY and is reported like any 4.5 failure.
  const LiveSetLoadResult r = loader_.commit_seamless(*pre_held_->staged);
  ++next_token_;
  active_job_ = job;
  ticks_since_jump_ = opt_.hold_ticks;
  busy_since_ = -1;
  start_status(entry);
  status_.bytes_total = status_.bytes_loaded = pre_held_->result.resident_bytes;
  o.result = SwitchOutcome::Result::kAccepted;
  o.token = job.token;
  const bool ok = r.status == LiveSetLoadStatus::kOk;
  if (!ok && r.status != LiveSetLoadStatus::kBusy) {
    finish_failure(r);  // typed refusal in the status; the READY preload stays
    return true;
  }
  active_ = true;
  held_ = std::move(pre_held_);
  pre_job_ = Job{};
  pre_status_.state = PreloadState::kNone;
  pre_status_.head_bytes_held = 0;
  pre_status_.head_bytes_current = 0;
  pre_status_.detail.clear();
  pre_publish();
  ++preload_commits_;
  if (ok) commit_succeeded();
  else (void)commit_refused(r);  // busy: held and retried on the following ticks like 4.5
  return true;
}

void LiveSetSwitcher::fill_status(ws::v1::State* out) const {
  ws::v1::LoadingStatus* l = out->mutable_loading();
  switch (status_.state) {
    case SwitchState::kIdle: l->set_state(ws::v1::LOADING_STATE_IDLE); break;
    case SwitchState::kLoading: l->set_state(ws::v1::LOADING_STATE_LOADING); break;
    case SwitchState::kReady: l->set_state(ws::v1::LOADING_STATE_READY); break;
    case SwitchState::kFailed: l->set_state(ws::v1::LOADING_STATE_FAILED); break;
  }
  l->set_loading(status_.loading());
  l->set_target_id(status_.target_id);
  l->set_target_name(status_.target_name);
  l->set_estimated_remaining_seconds(status_.estimated_remaining_s);
  l->set_progress(status_.progress);
  switch (status_.last_error) {
    case SwitchError::kNone: l->set_last_error(ws::v1::ERROR_CODE_UNSPECIFIED); break;
    case SwitchError::kHeadBudgetExceeded: l->set_last_error(ws::v1::ERROR_CODE_HEAD_BUDGET_EXCEEDED); break;
    case SwitchError::kLoadCancelled: l->set_last_error(ws::v1::ERROR_CODE_LOAD_CANCELLED); break;
    case SwitchError::kNoSuchLiveSet: l->set_last_error(ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET); break;
    case SwitchError::kLoadFailed: l->set_last_error(ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED); break;
  }
  l->set_last_error_informational(status_.informational());
  l->set_last_error_detail(status_.last_error_detail);
  l->set_status_seq(status_.seq);
  l->set_cancelled_loads(status_.cancelled_loads);
  l->set_bytes_loaded(status_.bytes_loaded);
  l->set_bytes_total(status_.bytes_total);
  out->set_live_set_id(loader_.loaded_id());
  out->set_live_set_name(loader_.loaded_name());
  ws::v1::PreloadStatus* pl = out->mutable_preload();
  switch (pre_status_.state) {
    case PreloadState::kNone: pl->set_state(ws::v1::PRELOAD_STATE_NONE); break;
    case PreloadState::kLoading: pl->set_state(ws::v1::PRELOAD_STATE_LOADING); break;
    case PreloadState::kReady: pl->set_state(ws::v1::PRELOAD_STATE_READY); break;
    case PreloadState::kSkippedBudget: pl->set_state(ws::v1::PRELOAD_STATE_SKIPPED_BUDGET); break;
    case PreloadState::kFailed: pl->set_state(ws::v1::PRELOAD_STATE_FAILED); break;
  }
  pl->set_ready(pre_status_.ready());
  pl->set_next_live_set_id(pre_status_.target_id);
  pl->set_next_live_set_name(pre_status_.target_name);
  pl->set_head_bytes_held(pre_status_.head_bytes_held);
  pl->set_head_budget_bytes(opt_.head_budget_bytes);
  pl->set_head_bytes_current(pre_status_.head_bytes_current);
  pl->set_head_bytes_needed(pre_status_.head_bytes_needed);
  pl->set_detail(pre_status_.detail);
  pl->set_status_seq(pre_status_.seq);
}

}  // namespace velora::host

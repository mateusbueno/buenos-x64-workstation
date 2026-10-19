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
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    if (pending_) pending_->cancel->store(true, std::memory_order_release);
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
  for (CatalogEntry& e : catalog_)
    if (e.id == entry.id) {
      e = std::move(entry);
      return;
    }
  catalog_.push_back(std::move(entry));
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

SwitchOutcome LiveSetSwitcher::jump_to(const std::string& id) {
  SwitchOutcome o;
  const CatalogEntry* target = nullptr;
  for (const CatalogEntry& e : catalog_)
    if (e.id == id) target = &e;
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
  if (id == loader_.loaded_id()) {  // already sounding: a no-op; a load that is running is the player changing their mind
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
  o.superseded_previous = active_;
  cancel_active(true);

  Job job;
  job.token = ++next_token_;
  job.target = entry;
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
  return o;
}

SwitchOutcome LiveSetSwitcher::cancel_load() {
  SwitchOutcome o;
  o.result = SwitchOutcome::Result::kAlreadyLoaded;
  o.target_id = status_.target_id;
  o.target_name = status_.target_name;
  if (!active_) return o;
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
  const liveset::Status st = liveset::read_file(job.target.path, ro, lr);
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
  ctl.head_budget_bytes = opt_.head_budget_bytes;
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
      cv_.wait(lk, [this] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    std::unique_ptr<JobResult> r;
    if (!job.cancel->load(std::memory_order_acquire)) r = run_job(job);
    if (!r) continue;
    std::lock_guard<std::mutex> lk(m_);
    if (!job.cancel->load(std::memory_order_acquire)) done_ = std::move(r);
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
    return true;
  }
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
  const bool changed = dirty_ || generation;
  dirty_ = false;
  return changed;
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
}

}  // namespace velora::host

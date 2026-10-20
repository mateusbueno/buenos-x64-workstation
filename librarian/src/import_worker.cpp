#include "import_worker.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>

#include "velora/format/category.hpp"
#include "velora/librarian/library_index.hpp"

namespace velora::librarian::detail {

namespace {

const char* point_name(Stage s) {
  switch (s) {
    case Stage::kStageCreated: return "stage-created";
    case Stage::kStaged: return "samples-written";
    case Stage::kVerified: return "verified";
    case Stage::kFsynced: return "fsynced";
    case Stage::kRenamed: return "renamed";
    case Stage::kLinkCreated: return "link-created";
    case Stage::kFlipped: return "link-flipped";
  }
  return "?";
}

// Story 5.8: the built-in "the source is gone" probe of a job. The source (a folder or a file) must stay stat-able on the device it was on when the job
// started; ENOENT / ENOTDIR / ENXIO / ENODEV / EIO / ESTALE or another device count as removal (an unmount, a pulled USB drive, a detached image).
struct SourceProbe {
  std::string path;
  dev_t dev = 0;
  bool have_dev = false;
  explicit SourceProbe(std::string p) : path(std::move(p)) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
      dev = st.st_dev;
      have_dev = true;
    }
  }
  bool gone() const {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return errno == ENOENT || errno == ENOTDIR || errno == ENXIO || errno == ENODEV || errno == EIO || errno == ESTALE;
    return have_dev && st.st_dev != dev;
  }
};

}  // namespace

void ImportWorker::start() {
  if (thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = false;
  }
  thread_ = std::thread([this] { run(); });
}

bool ImportWorker::enqueue(const std::shared_ptr<ImportJob>& job, std::size_t max_queued, std::size_t& ahead) {
  std::lock_guard<std::mutex> lk(m_);
  if (stop_ || queue_.size() >= max_queued) return false;
  ahead = queue_.size() + (running_ ? 1 : 0);
  queue_.push_back(job);
  cv_.notify_all();
  return true;
}

bool ImportWorker::enqueue_preflight(const std::shared_ptr<PreflightTask>& t, std::size_t max_queued) {
  std::lock_guard<std::mutex> lk(m_);
  if (stop_ || pre_queue_.size() >= max_queued) return false;
  pre_queue_.push_back(t);
  cv_.notify_all();
  return true;
}

// `running_pre_` is set by the caller in the same critical section that popped `t` (so stop_and_join() always sees the task either queued or running).
void ImportWorker::run_preflight(const std::shared_ptr<PreflightTask>& t) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (stop_) t->cancel.store(true);
  }
  if (cfg_.preflight_start_hook) cfg_.preflight_start_hook();
  if (t->cancel.load()) {
    t->result = PreflightResult{};
    t->result.ok = true;
    t->result.cancelled = true;
  } else {
    PreflightOptions o;
    o.source = t->source;
    o.volume_label = t->label;
    o.max_files = t->max_files;
    o.cancel = &t->cancel;
    t->result = preflight_scan(o);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    running_pre_.reset();
  }
  WorkerMsg m;
  m.kind = WorkerMsg::Kind::kPreflightDone;
  m.pre = t;
  post(std::move(m));
}

void ImportWorker::run_pending_preflights() {
  for (;;) {
    std::shared_ptr<PreflightTask> t;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (pre_queue_.empty()) return;
      t = pre_queue_.front();
      pre_queue_.pop_front();
      running_pre_ = t;
    }
    run_preflight(t);
  }
}

bool ImportWorker::stopping() {
  std::lock_guard<std::mutex> lk(m_);
  return stop_;
}

bool ImportWorker::remove_queued(const std::shared_ptr<ImportJob>& job) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = queue_.begin(); it != queue_.end(); ++it)
    if (*it == job) {
      queue_.erase(it);
      return true;
    }
  return false;
}

std::vector<WorkerMsg> ImportWorker::drain() {
  std::vector<WorkerMsg> out;
  std::lock_guard<std::mutex> lk(m_);
  while (!mailbox_.empty()) {
    out.push_back(std::move(mailbox_.front()));
    mailbox_.pop_front();
  }
  return out;
}

void ImportWorker::stop_and_join() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    if (running_) running_->cancel.store(true);
    for (auto& j : queue_) j->cancel.store(true);
    if (running_pre_) running_pre_->cancel.store(true);
    for (auto& t : pre_queue_) t->cancel.store(true);
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
}

void ImportWorker::post(WorkerMsg m) {
  {
    std::lock_guard<std::mutex> lk(m_);
    mailbox_.push_back(std::move(m));
  }
  if (cfg_.wake) cfg_.wake();
}

void ImportWorker::run() {
  for (;;) {
    std::shared_ptr<ImportJob> job;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || !queue_.empty() || !pre_queue_.empty(); });
      if (stop_ && queue_.empty() && pre_queue_.empty()) return;
      if (queue_.empty()) {
        lk.unlock();
        run_pending_preflights();
        continue;
      }
      job = queue_.front();
      queue_.pop_front();
      running_ = job;
    }
    SourceProbe probe(job->source);
    auto source_gone = [&]() { return probe.gone() || (cfg_.source_gone && cfg_.source_gone(job->source)); };
    {
      WorkerMsg m;
      m.kind = WorkerMsg::Kind::kJobStarted;
      m.job = job;
      post(std::move(m));
    }
    for (std::size_t i = 0; i < job->items.size(); ++i) {
      run_pending_preflights();  // read-only scans are answered between the items of a running job
      if (job->cancel.load()) break;
      const JobItem& it = job->items[i];
      {
        WorkerMsg m;
        m.kind = WorkerMsg::Kind::kItemStarted;
        m.job = job;
        m.index = i;
        post(std::move(m));
      }
      if (it.install) {
        InstallOptions io = it.install_opt;
        io.root = cfg_.data_dir;
        io.trusted = &cfg_.trusted;
        io.free_bytes_fn = cfg_.free_bytes_fn;
        const std::uint64_t ijid = job->id;
        io.cancelled = [&job] { return job->cancel.load(); };
        io.source_gone = source_gone;
        io.fault.on_stage = [&, ijid, i](Stage s) {
          if (cfg_.point_hook) cfg_.point_hook(point_name(s), ijid, i);
          // Like an import: a cancel (or the service stopping) takes effect at the next safe point before the bundle is renamed into place.
          if (job->cancel.load() && (s == Stage::kStageCreated || s == Stage::kStaged || s == Stage::kVerified || s == Stage::kFsynced)) return HookAction::kAbort;
          return HookAction::kContinue;
        };
        io.on_copy_chunk = [&, ijid, i](std::uint64_t) {
          if (cfg_.point_hook) cfg_.point_hook("copy-chunk", ijid, i);  // a test kill point mid-copy
        };
        if (cfg_.install_options_hook) cfg_.install_options_hook(ijid, i, io);
        WorkerMsg done;
        done.kind = WorkerMsg::Kind::kItemDone;
        done.job = job;
        done.index = i;
        done.is_install = true;
        done.install = install_protected(io);
        const InstallResult& ir = done.install;
        done.result.library_id = ir.library_id;
        done.result.bundle_id = ir.bundle_id;
        done.result.bundle_dir = ir.bundle_dir;
        done.result.sample_count = ir.samples;
        done.result.region_count = ir.regions;
        done.result.bundle_created = ir.bundle_created;
        done.result.current_flipped = ir.current_flipped;
        done.result.timbres_total = 1;
        if (ir.status == InstallStatus::kAborted) reconcile_staging(cfg_.data_dir);  // what the abort left in .staging
        if (ir.ok()) {
          done.result.status = Status::kOk;
          done.result.timbres_published = 1;
          DescribeOptions dopt;
          dopt.trusted = &cfg_.trusted;
          DescribeResult dr = describe_bundle(ir.bundle_dir, dopt);
          if (dr.status == DescribeStatus::kOk) {
            done.record = std::move(dr.record);
            done.have_record = true;
          } else {
            done.describe_error = dr.why;
          }
        }
        const bool removed = ir.status == InstallStatus::kDriveRemoved;
        done.ticket = std::make_shared<Ticket>();
        std::shared_ptr<Ticket> t = done.ticket;
        post(std::move(done));
        std::unique_lock<std::mutex> lk(t->m);
        t->cv.wait(lk, [&] { return t->done; });
        if (removed) break;
        continue;
      }
      ImportOptions o;
      o.sfz_path = it.path;
      o.root = cfg_.data_dir;
      o.category = it.category_key;
      o.write_catalog_meta = true;
      o.library_name = it.name;
      o.instrument_meta = it.meta;
      o.checkpoint_timbres = cfg_.checkpoint_timbres;
      o.checkpoint_bytes = cfg_.checkpoint_bytes;
      o.checkpoint_size_fn = cfg_.checkpoint_size_fn;
      o.source_gone = source_gone;
      if (job->source != it.path) o.source_root = job->source;
      o.on_timbres = [&job](std::size_t n) {
        if (n > 1) job->timbres_total.fetch_add(static_cast<std::uint32_t>(n - 1));  // the job counted this file as one timbre
      };
      const std::uint64_t jid = job->id;
      o.on_checkpoint = [&, i](const ImportResult& r) {
        // An intermediate bundle is published: the service thread writes its catalog rows before the next timbre is converted ("published, then rows").
        WorkerMsg cp;
        cp.kind = WorkerMsg::Kind::kCheckpoint;
        cp.job = job;
        cp.index = i;
        cp.result = r;
        DescribeResult dr = describe_bundle(r.bundle_dir);
        if (dr.status == DescribeStatus::kOk) {
          cp.record = std::move(dr.record);
          cp.have_record = true;
        } else {
          cp.describe_error = dr.why;
        }
        cp.ticket = std::make_shared<Ticket>();
        std::shared_ptr<Ticket> tk = cp.ticket;
        post(std::move(cp));
        std::unique_lock<std::mutex> lk(tk->m);
        tk->cv.wait(lk, [&] { return tk->done; });
      };
      o.fault.on_timbre = [&, jid](const char* point, std::size_t timbre) {
        if (cfg_.timbre_hook) cfg_.timbre_hook(point, jid, timbre);
        // A cancel takes effect before the next timbre is read; whatever a checkpoint published stays.
        if (job->cancel.load() && std::strcmp(point, "timbre-start") == 0) return HookAction::kAbort;
        return HookAction::kContinue;
      };
      o.fault.on_stage = [&, jid, i](Stage s) {
        if (cfg_.point_hook) cfg_.point_hook(point_name(s), jid, i);
        // A cancel (or the service stopping) takes effect at the next safe point before the bundle is renamed into place.
        if (job->cancel.load() && (s == Stage::kStageCreated || s == Stage::kStaged || s == Stage::kVerified || s == Stage::kFsynced))
          return HookAction::kAbort;
        return HookAction::kContinue;
      };
      if (cfg_.options_hook) cfg_.options_hook(jid, i, o);
      WorkerMsg done;
      done.kind = WorkerMsg::Kind::kItemDone;
      done.job = job;
      done.index = i;
      done.result = import_file(o);
      if (done.result.status == Status::kAborted || done.result.status == Status::kDriveRemoved)
        reconcile_staging(cfg_.data_dir);  // what the abort left in .staging (a removal leaves nothing: the check is cheap and keeps the invariant)
      const bool drive_removed = done.result.status == Status::kDriveRemoved;
      if (done.result.ok() || (drive_removed && done.result.timbres_published > 0)) {
        DescribeResult dr = describe_bundle(done.result.bundle_dir);
        if (dr.status == DescribeStatus::kOk) {
          done.record = std::move(dr.record);
          done.have_record = true;
        } else {
          done.describe_error = dr.why;
        }
      }
      done.ticket = std::make_shared<Ticket>();
      std::shared_ptr<Ticket> t = done.ticket;
      post(std::move(done));
      std::unique_lock<std::mutex> lk(t->m);
      t->cv.wait(lk, [&] { return t->done; });
      if (drive_removed) break;  // the drive is gone: the items after this one cannot be read either
    }
    {
      WorkerMsg m;
      m.kind = WorkerMsg::Kind::kJobDone;
      m.job = job;
      post(std::move(m));
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      running_.reset();
    }
  }
}

}  // namespace velora::librarian::detail

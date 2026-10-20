#pragma once
// Internal to the librarian service (Story 5.6): the import jobs and the worker thread that runs them.
//
// Threads: the service thread owns the catalog and every client connection; the worker thread owns the file system work (convert, stage, verify,
// fsync, rename, flip `current`). They talk through a mailbox: the worker posts messages (job started, item started, item done, job done) and the
// service thread drains them, writes the catalog rows of a finished item and pushes the progress events. The worker waits for the service thread to
// have handled an item before it starts the next one, so within a job "published, then catalog rows" holds item by item. One job runs at a time
// (serial FIFO, bounded queue).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "library_api.pb.h"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_records.hpp"
#include "velora/librarian/preflight.hpp"

namespace velora::librarian::detail {

struct JobItem {
  std::string path;                                  // canonical absolute path of the SFZ / SF2
  std::string category_key;                          // "category.piano": validated before the job exists
  std::string name;                                  // display name override, may be empty
  std::vector<ImportOptions::InstrumentMeta> meta;   // per-instrument overrides
  std::uint64_t bytes = 0;
};

// Story 5.7: a preflight scan. Runs on the worker thread (read-only; between jobs and between the items of a job), answered through the mailbox.
struct PreflightTask {
  std::uint64_t id = 0;          // service-side id (also the key of the stored result)
  std::uint64_t owner = 0;       // client id
  std::uint64_t request_id = 0;  // the PreflightRequest's request_id on that connection
  std::string source, label;
  std::uint32_t max_files = 0;
  std::atomic<bool> cancel{false};
  PreflightResult result;        // written by the worker before it posts the message
};

struct ImportJob {
  std::uint64_t id = 0;
  std::uint64_t owner = 0;  // the client that asked (0 once it is gone is not tracked: the id is looked up when an event is pushed)
  std::string source;
  std::vector<JobItem> items;
  std::uint64_t bytes_total = 0;
  std::atomic<bool> cancel{false};
  std::atomic<std::uint32_t> timbres_total{0};  // Story 5.8: items.size() when the job is created (1 per file), raised by the worker as each SF2 is opened (its preset count)
  std::uint64_t reserved = 0;  // service thread: space reserved for this job (released when it finishes)
  // --- service thread only ---
  ws::v1::ImportState state = ws::v1::IMPORT_STATE_QUEUED;
  std::uint32_t done = 0, imported = 0, unchanged = 0, skipped = 0, failed = 0, cancelled = 0;
  std::uint64_t bytes_done = 0, event_seq = 0;
  std::string current_item;
  std::uint32_t timbres_imported = 0, checkpoints = 0, cur_item_timbres = 0;  // Story 5.8: cur_item_timbres = already counted from the checkpoints of the running item
  bool drive_removed = false;
  std::string checkpoint_error;  // the last failed checkpoint catalog write (cleared by the next successful one)
  ws::v1::ImportProgress last_event;
  bool finished() const {
    return state == ws::v1::IMPORT_STATE_PUBLISHED || state == ws::v1::IMPORT_STATE_FAILED || state == ws::v1::IMPORT_STATE_CANCELLED ||
           state == ws::v1::IMPORT_STATE_DRIVE_REMOVED;
  }
};

// The service thread tells the worker (which waits on it) that an item has been handled.
struct Ticket {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
};

struct WorkerMsg {
  enum class Kind : std::uint8_t { kJobStarted, kItemStarted, kItemDone, kJobDone, kPreflightDone, kCheckpoint } kind = Kind::kJobStarted;
  std::shared_ptr<ImportJob> job;
  std::shared_ptr<PreflightTask> pre;  // kPreflightDone
  std::size_t index = 0;
  ImportResult result;           // kItemDone; kCheckpoint (the result so far)
  LibraryRecord record;          // kItemDone, when result.ok() (or a drive-removed item published timbres) and the bundle was described; kCheckpoint
  bool have_record = false;
  std::string describe_error;    // result.ok() but the published bundle could not be described: the item fails, nothing is cataloged
  std::shared_ptr<Ticket> ticket;
};

class ImportWorker {
 public:
  struct Config {
    std::string data_dir;
    // Called at every boundary of an item ("stage-created" ... "link-flipped"; the service adds the catalog and job points). May _exit().
    std::function<void(const char* point, std::uint64_t job_id, std::size_t item)> point_hook;
    std::function<void()> preflight_start_hook;  // Story 5.7 test hook: called at the start of every scan
    // Story 5.8. Called at the boundaries of a timbre ("timbre-start", "timbre-done", "checkpoint-published"; the third argument is the timbre index, or the
    // number of timbres in the bundle for the last one). May _exit(), raise SIGKILL or block.
    std::function<void(const char* point, std::uint64_t job_id, std::size_t timbre)> timbre_hook;
    // Story 5.8: true when `source` (the job's folder or file) is gone; combined with the built-in probe (the source no longer stat-able, or its device changed).
    std::function<bool(const std::string& source)> source_gone;
    std::function<void(std::uint64_t job_id, std::size_t item, ImportOptions&)> options_hook;  // tests: adjusts the options of each item (fault injection)
    std::uint32_t checkpoint_timbres = 0;  // 0 = the default
    std::uint64_t checkpoint_bytes = 0;
    std::function<std::uint64_t(std::size_t index, std::uint64_t actual)> checkpoint_size_fn;
    std::function<void()> wake;  // tells the service thread that the mailbox has messages
  };
  explicit ImportWorker(Config cfg) : cfg_(std::move(cfg)) {}
  ~ImportWorker() { stop_and_join(); }
  ImportWorker(const ImportWorker&) = delete;
  ImportWorker& operator=(const ImportWorker&) = delete;

  void start();
  // Jobs waiting (not started). `ahead` receives the jobs in front of a new one (waiting + running).
  bool enqueue(const std::shared_ptr<ImportJob>& job, std::size_t max_queued, std::size_t& ahead);
  bool stopping();  // stop_and_join() has begun: enqueue() refuses
  // Story 5.7: queues a preflight scan (at most `max_queued` waiting); false when full or stopping.
  bool enqueue_preflight(const std::shared_ptr<PreflightTask>& t, std::size_t max_queued);
  bool remove_queued(const std::shared_ptr<ImportJob>& job);  // true: it had not started and is gone
  std::vector<WorkerMsg> drain();
  // Asks the running item to stop at its next safe point, cancels queued jobs and joins. The service thread must keep draining until this returns.
  void stop_and_join();
  bool joinable() const { return thread_.joinable(); }

 private:
  void run();
  void post(WorkerMsg m);
  void run_preflight(const std::shared_ptr<PreflightTask>& t);
  void run_pending_preflights();  // the ones queued right now, without waiting

  Config cfg_;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<ImportJob>> queue_;
  std::shared_ptr<ImportJob> running_;
  std::deque<std::shared_ptr<PreflightTask>> pre_queue_;
  std::shared_ptr<PreflightTask> running_pre_;
  std::deque<WorkerMsg> mailbox_;
  bool stop_ = false;
};

}  // namespace velora::librarian::detail

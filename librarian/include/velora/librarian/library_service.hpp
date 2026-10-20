#pragma once
// The librarian service (Story 4.11, AR10 partial, AR25 partial): the host process that owns the catalog and serves the Library API (proto/
// library_api.proto, package ws.v1) over a Unix domain socket. Framing, handshake and closing rules are those of the engine's ControlServer
// (engine/host/src/control_server.*): 4-byte big-endian length + protobuf, the first message is LibraryHello (version + features), a framing
// violation / wrong first message / unsupported version / late handshake closes the connection after the error reply, after the handshake an
// unparsable frame or an invalid command gets an error reply and the connection stays, a client that falls kMaxQueuedBytes behind is dropped.
// One thread (the service thread) accepts, reads, handles commands one at a time (concurrent clients are serialised) and owns the Catalog; only
// the counters are readable from other threads. The socket is created 0600; a stale socket file is replaced, a live listener is never stolen.
//
// State kept in RAM and mirrored from the catalog: the favorites (in the order they were added) and Recent (most recent first, deduplicated,
// at most 20). Favorites are explicit user commands: written to SQLite immediately (one WAL transaction, synchronous=FULL, so committed means
// fsynced), then published to subscribers (FavoritesChanged). Recent: ReportLiveSetActive updates the RAM list at once and publishes
// RecentChanged; the SQLite write is deferred to an idle-safe moment, defined by the SetPlaying flag the engine/ui feeds: while playing is true
// Recent is never written; it is written (one transaction replacing the table) when playing becomes false, on an explicit FlushRecent (the caller
// asserts the moment is safe), or at once when a report arrives while not playing, and at orderly shutdown.
// A Live Set deletion (LiveSetDeleted, Reconcile) removes the favorite row at once; its recent row is removed from RAM at once and from SQLite
// with the same rule as any Recent write (deferred while playing). When idle both tables are cleaned in one transaction.
//
// Story 5.6: the service is also the ONLY writer of `<data>/lib` and of the libraries/instruments catalog tables. Before it serves, start() takes
// an exclusive flock on `<data>/lib/.service.lock` (the `librarian import` CLI refuses to run while it is held) and runs the reconcile pass
// (library_reconcile.hpp). ImportRequest queues a job; ONE worker thread runs jobs serially (FIFO, bounded queue) through the Epic 1 importer
// (import_file: convert, stage, verify bit-exact, fsync, rename, flip `current`) and hands every finished item back to the service thread, which
// writes the catalog rows (one transaction) only after that, then pushes ImportProgress events to the requester and to subscribers. Catalog access
// stays on the service thread; the worker never touches SQLite.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "library_api.pb.h"
#include "velora/librarian/catalog.hpp"
#include "velora/librarian/library_reconcile.hpp"

namespace velora::librarian::detail {
struct ImportJob;
struct WorkerMsg;
class ImportWorker;
}  // namespace velora::librarian::detail

namespace velora::librarian {

struct LibraryServiceOptions {
  std::string data_dir;     // must exist; the library root: the service owns `<data_dir>/lib` (bundles) and writes nothing else there
  std::string catalog_dir;  // holds catalog.db and catalog.lock; empty = data_dir. On the board: data_dir /data, catalog_dir /data/catalog
  std::string socket_path;
  std::size_t max_clients = 64;
  int handshake_timeout_ms = 5000;
  // Socket access: default mode 0600 (dev). With a group name the socket is chown'ed to that group and made 0660 (the systemd unit passes `velora`);
  // an unknown group refuses to start.
  std::string socket_group;
  // playing=true is a lease held by the connection that set it: when that connection closes, playing becomes false and Recent is flushed. With
  // playing_lease_ms > 0 it also expires that long after the last SetPlaying(true) (default 0 = no timeout). `clock` replaces steady_clock (tests).
  int playing_lease_ms = 0;
  std::function<std::chrono::steady_clock::time_point()> clock;
  // Story 5.6.
  std::size_t max_queued_jobs = 16;
  std::size_t reply_budget_bytes = 200000;  // size at which a ListLibraries / ListInstruments reply is cut (`more` is set); tests lower it  // jobs waiting behind the running one; one more is refused with QUEUE_FULL
  // Test hook (crash campaign): called at every boundary of an import with the point name, the job id and the item index:
  // "stage-created", "samples-written", "verified", "fsynced", "renamed", "link-created", "link-flipped", "catalog-row-written" (inside the catalog
  // transaction, before COMMIT), "catalog-committed", "job-finished"; and "reconcile.cleaned|add|update|remove" (job 0) during the startup pass. It
  // may _exit() the process or block (tests); it runs on the worker thread for the file points and on the service thread for the others.
  std::function<void(const char* point, std::uint64_t job_id, std::size_t item)> import_point_hook;
};

class LibraryService {
 public:
  static constexpr std::uint32_t kProtocolVersion = 1;
  static constexpr std::uint32_t kMaxFrame = 262144;
  static constexpr std::size_t kMaxQueuedBytes = 4u << 20;
  static constexpr std::size_t kMaxReconcileIds = 100000;

  explicit LibraryService(LibraryServiceOptions opts);
  ~LibraryService();
  LibraryService(const LibraryService&) = delete;
  LibraryService& operator=(const LibraryService&) = delete;

  // Test hook: installed on the Catalog right after it is opened (see Catalog::set_fault_hook). Set before start().
  void set_catalog_fault_hook(Catalog::FaultHook h) { fault_hook_ = std::move(h); }

  // Opens the catalog (single writer: kBusy when another service holds it), binds the socket and starts the thread. `status` (optional) receives
  // the catalog status; `err` a one-line reason.
  bool start(std::string& err, CatalogStatus* status = nullptr);
  // Orderly shutdown: stops serving, writes unwritten Recent (even when playing: the process is ending), removes the socket. Idempotent.
  void stop();
  const std::string& socket_path() const noexcept { return opts_.socket_path; }

  std::uint64_t accepted() const noexcept { return accepted_.load(std::memory_order_relaxed); }
  std::uint64_t frames_in() const noexcept { return frames_in_.load(std::memory_order_relaxed); }
  std::uint64_t dropped_slow() const noexcept { return dropped_slow_.load(std::memory_order_relaxed); }
  // What the startup reconcile pass did (valid after start()).
  const ReconcileReport& startup_reconcile() const noexcept { return reconcile_report_; }
  // Catalog write counters (valid after start(); the counters outlive stop()).
  std::uint64_t recent_writes() const noexcept { return recent_writes_.load(std::memory_order_relaxed); }
  std::uint64_t favorite_writes() const noexcept { return favorite_writes_.load(std::memory_order_relaxed); }

 private:
  struct Client {
    int fd = -1;
    std::string rx, tx;
    std::uint64_t id = 0;
    std::vector<std::string> reconcile_acc;  // ids of the chunks of an open chunked Reconcile
    bool handshaken = false, subscribed = false, closing = false, dead = false, eof = false;
    std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();
  };

  void run();
  void accept_clients();
  void read_client(Client& c);
  void process_frame(Client& c, const std::string& payload);
  void handle(Client& c, const ws::v1::LibraryClientMessage& msg);
  void send(Client& c, const ws::v1::LibraryServerMessage& msg);
  void flush(Client& c);
  void reply_error(Client& c, std::uint64_t rid, ws::v1::LibraryErrorCode code, const std::string& message);
  void reply_ack(Client& c, std::uint64_t rid, bool changed);
  void fill_state(ws::v1::LibraryState* s) const;
  void push_favorites();
  void push_recent();
  bool flush_recent(bool* wrote);  // false = the write failed (still unwritten; storage_status_ says why)
  void idle_flush();
  // Removes the rows of Live Sets that no longer exist: `existing` null = delete exactly `gone`; otherwise `gone` is derived from `existing` (Reconcile).
  // Returns false on a storage failure (the RAM favorites are then re-read from the catalog).
  CatalogStatus cascade_delete(const std::vector<std::string>& gone, const std::set<std::string>* existing, bool& changed_f, bool& changed_r);
  void reload_favorites();  // reads into a temporary and assigns only on success
  void check_playing_lease();
  std::chrono::steady_clock::time_point now() const { return opts_.clock ? opts_.clock() : std::chrono::steady_clock::now(); }
  void sync_counters();
  // --- Story 5.6 ---
  void handle_import_request(Client& c, std::uint64_t rid, const ws::v1::ImportRequest& req);
  void handle_list_libraries(Client& c, std::uint64_t rid, const ws::v1::ListLibraries& req);
  void handle_list_instruments(Client& c, std::uint64_t rid, const ws::v1::ListInstruments& req);
  void handle_cancel_import(Client& c, std::uint64_t rid, std::uint64_t job_id);
  void process_worker();
  void on_worker_msg(detail::WorkerMsg& m);
  void push_job_event(detail::ImportJob& job, const ws::v1::ImportItemResult* item);
  void finish_job(detail::ImportJob& job, bool all_items_processed);
  void trim_finished_jobs();
  void point(const char* name, std::uint64_t job, std::size_t item) {
    if (opts_.import_point_hook) opts_.import_point_hook(name, job, item);
  }

  LibraryServiceOptions opts_;
  Catalog::FaultHook fault_hook_;
  std::unique_ptr<Catalog> catalog_;
  int listen_fd_ = -1;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool started_ = false;
  std::vector<Client> clients_;
  // state (service thread only)
  std::vector<std::string> favorites_, recent_;
  bool playing_ = false;
  std::uint64_t playing_owner_ = 0, next_client_id_ = 1;
  std::chrono::steady_clock::time_point playing_since_{};
  CatalogStatus storage_status_ = CatalogStatus::kOk;  // the status behind the last failed write
  bool recent_dirty_ = false;   // RAM Recent differs from what SQLite holds
  std::atomic<bool> stopping_{false};  // stop() has begun: new imports are refused (SHUTTING_DOWN)
  int lib_lock_fd_ = -1;
  ReconcileReport reconcile_report_;
  std::unique_ptr<detail::ImportWorker> worker_;
  std::map<std::uint64_t, std::shared_ptr<detail::ImportJob>> jobs_;  // service thread only; finished jobs are trimmed to the newest 64
  std::uint64_t next_job_id_ = 1, cur_job_ = 0;
  std::size_t cur_item_ = 0;
  std::atomic<std::uint64_t> accepted_{0}, frames_in_{0}, dropped_slow_{0}, recent_writes_{0}, favorite_writes_{0};
};

}  // namespace velora::librarian

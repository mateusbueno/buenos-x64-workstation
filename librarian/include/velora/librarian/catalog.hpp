#pragma once
// The librarian catalog (Story 4.11, AR26 partial, AD-9): a SQLite database in WAL mode at `<data>/catalog.db` that only the `librarian` service
// writes.
//
//   meta(key TEXT PRIMARY KEY, value INTEGER)              'schema_version' (= kCatalogSchemaVersion) and 'next_seq', the persisted sequence counter
//   live_set_favorites(live_set_id TEXT PRIMARY KEY, added_seq INTEGER NOT NULL)            (schema 1)
//   live_set_recent(live_set_id TEXT PRIMARY KEY, last_played_seq INTEGER NOT NULL)         (schema 1)
//   libraries(library_id TEXT PRIMARY KEY, name, source_kind 'sfz'|'sf2'|'sysex', source_path, bundle_id, instrument_count, imported_seq)   (schema 2, Story 5.6; sysex Story 12.3)
//   instruments(library_id -> libraries ON DELETE CASCADE, instrument_key, name, category CHECK 1..17, sample_count, region_count, bytes,
//               PRIMARY KEY(library_id, instrument_key))                                                                     (schema 2, Story 5.6)
//   libraries.kind TEXT NOT NULL DEFAULT 'user' CHECK(kind IN ('user','factory'))                                                   (schema 3, Story 6.4)
// Search tables come with Story 5.16. A library row and its instrument rows are written in ONE transaction, after the bundle is published; the rows
// can always be rebuilt from the bundles (library_index.hpp, library_reconcile.hpp).
//
// Ordering uses sequence numbers from the persisted counter, never wall-clock time (the unit has no RTC). Every write is one transaction
// (BEGIN IMMEDIATE ... COMMIT) with synchronous=FULL, so a committed write has been fsynced through the WAL, and a process killed mid-write
// leaves the previous committed state (WAL recovery at the next open).
//
// Single writer: open() takes an exclusive flock on `<data>/catalog.lock` for the lifetime of the object; a second Catalog on the same directory
// (this or another process) gets kBusy. A Catalog is used from one thread at a time (the service thread); the write counters are atomic.
//
// Version policy: a database whose schema_version is newer than kSchemaVersion is refused (kSchemaTooNew) and left untouched; an older one is
// migrated forward after a PRAGMA quick_check (the only integrity check; none on a matching version). Version 2 (Story 5.6) adds the libraries and
// instruments tables and keeps every favorite and recent row; version 3 (Story 6.4) adds libraries.kind; version 4 (Story 12.3) adds 'sysex' to source_kind.
//
// No exception crosses this API: every call returns a typed CatalogStatus.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "velora/librarian/library_records.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace velora::librarian {

inline constexpr std::uint32_t kCatalogSchemaVersion = 4;
inline constexpr std::size_t kMaxRecent = 20;
// 2048 favorites of 64-byte ids (~135 KB) always fit one Library API frame (262144 bytes) together with Recent.
inline constexpr std::size_t kMaxFavorites = 2048;
inline constexpr std::size_t kMaxLiveSetIdBytes = 64;
inline constexpr const char* kCatalogFile = "catalog.db";
inline constexpr const char* kCatalogLockFile = "catalog.lock";

enum class CatalogStatus : std::uint8_t {
  kOk = 0,
  kIo,             // the directory is missing / not writable, or a system call failed
  kBusy,           // another Catalog (same or another process) holds the writer lock
  kNotACatalog,    // a SQLite database (or foreign file) that is not a Velora catalog
  kCorrupt,        // SQLite reports a damaged database
  kSchemaTooNew,   // schema_version above kCatalogSchemaVersion: refused, untouched
  kMigrationFailed,
  kSql,            // an unexpected SQLite error (see last_error())
  kInvalidId,
  kLimitReached,   // too many favorites
  kFaultInjected,  // the test hook failed the write; nothing changed
  kConstraint,     // a library / instrument row violates the schema (category outside 1..17, empty id, unknown source kind...); nothing changed
  kNotFound,       // no such library
  kCount
};
const char* to_string(CatalogStatus s);
constexpr bool ok(CatalogStatus s) { return s == CatalogStatus::kOk; }

// True for 1..64 bytes of [A-Za-z0-9._-] (UUIDs are the intended ids; the catalog stays generic).
bool valid_live_set_id(const std::string& id);

struct RemoveResult {
  bool favorite_removed = false;
  bool recent_removed = false;
};
struct ReconcileResult {
  std::vector<std::string> favorites_removed, recent_removed;
};

class Catalog {
 public:
  using FaultHook = std::function<bool(const char* point)>;  // see set_fault_hook
  // Opens (creating when absent) `<data_dir>/catalog.db`. `data_dir` must exist. `busy_timeout_ms` is SQLite's busy handler (another connection
  // reading the file, e.g. a diagnostic tool).
  // `open_hook` is installed before the schema is examined, so a migration can be failed by name ("migrate.before_commit") in tests.
  static CatalogStatus open(const std::string& data_dir, std::unique_ptr<Catalog>& out, int busy_timeout_ms = 2000, FaultHook open_hook = {});
  ~Catalog();
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  std::uint32_t schema_version() const noexcept { return schema_version_; }
  bool migrated() const noexcept { return migrated_; }  // open() brought an older schema forward
  const std::string& path() const noexcept { return db_path_; }
  const std::string& last_error() const noexcept { return last_error_; }  // SQLite's message for the last kSql/kCorrupt/kIo

  // --- favorites (explicit user commands, written immediately) ---
  CatalogStatus add_favorite(const std::string& id, bool& changed);     // idempotent: an existing favorite keeps its added_seq, changed=false
  CatalogStatus remove_favorite(const std::string& id, bool& changed);  // idempotent
  CatalogStatus favorites(std::vector<std::string>& out);               // ascending added_seq: the order they were added
  // --- recent ---
  // Replaces the stored Recent with `ids` (most recent first; only the first kMaxRecent are kept; ids must be valid and distinct) in ONE transaction.
  CatalogStatus write_recent(const std::vector<std::string>& ids);
  CatalogStatus recent(std::vector<std::string>& out);                  // most recent first, at most kMaxRecent
  // --- deletion ---
  CatalogStatus remove_live_set(const std::string& id, RemoveResult& out);                    // both tables, one transaction
  CatalogStatus reconcile(const std::set<std::string>& existing, ReconcileResult& out);       // removes the rows of every id not in `existing`

  // --- libraries (schema 2, Story 5.6) ---
  // Inserts or replaces the library row and ALL its instrument rows in one transaction. `rec.imported_seq` is set by the catalog: a new library gets the
  // next sequence number, an existing one whose content differs gets a new one (it was imported again), one with identical content is left untouched
  // (changed=false, no write). Every instrument needs a category 1..17 (kConstraint otherwise, the transaction rolled back).
  CatalogStatus publish_library(LibraryRecord& rec, bool& changed);
  CatalogStatus remove_library(const std::string& library_id, bool& removed);                        // cascades to the instrument rows
  CatalogStatus list_libraries(std::vector<LibraryRecord>& out, bool with_instruments = false);      // ascending imported_seq
  CatalogStatus get_library(const std::string& library_id, LibraryRecord& out, bool& found);         // with its instruments
  // A page of the instruments of one library (ordered by instrument_key; get_library returns them in manifest order); `total` = all of them, `found` false when the library does not exist.
  CatalogStatus list_instruments(const std::string& library_id, std::uint32_t offset, std::uint32_t limit, std::vector<InstrumentRecord>& out,
                                 std::uint32_t& total, bool& found);

  // Write counters (committed write transactions; reads and idempotent no-ops do not count).
  std::uint64_t favorite_writes() const noexcept { return favorite_writes_.load(std::memory_order_relaxed); }
  std::uint64_t recent_writes() const noexcept { return recent_writes_.load(std::memory_order_relaxed); }
  std::uint64_t total_writes() const noexcept { return favorite_writes() + recent_writes() + other_writes_.load(std::memory_order_relaxed); }

  // Test hook (crash injection): called with a point name inside every write transaction, after the rows are written and before COMMIT
  // ("favorite.before_commit", "recent.before_commit", "remove.before_commit", "reconcile.before_commit", "library.before_commit",
  // "library.remove_before_commit") and before the first row of Recent ("recent.begin"). A hook may _exit() the process; returning true fails the write (rolled back, kFaultInjected).
  void set_fault_hook(FaultHook h) { fault_ = std::move(h); }

 private:
  Catalog() = default;
  CatalogStatus init(const std::string& dir, int busy_timeout_ms);
  CatalogStatus inspect_existing(const std::string& path);  // read-only look at an existing file: refuses foreign / newer files without touching them
  CatalogStatus init_schema();
  CatalogStatus migrate_forward(std::uint32_t from);
  CatalogStatus map_error(int rc);
  CatalogStatus exec(const char* sql);
  CatalogStatus begin();
  CatalogStatus commit();
  void rollback();
  CatalogStatus next_seq(std::int64_t& out);  // inside a transaction
  CatalogStatus read_instruments(const std::string& id, std::uint32_t offset, std::uint32_t limit, std::vector<InstrumentRecord>& out, bool by_key = false);
  CatalogStatus prepare(sqlite3_stmt*& st, const char* sql);
  bool fault(const char* point) { return fault_ && fault_(point); }
  void close();

  int lock_fd_ = -1;
  sqlite3* db_ = nullptr;
  std::string db_path_, last_error_;
  std::uint32_t schema_version_ = 0;
  bool migrated_ = false;
  // prepared statements
  sqlite3_stmt* s_fav_exists_ = nullptr;
  sqlite3_stmt* s_fav_count_ = nullptr;
  sqlite3_stmt* s_fav_insert_ = nullptr;
  sqlite3_stmt* s_fav_delete_ = nullptr;
  sqlite3_stmt* s_fav_list_ = nullptr;
  sqlite3_stmt* s_recent_clear_ = nullptr;
  sqlite3_stmt* s_recent_insert_ = nullptr;
  sqlite3_stmt* s_recent_list_ = nullptr;
  sqlite3_stmt* s_recent_all_ = nullptr;
  sqlite3_stmt* s_recent_delete_ = nullptr;
  sqlite3_stmt* s_seq_get_ = nullptr;
  sqlite3_stmt* s_seq_set_ = nullptr;
  std::atomic<std::uint64_t> favorite_writes_{0}, recent_writes_{0}, other_writes_{0};
  FaultHook fault_;
};

}  // namespace velora::librarian

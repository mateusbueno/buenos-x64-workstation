#include "velora/librarian/catalog.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace velora::librarian {

const char* to_string(CatalogStatus s) {
  switch (s) {
    case CatalogStatus::kOk: return "ok";
    case CatalogStatus::kIo: return "io_error";
    case CatalogStatus::kBusy: return "busy";
    case CatalogStatus::kNotACatalog: return "not_a_catalog";
    case CatalogStatus::kCorrupt: return "corrupt";
    case CatalogStatus::kSchemaTooNew: return "schema_too_new";
    case CatalogStatus::kMigrationFailed: return "migration_failed";
    case CatalogStatus::kSql: return "sql_error";
    case CatalogStatus::kInvalidId: return "invalid_id";
    case CatalogStatus::kLimitReached: return "limit_reached";
    case CatalogStatus::kFaultInjected: return "fault_injected";
    case CatalogStatus::kConstraint: return "constraint";
    case CatalogStatus::kNotFound: return "not_found";
    case CatalogStatus::kCount: break;
  }
  return "unknown";
}

bool valid_live_set_id(const std::string& id) {
  if (id.empty() || id.size() > kMaxLiveSetIdBytes) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

namespace {

// Resets a statement on scope exit so the next use starts clean.
struct Reset {
  sqlite3_stmt* st;
  explicit Reset(sqlite3_stmt* s) : st(s) {}
  ~Reset() {
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
  }
};

constexpr const char* kSqliteMagic = "SQLite format 3";  // followed by a NUL: 16 bytes

// Schema 2 (Story 5.6). The category range is kMinCategoryId..kMaxCategoryId (1..17); "none" (0) can never be stored. IF NOT EXISTS: a migration that was
// interrupted cannot happen (one transaction), but a hand-edited version number must not make the step fail on tables that are already there.
constexpr const char* kSchemaV2Ddl =
    "CREATE TABLE IF NOT EXISTS libraries("
    " library_id TEXT PRIMARY KEY CHECK(length(library_id) BETWEEN 1 AND 64),"
    " name TEXT NOT NULL,"
    " source_kind TEXT NOT NULL CHECK(source_kind IN ('sfz','sf2')),"
    " source_path TEXT NOT NULL DEFAULT '',"
    " bundle_id TEXT NOT NULL CHECK(length(bundle_id) > 0),"
    " instrument_count INTEGER NOT NULL CHECK(instrument_count >= 0),"
    " imported_seq INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS instruments("
    " library_id TEXT NOT NULL REFERENCES libraries(library_id) ON DELETE CASCADE,"
    " instrument_key TEXT NOT NULL CHECK(length(instrument_key) > 0),"
    " name TEXT NOT NULL,"
    " category INTEGER NOT NULL CHECK(category BETWEEN 1 AND 17),"
    " sample_count INTEGER NOT NULL DEFAULT 0,"
    " region_count INTEGER NOT NULL DEFAULT 0,"
    " bytes INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(library_id, instrument_key));";
static_assert(kMinCategoryId == 1 && kMaxCategoryId == 17, "the CHECK constraint above encodes the category range");

// A one-shot prepared statement.
struct Stmt {
  sqlite3_stmt* st = nullptr;
  Stmt() = default;
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  ~Stmt() {
    if (st) sqlite3_finalize(st);
  }
};

}  // namespace

CatalogStatus Catalog::map_error(int rc) {
  if (db_) last_error_ = sqlite3_errmsg(db_);
  switch (rc & 0xFF) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE: return CatalogStatus::kOk;
    case SQLITE_CONSTRAINT: return CatalogStatus::kConstraint;
    case SQLITE_NOTADB: return CatalogStatus::kNotACatalog;
    case SQLITE_CORRUPT: return CatalogStatus::kCorrupt;
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return CatalogStatus::kBusy;
    case SQLITE_FULL:
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
    case SQLITE_READONLY:
    case SQLITE_PERM: return CatalogStatus::kIo;
    default: return CatalogStatus::kSql;
  }
}

CatalogStatus Catalog::open(const std::string& data_dir, std::unique_ptr<Catalog>& out, int busy_timeout_ms, FaultHook open_hook) {
  out.reset();
  std::unique_ptr<Catalog> c(new Catalog());
  c->fault_ = std::move(open_hook);
  const CatalogStatus st = c->init(data_dir, busy_timeout_ms);
  if (st != CatalogStatus::kOk) return st;
  out = std::move(c);
  return CatalogStatus::kOk;
}

Catalog::~Catalog() { close(); }

void Catalog::close() {
  for (sqlite3_stmt** s : {&s_fav_exists_, &s_fav_count_, &s_fav_insert_, &s_fav_delete_, &s_fav_list_, &s_recent_clear_, &s_recent_insert_,
                           &s_recent_list_, &s_recent_all_, &s_recent_delete_, &s_seq_get_, &s_seq_set_}) {
    if (*s) sqlite3_finalize(*s);
    *s = nullptr;
  }
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  if (lock_fd_ >= 0) ::close(lock_fd_);  // releases the flock
  lock_fd_ = -1;
}

CatalogStatus Catalog::exec(const char* sql) {
  char* msg = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    const CatalogStatus st = map_error(rc);
    if (msg) last_error_ = msg;
    sqlite3_free(msg);
    return st;
  }
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::begin() { return exec("BEGIN IMMEDIATE"); }

CatalogStatus Catalog::commit() {
  const CatalogStatus st = exec("COMMIT");
  if (st != CatalogStatus::kOk) rollback();
  return st;
}

void Catalog::rollback() { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }

CatalogStatus Catalog::prepare(sqlite3_stmt*& st, const char* sql) {
  const int rc = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr);
  if (rc != SQLITE_OK) {
    const CatalogStatus s = map_error(rc);
    return s == CatalogStatus::kSql ? CatalogStatus::kNotACatalog : s;  // a missing table or column: a foreign database
  }
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::init(const std::string& dir, int busy_timeout_ms) {
  struct stat sb{};
  if (::stat(dir.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
    last_error_ = dir + " is not a directory";
    return CatalogStatus::kIo;
  }
  const std::string base = dir.back() == '/' ? dir : dir + "/";
  lock_fd_ = ::open((base + kCatalogLockFile).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd_ < 0) {
    last_error_ = std::string("catalog.lock: ") + std::strerror(errno);
    return CatalogStatus::kIo;
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    const bool busy = errno == EWOULDBLOCK;
    last_error_ = busy ? "another librarian holds the catalog writer lock" : std::string("flock: ") + std::strerror(errno);
    close();
    return busy ? CatalogStatus::kBusy : CatalogStatus::kIo;
  }
  db_path_ = base + kCatalogFile;
  // A foreign file is recognised before SQLite touches it (SQLite would otherwise report it only on the first read; this keeps the file byte-identical).
  if (::stat(db_path_.c_str(), &sb) == 0 && sb.st_size > 0) {
    char magic[16] = {};
    const int fd = ::open(db_path_.c_str(), O_RDONLY | O_CLOEXEC);
    const ssize_t n = fd >= 0 ? ::read(fd, magic, sizeof magic) : -1;
    if (fd >= 0) ::close(fd);
    if (n < 0) {
      last_error_ = std::string("catalog.db: ") + std::strerror(errno);
      close();
      return CatalogStatus::kIo;
    }
    if (n != 16 || std::memcmp(magic, kSqliteMagic, 15) != 0 || magic[15] != '\0') {
      last_error_ = "catalog.db is not a SQLite database";
      close();
      return CatalogStatus::kNotACatalog;
    }
  }
  if (::stat(db_path_.c_str(), &sb) == 0 && sb.st_size > 0) {
    // Decide BEFORE any write: a foreign or newer database is refused without a single byte (or -wal/-shm file) being changed.
    const CatalogStatus is = inspect_existing(db_path_);
    if (is != CatalogStatus::kOk) {
      close();
      return is;
    }
  }
  int rc = sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    const CatalogStatus st = map_error(rc);
    close();
    return st;
  }
  sqlite3_busy_timeout(db_, busy_timeout_ms);
  {  // journal_mode=WAL answers the resulting mode as a row
    sqlite3_stmt* st = nullptr;
    rc = sqlite3_prepare_v2(db_, "PRAGMA journal_mode=WAL", -1, &st, nullptr);
    std::string mode;
    if (rc == SQLITE_OK) {
      rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        mode = t ? reinterpret_cast<const char*>(t) : "";
        rc = SQLITE_OK;
      }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
      const CatalogStatus s = map_error(rc);
      close();
      return s;
    }
    if (mode != "wal") {
      last_error_ = "cannot enable WAL (journal_mode answered '" + mode + "')";
      close();
      return CatalogStatus::kSql;
    }
  }
  CatalogStatus st = exec("PRAGMA synchronous=FULL");
  if (st == CatalogStatus::kOk) st = exec("PRAGMA foreign_keys=ON");  // per connection; the instruments cascade with their library
  if (st == CatalogStatus::kOk) st = init_schema();
  if (st == CatalogStatus::kOk) {
    const struct { sqlite3_stmt** s; const char* sql; } stmts[] = {
        {&s_fav_exists_, "SELECT 1 FROM live_set_favorites WHERE live_set_id=?1"},
        {&s_fav_count_, "SELECT count(*) FROM live_set_favorites"},
        {&s_fav_insert_, "INSERT INTO live_set_favorites(live_set_id, added_seq) VALUES(?1, ?2)"},
        {&s_fav_delete_, "DELETE FROM live_set_favorites WHERE live_set_id=?1"},
        {&s_fav_list_, "SELECT live_set_id FROM live_set_favorites ORDER BY added_seq ASC"},
        {&s_recent_clear_, "DELETE FROM live_set_recent"},
        {&s_recent_insert_, "INSERT INTO live_set_recent(live_set_id, last_played_seq) VALUES(?1, ?2)"},
        {&s_recent_list_, "SELECT live_set_id FROM live_set_recent ORDER BY last_played_seq DESC LIMIT 20"},
        {&s_recent_all_, "SELECT live_set_id FROM live_set_recent"},
        {&s_recent_delete_, "DELETE FROM live_set_recent WHERE live_set_id=?1"},
        {&s_seq_get_, "SELECT value FROM meta WHERE key='next_seq'"},
        {&s_seq_set_, "UPDATE meta SET value=?1 WHERE key='next_seq'"},
    };
    for (const auto& d : stmts) {
      st = prepare(*d.s, d.sql);
      if (st != CatalogStatus::kOk) break;
    }
  }
  if (st != CatalogStatus::kOk) close();
  return st;
}

CatalogStatus Catalog::inspect_existing(const std::string& path) {
  // Never write the file. With no (or an empty) -wal the main file is the whole truth: open it immutable. With a non-empty -wal (a crash left
  // committed pages there) inspect a private copy of both files, so recovery happens on the copy and the original stays byte-identical.
  struct stat wsb{};
  const bool has_wal = ::stat((path + "-wal").c_str(), &wsb) == 0 && wsb.st_size > 0;
  std::string open_path = path, scratch;
  int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
  if (has_wal) {
    std::string tmpl = path.substr(0, path.rfind('/')) + "/.inspect.XXXXXX";
    if (!::mkdtemp(tmpl.data())) {
      last_error_ = std::string("mkdtemp: ") + std::strerror(errno);
      return CatalogStatus::kIo;
    }
    scratch = tmpl;
    std::error_code ec;
    std::filesystem::copy_file(path, scratch + "/catalog.db", ec);
    if (!ec) std::filesystem::copy_file(path + "-wal", scratch + "/catalog.db-wal", ec);
    if (ec) {
      std::filesystem::remove_all(scratch, ec);
      last_error_ = "cannot copy the catalog for inspection";
      return CatalogStatus::kIo;
    }
    open_path = scratch + "/catalog.db";
    flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  } else {
    std::string uri = "file:";
    for (unsigned char ch : path) {
      const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '_' || ch == '-';
      char buf[4];
      if (safe) {
        uri += static_cast<char>(ch);
      } else {
        std::snprintf(buf, sizeof buf, "%%%02X", ch);
        uri += buf;
      }
    }
    open_path = uri + "?immutable=1";
    flags |= SQLITE_OPEN_URI;
  }
  sqlite3* h = nullptr;
  int rc = sqlite3_open_v2(open_path.c_str(), &h, flags, nullptr);
  sqlite3* saved = db_;
  db_ = h;
  CatalogStatus result = CatalogStatus::kOk;
  int objects = 0;
  bool has_meta = false;
  std::int64_t version = -1;
  if (rc != SQLITE_OK) {
    result = map_error(rc);
  } else {
    sqlite3_busy_timeout(h, 2000);
    sqlite3_stmt* q = nullptr;
    rc = sqlite3_prepare_v2(h, "SELECT type, name FROM sqlite_master", -1, &q, nullptr);
    if (rc == SQLITE_OK) {
      while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        ++objects;  // every object type counts: tables, views, indexes, triggers
        const unsigned char* t = sqlite3_column_text(q, 0);
        const unsigned char* n = sqlite3_column_text(q, 1);
        if (t && n && std::strcmp(reinterpret_cast<const char*>(t), "table") == 0 && std::strcmp(reinterpret_cast<const char*>(n), "meta") == 0)
          has_meta = true;
      }
      if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_OK) {
      result = map_error(rc);
    } else if (has_meta) {
      q = nullptr;
      rc = sqlite3_prepare_v2(h, "SELECT value FROM meta WHERE key='schema_version'", -1, &q, nullptr);
      if (rc == SQLITE_OK) {
        rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) version = sqlite3_column_int64(q, 0);
        if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_OK;
      }
      sqlite3_finalize(q);
      if (rc != SQLITE_OK) {
        const CatalogStatus s = map_error(rc);
        result = s == CatalogStatus::kSql ? CatalogStatus::kNotACatalog : s;
      } else if (version < 1) {
        last_error_ = "the meta table holds no valid schema_version";
        result = CatalogStatus::kNotACatalog;
      } else if (version > static_cast<std::int64_t>(kCatalogSchemaVersion)) {
        last_error_ = "catalog schema_version " + std::to_string(version) + " is newer than this librarian (" + std::to_string(kCatalogSchemaVersion) + ")";
        result = CatalogStatus::kSchemaTooNew;
      }
    } else if (objects != 0) {
      last_error_ = "the database holds objects but no catalog meta table";
      result = CatalogStatus::kNotACatalog;
    }
  }
  const std::string keep = last_error_;
  if (h) sqlite3_close(h);
  db_ = saved;
  last_error_ = keep;
  if (!scratch.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
  }
  return result;
}

CatalogStatus Catalog::init_schema() {
  // What is in the file? Count ALL objects (tables, views, indexes, triggers) and look for `meta`.
  int tables = 0;
  bool has_meta = false;
  {
    sqlite3_stmt* q = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' OR type<>'table'", -1, &q, nullptr);
    if (rc != SQLITE_OK) return map_error(rc);
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
      ++tables;
      const unsigned char* n = sqlite3_column_text(q, 0);
      if (n && std::strcmp(reinterpret_cast<const char*>(n), "meta") == 0) has_meta = true;
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return map_error(rc);
  }
  if (!has_meta) {
    if (tables != 0) {
      last_error_ = "the database has tables but no catalog meta table";
      return CatalogStatus::kNotACatalog;
    }
    CatalogStatus st = begin();
    if (st != CatalogStatus::kOk) return st;
    st = exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL)");
    if (st == CatalogStatus::kOk) st = exec("CREATE TABLE live_set_favorites(live_set_id TEXT PRIMARY KEY, added_seq INTEGER NOT NULL)");
    if (st == CatalogStatus::kOk) st = exec("CREATE TABLE live_set_recent(live_set_id TEXT PRIMARY KEY, last_played_seq INTEGER NOT NULL)");
    if (st == CatalogStatus::kOk) st = exec(kSchemaV2Ddl);
    if (st == CatalogStatus::kOk)
      st = exec(("INSERT INTO meta(key, value) VALUES('schema_version', " + std::to_string(kCatalogSchemaVersion) + "), ('next_seq', 1)").c_str());
    if (st == CatalogStatus::kOk) st = exec(("PRAGMA user_version=" + std::to_string(kCatalogSchemaVersion)).c_str());
    if (st != CatalogStatus::kOk) {
      rollback();
      return st;
    }
    st = commit();
    if (st != CatalogStatus::kOk) return st;
    schema_version_ = kCatalogSchemaVersion;
    return CatalogStatus::kOk;
  }
  // An existing catalog: version policy.
  std::int64_t version = -1;
  {
    sqlite3_stmt* q = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key='schema_version'", -1, &q, nullptr);
    if (rc != SQLITE_OK) {
      const CatalogStatus s = map_error(rc);
      return s == CatalogStatus::kSql ? CatalogStatus::kNotACatalog : s;
    }
    rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) version = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return map_error(rc);
  }
  if (version < 1) {
    last_error_ = "the meta table holds no valid schema_version";
    return CatalogStatus::kNotACatalog;
  }
  if (version > static_cast<std::int64_t>(kCatalogSchemaVersion)) {
    last_error_ = "catalog schema_version " + std::to_string(version) + " is newer than this librarian (" + std::to_string(kCatalogSchemaVersion) + ")";
    return CatalogStatus::kSchemaTooNew;
  }
  schema_version_ = static_cast<std::uint32_t>(version);
  {  // the sequence counter must exist; a damaged meta table gets it back above everything already stored
    sqlite3_stmt* q = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT count(*) FROM meta WHERE key='next_seq'", -1, &q, nullptr);
    std::int64_t have = -1;
    if (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) have = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    if (have < 0) return CatalogStatus::kNotACatalog;
    if (have == 0) {
      CatalogStatus st = begin();
      if (st == CatalogStatus::kOk)
        st = exec("INSERT INTO meta(key, value) SELECT 'next_seq', max((SELECT COALESCE(max(added_seq), 0) FROM live_set_favorites), "
                  "(SELECT COALESCE(max(last_played_seq), 0) FROM live_set_recent)) + 1");
      if (st == CatalogStatus::kOk) st = commit();
      else rollback();
      if (st != CatalogStatus::kOk) return st == CatalogStatus::kSql ? CatalogStatus::kNotACatalog : st;
    }
  }
  if (schema_version_ < kCatalogSchemaVersion) {  // the only integrity check: when the version does not match
    sqlite3_stmt* q = nullptr;
    int rc = sqlite3_prepare_v2(db_, "PRAGMA quick_check", -1, &q, nullptr);
    std::string verdict;
    if (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
      const unsigned char* t = sqlite3_column_text(q, 0);
      verdict = t ? reinterpret_cast<const char*>(t) : "";
    }
    sqlite3_finalize(q);
    if (verdict != "ok") {
      last_error_ = "quick_check failed: " + verdict;
      return CatalogStatus::kCorrupt;
    }
    return migrate_forward(schema_version_);
  }
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::migrate_forward(std::uint32_t from) {
  // Forward only; each step is one transaction that also writes the new schema_version and user_version, so a crash leaves the old or the new
  // version, never a mix. Step 1 -> 2 (Story 5.6) adds the libraries and instruments tables; favorites and recent rows are not touched.
  for (std::uint32_t v = from; v < kCatalogSchemaVersion; ++v) {
    CatalogStatus st = begin();
    if (st != CatalogStatus::kOk) return st == CatalogStatus::kSql ? CatalogStatus::kMigrationFailed : st;
    if (v == 1) {
      st = exec(kSchemaV2Ddl);
    } else {
      last_error_ = "no migration from schema_version " + std::to_string(v);
      rollback();
      return CatalogStatus::kMigrationFailed;
    }
    if (st == CatalogStatus::kOk) st = exec(("UPDATE meta SET value=" + std::to_string(v + 1) + " WHERE key='schema_version'").c_str());
    if (st == CatalogStatus::kOk) st = exec(("PRAGMA user_version=" + std::to_string(v + 1)).c_str());
    if (st == CatalogStatus::kOk && fault("migrate.before_commit")) st = CatalogStatus::kFaultInjected;
    if (st != CatalogStatus::kOk) {
      rollback();
      return st == CatalogStatus::kFaultInjected ? st : CatalogStatus::kMigrationFailed;
    }
    st = commit();
    if (st != CatalogStatus::kOk) return st;
  }
  migrated_ = true;
  schema_version_ = kCatalogSchemaVersion;
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::next_seq(std::int64_t& out) {
  int rc = sqlite3_step(s_seq_get_);
  if (rc != SQLITE_ROW) {
    const CatalogStatus s = map_error(rc);
    sqlite3_reset(s_seq_get_);
    return s == CatalogStatus::kOk ? CatalogStatus::kNotACatalog : s;
  }
  out = sqlite3_column_int64(s_seq_get_, 0);
  sqlite3_reset(s_seq_get_);
  sqlite3_bind_int64(s_seq_set_, 1, out + 1);
  rc = sqlite3_step(s_seq_set_);
  sqlite3_reset(s_seq_set_);
  sqlite3_clear_bindings(s_seq_set_);
  return rc == SQLITE_DONE ? CatalogStatus::kOk : map_error(rc);
}

CatalogStatus Catalog::add_favorite(const std::string& id, bool& changed) {
  changed = false;
  if (!valid_live_set_id(id)) return CatalogStatus::kInvalidId;
  {
    Reset r(s_fav_exists_);
    sqlite3_bind_text(s_fav_exists_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_fav_exists_);
    if (rc == SQLITE_ROW) return CatalogStatus::kOk;
    if (rc != SQLITE_DONE) return map_error(rc);
  }
  {
    Reset r(s_fav_count_);
    const int rc = sqlite3_step(s_fav_count_);
    if (rc != SQLITE_ROW) return map_error(rc);
    if (static_cast<std::size_t>(sqlite3_column_int64(s_fav_count_, 0)) >= kMaxFavorites) return CatalogStatus::kLimitReached;
  }
  CatalogStatus st = begin();
  if (st != CatalogStatus::kOk) return st;
  std::int64_t seq = 0;
  st = next_seq(seq);
  if (st == CatalogStatus::kOk) {
    Reset r(s_fav_insert_);
    sqlite3_bind_text(s_fav_insert_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s_fav_insert_, 2, seq);
    const int rc = sqlite3_step(s_fav_insert_);
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("favorite.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  favorite_writes_.fetch_add(1, std::memory_order_relaxed);
  changed = true;
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::remove_favorite(const std::string& id, bool& changed) {
  changed = false;
  if (!valid_live_set_id(id)) return CatalogStatus::kInvalidId;
  {
    Reset r(s_fav_exists_);
    sqlite3_bind_text(s_fav_exists_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_fav_exists_);
    if (rc == SQLITE_DONE) return CatalogStatus::kOk;
    if (rc != SQLITE_ROW) return map_error(rc);
  }
  CatalogStatus st = begin();
  if (st != CatalogStatus::kOk) return st;
  {
    Reset r(s_fav_delete_);
    sqlite3_bind_text(s_fav_delete_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_fav_delete_);
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("favorite.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  favorite_writes_.fetch_add(1, std::memory_order_relaxed);
  changed = true;
  return CatalogStatus::kOk;
}

static CatalogStatus read_ids(sqlite3_stmt* s, std::vector<std::string>& out, CatalogStatus (*map)(int, void*), void* self) {
  out.clear();
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(s, 0);
    out.emplace_back(t ? reinterpret_cast<const char*>(t) : "");
  }
  sqlite3_reset(s);
  return rc == SQLITE_DONE ? CatalogStatus::kOk : map(rc, self);
}

CatalogStatus Catalog::favorites(std::vector<std::string>& out) {
  return read_ids(s_fav_list_, out, [](int rc, void* self) { return static_cast<Catalog*>(self)->map_error(rc); }, this);
}

CatalogStatus Catalog::recent(std::vector<std::string>& out) {
  return read_ids(s_recent_list_, out, [](int rc, void* self) { return static_cast<Catalog*>(self)->map_error(rc); }, this);
}

CatalogStatus Catalog::write_recent(const std::vector<std::string>& ids_in) {
  std::vector<std::string> ids(ids_in.begin(), ids_in.begin() + static_cast<std::ptrdiff_t>(std::min(ids_in.size(), kMaxRecent)));
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (!valid_live_set_id(ids[i])) return CatalogStatus::kInvalidId;
    for (std::size_t j = 0; j < i; ++j)
      if (ids[j] == ids[i]) return CatalogStatus::kInvalidId;
  }
  CatalogStatus st = begin();
  if (st != CatalogStatus::kOk) return st;
  if (fault("recent.begin")) st = CatalogStatus::kFaultInjected;
  if (st == CatalogStatus::kOk) {
    Reset r(s_recent_clear_);
    const int rc = sqlite3_step(s_recent_clear_);
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  // Oldest first, so the most recent gets the highest sequence number.
  for (std::size_t k = ids.size(); st == CatalogStatus::kOk && k > 0; --k) {
    std::int64_t seq = 0;
    st = next_seq(seq);
    if (st != CatalogStatus::kOk) break;
    Reset r(s_recent_insert_);
    sqlite3_bind_text(s_recent_insert_, 1, ids[k - 1].c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s_recent_insert_, 2, seq);
    const int rc = sqlite3_step(s_recent_insert_);
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("recent.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  recent_writes_.fetch_add(1, std::memory_order_relaxed);
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::remove_live_set(const std::string& id, RemoveResult& out) {
  out = {};
  if (!valid_live_set_id(id)) return CatalogStatus::kInvalidId;
  CatalogStatus st = begin();
  if (st != CatalogStatus::kOk) return st;
  RemoveResult res;
  {
    Reset r(s_fav_delete_);
    sqlite3_bind_text(s_fav_delete_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_fav_delete_);
    if (rc != SQLITE_DONE) st = map_error(rc);
    else res.favorite_removed = sqlite3_changes(db_) > 0;
  }
  if (st == CatalogStatus::kOk) {
    Reset r(s_recent_delete_);
    sqlite3_bind_text(s_recent_delete_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_recent_delete_);
    if (rc != SQLITE_DONE) st = map_error(rc);
    else res.recent_removed = sqlite3_changes(db_) > 0;
  }
  if (st == CatalogStatus::kOk && fault("remove.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  if (res.favorite_removed || res.recent_removed) other_writes_.fetch_add(1, std::memory_order_relaxed);
  out = res;
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::reconcile(const std::set<std::string>& existing, ReconcileResult& out) {
  out = {};
  std::vector<std::string> favs, recs;
  CatalogStatus st = favorites(favs);
  if (st != CatalogStatus::kOk) return st;
  // EVERY recent row (no LIMIT 20): the listing shows 20, but reconcile must clean whatever the table holds.
  st = read_ids(s_recent_all_, recs, [](int rc, void* self) { return static_cast<Catalog*>(self)->map_error(rc); }, this);
  if (st != CatalogStatus::kOk) return st;
  ReconcileResult res;
  for (const std::string& f : favs)
    if (!existing.count(f)) res.favorites_removed.push_back(f);
  for (const std::string& r : recs)
    if (!existing.count(r)) res.recent_removed.push_back(r);
  if (res.favorites_removed.empty() && res.recent_removed.empty()) return CatalogStatus::kOk;
  st = begin();
  if (st != CatalogStatus::kOk) return st;
  for (const std::string& f : res.favorites_removed) {
    Reset r(s_fav_delete_);
    sqlite3_bind_text(s_fav_delete_, 1, f.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_fav_delete_);
    if (rc != SQLITE_DONE) {
      st = map_error(rc);
      break;
    }
  }
  for (std::size_t i = 0; st == CatalogStatus::kOk && i < res.recent_removed.size(); ++i) {
    Reset r(s_recent_delete_);
    sqlite3_bind_text(s_recent_delete_, 1, res.recent_removed[i].c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s_recent_delete_);
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("reconcile.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  other_writes_.fetch_add(1, std::memory_order_relaxed);
  out = std::move(res);
  return CatalogStatus::kOk;
}

// --- libraries (Story 5.6) ------------------------------------------------------------------------------------------------------------------

namespace {

void bind_text(sqlite3_stmt* st, int i, const std::string& v) { sqlite3_bind_text(st, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }

std::string col_text(sqlite3_stmt* st, int i) {
  const unsigned char* t = sqlite3_column_text(st, i);
  return t ? std::string(reinterpret_cast<const char*>(t), static_cast<std::size_t>(sqlite3_column_bytes(st, i))) : std::string();
}

}  // namespace

CatalogStatus Catalog::read_instruments(const std::string& id, std::uint32_t offset, std::uint32_t limit, std::vector<InstrumentRecord>& out, bool by_key) {
  out.clear();
  Stmt q;
  int rc = sqlite3_prepare_v2(db_,
                              "SELECT instrument_key, name, category, sample_count, region_count, bytes FROM instruments WHERE library_id=?1 "
                              "ORDER BY CASE WHEN ?4 THEN instrument_key END, rowid LIMIT ?2 OFFSET ?3",
                              -1, &q.st, nullptr);
  if (rc != SQLITE_OK) return map_error(rc);
  bind_text(q.st, 1, id);
  sqlite3_bind_int64(q.st, 2, limit == 0 ? -1 : static_cast<sqlite3_int64>(limit));
  sqlite3_bind_int64(q.st, 3, offset);
  sqlite3_bind_int(q.st, 4, by_key ? 1 : 0);
  while ((rc = sqlite3_step(q.st)) == SQLITE_ROW) {
    InstrumentRecord r;
    r.key = col_text(q.st, 0);
    r.name = col_text(q.st, 1);
    r.category = static_cast<std::uint32_t>(sqlite3_column_int64(q.st, 2));
    r.sample_count = static_cast<std::uint32_t>(sqlite3_column_int64(q.st, 3));
    r.region_count = static_cast<std::uint32_t>(sqlite3_column_int64(q.st, 4));
    r.bytes = static_cast<std::uint64_t>(sqlite3_column_int64(q.st, 5));
    out.push_back(std::move(r));
  }
  return rc == SQLITE_DONE ? CatalogStatus::kOk : map_error(rc);
}

CatalogStatus Catalog::get_library(const std::string& id, LibraryRecord& out, bool& found) {
  found = false;
  out = LibraryRecord{};
  Stmt q;
  int rc = sqlite3_prepare_v2(db_, "SELECT library_id, name, source_kind, source_path, bundle_id, imported_seq FROM libraries WHERE library_id=?1", -1,
                              &q.st, nullptr);
  if (rc != SQLITE_OK) return map_error(rc);
  bind_text(q.st, 1, id);
  rc = sqlite3_step(q.st);
  if (rc == SQLITE_DONE) return CatalogStatus::kOk;
  if (rc != SQLITE_ROW) return map_error(rc);
  LibraryRecord r;
  r.library_id = col_text(q.st, 0);
  r.name = col_text(q.st, 1);
  r.source_kind = col_text(q.st, 2);
  r.source_path = col_text(q.st, 3);
  r.bundle_id = col_text(q.st, 4);
  r.imported_seq = sqlite3_column_int64(q.st, 5);
  const CatalogStatus st = read_instruments(id, 0, 0, r.instruments);
  if (st != CatalogStatus::kOk) return st;
  out = std::move(r);
  found = true;
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::list_libraries(std::vector<LibraryRecord>& out, bool with_instruments) {
  out.clear();
  std::vector<LibraryRecord> rows;
  std::vector<std::uint32_t> counts;
  {
    Stmt q;
    int rc = sqlite3_prepare_v2(db_,
                                "SELECT library_id, name, source_kind, source_path, bundle_id, instrument_count, imported_seq FROM libraries "
                                "ORDER BY imported_seq, library_id",
                                -1, &q.st, nullptr);
    if (rc != SQLITE_OK) return map_error(rc);
    while ((rc = sqlite3_step(q.st)) == SQLITE_ROW) {
      LibraryRecord r;
      r.library_id = col_text(q.st, 0);
      r.name = col_text(q.st, 1);
      r.source_kind = col_text(q.st, 2);
      r.source_path = col_text(q.st, 3);
      r.bundle_id = col_text(q.st, 4);
      counts.push_back(static_cast<std::uint32_t>(sqlite3_column_int64(q.st, 5)));
      r.imported_seq = sqlite3_column_int64(q.st, 6);
      rows.push_back(std::move(r));
    }
    if (rc != SQLITE_DONE) return map_error(rc);
  }
  if (with_instruments) {
    for (auto& r : rows) {
      const CatalogStatus st = read_instruments(r.library_id, 0, 0, r.instruments);
      if (st != CatalogStatus::kOk) return st;
    }
  } else {
    // Listing without the instrument rows still reports how many there are: placeholders keep instrument_count() truthful.
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i].instruments.assign(counts[i], InstrumentRecord{});
  }
  out = std::move(rows);
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::list_instruments(const std::string& id, std::uint32_t offset, std::uint32_t limit, std::vector<InstrumentRecord>& out,
                                        std::uint32_t& total, bool& found) {
  out.clear();
  total = 0;
  found = false;
  {
    Stmt q;
    int rc = sqlite3_prepare_v2(db_, "SELECT instrument_count FROM libraries WHERE library_id=?1", -1, &q.st, nullptr);
    if (rc != SQLITE_OK) return map_error(rc);
    bind_text(q.st, 1, id);
    rc = sqlite3_step(q.st);
    if (rc == SQLITE_DONE) return CatalogStatus::kOk;
    if (rc != SQLITE_ROW) return map_error(rc);
    total = static_cast<std::uint32_t>(sqlite3_column_int64(q.st, 0));
    found = true;
  }
  return read_instruments(id, offset, limit, out, /*by_key=*/true);  // the API order: by instrument_key
}

CatalogStatus Catalog::publish_library(LibraryRecord& rec, bool& changed) {
  changed = false;
  if (rec.library_id.empty() || rec.library_id.size() > 64 || rec.bundle_id.empty()) return CatalogStatus::kConstraint;
  LibraryRecord have;
  bool found = false;
  CatalogStatus st = get_library(rec.library_id, have, found);
  if (st != CatalogStatus::kOk) return st;
  if (found && same_content(have, rec)) {
    rec.imported_seq = have.imported_seq;
    return CatalogStatus::kOk;
  }
  st = begin();
  if (st != CatalogStatus::kOk) return st;
  std::int64_t seq = 0;
  st = next_seq(seq);
  if (st == CatalogStatus::kOk && found) {
    Stmt del;
    int rc = sqlite3_prepare_v2(db_, "DELETE FROM libraries WHERE library_id=?1", -1, &del.st, nullptr);
    if (rc == SQLITE_OK) {
      bind_text(del.st, 1, rec.library_id);
      rc = sqlite3_step(del.st);
    }
    if (rc != SQLITE_DONE) st = map_error(rc);  // the foreign key cascade removes its instrument rows
  }
  if (st == CatalogStatus::kOk) {
    Stmt ins;
    int rc = sqlite3_prepare_v2(db_,
                                "INSERT INTO libraries(library_id, name, source_kind, source_path, bundle_id, instrument_count, imported_seq) "
                                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                                -1, &ins.st, nullptr);
    if (rc == SQLITE_OK) {
      bind_text(ins.st, 1, rec.library_id);
      bind_text(ins.st, 2, rec.name);
      bind_text(ins.st, 3, rec.source_kind);
      bind_text(ins.st, 4, rec.source_path);
      bind_text(ins.st, 5, rec.bundle_id);
      sqlite3_bind_int64(ins.st, 6, rec.instrument_count());
      sqlite3_bind_int64(ins.st, 7, seq);
      rc = sqlite3_step(ins.st);
    }
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk) {
    Stmt ins;
    int rc = sqlite3_prepare_v2(db_,
                                "INSERT INTO instruments(library_id, instrument_key, name, category, sample_count, region_count, bytes) "
                                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                                -1, &ins.st, nullptr);
    for (std::size_t i = 0; rc == SQLITE_OK && i < rec.instruments.size(); ++i) {
      const InstrumentRecord& r = rec.instruments[i];
      sqlite3_reset(ins.st);
      bind_text(ins.st, 1, rec.library_id);
      bind_text(ins.st, 2, r.key);
      bind_text(ins.st, 3, r.name);
      sqlite3_bind_int64(ins.st, 4, r.category);
      sqlite3_bind_int64(ins.st, 5, r.sample_count);
      sqlite3_bind_int64(ins.st, 6, r.region_count);
      sqlite3_bind_int64(ins.st, 7, static_cast<sqlite3_int64>(r.bytes));
      rc = sqlite3_step(ins.st);
      if (rc == SQLITE_DONE) rc = SQLITE_OK;
      else break;
    }
    if (rc != SQLITE_OK) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("library.before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  other_writes_.fetch_add(1, std::memory_order_relaxed);
  rec.imported_seq = seq;
  changed = true;
  return CatalogStatus::kOk;
}

CatalogStatus Catalog::remove_library(const std::string& id, bool& removed) {
  removed = false;
  CatalogStatus st = begin();
  if (st != CatalogStatus::kOk) return st;
  int changes = 0;
  {
    Stmt del;
    int rc = sqlite3_prepare_v2(db_, "DELETE FROM libraries WHERE library_id=?1", -1, &del.st, nullptr);
    if (rc == SQLITE_OK) {
      bind_text(del.st, 1, id);
      rc = sqlite3_step(del.st);
      changes = sqlite3_changes(db_);
    }
    if (rc != SQLITE_DONE) st = map_error(rc);
  }
  if (st == CatalogStatus::kOk && fault("library.remove_before_commit")) st = CatalogStatus::kFaultInjected;
  if (st != CatalogStatus::kOk) {
    rollback();
    return st;
  }
  st = commit();
  if (st != CatalogStatus::kOk) return st;
  if (changes > 0) {
    removed = true;
    other_writes_.fetch_add(1, std::memory_order_relaxed);
  }
  return CatalogStatus::kOk;
}

}  // namespace velora::librarian

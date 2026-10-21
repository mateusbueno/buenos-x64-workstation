#include "database.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>

#include "velora/format/hash.hpp"

namespace velora::registry {
namespace fmt = velora::format;
namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS devices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  device_public_key BLOB NOT NULL CHECK (length(device_public_key) = 32),
  som_id TEXT NOT NULL,
  serial_number TEXT NOT NULL,
  tier TEXT NOT NULL,
  provisioning_date TEXT NOT NULL,
  station_id TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('active', 'revoked')),
  replaces INTEGER REFERENCES devices(id),
  registered_at INTEGER NOT NULL,
  revoked_at INTEGER,
  revoked_by TEXT,
  revoke_reason TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS devices_active_device_id ON devices(device_id) WHERE status = 'active';
CREATE UNIQUE INDEX IF NOT EXISTS devices_active_serial ON devices(lower(serial_number)) WHERE status = 'active';
CREATE UNIQUE INDEX IF NOT EXISTS devices_public_key ON devices(device_public_key);
CREATE UNIQUE INDEX IF NOT EXISTS devices_replaces ON devices(replaces) WHERE replaces IS NOT NULL;
CREATE TABLE IF NOT EXISTS audit (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  actor TEXT NOT NULL,
  action TEXT NOT NULL,
  device_id TEXT NOT NULL,
  library_id TEXT NOT NULL,
  outcome TEXT NOT NULL,
  code TEXT NOT NULL,
  detail TEXT NOT NULL,
  prev_hash BLOB NOT NULL CHECK (length(prev_hash) = 32),
  hash BLOB NOT NULL CHECK (length(hash) = 32)
);
CREATE TABLE IF NOT EXISTS audit_head (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  seq INTEGER NOT NULL,
  hash BLOB NOT NULL CHECK (length(hash) = 32)
);
CREATE TRIGGER IF NOT EXISTS audit_head_no_delete BEFORE DELETE ON audit_head BEGIN SELECT RAISE(ABORT, 'audit head is append-only'); END;
CREATE TRIGGER IF NOT EXISTS audit_insert_order BEFORE INSERT ON audit
  WHEN NEW.seq != COALESCE((SELECT MAX(seq) FROM audit), 0) + 1 BEGIN SELECT RAISE(ABORT, 'audit log is append-only: out-of-order insert'); END;
CREATE TRIGGER IF NOT EXISTS audit_no_update BEFORE UPDATE ON audit BEGIN SELECT RAISE(ABORT, 'audit log is append-only'); END;
CREATE TRIGGER IF NOT EXISTS audit_no_delete BEFORE DELETE ON audit BEGIN SELECT RAISE(ABORT, 'audit log is append-only'); END;
)SQL";

struct Stmt {
  sqlite3_stmt* s = nullptr;
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
  }
  ~Stmt() { sqlite3_finalize(s); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  explicit operator bool() const { return s != nullptr; }
  void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
  void blob(int i, const void* p, std::size_t n) { sqlite3_bind_blob(s, i, p, static_cast<int>(n), SQLITE_TRANSIENT); }
  void i64(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); }
  void null(int i) { sqlite3_bind_null(s, i); }
  std::string col_text(int i) const {
    const unsigned char* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s, i))) : std::string();
  }
};

// The CREATE text SQLite stores for the security-relevant objects, taken from a scratch database built with kSchema (so it is whatever this SQLite version records).
const std::map<std::string, std::string>& expected_sql() {
  static const std::map<std::string, std::string> m = [] {
    std::map<std::string, std::string> out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK && sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) == SQLITE_OK) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_master WHERE name IN ('audit','audit_head','audit_no_update','audit_no_delete','audit_insert_order','audit_head_no_delete')", -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) out[reinterpret_cast<const char*>(sqlite3_column_text(st, 0))] = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
      sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
  }();
  return m;
}

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

void put_u64(std::string& m, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put_field(std::string& m, const std::string& f) {
  for (int i = 3; i >= 0; --i) m.push_back(static_cast<char>((f.size() >> (8 * i)) & 0xff));
  m += f;
}

// hash = SHA-256("velora/registry-audit/v1" 0x00 || prev_hash || seq(u64) || ts(u64) || len-prefixed actor, action, device_id, library_id, outcome, code, detail).
fmt::Digest256 chain_hash(const fmt::Digest256& prev, std::int64_t seq, std::int64_t ts, const AuditEvent& e) {
  std::string m = "velora/registry-audit/v1";
  m.push_back('\0');
  m.append(reinterpret_cast<const char*>(prev.data()), prev.size());
  put_u64(m, static_cast<std::uint64_t>(seq));
  put_u64(m, static_cast<std::uint64_t>(ts));
  for (const std::string* f : {&e.actor, &e.action, &e.device_id, &e.library_id, &e.outcome, &e.code, &e.detail}) put_field(m, *f);
  return fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()));
}

Device read_device(Stmt& st) {
  Device d;
  d.row_id = sqlite3_column_int64(st.s, 0);
  d.device_id = st.col_text(1);
  if (sqlite3_column_bytes(st.s, 2) == 32) std::memcpy(d.public_key.data(), sqlite3_column_blob(st.s, 2), 32);
  d.som_id = st.col_text(3);
  d.serial_number = st.col_text(4);
  d.tier = st.col_text(5);
  d.provisioning_date = st.col_text(6);
  d.station_id = st.col_text(7);
  d.status = st.col_text(8);
  d.replaces_row = sqlite3_column_type(st.s, 9) == SQLITE_NULL ? 0 : sqlite3_column_int64(st.s, 9);
  d.registered_at = sqlite3_column_int64(st.s, 10);
  d.revoked_at = sqlite3_column_type(st.s, 11) == SQLITE_NULL ? 0 : sqlite3_column_int64(st.s, 11);
  d.revoked_by = st.col_text(12);
  d.revoke_reason = st.col_text(13);
  d.replaces_device_id = st.col_text(14);
  return d;
}

constexpr const char* kDeviceCols =
    "d.id, d.device_id, d.device_public_key, d.som_id, d.serial_number, d.tier, d.provisioning_date, d.station_id, d.status, d.replaces, d.registered_at, "
    "d.revoked_at, d.revoked_by, d.revoke_reason, COALESCE(r.device_id, '') FROM devices d LEFT JOIN devices r ON r.id = d.replaces";

std::optional<Device> query_device(sqlite3* db, const std::string& device_id, bool active_only) {
  const std::string sql = std::string("SELECT ") + kDeviceCols + " WHERE d.device_id = ?1" + (active_only ? " AND d.status = 'active'" : "") +
                          " ORDER BY (d.status = 'active') DESC, d.id DESC LIMIT 1";
  Stmt st(db, sql.c_str());
  if (!st) return std::nullopt;
  st.text(1, device_id);
  if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
  return read_device(st);
}

}  // namespace

const char* to_string(DbStatus s) {
  switch (s) {
    case DbStatus::kOk: return "ok";
    case DbStatus::kDuplicateDeviceId: return "duplicate_device_id";
    case DbStatus::kDuplicateSerial: return "duplicate_serial";
    case DbStatus::kDuplicatePublicKey: return "duplicate_public_key";
    case DbStatus::kInvalidReplaces: return "invalid_replaces";
    case DbStatus::kNotFound: return "not_found";
    case DbStatus::kNotActive: return "not_active";
    case DbStatus::kAuditFailed: return "audit_failed";
    case DbStatus::kAlreadyRevoked: return "already_revoked";
    case DbStatus::kError: return "error";
  }
  return "error";
}

DbStatus map_unique_violation(const char* msg) {
  const std::string m = msg ? msg : "";
  if (m.find("UNIQUE constraint failed") == std::string::npos) return DbStatus::kError;
  if (m.find("devices_active_serial") != std::string::npos || m.find("serial_number") != std::string::npos) return DbStatus::kDuplicateSerial;
  if (m.find("device_public_key") != std::string::npos || m.find("devices_public_key") != std::string::npos) return DbStatus::kDuplicatePublicKey;
  if (m.find("devices.replaces") != std::string::npos || m.find("devices_replaces") != std::string::npos) return DbStatus::kInvalidReplaces;
  if (m.find("device_id") != std::string::npos) return DbStatus::kDuplicateDeviceId;
  return DbStatus::kError;
}

std::unique_ptr<Database> Database::open(const std::string& path, std::string* err, bool exclusive_lock) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<Database> {
    if (err) *err = path + ": " + m;
    return nullptr;
  };
  std::unique_ptr<Database> d(new Database());
  if (exclusive_lock) {
    d->lock_fd_ = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (d->lock_fd_ < 0) return fail(std::string("cannot create the lock file: ") + std::strerror(errno));
    if (::flock(d->lock_fd_, LOCK_EX | LOCK_NB) != 0) return fail("the database is in use by another registry process (lock held on " + path + ".lock)");
  }
  // Create the file 0600 ourselves: SQLite would use the umask, and the journal inherits the database file's mode.
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return fail(std::strerror(errno));
  ::close(fd);
  if (sqlite3_open_v2(path.c_str(), &d->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    return fail(d->db_ ? sqlite3_errmsg(d->db_) : "cannot open");
  sqlite3_busy_timeout(d->db_, 5000);
  if (!exec(d->db_, "PRAGMA foreign_keys = ON; PRAGMA synchronous = FULL; PRAGMA trusted_schema = OFF;")) return fail(sqlite3_errmsg(d->db_));
  int version = 0;
  {
    Stmt st(d->db_, "PRAGMA user_version");
    if (st && sqlite3_step(st.s) == SQLITE_ROW) version = sqlite3_column_int(st.s, 0);
  }
  if (version != 0 && version != kSchemaVersion)
    return fail("registry schema version " + std::to_string(version) + " is not supported (this build: " + std::to_string(kSchemaVersion) + ")");
  if (!exec(d->db_, kSchema)) return fail(std::string("schema: ") + sqlite3_errmsg(d->db_));
  if (version == 0 && !exec(d->db_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str())) return fail(sqlite3_errmsg(d->db_));
  return d;
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
  if (lock_fd_ >= 0) ::close(lock_fd_);  // closing releases the flock
}

AuditHead Database::head_of(sqlite3* db) {
  AuditHead h;
  Stmt st(db, "SELECT seq, hash FROM audit ORDER BY seq DESC LIMIT 1");
  if (st && sqlite3_step(st.s) == SQLITE_ROW) {
    h.seq = sqlite3_column_int64(st.s, 0);
    if (sqlite3_column_bytes(st.s, 1) == 32) std::memcpy(h.hash.data(), sqlite3_column_blob(st.s, 1), 32);
  }
  return h;
}

AuditHead Database::head() {
  std::lock_guard<std::mutex> lk(mu_);
  return head_of(db_);
}

// Appends one row. Before it writes anything it checks that the log is exactly what the previous append left: the last row, SQLite's AUTOINCREMENT counter and the
// stored head row all agree. A log that was truncated, rolled back or edited under the server's feet makes every write fail (and the caller refuses to answer) rather than
// extend a doubtful chain.
bool Database::append_audit_locked(const AuditEvent& e, std::int64_t ts_ms) {
  const AuditHead last = head_of(db_);
  std::int64_t used = 0;
  {
    Stmt sq(db_, "SELECT seq FROM sqlite_sequence WHERE name = 'audit'");
    if (!sq) return false;
    if (sqlite3_step(sq.s) == SQLITE_ROW) used = sqlite3_column_int64(sq.s, 0);
  }
  AuditHead stored;
  {
    Stmt hs(db_, "SELECT seq, hash FROM audit_head WHERE id = 1");
    if (!hs) return false;
    if (sqlite3_step(hs.s) == SQLITE_ROW) {
      stored.seq = sqlite3_column_int64(hs.s, 0);
      if (sqlite3_column_bytes(hs.s, 1) != 32) return false;
      std::memcpy(stored.hash.data(), sqlite3_column_blob(hs.s, 1), 32);
    }
  }
  if (last.seq != used || last.seq != stored.seq || last.hash != stored.hash) return false;
  const std::int64_t seq = last.seq + 1;
  const fmt::Digest256 h = chain_hash(last.hash, seq, ts_ms, e);
  Stmt ins(db_, "INSERT INTO audit (seq, ts, actor, action, device_id, library_id, outcome, code, detail, prev_hash, hash) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
  if (!ins) return false;
  ins.i64(1, seq);
  ins.i64(2, ts_ms);
  ins.text(3, e.actor);
  ins.text(4, e.action);
  ins.text(5, e.device_id);
  ins.text(6, e.library_id);
  ins.text(7, e.outcome);
  ins.text(8, e.code);
  ins.text(9, e.detail);
  ins.blob(10, last.hash.data(), last.hash.size());
  ins.blob(11, h.data(), h.size());
  if (sqlite3_step(ins.s) != SQLITE_DONE) return false;
  Stmt up(db_, "INSERT OR REPLACE INTO audit_head (id, seq, hash) VALUES (1, ?1, ?2)");
  if (!up) return false;
  up.i64(1, seq);
  up.blob(2, h.data(), h.size());
  return sqlite3_step(up.s) == SQLITE_DONE;
}

bool Database::audit(const AuditEvent& e) { return audit_many({e}); }

bool Database::audit_many(const std::vector<AuditEvent>& events) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!exec(db_, "BEGIN IMMEDIATE")) return false;
  const std::int64_t ts = now_ms();
  for (const AuditEvent& e : events)
    if (!append_audit_locked(e, ts)) {
      exec(db_, "ROLLBACK");
      return false;
    }
  if (!exec(db_, "COMMIT")) {
    exec(db_, "ROLLBACK");
    return false;
  }
  return true;
}

DbStatus Database::audit_issuance(std::int64_t device_row, const std::vector<AuditEvent>& events) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!exec(db_, "BEGIN IMMEDIATE")) return DbStatus::kError;
  {
    Stmt st(db_, "SELECT 1 FROM devices WHERE id = ?1 AND status = 'active'");
    if (!st) {
      exec(db_, "ROLLBACK");
      return DbStatus::kError;
    }
    st.i64(1, device_row);
    if (sqlite3_step(st.s) != SQLITE_ROW) {
      exec(db_, "ROLLBACK");
      return DbStatus::kNotActive;
    }
  }
  const std::int64_t ts = now_ms();
  for (const AuditEvent& e : events)
    if (!append_audit_locked(e, ts)) {
      exec(db_, "ROLLBACK");
      return DbStatus::kAuditFailed;
    }
  if (!exec(db_, "COMMIT")) {
    exec(db_, "ROLLBACK");
    return DbStatus::kAuditFailed;
  }
  return DbStatus::kOk;
}

std::optional<Device> Database::find_device(const std::string& device_id) {
  std::lock_guard<std::mutex> lk(mu_);
  return query_device(db_, device_id, false);
}
std::optional<Device> Database::find_active_device(const std::string& device_id) {
  std::lock_guard<std::mutex> lk(mu_);
  return query_device(db_, device_id, true);
}

DbStatus Database::register_device(const NewDevice& d, const std::string& actor, Device& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!exec(db_, "BEGIN IMMEDIATE")) return DbStatus::kError;
  auto rollback = [&](DbStatus s) {
    exec(db_, "ROLLBACK");
    return s;
  };
  // 1 = a row exists, 0 = none, -1 = the query itself failed (fail closed as an error, never as "free")
  auto exists = [&](const char* sql, const std::string& text, const void* blob) {
    Stmt st(db_, sql);
    if (!st) return -1;
    if (blob) st.blob(1, blob, 32);
    else st.text(1, text);
    const int rc = sqlite3_step(st.s);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
  };
  int x = exists("SELECT 1 FROM devices WHERE device_id = ?1 AND status = 'active'", d.device_id, nullptr);
  if (x < 0) return rollback(DbStatus::kError);
  if (x) return rollback(DbStatus::kDuplicateDeviceId);
  x = exists("SELECT 1 FROM devices WHERE lower(serial_number) = lower(?1) AND status = 'active'", d.serial_number, nullptr);
  if (x < 0) return rollback(DbStatus::kError);
  if (x) return rollback(DbStatus::kDuplicateSerial);
  // A key that was ever registered, revoked or not, can not come back as a new record: a revoked unit must not be revived by registering its key again.
  x = exists("SELECT 1 FROM devices WHERE device_public_key = ?1", "", d.public_key.data());
  if (x < 0) return rollback(DbStatus::kError);
  if (x) return rollback(DbStatus::kDuplicatePublicKey);
  std::int64_t replaces_row = 0;
  if (!d.replaces.empty()) {
    // The record being superseded: the most recent REVOKED record of that device_id that no other record supersedes yet, and it must be the same unit:
    // same serial number (case-insensitive) or same SoM id.
    Stmt st(db_,
            "SELECT id FROM devices WHERE device_id = ?1 AND status = 'revoked' AND id NOT IN (SELECT replaces FROM devices WHERE replaces IS NOT NULL) "
            "AND (lower(serial_number) = lower(?2) OR som_id = ?3) ORDER BY id DESC LIMIT 1");
    if (!st) return rollback(DbStatus::kError);
    st.text(1, d.replaces);
    st.text(2, d.serial_number);
    st.text(3, d.som_id);
    if (sqlite3_step(st.s) != SQLITE_ROW) return rollback(DbStatus::kInvalidReplaces);
    replaces_row = sqlite3_column_int64(st.s, 0);
  }
  const std::int64_t ts = now_ms();
  {
    Stmt ins(db_,
             "INSERT INTO devices (device_id, device_public_key, som_id, serial_number, tier, provisioning_date, station_id, status, replaces, registered_at) "
             "VALUES (?1,?2,?3,?4,?5,?6,?7,'active',?8,?9)");
    if (!ins) return rollback(DbStatus::kError);
    ins.text(1, d.device_id);
    ins.blob(2, d.public_key.data(), 32);
    ins.text(3, d.som_id);
    ins.text(4, d.serial_number);
    ins.text(5, d.tier);
    ins.text(6, d.provisioning_date);
    ins.text(7, d.station_id);
    if (replaces_row) ins.i64(8, replaces_row);
    else ins.null(8);
    ins.i64(9, ts);
    if (sqlite3_step(ins.s) != SQLITE_DONE) {
      // The unique indexes are the backstop for a writer in another process that won between our check and the insert: name the constraint that fired.
      return rollback(map_unique_violation(sqlite3_errmsg(db_)));
    }
  }
  AuditEvent ev;
  ev.actor = actor;
  ev.action = "register_device";
  ev.device_id = d.device_id;
  ev.outcome = "ok";
  ev.detail = "station=" + d.station_id + " serial=" + d.serial_number + " som=" + d.som_id + " tier=" + d.tier + (d.replaces.empty() ? "" : " supersedes=" + d.replaces);
  if (!append_audit_locked(ev, ts)) return rollback(DbStatus::kAuditFailed);
  if (!exec(db_, "COMMIT")) return rollback(DbStatus::kError);
  out = *query_device(db_, d.device_id, true);
  return DbStatus::kOk;
}

DbStatus Database::revoke_device(const std::string& device_id, const std::string& actor, const std::string& reason, Device& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!exec(db_, "BEGIN IMMEDIATE")) return DbStatus::kError;
  auto rollback = [&](DbStatus s) {
    exec(db_, "ROLLBACK");
    return s;
  };
  const std::optional<Device> cur = query_device(db_, device_id, false);
  if (!cur) return rollback(DbStatus::kNotFound);
  if (cur->status != "active") return rollback(DbStatus::kAlreadyRevoked);
  const std::int64_t ts = now_ms();
  {
    Stmt up(db_, "UPDATE devices SET status = 'revoked', revoked_at = ?2, revoked_by = ?3, revoke_reason = ?4 WHERE id = ?1 AND status = 'active'");
    if (!up) return rollback(DbStatus::kError);
    up.i64(1, cur->row_id);
    up.i64(2, ts);
    up.text(3, actor);
    up.text(4, reason);
    if (sqlite3_step(up.s) != SQLITE_DONE || sqlite3_changes(db_) != 1) return rollback(DbStatus::kError);
  }
  AuditEvent ev;
  ev.actor = actor;
  ev.action = "revoke_device";
  ev.device_id = device_id;
  ev.outcome = "ok";
  ev.detail = "station=" + cur->station_id + (reason.empty() ? "" : " reason=" + reason);
  if (!append_audit_locked(ev, ts)) return rollback(DbStatus::kAuditFailed);
  if (!exec(db_, "COMMIT")) return rollback(DbStatus::kError);
  out = *query_device(db_, device_id, false);
  return DbStatus::kOk;
}

std::vector<AuditRow> Database::audit_rows(std::int64_t from_seq, std::int64_t limit) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<AuditRow> rows;
  Stmt st(db_, "SELECT seq, ts, actor, action, device_id, library_id, outcome, code, detail FROM audit WHERE seq >= ?1 ORDER BY seq LIMIT ?2");
  if (!st) return rows;
  st.i64(1, from_seq);
  st.i64(2, limit);
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    AuditRow r;
    r.seq = sqlite3_column_int64(st.s, 0);
    r.ts_ms = sqlite3_column_int64(st.s, 1);
    r.actor = st.col_text(2);
    r.action = st.col_text(3);
    r.device_id = st.col_text(4);
    r.library_id = st.col_text(5);
    r.outcome = st.col_text(6);
    r.code = st.col_text(7);
    r.detail = st.col_text(8);
    rows.push_back(std::move(r));
  }
  return rows;
}

AuditVerify Database::verify_audit() {
  std::lock_guard<std::mutex> lk(mu_);
  return verify_audit(db_);
}

AuditVerify Database::verify_audit(sqlite3* db) {
  AuditVerify v;
  auto bad = [&](std::int64_t seq, const std::string& m) {
    v.ok = false;
    v.bad_seq = seq;
    v.message = m;
    return v;
  };
  Stmt st(db, "SELECT seq, ts, actor, action, device_id, library_id, outcome, code, detail, prev_hash, hash FROM audit ORDER BY seq");
  if (!st) return bad(0, std::string("cannot read the audit table: ") + sqlite3_errmsg(db));
  fmt::Digest256 prev{};
  std::int64_t expect = 1;
  int rc;
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
    AuditEvent e;
    const std::int64_t seq = sqlite3_column_int64(st.s, 0);
    const std::int64_t ts = sqlite3_column_int64(st.s, 1);
    e.actor = st.col_text(2);
    e.action = st.col_text(3);
    e.device_id = st.col_text(4);
    e.library_id = st.col_text(5);
    e.outcome = st.col_text(6);
    e.code = st.col_text(7);
    e.detail = st.col_text(8);
    if (seq != expect) return bad(expect, "audit entry " + std::to_string(expect) + " is missing (found " + std::to_string(seq) + ")");
    if (sqlite3_column_bytes(st.s, 9) != 32 || sqlite3_column_bytes(st.s, 10) != 32) return bad(seq, "audit entry " + std::to_string(seq) + " has a malformed hash");
    if (std::memcmp(sqlite3_column_blob(st.s, 9), prev.data(), 32) != 0) return bad(seq, "audit entry " + std::to_string(seq) + " does not chain to its predecessor");
    const fmt::Digest256 h = chain_hash(prev, seq, ts, e);
    if (std::memcmp(sqlite3_column_blob(st.s, 10), h.data(), 32) != 0) return bad(seq, "audit entry " + std::to_string(seq) + " was modified (hash mismatch)");
    prev = h;
    v.entries = seq;
    ++expect;
  }
  if (rc != SQLITE_DONE) return bad(0, "cannot read the audit table");
  // Tail truncation: AUTOINCREMENT remembers the highest sequence number ever used.
  {
    Stmt sq(db, "SELECT seq FROM sqlite_sequence WHERE name = 'audit'");
    std::int64_t used = 0;
    if (sq && sqlite3_step(sq.s) == SQLITE_ROW) used = sqlite3_column_int64(sq.s, 0);
    if (used != v.entries)
      return bad(v.entries + 1, "the audit log was truncated (sequence " + std::to_string(used) + " was used, last entry is " + std::to_string(v.entries) + ")");
  }
  // The stored head row must be the last audit row.
  {
    Stmt hs(db, "SELECT seq, hash FROM audit_head WHERE id = 1");
    if (!hs) return bad(0, "cannot read the audit head");
    AuditHead stored;
    if (sqlite3_step(hs.s) == SQLITE_ROW) {
      stored.seq = sqlite3_column_int64(hs.s, 0);
      if (sqlite3_column_bytes(hs.s, 1) == 32) std::memcpy(stored.hash.data(), sqlite3_column_blob(hs.s, 1), 32);
    }
    if (stored.seq != v.entries || stored.hash != prev) return bad(v.entries, "the stored audit head does not match the last audit entry");
  }
  // The audit tables and the triggers that make them append-only must be exactly the ones this build creates (the CREATE text is compared, not just the names:
  // a trigger renamed to do nothing, or a column added to the table, is a different schema).
  for (const auto& [name, want] : expected_sql()) {
    Stmt tr(db, "SELECT sql FROM sqlite_master WHERE name = ?1");
    if (!tr) return bad(0, "cannot read the schema");
    tr.text(1, name);
    if (sqlite3_step(tr.s) != SQLITE_ROW) return bad(0, "the schema object " + name + " is missing (append-only trigger or audit table removed)");
    if (tr.col_text(0) != want) return bad(0, "the schema object " + name + " was altered (its SQL text is not the expected one)");
  }
  v.ok = true;
  v.message = "audit chain ok: " + std::to_string(v.entries) + " entries";
  return v;
}

AuditVerify Database::verify_audit_file(const std::string& path) {
  AuditVerify v;
  if (::access(path.c_str(), R_OK) != 0) {
    v.message = path + ": " + std::strerror(errno);
    return v;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    v.message = path + ": cannot open as SQLite";
    sqlite3_close(db);
    return v;
  }
  v = verify_audit(db);
  sqlite3_close(db);
  return v;
}

}  // namespace velora::registry

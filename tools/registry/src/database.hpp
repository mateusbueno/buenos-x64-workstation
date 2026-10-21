#pragma once
// Story 6.8: the registry database (SQLite): the device table and the append-only, hash-chained audit log. All writes go through this class, under one mutex,
// each in a transaction together with its audit row. Vendor-side only.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

struct sqlite3;

namespace velora::registry {

struct Device {
  std::int64_t row_id = 0;
  std::string device_id;
  Key32 public_key{};
  std::string som_id, serial_number, tier, provisioning_date, station_id;
  std::string status;                 // "active" | "revoked"
  std::int64_t replaces_row = 0;      // row id of the revoked record this one supersedes (SoM swap), 0 = none
  std::string replaces_device_id;     // its device_id
  std::int64_t registered_at = 0;     // Unix ms
  std::int64_t revoked_at = 0;        // Unix ms, 0 = not revoked
  std::string revoked_by, revoke_reason;
};

struct NewDevice {
  std::string device_id;
  Key32 public_key{};
  std::string som_id, serial_number, tier, provisioning_date, station_id;
  std::string replaces;               // device_id of a revoked record to supersede, empty = none
};

struct AuditEvent {
  std::string actor;      // "station:<id>" | "admin:<id>" | "anonymous:<ip>" | "system"
  std::string action;     // "register_device", "revoke_device", "wrapped_key_issued", "refused", "tls_handshake_failed", ...
  std::string device_id;
  std::string library_id;
  std::string outcome;    // "ok" | "refused" | "error"
  std::string code;       // typed error code for refusals, empty for ok
  std::string detail;     // short, never request bodies, never key material
};

struct AuditRow : AuditEvent {
  std::int64_t seq = 0;
  std::int64_t ts_ms = 0;
};

enum class DbStatus {
  kOk,
  kDuplicateDeviceId,
  kDuplicateSerial,
  kDuplicatePublicKey,
  kInvalidReplaces,
  kNotFound,
  kNotActive,       // audit_issuance: the device is no longer active (revoked after the lookup)
  kAlreadyRevoked,
  kAuditFailed,     // the audit row could not be appended (also: the chain, sequence counter or head failed the consistency check); nothing was written
  kError,
};
const char* to_string(DbStatus s);
// Which unique constraint a SQLite "UNIQUE constraint failed" message names (kError for anything else).
DbStatus map_unique_violation(const char* sqlite_message);

struct AuditHead {
  std::int64_t seq = 0;                 // 0 = empty log
  std::array<std::uint8_t, 32> hash{};  // all zero when empty
};

struct AuditVerify {
  bool ok = false;
  std::int64_t entries = 0;
  std::int64_t bad_seq = 0;   // first entry that does not verify, 0 if the failure is not about one entry
  std::string message;
};

class Database {
 public:
  // Opens (creating, mode 0600, when absent) and migrates. nullptr and `err` on failure (including a database of another schema version).
  // `exclusive_lock` (the server): also takes an flock on `<path>.lock` for the life of the object, so a second server or a `restore` over a live database is refused.
  static std::unique_ptr<Database> open(const std::string& path, std::string* err, bool exclusive_lock = false);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Writes the device row and its audit row in one transaction. `actor` is the audit actor text. On a refusal nothing is written (the caller audits the refusal).
  DbStatus register_device(const NewDevice& d, const std::string& actor, Device& out);
  DbStatus revoke_device(const std::string& device_id, const std::string& actor, const std::string& reason, Device& out);
  // The ACTIVE record of this device_id, else the most recently registered (revoked) one.
  std::optional<Device> find_device(const std::string& device_id);
  std::optional<Device> find_active_device(const std::string& device_id);

  // Appends the issuance audit rows in one IMMEDIATE transaction that first re-checks that row `device_row` is still the ACTIVE record of its device:
  // kNotActive (nothing written) when it was revoked between the caller's lookup and now.
  DbStatus audit_issuance(std::int64_t device_row, const std::vector<AuditEvent>& events);
  bool audit(const AuditEvent& e);                         // one row
  bool audit_many(const std::vector<AuditEvent>& events);  // one transaction, all or nothing
  std::vector<AuditRow> audit_rows(std::int64_t from_seq = 1, std::int64_t limit = 1000);

  AuditHead head();
  static AuditHead head_of(sqlite3* db);   // the last audit row (seq, hash)
  AuditVerify verify_audit();
  static AuditVerify verify_audit(sqlite3* db);   // the chain, gaps, tail truncation and the append-only triggers of any open database
  static AuditVerify verify_audit_file(const std::string& path);

  sqlite3* handle() { return db_; }
  std::mutex& mutex() { return mu_; }

 private:
  Database() = default;
  bool append_audit_locked(const AuditEvent& e, std::int64_t ts_ms);
  sqlite3* db_ = nullptr;
  int lock_fd_ = -1;
  std::mutex mu_;
};

}  // namespace velora::registry

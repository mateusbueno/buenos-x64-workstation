#pragma once
// The persistent system configuration owned by sysd (Foundation: sysd config service; AR17, AR39, AD-12, NFR44, FR49).
//
// One file, `<dir>/sysd_config.pb` (dir = `<data>/config`, mode 0644, owned by the sysd user per AD-9), holding one velora.sysd.v1.SysdConfig protobuf
// (proto/sysd_config.proto): schema_version, the config revision and the ws.v1 Config. Single writer: only sysd writes it (an exclusive flock on
// `<dir>/.writer.lock` is held for the life of the store) and every mutation goes through apply() on one thread (ConfigService serialises callers).
//
// Load policy (never a crash, never a write on read, except the corrupt file which is set aside):
//   * missing file                       -> defaults (default_config()), revision 0, nothing is created until the first change;
//   * corrupt bytes / invalid content    -> defaults, the bad file is renamed `sysd_config.pb.corrupt` (kept for diagnosis), a typed reason is logged;
//   * schema_version == N                -> loaded;
//   * schema_version == N-1              -> loaded, read-only as to its version stamp: it is rewritten as N only by migrate() (the Story 8.5 migration
//                                           runner, gated by the slot probe: after the slot is marked good). A SetConfig while the file is still N-1 and
//                                           the slot is not good writes the new content KEEPING the N-1 stamp, so a rollback to the previous firmware
//                                           still reads it. Fields this release does not know are kept (protobuf unknown fields) on every rewrite;
//   * any other schema_version           -> defaults + typed reason (too_old / too_new / too_large / unreadable). The file is left untouched on read.
//                                           Before the first write replaces it a hard link `sysd_config.pb.<reason>[.N]` keeps its bytes (a name that does
//                                           not exist yet: an aside is never overwritten); the primary stays in place until the atomic replace succeeds.
// Corrupt / invalid files are renamed `sysd_config.pb.corrupt[.N]` at load (the same never-overwrite rule), so two corruptions in a row keep both.
// Persist failure policy (the matrix row "Persist failure"): the change is REJECTED with a typed error (ERROR_CODE_STORAGE_FULL for ENOSPC, else
// ERROR_CODE_STORAGE_FAILED) and nothing changes in memory or on disk; the exception is a rename that happened but whose directory sync failed
// (kCommittedNotDurable): the change is applied and the reply carries not_durable = true, as Story 4.6 does for setlists.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "sysd_config.pb.h"
#include "velora/host/live_set_store.hpp"
#include "velora/host/migration_runner.hpp"
#include "ws_v1.pb.h"

namespace velora::sysd {

constexpr std::uint32_t kSchemaVersion = 1;      // N of this release
constexpr std::size_t kMaxFileBytes = 4096;
constexpr std::uint32_t kMinPedalSpan = 256;      // Story 3.11: a calibrated pedal range needs max - min >= 256 counts
constexpr std::uint32_t kPedalMax = 4095;        // 12-bit pedal range (mcu/common/MCU-ID-MAP.md section 6)
constexpr std::size_t kMaxLiveSetIdBytes = 64;
inline constexpr const char* kFileName = "sysd_config.pb";
inline constexpr const char* kTmpFileName = "sysd_config.pb.tmp";
inline constexpr const char* kLockFileName = ".writer.lock";

constexpr std::uint32_t min_readable_version(std::uint32_t current) { return current > 1 ? current - 1 : 1; }

enum class LoadReason : std::uint8_t {
  kLoaded = 0,
  kMissing,        // no file: first start
  kCorrupt,        // bytes do not parse as a SysdConfig
  kInvalidContent, // parses, but a value is out of range (an enum this release does not know, pedal min >= max, ...)
  kTooOld,         // schema_version below N-1 (or 0)
  kTooNew,         // schema_version above N
  kTooLarge,
  kUnreadable,     // an I/O error reading it
};
const char* to_string(LoadReason r);

struct StoreOptions {
  std::string dir;                              // the config directory; must exist
  std::uint32_t current_version = kSchemaVersion;  // N (tests run N = 2 to exercise N-1)
  host::migration::SlotProbe slot_good;         // true once the running slot is marked good (RAUC); empty = never (migration deferred)
  std::function<void(const std::string&)> log; // one line per notable event (load reason, set-aside, migration); empty = silent
  // Test hooks, as the other single-writer files of the data root.
  std::function<host::liveset::HookAction(host::liveset::Stage)> on_stage;
  bool inject_dir_sync_failure = false;
  int inject_write_errno = 0;
};

// Why a SetConfig was refused: a closed ws.v1 error code, a message and details (field, reason).
struct Rejection {
  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_INTERNAL;
  std::string message;
  std::map<std::string, std::string> details;
};

enum class ApplyStatus : std::uint8_t {
  kApplied = 0,    // validated, persisted, revision + 1
  kUnchanged,      // valid, but identical to the current config: nothing written, revision unchanged
  kNotDurable,     // applied and in place, but the directory sync failed
  kRejected,       // nothing changed (see Rejection)
};

struct ApplyResult {
  ApplyStatus status = ApplyStatus::kRejected;
  Rejection rejection;  // set when kRejected
  bool changed() const noexcept { return status == ApplyStatus::kApplied || status == ApplyStatus::kNotDurable; }
};

// Validation of a patch against the current config (pure; nothing is applied). Empty = valid.
bool validate_patch(const ws::v1::Config& current, const ws::v1::ConfigPatch& patch, Rejection& why);
// A Config as stored or served: every field set, unspecified enums replaced by their defaults.
ws::v1::Config normalized(ws::v1::Config c);
// True when `c` (as loaded from disk) holds only valid values.
bool valid_config(const ws::v1::Config& c, Rejection* why = nullptr);

class ConfigStore {
 public:
  explicit ConfigStore(StoreOptions opt);  // loads; never throws, never crashes on a bad file
  ~ConfigStore();
  ConfigStore(const ConfigStore&) = delete;
  ConfigStore& operator=(const ConfigStore&) = delete;

  bool writer_locked() const noexcept { return lock_fd_ >= 0; }  // false: another sysd owns the directory (the caller must not serve)
  const ws::v1::Config& config() const noexcept { return config_; }
  std::uint64_t revision() const noexcept { return file_.revision(); }
  LoadReason load_reason() const noexcept { return reason_; }
  std::uint32_t file_version() const noexcept { return file_version_; }  // schema_version of the file as loaded / last written (N for a fresh store)
  const StoreOptions& options() const { return opt_; }

  // Single thread. Validates, merges, persists atomically (temp, fsync, rename, dir fsync) and only then replaces the in-memory config.
  ApplyResult apply(const ws::v1::ConfigPatch& patch);

  // Rewrites an N-1 file as N through the migration runner (only after slot_good() says so). Returns the runner's outcome; kUpToDate / kAbsent when
  // there is nothing to do. On success the store's file_version() becomes N.
  host::migration::Outcome migrate();

 private:
  void load();
  void log(const std::string& s) const {
    if (opt_.log) opt_.log(s);
  }
  StoreOptions opt_;
  int lock_fd_ = -1;
  velora::sysd::v1::SysdConfig file_;  // the whole file, unknown fields included; file_.config() is normalized
  ws::v1::Config config_;
  LoadReason reason_ = LoadReason::kMissing;
  std::uint32_t file_version_ = 0;
  bool aside_kept_ = false;   // the aside copy of the unreadable file exists already
  std::string aside_suffix_;  // ".too_old" / ".too_new" / ".too_large" / ".unreadable": a link keeps the file under this name before the first write
};

// The migration unit of the Story 8.5 runner for this file (from N-1 to N: the stamp changes, nothing else, unknown fields kept).
host::migration::Unit migration_unit(std::uint32_t current_version);

}  // namespace velora::sysd

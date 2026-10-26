#pragma once
// Live Set persistence (Story 4.2). The persisted form is the protobuf `velora.liveset.v1.LiveSet`
// (proto/live_set.proto). This header is the control-thread side: parse/validate/serialise, the N / N-1
// version policy, UUIDv4 minting with an injectable random source, the "resolve parts" report and the
// atomic single-writer file store. Portable C++ with protobuf; engine/core never sees any of it.
//
// Threading: every function here allocates and does file I/O; control thread / I-O threads only, never the audio
// thread. Only the control thread writes Live Set files: `LiveSetStore::write` is the single writer entry point; the
// store is opened by the control thread (which becomes the only thread allowed to call write/reconcile) and holds an
// exclusive flock on `<dir>/.writer.lock`, so a second store (same or another process) on the directory is refused.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "live_set.pb.h"

namespace velora::core {
class EngineRegistry;
}

namespace velora::host::liveset {

namespace lsp = ::velora::liveset::v1;

// ---- limits (documented in engine/host/README.md) ----------------------------------------------------------------
inline constexpr std::uint32_t kFormatVersion = 1;            // current format_version (N)
inline constexpr std::size_t kMaxFileBytes = 1u << 20;        // 1 MiB: larger files are refused before parsing
inline constexpr std::uint32_t kV1PartCount = 8;
inline constexpr std::uint32_t kProPartCount = 16;
inline constexpr std::size_t kMaxScenes = 8;
inline constexpr std::size_t kAssignSlots = 8;
inline constexpr std::size_t kMaxNameBytes = 64;              // Live Set and scene names, UTF-8 bytes
inline constexpr std::size_t kMaxInstrumentKeyBytes = 1024;
inline constexpr std::size_t kMaxInsertSlots = 5;
inline constexpr std::size_t kMaxParamEntries = 64;           // per ParamEntry list
// Tempo range (20..300 bpm) is read from the generated registry (params::kTempoBpm), not duplicated here.
inline constexpr int kOctaveMin = -4, kOctaveMax = 4;
inline constexpr int kTransposeMin = -12, kTransposeMax = 12; // semitones

// ---- typed results (closed enums; nothing crosses this API as an exception) ---------------------------------------
enum class Status : std::uint8_t {
  kOk = 0,
  kIo,                  // open/read/write/fsync/rename failed
  kNotFound,            // no such Live Set file
  kTooLarge,            // file (or serialised message) above kMaxFileBytes
  kParseError,          // not a valid protobuf message (truncated, garbage, bad UTF-8, too deep)
  kUnsupportedVersion,  // format_version outside {N-1, N}
  kInvalid,             // parsed, but a validation rule failed (see the report)
  kBadId,               // an id that is not a canonical UUIDv4 was given to a path-building call
  kNotControlThread,    // write/reconcile from a thread other than the one that opened the store
  kWriterBusy,          // another store holds the directory's writer lock
  kFaultInjected,       // a test fault hook aborted the write (simulated crash)
  kCommittedNotDurable, // write: the new file is in place (rename done) but fsync of the directory failed
  kNoSpace,             // Story 4.9: the storage is full (ENOSPC / EDQUOT); nothing was written, the old file is intact and no temp file is left
  kCount
};

const char* to_string(Status s);  // closed names, for logs

enum class IssueCode : std::uint8_t {
  kBadId = 0,
  kBadName,
  kBadTempo,
  kBadPartCount,
  kBadTimbre,
  kBadKeyRange,
  kBadOctave,
  kBadTranspose,
  kBadVolume,
  kBadOutput,
  kBadSend,
  kBadEngineType,
  kTooManyInserts,
  kBadInsertSlot,
  kTooManyParams,
  kBadParamValue,
  kWrongScope,
  kWrongClass,       // a known id of the wrong class for this list (e.g. a structural part param in a sound list)
  kDuplicateParam,
  kTooManyScenes,
  kBadSceneName,
  kTooManySceneParts,
  kBadAssignCount,
  kBadAssignParam,   // a known ASSIGN param that is not a performance parameter
  kBadArp,
  kUnknownParam,     // warning: id not in this build's registry; preserved
  kUnknownInsertType,  // warning: insert type above this build's registry enum; preserved (its params are not checked)
  kUnknownEnumValue,   // warning: PartOutput / ArpPattern / ArpRate value above this build's known range; preserved
  kCount
};

struct Issue {
  IssueCode code = IssueCode::kBadId;
  int part = -1;    // part index when the issue is inside a part (or a scene part)
  int scene = -1;   // scene index when inside a scene
  int index = -1;   // entry / slot index within a list, or the param id for kUnknownParam
  friend bool operator==(const Issue&, const Issue&) = default;
};

struct ValidationReport {
  std::vector<Issue> errors;          // any entry: the Live Set is rejected (Status::kInvalid)
  std::vector<Issue> unknown_params;  // warnings (kUnknownParam / kUnknownInsertType): preserved, never rejected
  bool ok() const { return errors.empty(); }
};

// Validates `ls` for a tier with `part_count` parts (8 v1, 16 Pro). Checks every rule of the story; format_version
// is checked by the reader/writer, not here.
ValidationReport validate(const lsp::LiveSet& ls, std::uint32_t part_count);

// ---- reading -----------------------------------------------------------------------------------------------------
struct ReadOptions {
  std::uint32_t part_count = kV1PartCount;  // 0 = tool mode: accept 8 or 16 according to the file
  std::uint32_t current_version = kFormatVersion;  // N; tests set N=2 to exercise the N-1 path
};

struct LoadResult {
  lsp::LiveSet live_set;
  ValidationReport report;        // filled for kOk (warnings) and kInvalid (errors)
  std::uint32_t file_version = 0; // format_version found in the file
  bool migrated = false;          // N-1 file migrated in memory to N
};

// Smallest readable version for a reader of version `current` (N-1, never below 1).
constexpr std::uint32_t min_readable_version(std::uint32_t current) { return current > 1 ? current - 1 : 1; }

// Parses `bytes`, applies the version policy (N and N-1 load, anything else kUnsupportedVersion), migrates N-1 -> N in
// memory (stub: no field changes exist yet, only format_version moves) and validates. Unknown fields stay in the message.
Status parse(std::string_view bytes, const ReadOptions& opt, LoadResult& out);
// parse() of the whole file at `path` (a regular file, no symlink, at most kMaxFileBytes).
Status read_file(const std::string& path, const ReadOptions& opt, LoadResult& out);
// Migration stub: from == to is a no-op, from == to-1 moves format_version, anything else is kUnsupportedVersion.
Status migrate(lsp::LiveSet& ls, std::uint32_t from, std::uint32_t to);

// Deterministic serialisation (fields in number order, unknown fields last; the message has no map fields), so
// read -> write of an unchanged file is byte-for-byte identical. kTooLarge above kMaxFileBytes.
Status serialize(const lsp::LiveSet& ls, std::string& out);

// A present-but-empty TimbreRef (both fields empty) means "no timbre": normalised to unset. Applied by parse() and write().
void normalize(lsp::LiveSet& ls);

// ---- ids ---------------------------------------------------------------------------------------------------------
class RandomSource {
 public:
  virtual ~RandomSource() = default;
  // Fills n bytes; false = no randomness available (callers must fail closed, never use partial output).
  virtual bool fill(std::uint8_t* dst, std::size_t n) = 0;
};
// The operating system's random source (/dev/urandom). No RTC dependency. Fails closed: returns false when the
// device cannot be opened or read in full (no fallback generator).
class OsRandom final : public RandomSource {
 public:
  bool fill(std::uint8_t* dst, std::size_t n) override;
};
// Deterministic source for tests and the corpus generator (xorshift64*).
class SeededRandom final : public RandomSource {
 public:
  explicit SeededRandom(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  bool fill(std::uint8_t* dst, std::size_t n) override;

 private:
  std::uint64_t s_;
};

// RFC 4122 version 4, canonical lowercase "8-4-4-4-12".
// Returns the empty string (never a valid id) when the random source fails.
std::string mint_uuid_v4(RandomSource& rng);
bool is_canonical_uuid(std::string_view s);  // lowercase 8-4-4-4-12 hex, any version
bool is_uuid_v4(std::string_view s);         // canonical, version nibble 4, variant 10xx
// A Live Set loaded from an import or restore: when its id is already in `existing_ids` (or not a valid UUIDv4) it gets
// a fresh UUIDv4 (unique among `existing_ids`) and keeps all its content. At most kMaxAdoptAttempts draws; on a failing
// random source or exhausted attempts the Live Set is left untouched and kFailed is returned (fail closed).
inline constexpr int kMaxAdoptAttempts = 16;
enum class AdoptResult : std::uint8_t { kUnchanged, kReplaced, kFailed };
AdoptResult adopt_with_collision_check(lsp::LiveSet& ls, const std::set<std::string>& existing_ids, RandomSource& rng);

// A valid new Live Set: `part_count` Sample-engine parts with an empty timbre, full key range, no scenes, empty ASSIGN
// map, tempo 120. The caller mints the id.
lsp::LiveSet make_default_live_set(const std::string& id, const std::string& name, std::uint32_t part_count);

// ---- resolution report ---------------------------------------------------------------------------------------------
enum class PartState : std::uint8_t {
  kOk = 0,
  kEngineNotAvailable,  // reserved (or unknown) engine type with no compiled engine: the part stays silent, never substituted
  kTimbreEmpty,         // a sample part with no timbre reference
};
using EngineAvailability = std::function<bool(std::uint32_t engine_type_id)>;
// Which engine types are bound in `registry` at the time of the call. The returned function owns a copy of those ids and
// does not refer to the registry, so it may outlive it.
EngineAvailability availability_of(const core::EngineRegistry& registry);
// Per-part state without loading anything (no file or library access).
std::vector<PartState> resolve_parts(const lsp::LiveSet& ls, const EngineAvailability& engine_available);

// Shared with the setlist store (Story 4.6): a display name is 1..kMaxNameBytes bytes of valid UTF-8 without control characters.
bool valid_display_name(const std::string& s);

// ---- the store -----------------------------------------------------------------------------------------------------
enum class Stage : std::uint8_t { kTempWritten, kFsynced, kRenamed, kDirSynced };
enum class HookAction : std::uint8_t { kContinue, kAbort };

// The atomic replace discipline shared by every single-writer file of the data root (Live Sets, the setlist library, the default ASSIGN
// map): temp file `<dir>/<tmp_name>` (created exclusively, never following a symlink), fsync (F_FULLFSYNC on macOS), rename over
// `<dir>/<name>`, fsync of the directory. `on_stage` is the fault-injection hook (kAbort leaves the temp file, kFaultInjected).
// kCommittedNotDurable: renamed, but the directory fsync failed (or `inject_dir_sync_failure`). Control thread only (the caller checks ownership).
// `inject_write_errno` (test only, 0 = off) makes the write of the temp file fail with that errno (ENOSPC gives kNoSpace); the temp file is removed.
Status atomic_replace_file(const std::string& dir, const std::string& name, const std::string& tmp_name, const std::string& bytes,
                           const std::function<HookAction(Stage)>& on_stage, bool inject_dir_sync_failure, int inject_write_errno = 0, unsigned file_mode = 0644);
// Reads a regular file (no symlink) of at most `max_bytes` into `out`. kNotFound when absent, kTooLarge above the cap, kIo otherwise.
Status read_regular_file(const std::string& path, std::size_t max_bytes, std::string& out);

struct StoreOptions {
  std::string dir;   // an existing directory on the journaled partition; temp files live in it too
  std::uint32_t part_count = kV1PartCount;
  std::uint32_t current_version = kFormatVersion;
  // Test hook: called after each stage of a write; kAbort stops dead (temp file left behind, Status::kFaultInjected),
  // or the hook may _exit() to simulate a crash.
  std::function<HookAction(Stage)> on_stage;
  bool inject_dir_sync_failure = false;  // test only: makes the directory fsync after the rename report failure
  bool inject_remove_failure = false;    // test only: remove() fails with kIo before touching the file
  int inject_write_errno = 0;            // test only: the temp-file write of every write() fails with this errno (ENOSPC = storage full)
};

class LiveSetStore {
 public:
  // Takes the directory's exclusive writer lock and records the calling thread as THE control thread.
  static Status open(StoreOptions opt, std::unique_ptr<LiveSetStore>& out);
  ~LiveSetStore();
  LiveSetStore(const LiveSetStore&) = delete;
  LiveSetStore& operator=(const LiveSetStore&) = delete;

  // Explicit handover of the control-thread role to the calling thread. Call it from the NEW control thread after the old
  // one has stopped writing. Ownership is a std::thread::id compare, and ids may be reused after a thread exits, so a
  // thread that was never the owner could match a dead owner's id: hand over explicitly and keep one long-lived control thread.
  void rebind_control_thread() { owner_.store(std::this_thread::get_id()); }
  // The single writer entry point. Validates, serialises, then: temp file `<id>.liveset.tmp` in the same directory,
  // fsync file, rename over `<id>.liveset`, fsync directory. A crash leaves the old file or the new one, never a
  // partial one. Empty `timbre` messages are normalised to unset first (see normalize()). kCommittedNotDurable: the file
  // is in place but the directory sync failed. Invalid Live Sets are rejected with nothing written (`report` gets the issues).
  Status write(const lsp::LiveSet& ls, ValidationReport* report = nullptr);
  // Deletes `<id>.liveset` (the caller has already removed it from every setlist) and fsyncs the directory. kNotFound when there is no such
  // file, kBadId for a malformed id, kNotControlThread from another thread. Control thread only.
  // `path` (optional) is the file to delete when it is not `<id>.liveset` (a catalog entry from a directory scan): it must be a `.liveset` file directly
  // in the store directory, else kBadId.
  Status remove(const std::string& id, const std::string& path = {});
  // Removes leftover `*.liveset.tmp` files of an interrupted write. Control thread only.
  Status reconcile(std::size_t& removed);
  // Reads `<id>.liveset` (any thread).
  Status read(const std::string& id, LoadResult& out) const;

  std::string path_for(const std::string& id) const;
  const StoreOptions& options() const { return opt_; }
  // Story 4.9: how many times this store ATTEMPTED to touch a Live Set file: every write() that passed validation and went to the writer, whether it
  // then succeeded, hit a full disk or was aborted, plus every remove() that went to the disk (attempts, not commits: a failed ENOSPC write counts).
  // Playing, scene and Live Set changes, edits and stepping must leave it at zero: only an explicit Write (or Create / Delete Live Set) changes it. Any thread.
  std::uint64_t mutations() const noexcept { return mutations_.load(std::memory_order_relaxed); }
  // Test hook: makes later writes fail like a full disk (errno ENOSPC) or stop failing (0). Control thread.
  void set_inject_write_errno(int e) { opt_.inject_write_errno = e; }
  void set_inject_dir_sync_failure(bool b) { opt_.inject_dir_sync_failure = b; }

 private:
  explicit LiveSetStore(StoreOptions opt) : opt_(std::move(opt)) {}
  StoreOptions opt_;
  std::atomic<std::uint64_t> mutations_{0};
  int lock_fd_ = -1;
  std::atomic<std::thread::id> owner_;
};

}  // namespace velora::host::liveset

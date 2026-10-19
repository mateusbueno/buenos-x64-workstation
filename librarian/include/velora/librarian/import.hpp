#pragma once
// librarian: imports an SFZ library into an immutable bundle and publishes it atomically under
// <root>/lib/<library_id>/ (layout shared with velora/host/library_resolver.hpp).
//
// Publish protocol (Story 1.13), every step crash safe:
//   1. lock <root>/lib/.staging/.lock (flock), reconcile leftovers of earlier runs
//   2. parse the SFZ (opcode subset), load every sample, build the bundle in a staging directory
//      <root>/lib/.staging/<unique>/ on the same filesystem as the destination
//   3. verify: reopen the staged file, verify_all(), compare the region table and compare every
//      sample bit for bit against a fresh decode of its source WAV
//   4. fsync the file and the staging directory
//   5. rename staging dir -> <root>/lib/<library_id>/<bundle_id>/ (atomic; fsync parent)
//   6. flip `current` with a symlink-then-rename (atomic; fsync parent)
// A crash before step 5 leaves only staging garbage (removed by the next run); between 5 and 6
// it leaves a complete but unreferenced bundle directory; `current` always names a complete,
// verified bundle (the previous or the new one).
//
// SoundFont 2 (Story 5.3): `import_sf2` runs the same protocol for one .sf2 file = one library with
// one instrument per preset (instrument_key "bank:preset", see formatlib/sf2_import.hpp). The
// library_id is the UUIDv5 of the canonical absolute path, exactly as for SFZ. Step 3 reads the
// SF2 sample words instead of WAV files, and the verification compares every sample, region
// table and key against a fresh parse of the source file. `import_file` picks the importer by
// extension (.sf2, case-insensitive, else SFZ).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "velora/format/error.hpp"

namespace velora::librarian {

// Points of the publish protocol at which a fault can be injected (tests / VELORA_LIBRARIAN_FAULT).
enum class Stage : std::uint8_t {
  kStaged = 0,       // bundle written to the staging directory
  kVerified,         // bit-exact verification passed
  kFsynced,          // file and staging directory fsynced
  kRenamed,          // bundle directory in place, `current` not yet touched
  kLinkCreated,      // temporary symlink created, not yet renamed over `current`
  kFlipped,          // `current` flipped
};
const char* to_string(Stage s);
bool parse_stage(const std::string& name, Stage& out);

enum class HookAction : std::uint8_t {
  kContinue = 0,
  kAbort,   // stop immediately without any cleanup, as if the process had been killed
};

struct FaultInjection {
  std::function<HookAction(Stage)> on_stage;
  // Flip one bit in the staged bundle's block data before verification (must be detected).
  bool corrupt_staged_before_verify = false;
  // Behave as if the staging directory were on another filesystem than the destination.
  bool pretend_cross_device = false;
};

struct ImportOptions {
  std::string sfz_path;   // the source file: an .sfz, or an .sf2 for import_sf2/import_file
  std::string root;       // existing data root directory: bundles go to <root>/lib/...
  std::string base_dir;   // instrument_key is the sfz path relative to this; default: sfz's directory
  std::string category;   // optional, stored as <bundle_dir>/category inside the atomic publish (Epic 5 owns the UX)
  FaultInjection fault;
};

enum class Status : std::uint8_t {
  kOk = 0,
  kSkipped,        // SFZ unsupported/invalid or audio unsupported/missing: nothing published (see ReasonCode)
  kInvalidArgument,
  kVerifyFailed,   // staged bundle did not verify bit-exact: nothing published
  kIo,
  kCrossDevice,    // staging not on the destination filesystem
  kBusy,           // another import holds the lock
  kAborted,        // fault injection stopped the run
};
const char* to_string(Status s);

// Why a file was skipped (closed set, Story 5.1). Anything that is not a skip (I/O errors, a busy
// lock, verification failures, bad arguments) has reason `none` and is told by `status`.
enum class ReasonCode : std::uint8_t {
  kNone = 0,
  kUnsupportedFeatures,  // the file uses something outside the supported subset (see `offenders`)
  kDamaged,              // cannot be parsed, missing sample, unreadable or truncated WAV (see `failing_path`)
};
const char* to_string(ReasonCode r);  // "none", "unsupported_features", "damaged"

// One unsupported construct: the opcode (or "<header>", or "sample" for an audio format outside
// the supported set) with the file:line that uses it and a human readable message.
struct Offender {
  std::string opcode, file;
  std::size_t line = 0;
  std::string message;
};

// An allow-listed (non-sounding) opcode that was accepted and ignored.
struct AllowListedUse {
  std::string opcode, file;
  std::size_t line = 0;
};

// A warning in the import report. Closed set of codes: `ignored_opcode` = a sound-affecting
// opcode was accepted but has no effect (fil_type/resonance without cutoff); `ignored_sf2_feature`
// (Story 5.3) = an SF2 generator or modulator outside the instrument model was ignored (`ignored_zone` = a zone the
// model cannot hold was dropped, `ignored_empty_preset` = a preset without a sounding region was not imported): `preset` is
// the "bank:preset" id, `opcode` carries the generator name or the modulator description, `line`
// is 0. Warnings never skip a file.
struct ImportWarning {
  std::string code, opcode, file;
  std::size_t line = 0;
  std::string message;
  std::string preset;  // ignored_sf2_feature only
};

// How a failure of BundleWriter::add_sample is reported: kUnsupportedFormat is a skipped file with
// reason unsupported_features; kInvalidArgument is Status::kInvalidArgument and kEncoderFailure
// Status::kIo (both "failed", never "damaged WAV"). Any other error is a skipped, damaged file.
Status status_for_add_sample_error(velora::format::Error e, ReasonCode* reason);

struct ImportResult {
  Status status = Status::kInvalidArgument;
  ReasonCode reason_code = ReasonCode::kNone;  // set when status == kSkipped
  std::vector<Offender> offenders;             // reason_code unsupported_features (may also be set for damaged files)
  std::string failing_path;                    // reason_code damaged: the file that is missing, unreadable or malformed
  std::vector<ImportWarning> warnings;         // e.g. ignored_opcode (logged, counted, never a reason to skip)
  std::vector<AllowListedUse> allowlisted;     // accepted non-sounding opcodes (logged, never a reason to skip)
  std::vector<std::string> reasons;  // one line per problem ("file:line: ..."), or the failure text
  std::string library_id;            // canonical UUID string (empty before it is known)
  std::string bundle_id;             // 64 lowercase hex
  std::string bundle_dir;            // <root>/lib/<library_id>/<bundle_id>
  bool bundle_created = false;       // false when an identical bundle was already there
  bool current_flipped = false;      // false when `current` already named this bundle
  std::size_t sample_count = 0;
  std::size_t region_count = 0;
  std::size_t source_region_count = 0;  // <region> headers in the SFZ sources
  std::size_t reconciled = 0;           // leftovers removed at startup
  // Story 5.3 (SF2 only; `sf2` is false for SFZ): instruments = presets, source_region_count = zones
  // that became regions, and the file-level counters of ignored_sf2_feature warnings.
  bool sf2 = false;
  std::size_t preset_count = 0, ignored_generators = 0, ignored_modulators = 0;
  bool ok() const { return status == Status::kOk; }
};

ImportResult import_sfz(const ImportOptions& opt);
ImportResult import_sf2(const ImportOptions& opt);   // Story 5.3
ImportResult import_file(const ImportOptions& opt);  // by extension: .sf2 -> import_sf2, else import_sfz
bool is_sf2_path(const std::string& path);           // extension .sf2, case-insensitive

// Removes leftovers of crashed imports: everything under <root>/lib/.staging/ except the lock.
// Called by import_sfz under the lock; exposed for tests and a future service start-up.
// Returns the number of entries removed, or -1 if the directory could not be read.
int reconcile_staging(const std::string& root);

inline constexpr const char* kStagingDirName = ".staging";
inline constexpr const char* kLockFileName = ".lock";
// Stale `.current.tmp.*` links are swept only in the directory of the library being imported.
inline constexpr const char* kCategoryFileName = "category";

}  // namespace velora::librarian

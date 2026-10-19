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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
  std::string sfz_path;
  std::string root;       // existing data root directory: bundles go to <root>/lib/...
  std::string base_dir;   // instrument_key is the sfz path relative to this; default: sfz's directory
  std::string category;   // optional, stored as <bundle_dir>/category inside the atomic publish (Epic 5 owns the UX)
  FaultInjection fault;
};

enum class Status : std::uint8_t {
  kOk = 0,
  kSkipped,        // SFZ unsupported/invalid or audio unsupported/missing: nothing published
  kInvalidArgument,
  kVerifyFailed,   // staged bundle did not verify bit-exact: nothing published
  kIo,
  kCrossDevice,    // staging not on the destination filesystem
  kBusy,           // another import holds the lock
  kAborted,        // fault injection stopped the run
};
const char* to_string(Status s);

struct ImportResult {
  Status status = Status::kInvalidArgument;
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
  bool ok() const { return status == Status::kOk; }
};

ImportResult import_sfz(const ImportOptions& opt);

// Removes leftovers of crashed imports: everything under <root>/lib/.staging/ except the lock.
// Called by import_sfz under the lock; exposed for tests and a future service start-up.
// Returns the number of entries removed, or -1 if the directory could not be read.
int reconcile_staging(const std::string& root);

inline constexpr const char* kStagingDirName = ".staging";
inline constexpr const char* kLockFileName = ".lock";
// Stale `.current.tmp.*` links are swept only in the directory of the library being imported.
inline constexpr const char* kCategoryFileName = "category";

}  // namespace velora::librarian

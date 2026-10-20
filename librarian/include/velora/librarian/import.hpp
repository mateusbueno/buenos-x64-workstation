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
//
// Story 5.8, checkpoints and drive removal. One SF2 with many presets (timbres) is converted preset by preset. Every `checkpoint_timbres` finished
// timbres (default 10) or `checkpoint_bytes` of decoded PCM (default 20 GiB), whichever comes first, the timbres finished so far are published as a
// NEW immutable bundle of the same library (same protocol: stage, verify, fsync, rename, flip `current`), so a long import becomes visible in steps
// and a pulled drive or a power cut loses at most the timbres after the last checkpoint. The bundle is a single file (header, manifest, index,
// blocks), so a bigger bundle cannot share bytes with a smaller one by a hard link: the writer keeps the verified encoded blocks of the finished
// timbres in memory and each checkpoint writes them again (no re-read of the source, no re-encode). A library that already has a `current` bundle
// (Import again) is never checkpointed: its old bundle stays current until the whole new one is verified. When the source disappears (the injectable
// `source_gone` probe says so, or reading a parsed file fails with an I/O error) the timbre in progress is discarded whole, the finished timbres are
// published (unless the library was published before: the old bundle stays) and the result has Status::kDriveRemoved.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <memory>

#include "velora/format/byte_source.hpp"
#include "velora/format/category.hpp"
#include "velora/format/error.hpp"
#include "velora/format/hash.hpp"

namespace velora::format {
class BundleWriter;
}

namespace velora::librarian {

// Story 6.3: what a `bundle_sink` receives (see ImportOptions::bundle_sink). The pointers are valid only during the call.
struct BuiltBundle {
  const velora::format::BundleWriter* writer = nullptr;  // the verified content; the sink finalizes it (e.g. as a protected bundle)
  std::string library_id;                                // canonical UUID string (the override when ImportOptions::library_id is set)
  std::string plain_bundle_id;                           // 64 hex: bundle_id of the UNPROTECTED bundle of the same content
  velora::format::Digest256 plain_sha256{};              // SHA-256 of the unprotected bundle's bytes (BundleWriter::nonce_from_plain_digest)
  std::string catalog_meta;                              // catalog.meta text (library_index.hpp) when write_catalog_meta was set, else empty
};

// Points of the publish protocol at which a fault can be injected (tests / VELORA_LIBRARIAN_FAULT).
enum class Stage : std::uint8_t {
  kStaged = 0,       // bundle written to the staging directory
  kVerified,         // bit-exact verification passed
  kFsynced,          // file and staging directory fsynced
  kRenamed,          // bundle directory in place, `current` not yet touched
  kLinkCreated,      // temporary symlink created, not yet renamed over `current`
  kFlipped,          // `current` flipped
  kStageCreated,     // Story 5.6: the (empty) staging directory exists, nothing written into it yet. Appended: the values above are unchanged.
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
  // Story 5.8: 0 = corrupt every staged bundle (the old behaviour); n > 0 = only the n-th bundle staged by this import (a verification failure AFTER a checkpoint).
  int corrupt_round = 0;
  mutable int rounds_seen = 0;
  // Behave as if the staging directory were on another filesystem than the destination.
  bool pretend_cross_device = false;
  // Story 5.8: called at the boundaries of a timbre (an SF2 preset; an SFZ file is one timbre): "timbre-start" (before anything of it is read),
  // "timbre-done" (converted, before any publish) and "checkpoint-published" (after an intermediate publish; `timbre` = timbres in the bundle).
  // kAbort stops the import there (Status::kAborted); whatever was published before stays.
  std::function<HookAction(const char* point, std::size_t timbre)> on_timbre;
  // Tests: replaces the byte source an SF2 is parsed and read from (also for the verification re-parse), to inject I/O errors and short reads.
  std::function<std::shared_ptr<const velora::format::ByteSource>(const std::string& path)> open_sf2;
};

inline constexpr std::uint32_t kDefaultCheckpointTimbres = 10;
inline constexpr std::uint64_t kDefaultCheckpointBytes = 20ull << 30;

struct ImportResult;

struct ImportOptions {
  std::string sfz_path;   // the source file: an .sfz, or an .sf2 for import_sf2/import_file
  std::string root;       // existing data root directory: bundles go to <root>/lib/...
  std::string base_dir;   // instrument_key is the sfz path relative to this; default: sfz's directory
  // Optional (empty = none stored). A category key (`category.piano`), slug or English name (formatlib
  // `parse_category`), never "none"; anything else is `kInvalidArgument`. Stored as the stable key text in
  // <bundle_dir>/category inside the atomic publish (Story 5.5; bundles written before it hold free text,
  // which `parse_category_file` still reads).
  std::string category;
  // Story 5.6 (the librarian service sets these; the CLI does not, so its bundles stay byte-for-byte what they were): write `catalog.meta`
  // (library_index.hpp) into the staged bundle directory, before the rename, so the catalog can be rebuilt from bundles alone. It records the
  // library name, the source and each instrument's display name and category. `category` (required then) is the default category of every
  // instrument; `instrument_meta` overrides name and/or category per instrument key; `library_name` overrides the library's display name (and,
  // for a one-instrument SFZ, the instrument's, unless that instrument has its own override). None of this touches `bundle.vlb` or `bundle_id`.
  bool write_catalog_meta = false;
  std::string library_name;
  struct InstrumentMeta {
    std::string key, name, category;  // name / category empty = keep the default
  };
  std::vector<InstrumentMeta> instrument_meta;
  // Story 5.8. Checkpoint interval of a multi-timbre (SF2) import; 0 = the default. Decoded PCM bytes since the last publish; 0 = the default.
  std::uint32_t checkpoint_timbres = 0;
  std::uint64_t checkpoint_bytes = 0;
  // Tests: the size counted for timbre `index` towards `checkpoint_bytes` (default: its decoded PCM size, `actual`).
  std::function<std::uint64_t(std::size_t index, std::uint64_t actual)> checkpoint_size_fn;
  // True once the source can no longer be read (volume unmounted / device gone). Polled between timbres, between samples and whenever a read or a
  // verification fails; empty = never (an I/O error while reading a parsed SF2 still counts as removal).
  std::function<bool()> source_gone;
  // The folder a failed read is examined in (opendir + readdir) to tell a vanished drive from a bad file; default: the directory of `sfz_path`.
  std::string source_root;
  // Set by the importers, not by callers: the number of timbres of the SOURCE file (decides whether a one-timbre bundle is "single" for naming, so a checkpoint of
  // one timbre keeps the file-stem library name), and whether the bundle being staged is an intermediate one of a checkpointed import (`checkpoint` marker file).
  std::size_t source_timbres = 0;
  bool mark_checkpoint = false;
  // Called after an intermediate publish (a checkpoint) with the result so far (library_id, bundle_id, bundle_dir, timbres_published). It may block.
  std::function<void(const ImportResult&)> on_checkpoint;
  // Called once the number of timbres of the source is known (an SF2 parsed; not called for an SFZ or a file that does not parse).
  std::function<void(std::size_t timbres)> on_timbres;
  FaultInjection fault;
  // Story 6.3 (tools/content-pack). Both are additive; the librarian service and the CLI never set them, so user imports are unchanged.
  // `library_id`: a vendor library id (canonical UUID text) used instead of the one derived from the source path. Empty = derived. An
  // unparsable or all-zero id is kInvalidArgument. A path-derived id stays the rule for every user import (FR24).
  std::string library_id;
  // `bundle_sink`: build-only mode for the offline content-pack tool. The importer parses, loads and encodes exactly as for a user import and
  // verifies the result bit for bit against the source, but IN MEMORY: no data root is needed or touched, nothing is staged, locked, written,
  // renamed or flipped (so no plaintext bundle, block or PCM reaches the disk), there are no checkpoints, and the writer is handed to the
  // sink instead of being published. A sink error is Status::kIo. The result is kOk with library_id and bundle_id (of the unprotected
  // bundle) filled, bundle_created and current_flipped false, and bundle_dir empty. The librarian itself never links any cipher; the sink is where
  // a tool plugs in its own.
  std::function<velora::format::Error(const BuiltBundle&)> bundle_sink;
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
  kDriveRemoved,   // Story 5.8: the source disappeared mid-import; the timbre in progress was discarded whole, `timbres_published` of `timbres_total` are published
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
  // Story 5.5: the librarian's suggestion for this file (kNone = no confident suggestion). Never stored in the
  // bundle and never a substitute for the caller's explicit category.
  velora::format::Category suggested_category = velora::format::Category::kNone;
  // Story 5.8: timbres (SF2 presets; 1 for an SFZ) of the file, and how many are in the published bundle (also with kDriveRemoved, kAborted or a
  // failure after a checkpoint). `checkpoints` = intermediate publishes made (the final publish is not counted).
  std::size_t timbres_total = 0, timbres_published = 0, checkpoints = 0;
  std::uint64_t converted_bytes = 0;  // decoded PCM bytes of the timbres that were fully converted (finished); with kDriveRemoved: what the removed item got done
  bool ok() const { return status == Status::kOk; }
};

// "" if `category` is empty or a valid assignable category (accepted spellings: see ImportOptions::category),
// otherwise the reason it is refused (unknown name, "none", control characters).
std::string category_problem(const std::string& category);

ImportResult import_sfz(const ImportOptions& opt);
ImportResult import_sf2(const ImportOptions& opt);   // Story 5.3
ImportResult import_file(const ImportOptions& opt);  // by extension: .sf2 -> import_sf2, else import_sfz
bool is_sf2_path(const std::string& path);           // extension .sf2, case-insensitive

// Removes leftovers of crashed imports: everything under <root>/lib/.staging/ except the lock.
// Called by import_sfz under the lock; exposed for tests and a future service start-up.
// Returns the number of entries removed, or -1 if the directory could not be read.
int reconcile_staging(const std::string& root);

inline constexpr const char* kStagingDirName = ".staging";
// Story 5.6: `<root>/lib/.service.lock`. The librarian service holds it exclusively for its lifetime (it owns the `<root>/lib` tree and the catalog); the
// `librarian import` CLI creates it if needed and takes it shared for the whole run, so it refuses to run while a service holds it and a service refuses to start (it needs the lock exclusively) during a CLI import. It sits inside
// `lib/` so the service's sandbox needs write access to that one tree only (Story 5.9).
inline constexpr const char* kServiceLockFileName = ".service.lock";
class CliWriterGuard {
 public:
  explicit CliWriterGuard(const std::string& root);
  ~CliWriterGuard();
  CliWriterGuard(const CliWriterGuard&) = delete;
  CliWriterGuard& operator=(const CliWriterGuard&) = delete;
  bool ok() const { return ok_; }                 // false: a service holds the lock (or it cannot be examined); see error()
  const std::string& error() const { return err_; }

 private:
  int fd_ = -1;
  bool ok_ = true;
  std::string err_;
};
inline constexpr const char* kLockFileName = ".lock";
// Stale `.current.tmp.*` links are swept only in the directory of the library being imported.
inline constexpr const char* kCategoryFileName = "category";
// Story 5.8: written inside the staging directory of every bundle of a first-time SF2 import (so it is part of the atomic publish) and removed again once the LAST
// bundle of the run is current. A bundle that still has it and is not `current` is a superseded checkpoint or an unflipped publish: the service deletes it
// (in-run after the next bundle is current, and the startup reconcile for what a kill left). Bundles without it (final bundles, SFZ, Import again) are never pruned.
inline constexpr const char* kCheckpointMarkerName = "checkpoint";

}  // namespace velora::librarian

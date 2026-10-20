#pragma once
// Import preflight (Story 5.7): scan a source folder (a mounted volume) for .sfz / .sf2 files WITHOUT converting anything, classify every timbre and
// estimate the space an import needs. Pure file system work: no catalog, no sockets, no writes (the service adds the catalog lookup for `already
// imported` and the free space, see library_service.hpp).
//
// Headers only. An SFZ is parsed as text (formatlib's SFZ parser, which stats the samples); every sample is then examined through its WAV header
// (RIFF chunk walk with pread: format chunk and the data chunk's size, compared with the file size) and never read; an SF2 is parsed through formatlib's
// SF2 reader over a counting byte source, which reads the `pdta` tables and the headers of the samples that are referenced and never the sample words.
// `PreflightResult::bytes_read` counts every byte the scan read (tests assert it stays far below the size of the sample data). Classification is the
// importer's: unsupported features, damage (unreadable / truncated / missing samples, a loop or offset that does not fit its sample) and the file
// level reasons of Story 5.1.
//
// Libraries: a library of the preflight is the top-level folder of the source (its files at any depth below it); files directly in the source form
// the first library, named after the volume label (`volume_label`, default the source folder's name). This is the grouping the keyboardist sees. The
// IMPORT unit is unchanged (Story 5.1): every SFZ / SF2 file is its own library in the catalog with its own `library_id`, minted from the canonical
// absolute path by formatlib (`library_id_from_source`, the function BundleWriter::set_source_id uses).
//
// Rows: one per timbre. An SFZ file is one timbre (`instrument_key` = its file name, as the importer keys it); an SF2 file has one row per preset
// (`instrument_key` "bank:preset"). A file that cannot be parsed is one row with an empty instrument_key and its file name as the name.
//
// Bounds: at most `max_files` .sfz / .sf2 files are examined (default kPreflightMaxFiles) and at most kPreflightMaxEntries directory entries are
// visited, at most kPreflightMaxDepth levels deep; reaching any of them sets `truncated`. Symbolic links are never followed: a linked directory is
// skipped; a linked file is used only when its target lies inside the source root, and a sample that is a link leaving the root makes its SFZ damaged
// (nothing outside the volume is read). The scan polls `cancel` between entries.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "velora/format/category.hpp"

namespace velora::librarian {

inline constexpr std::size_t kPreflightMaxFiles = 20000;
inline constexpr std::size_t kPreflightMaxEntries = 400000;
inline constexpr std::size_t kPreflightMaxDepth = 32;
// At most this many rows are listed (checked before each file, so the last file's presets may overshoot it); reaching it sets `truncated`.
inline constexpr std::size_t kPreflightMaxRows = 50000;

// Space rule (documented in librarian/README.md): an import of items whose decoded PCM sizes add up to P needs
//     required = kImportPcmFactor * P + kImportReserveBytes          (0 items: 0)
// P is the worst-case UNCOMPRESSED size (frames * channels * bytes per sample; the bundle is FLAC coded and never larger). kImportPcmFactor = 2: the
// published bundle plus the staging copy (an item is staged and verified before the previous bundle of a re-imported library is released). The fixed
// reserve covers the catalog, the manifests, filesystem metadata and the file system's own slack.
inline constexpr std::uint64_t kImportPcmFactor = 2;
inline constexpr std::uint64_t kImportReserveBytes = 256ull << 20;
std::uint64_t required_import_bytes(std::uint64_t pcm_total, std::size_t item_count);

// The closed set of row states (proto: PreflightStatus). `already_imported` is decided by the service from the catalog; the scan itself never sets it.
enum class RowStatus : std::uint8_t {
  kReady = 0,                    // a category is suggested (Story 5.5)
  kNeedsCategory,                // no confident suggestion: the user must choose one (FR21)
  kSkippedUnsupportedFeatures,   // the importer would skip it (Story 5.1 reason unsupported_features)
  kSkippedDamaged,               // ... damaged
  kAlreadyImported,
};
const char* to_string(RowStatus s);  // "ready", "needs_category", "skipped_unsupported_features", "skipped_damaged", "already_imported"

struct PreflightRow {
  std::string path;            // canonical absolute path of the .sfz / .sf2 file (the import item path)
  std::string rel_path;        // relative to the source
  std::string name;            // display name: the file stem (SFZ) or the preset name (SF2)
  std::string format;          // "sfz" | "sf2"
  std::string library_id;      // minted from the source fingerprint (the importer's id)
  std::string instrument_key;  // see above
  std::uint64_t size_bytes = 0;  // SFZ: the .sfz plus its sample files on disk; SF2: the decoded sample bytes of the preset (an unparsable file: its size)
  std::uint64_t pcm_bytes = 0;   // worst-case decoded size an import of this timbre writes (0 for a skipped row)
  velora::format::Category suggested = velora::format::Category::kNone;
  RowStatus status = RowStatus::kNeedsCategory;
  std::size_t library = 0;     // index into PreflightResult::libraries
  std::string detail;          // the first reason of a skipped row ("file:line: ..."), else empty
};

struct PreflightLibrary {
  std::string name;            // the folder name, or the volume label for the files at the root
  std::string folder;          // top-level folder relative to the source; empty for the root library
  std::size_t first_row = 0, row_count = 0;  // rows are contiguous
};

// A file a timbre depends on (the .sfz, its #include files and its samples; the .sf2 itself) with the size and mtime seen by the scan. The service keeps them
// beside the decoded size so the figure is reused for an ImportRequest only while none of them has changed.
struct FileDep {
  std::string path;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
};

struct PreflightOptions {
  std::string source;          // an existing folder
  std::string volume_label;    // empty = the source folder's name
  std::size_t max_files = kPreflightMaxFiles;  // 0 = the default; never above kPreflightMaxFiles
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(std::size_t files_done)> on_file;  // test hook: called after every file (a test sets `cancel` from it)
};

struct PreflightResult {
  bool ok = false;             // false: the source could not be scanned (`error` says why)
  std::string error;
  std::string source;          // canonical source path
  std::string volume_label;
  std::vector<PreflightLibrary> libraries;
  std::vector<PreflightRow> rows;
  std::size_t files_scanned = 0;     // .sfz / .sf2 files examined
  std::size_t entries_visited = 0;
  std::size_t symlinks_skipped = 0;  // links not followed (linked directories, links leaving the source)
  std::size_t unreadable_dirs = 0;
  bool truncated = false;            // a bound was reached: the listing is partial
  bool cancelled = false;
  std::size_t aliases_skipped = 0;   // links to a file that is already listed under its real path (listed once, counted once)
  std::map<std::string, std::vector<FileDep>> deps;  // real path of a file -> what its rows depend on
  std::uint64_t bytes_read = 0;      // bytes the scan read from files (headers, tables, SFZ text)
  std::uint64_t duration_ms = 0;     // wall clock of the scan (the Mac figure is not representative of the board)
};

PreflightResult preflight_scan(const PreflightOptions& opt);

// What an import of one file needs: the decoded PCM bytes of everything it would write. Used by the service for the space check of an ImportRequest.
// NOT confined to a root: the importer accepts samples anywhere the SFZ parser lets them be (e.g. a sibling folder), so the estimate must see them too. `skipped` is
// true when the importer would skip the file (then pcm_bytes is 0 and the caller must not take that as "needs nothing": the service charges the file's own size).
struct ImportEstimate {
  bool skipped = false;
  std::uint64_t pcm_bytes = 0;
  std::uint64_t bytes_read = 0;
  std::vector<FileDep> deps;
};
ImportEstimate estimate_import(const std::string& file);

}  // namespace velora::librarian

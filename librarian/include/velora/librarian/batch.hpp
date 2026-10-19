#pragma once
// Batch import and the machine-readable import report (Story 5.1).
//
// One SFZ is still one library (import_sfz); a batch runs import_sfz for every file of the
// inputs, continues past any file that is skipped or fails, and reports per file.
//
// Report format (stable, JSON Lines, one object per line, UTF-8, keys in the order shown):
//   {"type":"file","file":S,"status":"imported|unchanged|skipped|failed","reason":"none|unsupported_features|damaged",
//    "failing_path":S,"offenders":[{"opcode":S,"file":S,"line":N,"message":S}],
//    "allowlisted_count":N,"allowlisted":[{"opcode":S,"file":S,"line":N}],
//    "warnings":[{"code":"ignored_opcode","opcode":S,"file":S,"line":N,"message":S}],"warning_count":N,
//    "messages":[S],"library_id":S,"bundle_id":S,"samples":N,"regions":N,"detail":S}
//   {"type":"summary","files":N,"imported":N,"unchanged":N,"skipped":N,"failed":N}
// `status` skipped = the file was not imported for the given `reason` (a closed set); `failed` =
// an infrastructure problem (I/O error, busy lock, verification failure, bad arguments), reason
// `none`, `detail` names the Status. `failing_path` is empty unless the reason is damaged.
// Nothing of a skipped file is published. All strings are valid UTF-8 (invalid bytes become U+FFFD).
#include <cstddef>
#include <string>
#include <vector>

#include "velora/librarian/import.hpp"

namespace velora::librarian {

struct BatchOptions {
  std::vector<std::string> inputs;  // .sfz files and/or directories (searched recursively for *.sfz)
  std::string root;
  std::string base_dir;             // as ImportOptions::base_dir (default: each SFZ's own directory)
  std::string category;
  FaultInjection fault;
};

struct FileReport {
  std::string sfz_path;
  ImportResult result;
};

struct BatchResult {
  std::vector<FileReport> files;
  std::size_t imported = 0, unchanged = 0, skipped = 0, failed = 0;
  bool all_ok() const { return !files.empty() && skipped == 0 && failed == 0; }
};

// Expands directories (recursively, `.sfz` case-insensitive, sorted) and keeps files as given.
// Duplicates are dropped. A directory without SFZ files yields no entries; `empty_dirs` lists it.
// The same file reached through different spellings (./a.sfz, a.sfz, a symlink) is listed once.
// `walk_errors` receives "dir: message" when a directory walk failed part-way (import_batch turns
// each into a failed entry; files found before the error are kept).
std::vector<std::string> expand_inputs(const std::vector<std::string>& inputs,
                                       std::vector<std::string>* empty_dirs = nullptr,
                                       std::vector<std::string>* walk_errors = nullptr);

BatchResult import_batch(const BatchOptions& opt);

// "imported" | "unchanged" | "skipped" | "failed"
const char* file_status(const ImportResult& r);

std::string report_file_line(const FileReport& f);      // no trailing newline
std::string report_summary_line(const BatchResult& b);  // no trailing newline
std::string report_jsonl(const BatchResult& b);         // every file line then the summary, each ending in '\n'

}  // namespace velora::librarian

#pragma once
// Batch import and the machine-readable import report (Story 5.1).
//
// One SFZ is still one library (import_sfz), and so is one SF2 (import_sf2, Story 5.3: its presets are the
// library's instruments); a batch runs import_file for every file of the inputs, continues past any file that is skipped or fails, and reports per file.
//
// Report format (stable, JSON Lines, one object per line, UTF-8, keys in the order shown):
//   {"type":"file","file":S,"status":"imported|unchanged|skipped|failed","reason":"none|unsupported_features|damaged",
//    "failing_path":S,"offenders":[{"opcode":S,"file":S,"line":N,"message":S}],
//    "allowlisted_count":N,"allowlisted":[{"opcode":S,"file":S,"line":N}],
//    "warnings":[{"code":"ignored_opcode","opcode":S,"file":S,"line":N,"message":S}],"warning_count":N,
//    "messages":[S],"library_id":S,"bundle_id":S,"samples":N,"regions":N,"detail":S}
//   An SF2 file line (Story 5.3) has the same keys and ends with
//    ,"format":"sf2","presets":N,"ignored_generators":N,"ignored_modulators":N
//   its warnings have the code "ignored_sf2_feature" (or "ignored_zone", "ignored_empty_preset") and one more key after "code":
//    {"code":"ignored_sf2_feature","preset":"bank:preset","opcode":GENERATOR_OR_MODULATOR,"file":S,"line":0,"message":S}
//   (one warning per ignored item and preset; the two counters are the file-level totals of those warnings;
//   for an SF2 the reasons of a skipped file are "file: detail" lines and `offenders` list unsupported
//   items as {"opcode":"sample"|"ifil",...}).
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
  std::vector<std::string> inputs;  // .sfz/.sf2 files and/or directories (searched recursively for *.sfz and *.sf2)
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

// Expands directories (recursively, `.sfz` and `.sf2` case-insensitive, sorted) and keeps files as given.
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

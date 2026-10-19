// librarian import <file.sfz | file.sf2 | dir>... --root <data root> [--base DIR] [--category NAME] [--report FILE|-]
//
// One SFZ or one SF2 is one library (an SF2's presets are its instruments). Several files and/or directories
// (searched recursively for *.sfz and *.sf2) are
// a batch: every file is imported on its own, a skipped or failed file never stops the others.
// Exit codes: 0 every file imported (or already up to date), 1 at least one file not imported
// (reasons on stderr), 2 usage. `--report FILE` writes the JSON Lines import report
// (velora/librarian/batch.hpp); `--report -` writes it to stdout instead of the text output.
// Test builds only (-DVELORA_LIBRARIAN_TEST_HOOKS): VELORA_LIBRARIAN_FAULT=<stage>
// (staged|verified|fsynced|renamed|link-created|flipped) makes the process _exit(137) there.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "velora/librarian/batch.hpp"

using namespace velora::librarian;

int main(int argc, char** argv) {
  auto usage = [&]() {
    std::fprintf(stderr,
                 "usage: %s import <file.sfz|file.sf2|dir>... --root DIR [--base DIR] [--category NAME] [--report FILE|-]\n",
                 argv[0]);
    return 2;
  };
  if (argc < 3 || std::strcmp(argv[1], "import") != 0 || std::strncmp(argv[2], "--", 2) == 0) return usage();
  BatchOptions opt;
  std::string report_path;
  int i = 2;
  for (; i < argc && std::strncmp(argv[i], "--", 2) != 0; ++i) opt.inputs.push_back(argv[i]);
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage();
    if (a == "--root") opt.root = argv[++i];
    else if (a == "--base") opt.base_dir = argv[++i];
    else if (a == "--category") opt.category = argv[++i];
    else if (a == "--report") report_path = argv[++i];
    else return usage();
  }
  if (opt.root.empty()) return usage();
#ifdef VELORA_LIBRARIAN_TEST_HOOKS
  if (const char* f = std::getenv("VELORA_LIBRARIAN_FAULT")) {
    Stage at;
    if (!parse_stage(f, at)) return usage();
    opt.fault.on_stage = [at](Stage s) {
      if (s == at) ::_exit(137);
      return HookAction::kContinue;
    };
  }
#endif
  // A single plain file keeps the Story 1.13 text output; anything else is a batch.
  struct stat st;
  bool single = false;
  if (opt.inputs.size() == 1) single = ::stat(opt.inputs[0].c_str(), &st) != 0 || !S_ISDIR(st.st_mode);
  const BatchResult b = import_batch(opt);
  const bool report_to_stdout = report_path == "-";
  bool report_failed = false;  // the per-file reasons and the summary are still printed
  if (!report_path.empty()) {
    const std::string text = report_jsonl(b);
    if (report_to_stdout) {
      std::fwrite(text.data(), 1, text.size(), stdout);
    } else {
      std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
      out << text;
      out.flush();
      if (!out) {
        std::fprintf(stderr, "librarian: cannot write report %s\n", report_path.c_str());
        report_failed = true;
      }
    }
  }
  for (const auto& f : b.files) {
    const ImportResult& r = f.result;
    if (!r.ok()) {
      std::fprintf(stderr, "librarian: %s: %s%s%s: nothing published\n", f.sfz_path.c_str(), to_string(r.status),
                   r.reason_code == ReasonCode::kNone ? "" : " ", r.reason_code == ReasonCode::kNone ? "" : to_string(r.reason_code));
      for (const auto& why : r.reasons) std::fprintf(stderr, "  %s\n", why.c_str());
      continue;
    }
    if (report_to_stdout) continue;
    const char* outcome = r.bundle_created ? (r.current_flipped ? "published" : "bundle added") : "unchanged";
    if (single)
      std::printf("library_id %s\nbundle_id %s\nsamples %zu\nregions %zu\n%s\n", r.library_id.c_str(),
                  r.bundle_id.c_str(), r.sample_count, r.region_count, outcome);
    else
      std::printf("%s: %s library_id %s bundle_id %s samples %zu regions %zu\n", f.sfz_path.c_str(), outcome,
                  r.library_id.c_str(), r.bundle_id.c_str(), r.sample_count, r.region_count);
  }
  if (!report_to_stdout && !single)
    std::printf("%zu file(s): %zu imported, %zu unchanged, %zu skipped, %zu failed\n", b.files.size(), b.imported,
                b.unchanged, b.skipped, b.failed);
  return b.all_ok() && !report_failed ? 0 : 1;
}

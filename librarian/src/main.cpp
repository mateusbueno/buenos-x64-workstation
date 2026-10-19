// librarian import <file.sfz | file.sf2 | dir>... --root <data root> [--base DIR] [--category NAME] [--report FILE|-]
//
// One SFZ or one SF2 is one library (an SF2's presets are its instruments). Several files and/or directories
// (searched recursively for *.sfz and *.sf2) are
// a batch: every file is imported on its own, a skipped or failed file never stops the others.
// Exit codes: 0 every file imported (or already up to date), 1 at least one file not imported
// (reasons on stderr), 2 usage. `--report FILE` writes the JSON Lines import report
// (velora/librarian/batch.hpp); `--report -` writes it to stdout instead of the text output.
//
// librarian suggest-category [--table FILE] <name>...
// Prints "<name><TAB><category key | none>" per argument (Story 5.5): the keyword-table suggestion for a file name
// or a path (its folders count too, below the file name). `--table` replaces the embedded keyword table with a JSON
// file (same format as librarian/data/category_keywords.json). Exit 0 for a usable table, 2 on usage or an unreadable --table, 3 if the embedded table is invalid. Control characters in names are echoed as \\xNN.
// `import --category` takes a category key (category.piano), its slug or an English name, never "none".
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
#include "velora/librarian/category_suggest.hpp"

using namespace velora::librarian;

int main(int argc, char** argv) {
  auto usage = [&]() {
    std::fprintf(stderr,
                 "usage: %s import <file.sfz|file.sf2|dir>... --root DIR [--base DIR] [--category NAME] [--report FILE|-]\n"
                 "       %s suggest-category [--table FILE] <name>...\n",
                 argv[0], argv[0]);
    return 2;
  };
  if (argc >= 2 && std::strcmp(argv[1], "suggest-category") == 0) {
    KeywordTable custom;
    const KeywordTable* table = &KeywordTable::embedded();
    int a = 2;
    if (a + 1 < argc && std::strcmp(argv[a], "--table") == 0) {
      std::string err;
      if (!KeywordTable::load_file(argv[a + 1], custom, &err)) {
        std::fprintf(stderr, "librarian: keyword table %s: %s\n", argv[a + 1], err.c_str());
        return 2;
      }
      table = &custom;
      a += 2;
    }
    if (table == &KeywordTable::embedded() && !KeywordTable::embedded_error().empty()) {
      std::fprintf(stderr, "librarian: the embedded keyword table is invalid: %s\n", KeywordTable::embedded_error().c_str());
      return 3;
    }
    if (a >= argc || std::strncmp(argv[a], "--", 2) == 0) {
      std::fprintf(stderr, "usage: %s suggest-category [--table FILE] <name>...\n", argv[0]);
      return 2;
    }
    for (; a < argc; ++a) {
      const auto c = suggest_category(suggest_input_for_path(argv[a], {}), *table);
      // Control characters in the echoed name (tab, newline...) would break the TSV: print them as \xNN.
      std::string shown;
      for (const char* q = argv[a]; *q; ++q) {
        const unsigned char ch = static_cast<unsigned char>(*q);
        if (ch < 0x20 || ch == 0x7f) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\x%02x", ch);
          shown += hex;
        } else {
          shown.push_back(*q);
        }
      }
      std::printf("%s\t%s\n", shown.c_str(), c ? std::string(velora::format::key(*c)).c_str() : "none");
    }
    return 0;
  }
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

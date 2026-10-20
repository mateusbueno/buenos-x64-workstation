// librarian import <file.sfz | file.sf2 | dir>... --root <data root> [--base DIR] [--category NAME] [--report FILE|-]
//                         [--checkpoint-timbres N] [--checkpoint-bytes N]
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
//
// librarian install-protected <pack dir> --root <data root> [--bundle FILE] [--wrapped-key FILE] [--library-id UUID] [--trust-list FILE]
// Story 6.4: verifies a protected vendor bundle (a tools/content-pack output directory) against the trusted content-signing keys of this build and installs it
// under <root>/lib/<library_id>/<bundle_id>/ (stage, verify every block hash, fsync, rename, flip `current`). Nothing is decrypted: the wrapped key is stored as
// it is. Exit codes: 0 installed (or the same bundle was already installed: "unchanged"), 1 refused or failed (the typed reason on stderr, nothing published),
// 2 usage. `--trust-list FILE` (dev variant only) names a `*.pub-list` of trusted public keys instead of the embedded ones. Like `import` it refuses to run while a
// librarian service owns the data root, and the catalog is not written (the service's next start lists the library).
// Test builds only (-DVELORA_LIBRARIAN_TEST_HOOKS): VELORA_LIBRARIAN_FAULT=<stage>
// (stage-created|staged|verified|fsynced|renamed|link-created|flipped) makes the process _exit(137) there. Story 5.8:
// VELORA_LIBRARIAN_KILL=<point>@<n> sends the process SIGKILL at the n-th (1-based) time it reaches <point>, a stage name as above or
// timbre-start|timbre-done|checkpoint-published; VELORA_LIBRARIAN_SOURCE_GONE=<point>@<n> makes the source "disappear" there instead (the importer then
// ends with drive removed).
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "velora/librarian/batch.hpp"
#include "velora/librarian/category_suggest.hpp"
#include "velora/librarian/install.hpp"
#include "velora/librarian/trust.hpp"

using namespace velora::librarian;

namespace {

#ifdef VELORA_LIBRARIAN_TEST_HOOKS
int g_copy_kill_at = 0;  // VELORA_LIBRARIAN_KILL=copy-chunk@N
// VELORA_LIBRARIAN_FAULT=<stage> _exit(137)s there; VELORA_LIBRARIAN_KILL=<stage>@<n> sends SIGKILL at the n-th arrival (installs: the stage points only).
bool install_test_hooks(FaultInjection& f) {
  if (const char* v = std::getenv("VELORA_LIBRARIAN_FAULT")) {
    Stage at;
    if (!parse_stage(v, at)) return false;
    f.on_stage = [at](Stage s) {
      if (s == at) ::_exit(137);
      return HookAction::kContinue;
    };
  }
  if (const char* v = std::getenv("VELORA_LIBRARIAN_KILL")) {
    const std::string t = v;
    const std::size_t at = t.find('@');
    const std::string name = t.substr(0, at);
    const int n = at == std::string::npos ? 1 : std::atoi(t.c_str() + at + 1);
    if (!name.empty() && n > 0) {
      auto count = std::make_shared<int>(0);
      if (name == "copy-chunk") {  // mid-copy of the bundle into the staging directory
        g_copy_kill_at = n;
      } else {
        auto prev = f.on_stage;
        f.on_stage = [=](Stage s) {
          if (name == to_string(s) && ++(*count) == n) ::kill(::getpid(), SIGKILL);
          return prev ? prev(s) : HookAction::kContinue;
        };
      }
    }
  }
  return true;
}
#endif

int install_command(int argc, char** argv) {
  auto usage = [&]() {
    std::fprintf(stderr, "usage: %s install-protected <pack dir> --root DIR [--bundle FILE] [--wrapped-key FILE] [--library-id UUID] [--trust-list FILE]\n", argv[0]);
    return 2;
  };
  InstallOptions opt;
  std::string trust_list;
  int i = 2;
  if (i < argc && std::strncmp(argv[i], "--", 2) != 0) opt.source_dir = argv[i++];
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage();
    if (a == "--root") opt.root = argv[++i];
    else if (a == "--bundle") opt.bundle_path = argv[++i];
    else if (a == "--wrapped-key") opt.wrapped_key_path = argv[++i];
    else if (a == "--library-id") opt.expected_library_id = argv[++i];
    else if (a == "--trust-list") trust_list = argv[++i];
    else return usage();
  }
  if (opt.root.empty() || (opt.source_dir.empty() && opt.bundle_path.empty())) return usage();
  velora::format::TrustedKeys trusted;
  std::string err;
  if (!resolve_trusted_keys(trust_list, trusted, err)) {
    std::fprintf(stderr, "librarian: %s\n", err.c_str());
    return 2;
  }
  opt.trusted = &trusted;
#ifdef VELORA_LIBRARIAN_TEST_HOOKS
  if (!install_test_hooks(opt.fault)) return usage();
  if (g_copy_kill_at > 0) opt.on_copy_chunk = [](std::uint64_t chunk) {
    if (static_cast<int>(chunk) == g_copy_kill_at) ::kill(::getpid(), SIGKILL);
  };
#endif
  CliWriterGuard writer_guard(opt.root);
  if (!writer_guard.ok()) {
    std::fprintf(stderr, "librarian: %s\n", writer_guard.error().c_str());
    return 1;
  }
  const std::string what = !opt.source_dir.empty() ? opt.source_dir : opt.bundle_path;
  const InstallResult r = install_protected(opt);
  if (r.ok()) {
    std::printf("%s\nlibrary_id %s\nbundle_id %s\ninstruments %u\nblocks %llu\n", r.status == InstallStatus::kInstalled ? "installed" : "unchanged", r.library_id.c_str(),
                r.bundle_id.c_str(), r.instruments, static_cast<unsigned long long>(r.blocks));
    return 0;
  }
  if (r.status == InstallStatus::kRejected)
    std::fprintf(stderr, "librarian: %s: rejected (%s): %s: nothing published\n", what.c_str(), to_string(r.reason), r.detail.c_str());
  else
    std::fprintf(stderr, "librarian: %s: %s: %s: nothing published\n", what.c_str(), to_string(r.status), r.detail.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  auto usage = [&]() {
    std::fprintf(stderr,
                 "usage: %s import <file.sfz|file.sf2|dir>... --root DIR [--base DIR] [--category NAME] [--report FILE|-]\n"
                 "          [--checkpoint-timbres N] [--checkpoint-bytes N]\n"
                 "       %s suggest-category [--table FILE] <name>...\n"
                 "       %s install-protected <pack dir> --root DIR [--bundle FILE] [--wrapped-key FILE] [--library-id UUID] [--trust-list FILE]\n",
                 argv[0], argv[0], argv[0]);
    return 2;
  };
  if (argc >= 2 && std::strcmp(argv[1], "install-protected") == 0) return install_command(argc, argv);
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
    else if (a == "--checkpoint-timbres" || a == "--checkpoint-bytes") {
      // Story 5.8: a SoundFont is published in steps of N finished presets / N bytes of decoded PCM (defaults 10 and 20 GiB). Plain decimal, at least 1.
      char* end = nullptr;
      const char* v = argv[++i];
      errno = 0;
      const unsigned long long n = std::strtoull(v, &end, 10);
      if (errno != 0 || end == v || *end != '\0' || n == 0 || v[0] == '-' || (a == "--checkpoint-timbres" && n > 0xFFFFFFFFull)) return usage();
      if (a == "--checkpoint-timbres") opt.checkpoint_timbres = static_cast<std::uint32_t>(n);
      else opt.checkpoint_bytes = n;
    }
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
  // <point>@<n>: counts the arrivals at a named point (stage names and the timbre points share one counter per name).
  struct PointSpec {
    std::string name;
    int n = 0;
    bool set = false;
  };
  auto parse_spec = [](const char* env) {
    PointSpec p;
    if (const char* v = std::getenv(env)) {
      const std::string t = v;
      const std::size_t at = t.find('@');
      p.name = t.substr(0, at);
      p.n = at == std::string::npos ? 1 : std::atoi(t.c_str() + at + 1);
      p.set = !p.name.empty() && p.n > 0;
    }
    return p;
  };
  const PointSpec kill_at = parse_spec("VELORA_LIBRARIAN_KILL"), gone_at = parse_spec("VELORA_LIBRARIAN_SOURCE_GONE");
  if (kill_at.set || gone_at.set) {
    auto counts = std::make_shared<std::map<std::string, int>>();
    auto gone_flag = std::make_shared<bool>(false);
    auto arrive = [=](const char* name) {
      const int c = ++(*counts)[name];
      if (kill_at.set && kill_at.name == name && c == kill_at.n) ::kill(::getpid(), SIGKILL);
      if (gone_at.set && gone_at.name == name && c == gone_at.n) *gone_flag = true;
    };
    auto prev_stage = opt.fault.on_stage;
    opt.fault.on_stage = [=](Stage s) {
      arrive(to_string(s));
      return prev_stage ? prev_stage(s) : HookAction::kContinue;
    };
    opt.fault.on_timbre = [=](const char* point, std::size_t) {
      arrive(point);
      return HookAction::kContinue;
    };
    if (gone_at.set) opt.source_gone = [=]() { return *gone_flag; };
  }
#endif
  // A single plain file keeps the Story 1.13 text output; anything else is a batch.
  struct stat st;
  bool single = false;
  if (opt.inputs.size() == 1) single = ::stat(opt.inputs[0].c_str(), &st) != 0 || !S_ISDIR(st.st_mode);
  // Story 5.6: while a librarian service owns this data root, the CLI must not write into it.
  CliWriterGuard writer_guard(opt.root);
  if (!writer_guard.ok()) {
    std::fprintf(stderr, "librarian: %s\n", writer_guard.error().c_str());
    return 1;
  }
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
    if (r.status == Status::kDriveRemoved) {  // Story 5.8: the finished timbres are published, the one in progress was discarded whole
      std::fprintf(stderr, "librarian: %s: drive removed: %zu of %zu timbre(s) published\n", f.sfz_path.c_str(), r.timbres_published, r.timbres_total);
      continue;
    }
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

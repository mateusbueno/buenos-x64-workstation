// librarian import <file.sfz> --root <data root> [--base DIR] [--category NAME]
//
// Exit codes: 0 imported (or already up to date), 1 not imported (reasons on stderr), 2 usage.
// Test builds only (-DVELORA_LIBRARIAN_TEST_HOOKS): VELORA_LIBRARIAN_FAULT=<stage>
// (staged|verified|fsynced|renamed|link-created|flipped) makes the process _exit(137) there.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "velora/librarian/import.hpp"

using namespace velora::librarian;

int main(int argc, char** argv) {
  auto usage = [&]() {
    std::fprintf(stderr, "usage: %s import <file.sfz> --root DIR [--base DIR] [--category NAME]\n", argv[0]);
    return 2;
  };
  if (argc < 3 || std::strcmp(argv[1], "import") != 0 || std::strncmp(argv[2], "--", 2) == 0) return usage();
  ImportOptions opt;
  opt.sfz_path = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage();
    if (a == "--root") opt.root = argv[++i];
    else if (a == "--base") opt.base_dir = argv[++i];
    else if (a == "--category") opt.category = argv[++i];
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
  const ImportResult r = import_sfz(opt);
  if (!r.ok()) {
    std::fprintf(stderr, "librarian: %s: nothing published\n", to_string(r.status));
    for (const auto& why : r.reasons) std::fprintf(stderr, "  %s\n", why.c_str());
    return 1;
  }
  std::printf("library_id %s\nbundle_id %s\nsamples %zu\nregions %zu\n%s\n", r.library_id.c_str(),
              r.bundle_id.c_str(), r.sample_count, r.region_count,
              r.bundle_created ? (r.current_flipped ? "published" : "bundle added") : "unchanged");
  return 0;
}

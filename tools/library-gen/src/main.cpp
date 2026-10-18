// velora-library-gen --root DIR --libraries N [--pad-gib G] [--first-index K]
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "library_gen.hpp"

static bool parse_u64(const char* s, std::uint64_t& v) {
  if (!s || !*s || *s == '-') return false;
  char* end = nullptr;
  errno = 0;
  v = std::strtoull(s, &end, 10);
  return errno == 0 && *end == 0;
}

int main(int argc, char** argv) {
  velora::libgen::Options opt;
  bool have_root = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    std::uint64_t n = 0;
    if (a == "--root" && v) { opt.root = v; have_root = true; ++i; }
    else if (a == "--libraries" && parse_u64(v, n) && n <= 0xffffffffu) { opt.libraries = static_cast<std::uint32_t>(n); ++i; }
    else if (a == "--pad-gib" && parse_u64(v, n) && n < (1ull << 30)) { opt.pad_bytes = n << 30; ++i; }
    else if (a == "--first-index" && parse_u64(v, n) && n <= 0xffffffffu) { opt.first_index = static_cast<std::uint32_t>(n); ++i; }
    else {
      std::fprintf(stderr, "usage: %s --root DIR --libraries N [--pad-gib G] [--first-index K]\n"
                           "bad argument: %s\n", argv[0], a.c_str());
      return 2;
    }
  }
  if (!have_root || opt.libraries == 0) {
    std::fprintf(stderr, "--root is required and --libraries must be at least 1\n");
    return 2;
  }
  bool pad_ok = true;
  std::string err;
  if (!velora::libgen::generate_all(opt, &pad_ok, &err)) {
    std::fprintf(stderr, "library-gen: %s\n", err.c_str());
    return 1;
  }
  if (!pad_ok) std::fprintf(stderr, "warning: sparse padding unsupported here; libraries have no padding\n");
  std::printf("generated %u libraries under %s/lib (pad %llu GiB each, sparse)\n", opt.libraries,
              opt.root.c_str(), static_cast<unsigned long long>(opt.pad_bytes >> 30));
  return 0;
}

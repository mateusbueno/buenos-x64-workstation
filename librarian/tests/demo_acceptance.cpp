// Real-library acceptance (Story 1.13), performed manually by the founder.
// VELORA_SFZ_DEMO_DIR = directory holding e.g. the Salamander Grand Piano and/or a VSCO2
// instrument. Every *.sfz found below it is imported into a temporary data root and the number of
// regions in the published bundle must equal the number of <region> headers in the SFZ sources
// (counted by the parser and, independently, by a textual scan of the main file and its #includes).
// Exit code 77 (ctest SKIP) when the variable is unset; 0 pass; 1 fail.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;

namespace {
// Strips // line comments and /* */ block comments (state carried across lines).
std::string strip_comments(const std::string& line, bool& in_block) {
  std::string out;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (in_block) {
      if (line.compare(i, 2, "*/") == 0) { in_block = false; ++i; }
    } else if (line.compare(i, 2, "/*") == 0) {
      in_block = true; ++i;
    } else if (line.compare(i, 2, "//") == 0) {
      break;
    } else {
      out += line[i];
    }
  }
  return out;
}

// Counts <region> headers in `file` and everything it #includes. A file included twice counts
// twice (the parser reads it twice); `stack` only guards against include cycles.
std::size_t textual_regions(const fs::path& file, std::vector<std::string>& stack) {
  if (stack.size() > 16 || std::find(stack.begin(), stack.end(), file.string()) != stack.end()) return 0;
  std::ifstream in(file);
  std::string raw;
  std::size_t n = 0;
  bool in_block = false;
  static const std::regex region(R"re(<region>)re"), inc(R"re(^\s*#include\s+"([^"]+)")re");
  stack.push_back(file.string());
  while (std::getline(in, raw)) {
    const std::string line = strip_comments(raw, in_block);
    for (auto it = std::sregex_iterator(line.begin(), line.end(), region); it != std::sregex_iterator(); ++it) ++n;
    std::smatch m;
    if (std::regex_search(line, m, inc)) {
      std::string rel = m[1];
      for (char& c : rel)
        if (c == '\\') c = '/';
      n += textual_regions(file.parent_path() / rel, stack);
    }
  }
  stack.pop_back();
  return n;
}

bool is_sfz(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sfz";
}
}  // namespace

int main() {
  const char* dir = std::getenv("VELORA_SFZ_DEMO_DIR");
  if (!dir || !*dir) {
    std::printf("librarian_demo_acceptance: VELORA_SFZ_DEMO_DIR not set, skipped\n");
    return 77;
  }
  std::vector<fs::path> sfz;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec))
  {
    std::error_code fec;
    if (it->is_regular_file(fec) && !fec && is_sfz(it->path())) sfz.push_back(it->path());
  }
  std::sort(sfz.begin(), sfz.end());
  if (sfz.empty()) {
    std::printf("FAIL: no .sfz file found under %s\n", dir);
    return 1;
  }
  const fs::path root = fs::temp_directory_path(ec) / ("velora_demo_accept_" + std::to_string(::getpid()));
  int failures = 0;
  for (const auto& f : sfz) {
    velora::librarian::ImportOptions opt;
    opt.sfz_path = f.string();
    opt.root = root.string();
    const auto r = velora::librarian::import_sfz(opt);
    std::vector<std::string> stack;
    const std::size_t text_count = textual_regions(f, stack);
    if (!r.ok()) {
      std::printf("FAIL %s: %s\n", f.c_str(), velora::librarian::to_string(r.status));
      for (const auto& why : r.reasons) std::printf("     %s\n", why.c_str());
      ++failures;
      continue;
    }
    auto b = velora::format::BundleReader::open_file(r.bundle_dir + "/" + velora::host::kBundleFileName);
    const std::size_t in_bundle = b.ok() ? b.value().instruments()[0].regions.size() : 0;
    const bool ok = b.ok() && in_bundle == r.source_region_count && in_bundle == text_count;
    std::printf("%s %s: %zu samples, %zu regions (parser %zu, text scan %zu)\n", ok ? "ok  " : "FAIL",
                f.c_str(), r.sample_count, in_bundle, r.source_region_count, text_count);
    if (!ok) ++failures;
  }
  fs::remove_all(root, ec);
  return failures ? 1 : 0;
}

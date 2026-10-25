// Compatibility corpus gate for the sysd config (Story 8.5): every *.json under the corpus root (tests/compat, recursively) must load on this build.
// A failure names the file and exits non-zero, which fails the CTest gate. Usage: compat_config_test <tests/compat dir>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "velora/format/json_lite.hpp"
#include "velora/ui/system_config.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2 || !fs::is_directory(argv[1])) {
    std::fprintf(stderr, "usage: compat_config_test <tests/compat dir>\n");
    return 2;
  }
  std::vector<fs::path> files;
  int fail = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(argv[1], ec), end;
  if (ec) {
    std::printf("COMPAT FAIL: cannot walk %s: %s\n", argv[1], ec.message().c_str());
    return 1;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) {
      std::printf("COMPAT FAIL: directory walk error under %s: %s\n", argv[1], ec.message().c_str());
      ++fail;
      break;
    }
    const fs::path& p = it->path();
    if (p.filename().string().rfind(".", 0) == 0) {  // dotfiles (.DS_Store) are never corpus
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::error_code e2;
    if (ext == ".json" && it->is_regular_file(e2)) files.push_back(p);
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    std::string text, err;
    velora::ui::SystemConfig c;
    if (!velora::format::read_text_file(p.string(), text, &err) || c.load(text) != velora::ui::ConfigLoadStatus::Ok) {
      std::printf("COMPAT FAIL: config file does not open on this build: %s\n", p.string().c_str());
      ++fail;
      continue;
    }
    // Round trip: what is written back opens again with the same unknown members.
    velora::ui::SystemConfig d;
    if (d.load(c.to_json()) != velora::ui::ConfigLoadStatus::Ok || d.unknown_fields != c.unknown_fields || d.to_json() != c.to_json()) {
      std::printf("COMPAT FAIL: config file does not round-trip: %s\n", p.string().c_str());
      ++fail;
    }
  }
  if (files.empty()) {
    std::printf("COMPAT FAIL: no config files under %s\n", argv[1]);
    ++fail;
  }
  std::printf("compat_config_test: %zu files, %d failures\n", files.size(), fail);
  return fail ? 1 : 0;
}

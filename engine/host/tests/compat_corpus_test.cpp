// Compatibility corpus gate (Story 8.5, FR49, AR29): every persisted file under the corpus root (tests/compat, recursively) must open on the current
// build. A file that does not open, or that no reader claims, prints its path and fails the gate (non-zero exit = CTest failure).
//   *.liveset  Live Set (any tier)          *.library  setlist library          *.pb  default ASSIGN map
//   *.json     sysd config: opened by the ui build's compat_config_test (same root); *.md / *.txt: notes, ignored
// Usage: compat_corpus_test <tests/compat dir>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "velora/host/live_set_store.hpp"
#include "velora/host/setlist_store.hpp"

namespace fs = std::filesystem;
namespace lsn = velora::host::liveset;
namespace sln = velora::host::setlist;

int main(int argc, char** argv) {
  if (argc != 2 || !fs::is_directory(argv[1])) {
    std::fprintf(stderr, "usage: compat_corpus_test <tests/compat dir>\n");
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
    if (p.filename().string().rfind(".", 0) == 0) {  // .DS_Store and other dotfiles: never corpus (a dot directory is not descended)
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    std::error_code e2;
    if (it->is_regular_file(e2) || it->is_symlink(e2)) files.push_back(p);
  }
  std::sort(files.begin(), files.end());
  std::size_t json_files = 0;
  std::size_t opened = 0, livesets = 0, libraries = 0, assigns = 0;
  for (const fs::path& p : files) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".json") ++json_files;
    if (ext == ".json" || ext == ".md" || ext == ".txt") continue;
    const std::string path = p.string();
    if (ext == ".liveset") {
      lsn::ReadOptions ro;
      ro.part_count = 0;  // tool mode: the tier comes from the file
      lsn::LoadResult lr;
      const lsn::Status s = lsn::read_file(path, ro, lr);
      if (s != lsn::Status::kOk) {
        std::printf("COMPAT FAIL: Live Set does not open on this build (%s): %s\n", lsn::to_string(s), path.c_str());
        ++fail;
        continue;
      }
      ++livesets;
    } else if (ext == ".library" || ext == ".pb") {
      std::string bytes;
      const lsn::Status rs = lsn::read_regular_file(path, sln::kMaxFileBytes, bytes);
      if (rs != lsn::Status::kOk) {
        std::printf("COMPAT FAIL: cannot read (%s): %s\n", lsn::to_string(rs), path.c_str());
        ++fail;
        continue;
      }
      sln::Status s;
      if (ext == ".library") {
        sln::LoadResult lr;
        s = sln::parse_library(bytes, sln::kFormatVersion, lr);
        ++libraries;
      } else {
        sln::AssignResult ar;
        s = sln::parse_assign(bytes, sln::kFormatVersion, ar);
        ++assigns;
      }
      if (s != sln::Status::kOk) {
        std::printf("COMPAT FAIL: file does not open on this build (%s): %s\n", sln::to_string(s), path.c_str());
        ++fail;
        continue;
      }
    } else {
      std::printf("COMPAT FAIL: no reader claims this file (add one or remove it): %s\n", path.c_str());
      ++fail;
      continue;
    }
    ++opened;
  }
  if (livesets == 0 || libraries == 0 || assigns == 0) {
    std::printf("COMPAT FAIL: the corpus must hold Live Sets, setlist libraries and ASSIGN maps (found %zu / %zu / %zu)\n", livesets, libraries, assigns);
    ++fail;
  }
  if (json_files > 0)
    std::printf("NOTE: %zu config .json file(s) are NOT opened here: the ui build's compat_config_test must run on the same tree (the top-level CMake requires it)\n", json_files);
  std::printf("compat_corpus_test: %zu files opened (%zu Live Sets, %zu libraries, %zu ASSIGN maps), %d failures\n", opened, livesets, libraries, assigns, fail);
  return fail ? 1 : 0;
}

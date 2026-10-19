// Regenerates the setlist compatibility corpus and the golden file (Story 4.6):
//   cmake --build build --target setlist_corpus_regen
// Writes tests/corpus/setlists/* and tests/golden/setlist_library_fixed.bin from tests/setlist_fixtures.hpp using this release's writer. Commit the
// result only when the change is intended: the corpus is what every future build must keep opening.
#include <cstdio>
#include <fstream>
#include <string>

#include "setlist_fixtures.hpp"

static bool put(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return f.good();
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: setlist_corpus_gen <corpus_dir> <golden_dir>\n");
    return 2;
  }
  int rc = 0;
  for (const auto& c : sfixtures::corpus()) {
    if (!put(std::string(argv[1]) + "/" + c.name, c.bytes)) {
      std::fprintf(stderr, "failed: %s\n", c.name);
      rc = 1;
    } else {
      std::printf("wrote %s (%zu bytes)\n", c.name, c.bytes.size());
    }
  }
  std::string g;
  if (velora::host::setlist::serialize(sfixtures::basic(), g) != velora::host::setlist::Status::kOk ||
      !put(std::string(argv[2]) + "/setlist_library_fixed.bin", g))
    rc = 1;
  else
    std::printf("wrote setlist_library_fixed.bin (%zu bytes)\n", g.size());
  return rc;
}

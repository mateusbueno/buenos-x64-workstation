// Regenerates the Live Set compatibility corpus and the golden file (Story 4.2):
//   cmake --build build --target live_set_corpus_regen
// Writes tests/corpus/live_sets/*.liveset and tests/golden/live_set_fixed.bin from tests/live_set_fixtures.hpp using
// this release's writer. Commit the result only when the change is intended: the corpus is what every future build
// must keep opening.
#include <cstdio>
#include <fstream>
#include <string>

#include "live_set_fixtures.hpp"

static bool put(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return f.good();
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: live_set_corpus_gen <corpus_dir> <golden_dir>\n");
    return 2;
  }
  namespace ls = velora::host::liveset;
  int rc = 0;
  for (const auto& c : fixtures::corpus()) {
    std::string bytes;
    if (ls::serialize(c.set, bytes) != ls::Status::kOk || !put(std::string(argv[1]) + "/" + c.name, bytes)) {
      std::fprintf(stderr, "failed: %s\n", c.name);
      rc = 1;
    } else {
      std::printf("wrote %s (%zu bytes)\n", c.name, bytes.size());
    }
  }
  std::string g;
  if (ls::serialize(fixtures::golden(), g) != ls::Status::kOk || !put(std::string(argv[2]) + "/live_set_fixed.bin", g)) rc = 1;
  else std::printf("wrote live_set_fixed.bin (%zu bytes)\n", g.size());
  return rc;
}

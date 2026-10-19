// SFZ -> instrument model mapping (parse only; the importer tests cover bundles and publishing).
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "test_util.hpp"
#include "velora/format/sfz_import.hpp"

namespace fs = std::filesystem;
using namespace velora::format;

namespace {
fs::path g_dir;
void put(const char* name, const std::string& text) {
  fs::create_directories((g_dir / name).parent_path());
  std::ofstream(g_dir / name, std::ios::binary) << text;
}
SfzParseResult parse(const std::string& sfz) {
  put("t.sfz", sfz);
  return parse_sfz_file((g_dir / "t.sfz").string());
}
bool has(const SfzParseResult& r, SfzIssueKind k, const std::string& needle) {
  for (const auto& i : r.issues)
    if (i.kind == k && i.message.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

int main() {
  g_dir = fs::temp_directory_path() / ("velora_sfz_test_" + std::to_string(::getpid()));
  fs::remove_all(g_dir);
  put("a.wav", "x");
  put("sub/b.wav", "x");

  {  // inheritance: a later header resets the levels below it
    auto r = parse("<global>\nvolume=-1 lovel=5\n<master>\npan=30\n<group>\nhikey=70\n"
                   "<region> sample=a.wav lokey=10\n<region> sample=a.wav\n"
                   "<group>\n<region> sample=a.wav\n<master>\n<region> sample=a.wav\n"
                   "<global>\n<region> sample=a.wav\n");
    CHECK(r.ok() && r.regions.size() == 5 && r.samples.size() == 1);
    if (r.regions.size() == 5) {
      CHECK(r.regions[0].volume_db == -1 && r.regions[0].pan == 30 && r.regions[0].key_hi == 70 && r.regions[0].key_lo == 10 && r.regions[0].vel_lo == 5);
      CHECK(r.regions[1].key_lo == 0 && r.regions[1].key_hi == 70);   // region level does not leak
      CHECK(r.regions[2].key_hi == 127 && r.regions[2].pan == 30);    // new group drops hikey
      CHECK(r.regions[3].pan == 0 && r.regions[3].volume_db == -1);   // new master drops pan
      CHECK(r.regions[4].volume_db == 0 && r.regions[4].vel_lo == 0); // new global drops everything
    }
    CHECK(r.source_region_count == 5 && r.region_sources[0].line == 7);
  }
  {  // key sets lo/hi/center; later explicit opcodes win; note names; default_path and subfolders
    auto r = parse("<control>\ndefault_path=sub\\\n<region> sample=b.wav key=c#4 pitch_keycenter=a3\n"
                   "<region> sample=b.wav pitch_keycenter=c4 key=d4\n<region> sample=b.wav lokey=c-1 hikey=g9 \n");
    CHECK(r.ok() && r.regions.size() == 3 && r.samples.size() == 1);
    if (r.regions.size() == 3) {
      CHECK(r.regions[0].key_lo == 61 && r.regions[0].key_hi == 61 && r.regions[0].key_center == 57);
      CHECK(r.regions[1].key_center == 62);
      CHECK(r.regions[2].key_lo == 0 && r.regions[2].key_hi == 127);
    }
    if (!r.samples.empty()) CHECK(r.samples[0].name == "sub/b.wav");
  }
  {  // #define and $variables, defaults of every field
    auto r = parse("#define $K 64\n#define $S a.wav\n<region> sample=$S key=$K\n");
    CHECK(r.ok() && r.regions.size() == 1);
    if (!r.regions.empty()) {
      const Region d;
      Region want = d; want.key_lo = want.key_hi = want.key_center = 64;
      CHECK(r.regions[0] == want);
    }
  }
  {  // unsupported things are all reported, none silently dropped
    auto r = parse("<region> sample=a.wav key=60 cutoff=1 ampeg_release_oncc4=2 amp_veltrack=50\n"
                   "<effect>\n<region> sample=*sine key=1\n<region> sample=a.wav key=2 trigger=first\n"
                   "<region> sample=a.wav key=3 loop_mode=loop_bidir\n");
    CHECK(!r.ok());
    CHECK(has(r, SfzIssueKind::kUnsupportedOpcode, "'cutoff'") && has(r, SfzIssueKind::kUnsupportedOpcode, "'ampeg_release_oncc4'") &&
          has(r, SfzIssueKind::kUnsupportedOpcode, "'amp_veltrack'"));
    CHECK(has(r, SfzIssueKind::kUnsupportedHeader, "<effect>"));
    CHECK(has(r, SfzIssueKind::kUnsupportedValue, "'first'") && has(r, SfzIssueKind::kUnsupportedValue, "'loop_bidir'"));
    CHECK(has(r, SfzIssueKind::kUnsupportedValue, "*sine"));
  }
  {  // invalid values and structure
    CHECK(has(parse("<region> sample=a.wav lovel=200\n"), SfzIssueKind::kInvalidValue, "'200'"));
    CHECK(has(parse("<region> sample=a.wav lokey=80 hikey=70\n"), SfzIssueKind::kInvalidValue, "inverted"));
    CHECK(has(parse("<region> sample=a.wav seq_length=2 seq_position=3\n"), SfzIssueKind::kInvalidValue, "inverted"));
    CHECK(has(parse("sample=a.wav\n"), SfzIssueKind::kParse, "outside any header"));
    CHECK(has(parse("<region> key=60\n"), SfzIssueKind::kMissingSampleOpcode, "without"));
    CHECK(has(parse("<region> sample=missing.wav\n"), SfzIssueKind::kMissingSample, "missing.wav"));
    CHECK(has(parse("<region> sample=a.wav pan=abc\n"), SfzIssueKind::kInvalidValue, "'abc'"));
    CHECK(!parse_sfz_file((g_dir / "nope.sfz").string()).ok());
  }
  fs::remove_all(g_dir);
  if (g_fail == 0) std::printf("sfz_test OK\n");
  return g_fail == 0 ? 0 : 1;
}

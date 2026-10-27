// SFZ -> instrument model mapping (parse only; the importer tests cover bundles and publishing).
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
  {  // Story 13.2: sfizz value rules (fallback to the opcode default, logged), hikey=-1, inert <curve>
    for (const char* v : {"lokey=-1", "lokey=c-2", "lokey=zz"}) {
      auto r = parse(std::string("<region> sample=a.wav ") + v + "\n");
      CHECK(r.ok() && r.regions.size() == 1 && r.regions[0].key_lo == 0 && r.regions[0].key_hi == 127);
      CHECK(r.ignored_values.size() == 1 && r.ignored_values[0].opcode == "lokey" && r.ignored_values[0].line == 1);
      CHECK(r.never_note_triggered.size() == 1 && !r.never_note_triggered[0]);
    }
    {
      auto r = parse("<group>\nlokey=10\n<region> sample=a.wav lokey=-1\n");  // the default wins over the inherited value
      CHECK(r.ok() && r.regions[0].key_lo == 0 && r.ignored_values.size() == 1);
    }
    {
      auto r = parse("<region> sample=a.wav hikey=128\n");
      CHECK(r.ok() && r.regions[0].key_hi == 127 && r.ignored_values.size() == 1 && !r.never_note_triggered[0]);
    }
    {
      auto r = parse("<region> sample=a.wav hikey=-1\n<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.regions.size() == 2 && r.never_note_triggered.size() == 2);
      CHECK(r.never_note_triggered[0] && !r.never_note_triggered[1] && r.ignored_values.size() == 1);
      CHECK(r.ignored_values[0].opcode == "hikey");
      CHECK(drop_never_note_triggered(r) == 1 && r.regions.size() == 1 && r.regions[0].key_lo == 60 &&
            r.loops.size() == 1 && r.region_sources.size() == 1 && r.region_sources[0].line == 2);
    }
    {  // a valid key sets hikey too, so it clears hikey=-1 (same region or inherited)
      auto r = parse("<region> sample=a.wav hikey=-1 key=60\n");
      CHECK(r.ok() && !r.never_note_triggered[0] && r.regions[0].key_hi == 60);
      auto g = parse("<group>\nhikey=-1\n<region> sample=a.wav key=60\n");
      CHECK(g.ok() && !g.never_note_triggered[0] && g.regions[0].key_hi == 60);
    }
    {  // curve points: index above 255 is ignored and logged
      auto r = parse("<curve>\ncurve_index=1 v000=0 v999=1\n<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.curves.size() == 1 && r.curves[0].points.size() == 1 && r.ignored_values.size() == 1 &&
            r.ignored_values[0].opcode == "v999");
    }
    {  // a later valid hikey clears the flag
      auto r = parse("<group>\nhikey=-1\n<region> sample=a.wav hikey=70\n");
      CHECK(r.ok() && !r.never_note_triggered[0] && r.regions[0].key_hi == 70);
    }
    {
      auto r = parse("<region> sample=a.wav key=-1\n");
      CHECK(r.ok() && r.regions[0].key_lo == 0 && r.regions[0].key_hi == 127 && r.regions[0].key_center == 60 &&
            r.ignored_values.size() == 1 && r.ignored_values[0].opcode == "key");
    }
    {  // regular regions: nothing ignored
      auto r = parse("<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.ignored_values.empty() && r.curves.empty() && !r.never_note_triggered[0]);
    }
    {
      auto r = parse("<curve>\ncurve_index=1 v000=0 v127=1\n<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.curves.size() == 1 && r.curves[0].index == 1 && r.curves[0].points.size() == 2 &&
            r.curves[0].points[1].first == 127 && r.curves[0].points[1].second == 1.0 && r.regions.size() == 1);
    }
    {  // a named curve used through an unsupported opcode still skips the file, naming it (amp_velcurve_N itself is Story 13.4 and needs no <curve>)
      auto r = parse("<curve>\ncurve_index=1 v000=0 v127=1\n<region> sample=a.wav key=60 amp_veltrack_curvecc1=1\n");
      CHECK(!r.ok() && has(r, SfzIssueKind::kUnsupportedOpcode, "'amp_veltrack_curvecc1'"));
    }
    {
      auto r = parse("<group>\nampeg_dynamic=1\n<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.allowlisted.size() == 1 && r.regions.size() == 1);
    }
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
    auto r = parse("<region> sample=a.wav key=60 lfo01_freq=1 ampeg_release_oncc4=2 amp_veltrack_oncc7=50\n"
                   "<effect>\n<region> sample=*sine key=1\n<region> sample=a.wav key=2 trigger=bogus\n"
                   "<region> sample=a.wav key=3 loop_mode=loop_bidir\n");
    CHECK(!r.ok());
    CHECK(has(r, SfzIssueKind::kUnsupportedOpcode, "'lfo01_freq'") && has(r, SfzIssueKind::kUnsupportedOpcode, "'ampeg_release_oncc4'") &&
          has(r, SfzIssueKind::kUnsupportedOpcode, "'amp_veltrack_oncc7'"));
    CHECK(has(r, SfzIssueKind::kUnsupportedHeader, "<effect>"));
    CHECK(has(r, SfzIssueKind::kUnsupportedValue, "'bogus'") && has(r, SfzIssueKind::kUnsupportedValue, "'loop_bidir'"));
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

  // ---- Story 5.1 ----------------------------------------------------------------------------
  {  // keyswitches: every opcode, note names, inheritance from outer levels
    auto r = parse("<global>\nsw_default=d1\n<group>\nsw_lokey=c1 sw_hikey=e1\n"
                   "<region> sample=a.wav key=60 sw_last=d1 sw_down=36 sw_up=37 sw_previous=38\n"
                   "<region> sample=a.wav key=61\n");
    CHECK(r.ok() && r.regions.size() == 2);
    if (r.regions.size() == 2) {
      const Keyswitch& k = r.regions[0].sw;
      CHECK(k.lokey == 24 && k.hikey == 28 && k.last == 26 && k.down == 36 && k.up == 37 &&
            k.previous == 38 && k.default_key == 26);
      CHECK(r.regions[1].sw.last == -1 && r.regions[1].sw.lokey == 24 && r.regions[1].sw.default_key == 26);
      CHECK(!Keyswitch{}.any() && k.any());
    }
    CHECK(has(parse("<region> sample=a.wav sw_last=200\n"), SfzIssueKind::kInvalidValue, "'200'"));
    CHECK(has(parse("<region> sample=a.wav sw_lokey=40 sw_hikey=30\n"), SfzIssueKind::kInvalidValue, "sw_lokey > sw_hikey"));
    CHECK(has(parse("<region> sample=a.wav sw_lolast=30\n"), SfzIssueKind::kUnsupportedOpcode, "'sw_lolast'"));
  }
  {  // CC crossfades: several controllers, sorted by cc, defaults, curve
    auto r = parse("<region> sample=a.wav key=60 xfin_locc11=10 xfin_hicc11=20 xfin_locc1=0 xfin_hicc1=64 "
                   "xfout_locc74=64 xfout_hicc74=127 xf_cccurve=gain\n"
                   "<region> sample=a.wav key=61 xfin_hicc7=100\n"
                   "<region> sample=a.wav key=62\n");
    CHECK(r.ok() && r.regions.size() == 3);
    if (r.regions.size() == 3) {
      const Crossfade& x = r.regions[0].xf;
      CHECK(x.in_count == 2 && x.in[0] == (CcRange{1, 0, 64}) && x.in[1] == (CcRange{11, 10, 20}));
      CHECK(x.out_count == 1 && x.out[0] == (CcRange{74, 64, 127}) && x.curve == XfCurve::kGain);
      CHECK(x.in[2] == CcRange{} && x.out[1] == CcRange{});
      CHECK(r.regions[1].xf.in_count == 1 && r.regions[1].xf.in[0] == (CcRange{7, 0, 100}) &&
            r.regions[1].xf.curve == XfCurve::kPower);
      CHECK(r.regions[2].xf == Crossfade{} && !has_extension(r.regions[2]));
      CHECK(has_extension(r.regions[0]) && has_extension(r.regions[1]));
    }
    CHECK(parse("<region> sample=a.wav xf_cccurve=power\n").regions[0].xf == Crossfade{});
    CHECK(has_extension(parse("<region> sample=a.wav xf_cccurve=gain\n").regions[0]));
    // Bound: four controllers per direction are fine, a fifth is an unsupported value.
    auto four = parse("<region> sample=a.wav xfin_hicc1=1 xfin_hicc2=1 xfin_hicc3=1 xfin_hicc4=1 "
                      "xfout_hicc1=1 xfout_hicc2=1 xfout_hicc3=1 xfout_hicc4=1\n");
    CHECK(four.ok() && four.regions[0].xf.in_count == 4 && four.regions[0].xf.out_count == 4);
    CHECK(has(parse("<region> sample=a.wav xfin_hicc1=1 xfin_hicc2=1 xfin_hicc3=1 xfin_hicc4=1 xfin_hicc5=1\n"),
              SfzIssueKind::kUnsupportedValue, "more than 4 xfin"));
    CHECK(has(parse("<region> sample=a.wav xfout_locc30=1 xfout_locc31=1 xfout_locc32=1 xfout_locc33=1 xfout_locc34=1\n"),
              SfzIssueKind::kUnsupportedValue, "more than 4 xfout"));
    CHECK(has(parse("<region> sample=a.wav xfin_locc9=50\n"), SfzIssueKind::kInvalidValue, "xfin_locc9 > xfin_hicc9"));
    CHECK(has(parse("<region> sample=a.wav xfin_locc200=1\n"), SfzIssueKind::kUnsupportedValue, "'xfin_locc200'"));
    CHECK(has(parse("<region> sample=a.wav xfin_locc1=200\n"), SfzIssueKind::kInvalidValue, "'200'"));
    CHECK(has(parse("<region> sample=a.wav xf_cccurve=sine\n"), SfzIssueKind::kUnsupportedValue, "'sine'"));
    CHECK(has(parse("<region> sample=a.wav xfin_locc=1\n"), SfzIssueKind::kUnsupportedOpcode, "'xfin_locc'"));
    CHECK(has(parse("<region> sample=a.wav xfin_lovel=1\n"), SfzIssueKind::kUnsupportedOpcode, "'xfin_lovel'"));
  }
  {  // filters
    struct Case { const char* type; FilterType want; } cases[] = {
        {"lpf_1p", FilterType::kLpf1p}, {"lpf_2p", FilterType::kLpf2p}, {"hpf_1p", FilterType::kHpf1p},
        {"hpf_2p", FilterType::kHpf2p}, {"bpf_2p", FilterType::kBpf2p}};
    for (const auto& c : cases) {
      auto r = parse(std::string("<region> sample=a.wav fil_type=") + c.type + " cutoff=1200.5 resonance=3.5\n");
      CHECK(r.ok() && r.regions.size() == 1);
      if (r.regions.size() == 1)
        CHECK(r.regions[0].filter == (Filter{c.want, 1200.5f, 3.5f}));
    }
    auto d = parse("<region> sample=a.wav cutoff=800\n<region> sample=a.wav fil_type=hpf_2p\n"
                   "<region> sample=a.wav resonance=6\n");
    CHECK(d.ok() && d.regions.size() == 3);
    if (d.regions.size() == 3) {
      CHECK(d.regions[0].filter == (Filter{FilterType::kLpf2p, 800.f, 0.f}));  // fil_type defaults to lpf_2p
      CHECK(d.regions[1].filter == Filter{} && d.regions[2].filter == Filter{});  // inert without cutoff
      CHECK(!has_extension(d.regions[1]) && has_extension(d.regions[0]));
    }
    CHECK(has(parse("<region> sample=a.wav fil_type=lpf_4p cutoff=100\n"), SfzIssueKind::kUnsupportedValue, "'lpf_4p'"));
    CHECK(has(parse("<region> sample=a.wav cutoff=-5\n"), SfzIssueKind::kInvalidValue, "'-5'"));
    CHECK(has(parse("<region> sample=a.wav cutoff=100 resonance=99\n"), SfzIssueKind::kInvalidValue, "'99'"));
    CHECK(has(parse("<region> sample=a.wav cutoff_oncc1=100\n"), SfzIssueKind::kUnsupportedOpcode, "'cutoff_oncc1'"));
    CHECK(has(parse("<region> sample=a.wav fil_veltrack=100\n"), SfzIssueKind::kUnsupportedOpcode, "'fil_veltrack'"));
  }
  {  // sound-affecting opcodes that have no effect are reported as warnings, once per opcode line
    auto r = parse("<group>\nfil_type=lpf_1p\n<region> sample=a.wav key=1 resonance=3\n<region> sample=a.wav key=2\n"
                   "<region> sample=a.wav key=3 cutoff=100 resonance=2\n");
    CHECK(r.ok() && r.warnings.size() == 2);
    if (r.warnings.size() == 2) {
      CHECK(r.warnings[0].opcode == "fil_type" && r.warnings[0].line == 2 && !r.warnings[0].file.empty());
      CHECK(r.warnings[1].opcode == "resonance" && r.warnings[1].line == 3);
    }
    CHECK(parse("<region> sample=a.wav cutoff=1 fil_type=lpf_2p resonance=1\n").warnings.empty());
    // One controller must have one spelling.
    CHECK(has(parse("<region> sample=a.wav xfin_locc007=1\n"), SfzIssueKind::kInvalidValue, "non-canonical controller number"));
    CHECK(has(parse("<region> sample=a.wav xfout_hicc00=1\n"), SfzIssueKind::kInvalidValue, "write 'xfout_hicc0'"));
    CHECK(parse("<region> sample=a.wav xfin_hicc0=1 xfin_hicc7=2 xfin_hicc127=3\n").ok());
  }
  {  // loops: all four modes
    const char* modes[] = {"no_loop", "one_shot", "loop_continuous", "loop_sustain"};
    for (unsigned i = 0; i < 4; ++i) {
      auto r = parse(std::string("<region> sample=a.wav loop_mode=") + modes[i] + " loop_start=3 loop_end=9\n");
      CHECK(r.ok() && r.regions[0].loop_mode == static_cast<LoopMode>(i) && r.regions[0].loop_start == 3 &&
            r.regions[0].loop_end == 9 && r.regions[0].has_loop_points);
    }
    CHECK(has(parse("<region> sample=a.wav loop_mode=loop_bidir\n"), SfzIssueKind::kUnsupportedValue, "'loop_bidir'"));
    CHECK(has(parse("<region> sample=a.wav loop_crossfade=0.1\n"), SfzIssueKind::kUnsupportedOpcode, "'loop_crossfade'"));
  }
  {  // allow-listed opcodes are accepted, counted and never stored; sounding neighbours are unaffected
    auto r = parse("<control>\nlabel_cc1=Mod hint_ram_based=1 label_key36=KS image=pic.png\n"
                   "<global>\nglobal_label=g hint_x=1\n<master>\nmaster_label=m\n<group>\ngroup_label=g sw_label=KS\n"
                   "<region> sample=a.wav key=60 region_label=r label_cc74=Cut\n");
    CHECK(r.ok() && r.regions.size() == 1 && r.regions[0] == [] { Region d; d.key_lo = d.key_hi = d.key_center = 60; return d; }());
    std::map<std::string, int> seen;
    for (const auto& a : r.allowlisted) {
      ++seen[a.opcode];
      CHECK(!a.file.empty() && a.line >= 1);
    }
    CHECK(r.allowlisted.size() == 11 && seen["label_cc1"] == 1 && seen["hint_ram_based"] == 1 &&
          seen["image"] == 1 && seen["sw_label"] == 1 && seen["label_cc74"] == 1);
    CHECK(r.allowlisted[0].line == 2);  // file:line of the first one
    // An allow-listed opcode outside any header is still a structural error.
    CHECK(has(parse("label_cc1=x\n<region> sample=a.wav\n"), SfzIssueKind::kParse, "outside any header"));
  }
  {  // Story 5.4: ampeg_dynamic (every VSCO2 file) is accepted and never stored; the opcodes that would make it matter still reject the file
    auto r = parse("<group>\nampeg_dynamic=1 ampeg_release=0.8\n<region> sample=a.wav key=60\n");
    CHECK(r.ok() && r.regions.size() == 1 && r.regions[0].ampeg.release == 0.8f);
    CHECK(r.allowlisted.size() == 1 && r.allowlisted[0].opcode == "ampeg_dynamic" && r.allowlisted[0].line == 2);
    CHECK(classify_opcode("ampeg_dynamic") == OpcodeClass::kAllowListed);
    CHECK(has(parse("<group>\nampeg_dynamic=1 ampeg_release_oncc72=2\n<region> sample=a.wav key=60\n"), SfzIssueKind::kUnsupportedOpcode, "'ampeg_release_oncc72'"));
    CHECK(parse("<region> sample=a.wav key=60 ampeg_vel2release=1\n").ok());  // Story 13.4: supported, and it does not make ampeg_dynamic matter
    CHECK(has(parse("<region> sample=a.wav key=60 ampeg_vel2hold=1\n"), SfzIssueKind::kUnsupportedOpcode, "'ampeg_vel2hold'"));
  }
  {  // Story 13.3: end and the trigger modes
    auto r = parse("<region> sample=a.wav key=60 end=100\n<region> sample=a.wav key=61 end=0\n<region> sample=a.wav key=62 end=-1\n"
                   "<region> sample=a.wav key=63\n<region> sample=a.wav key=64 trigger=first\n<region> sample=a.wav key=65 trigger=legato\n"
                   "<region> sample=a.wav key=66 trigger=release_key\n<region> sample=a.wav key=67 trigger=release\n"
                   "<region> sample=a.wav key=68 trigger=attack end=44100\n");
    CHECK(r.ok() && r.regions.size() == 9);
    if (r.regions.size() == 9) {
      CHECK(r.regions[0].has_end && r.regions[0].end == 100);
      CHECK(r.regions[1].has_end && r.regions[1].end == 0);  // end=0 disables the region
      CHECK(r.regions[2].has_end && r.regions[2].end == 0);  // below 0 too
      CHECK(!r.regions[3].has_end && r.regions[3].end == 0);
      CHECK(r.regions[4].trigger == TriggerMode::kFirst && r.regions[5].trigger == TriggerMode::kLegato);
      CHECK(r.regions[6].trigger == TriggerMode::kReleaseKey && r.regions[7].trigger == TriggerMode::kRelease);
      CHECK(r.regions[8].trigger == TriggerMode::kAttack && r.regions[8].end == 44100);
      CHECK( !has_trigger_extension(r.regions[3]) && has_trigger_extension(r.regions[0]) &&
            has_trigger_extension(r.regions[4]) && !has_trigger_extension(r.regions[7]));
    }
    CHECK(classify_opcode("end") == OpcodeClass::kSupported);
    CHECK(!parse("<region> sample=a.wav key=60 end=abc\n").ok());
    CHECK(has(parse("<region> sample=a.wav key=60 end=abc\n"), SfzIssueKind::kInvalidValue, "'end'"));
    CHECK(has(parse("<region> sample=a.wav key=60 trigger=bogus\n"), SfzIssueKind::kUnsupportedValue, "'bogus'"));
    // end is inherited like any region opcode.
    auto g = parse("<group> end=50\n<region> sample=a.wav key=60\n<region> sample=a.wav key=61 end=70\n");
    CHECK(g.ok() && g.regions.size() == 2 && g.regions[0].end == 50 && g.regions[1].end == 70);
  }
  {  // Story 13.4: the velocity and amplitude model
    auto r = parse("<region> sample=a.wav key=60\n"
                   "<region> sample=a.wav key=61 amp_veltrack=40 amplitude=50 amp_keytrack=0.5 amp_keycenter=c4\n"
                   "<region> sample=a.wav key=62 amp_velcurve_127=0.9 amp_velcurve_1=0.1 amp_velcurve_64=0.6\n"
                   "<region> sample=a.wav key=63 ampeg_vel2attack=0.1 ampeg_vel2decay=-0.2 ampeg_vel2sustain=-30 ampeg_vel2release=2\n"
                   "<master>master_volume=-4 master_amplitude=50\n<group>group_volume=1.5 group_amplitude=50\n"
                   "<region> sample=a.wav key=64 volume=-2 amplitude=80\n<region> sample=a.wav key=65\n"
                   "<global>global_volume=2\n<master>\n<group>\n<region> sample=a.wav key=66\n"
                   "<region> sample=a.wav key=67 amp_velcurve_10=0.2\n");
    CHECK(r.ok() && r.regions.size() == 8 && r.ignored_values.empty());
    if (r.regions.size() == 8) {
      CHECK(!has_amp_extension(r.regions[0]) && r.regions[0].amp == AmpMap{});  // plain regions keep the default map (no record, same bytes)
      const AmpMap& a = r.regions[1].amp;
      CHECK(a.veltrack == 40.f && a.amplitude == 50.f && a.keytrack == 0.5f && a.keycenter == 60 && a.curve_count == 0 && has_amp_extension(r.regions[1]));
      const AmpMap& c = r.regions[2].amp;
      CHECK(c.curve_count == 3 && c.curve[0].vel == 1 && c.curve[0].gain == 0.1f && c.curve[1].vel == 64 && c.curve[2].vel == 127 && c.curve[2].gain == 0.9f);
      CHECK(c.veltrack == 100.f && c.amplitude == 100.f);
      const AmpMap& v = r.regions[3].amp;
      CHECK(v.vel2attack == 0.1f && v.vel2decay == -0.2f && v.vel2sustain == -30.f && v.vel2release == 2.f);
      // master_volume + group_volume add to volume; the *_amplitude opcodes multiply amplitude.
      CHECK(std::fabs(r.regions[4].volume_db - (-2.f - 4.f + 1.5f)) < 1e-6f && std::fabs(r.regions[4].amp.amplitude - 80.f * 0.5f * 0.5f) < 1e-4f);
      CHECK(std::fabs(r.regions[5].volume_db - (-4.f + 1.5f)) < 1e-6f && std::fabs(r.regions[5].amp.amplitude - 25.f) < 1e-4f);
      CHECK(r.regions[6].volume_db == 2.f && r.regions[6].amp == AmpMap{});  // a later header drops the lower levels, global_volume stays
      CHECK(r.regions[7].amp.curve_count == 1 && r.regions[7].amp.curve[0].vel == 10);
      for (const auto& rg : r.regions) CHECK(validate_region(rg, 100) == Error::kNone);
    }
    // inheritance: curve points of a group and of the region merge, the region's point with the same N wins
    auto g = parse("<group>amp_velcurve_20=0.2 amp_velcurve_40=0.4\n<region> sample=a.wav key=60 amp_velcurve_40=0.5 amp_velcurve_5=0.1\n");
    CHECK(g.ok() && g.regions[0].amp.curve_count == 3 && g.regions[0].amp.curve[0].vel == 5 && g.regions[0].amp.curve[2].vel == 40 && g.regions[0].amp.curve[2].gain == 0.5f);
    // out-of-range values are clamped and recorded; N above 127 is dropped (sfizz); a non-number is invalid; the volume sum is clamped
    auto o = parse("<region> sample=a.wav key=60 amp_velcurve_64=1.5 amp_veltrack=250 amp_keytrack=20 ampeg_vel2attack=500 amp_velcurve_200=1\n"
                   "<region> sample=a.wav key=61 volume=40 group_volume=30 amp_keycenter=200\n");
    CHECK(o.ok() && o.regions.size() == 2 && o.ignored_values.size() == 6);
    if (o.regions.size() == 2) {
      CHECK(o.regions[0].amp.curve_count == 1 && o.regions[0].amp.curve[0].gain == 1.f && o.regions[0].amp.veltrack == 100.f);
      CHECK(o.regions[0].amp.keytrack == 12.f && o.regions[0].amp.vel2attack == 100.f);
      CHECK(o.regions[1].volume_db == 48.f && o.regions[1].amp.keycenter == 60);
    }
    CHECK(has(parse("<region> sample=a.wav key=60 amp_veltrack=loud\n"), SfzIssueKind::kInvalidValue, "'amp_veltrack'"));
    {  // more than kMaxVelPoints distinct points is unsupported, naming the opcode
      std::string many = "<region> sample=a.wav key=60";
      for (int i = 0; i <= int(kMaxVelPoints); ++i) many += " amp_velcurve_" + std::to_string(i * 3) + "=0.5";
      auto m = parse(many + "\n");
      CHECK(!m.ok() && has(m, SfzIssueKind::kUnsupportedValue, "amp_velcurve_<N>"));
      many = "<region> sample=a.wav key=60";
      for (int i = 0; i < int(kMaxVelPoints); ++i) many += " amp_velcurve_" + std::to_string(i * 3) + "=0.5";
      CHECK(parse(many + "\n").ok());
    }
    // non-canonical velocity numbers are invalid (two spellings of one point); an unparsable amp_keycenter keeps the inherited value; -0 is stored as +0
    CHECK(has(parse("<region> sample=a.wav key=60 amp_velcurve_064=0.5\n"), SfzIssueKind::kInvalidValue, "non-canonical velocity number"));
    CHECK(parse("<region> sample=a.wav key=60 amp_velcurve_0=0.5\n").ok());
    {
      auto k = parse("<group>amp_keycenter=40\n<region> sample=a.wav key=60 amp_keycenter=bogus\n<region> sample=a.wav key=61 amp_keycenter=c5 amp_keytrack=-0\n");
      CHECK(k.ok() && k.regions.size() == 2 && k.regions[0].amp.keycenter == 40 && k.ignored_values.size() == 1 && k.regions[1].amp.keycenter == 72);
      CHECK(k.regions.size() == 2 && !std::signbit(k.regions[1].amp.keytrack) && has_amp_extension(k.regions[1]));
    }
    for (const char* op : {"amp_veltrack", "amp_velcurve_<N>", "amplitude", "global_amplitude", "master_amplitude", "group_amplitude", "global_volume",
                           "master_volume", "group_volume", "amp_keytrack", "amp_keycenter", "ampeg_vel2attack", "ampeg_vel2decay", "ampeg_vel2sustain", "ampeg_vel2release"})
      CHECK(classify_opcode(std::string(op) == "amp_velcurve_<N>" ? "amp_velcurve_64" : op) == OpcodeClass::kSupported);
    // determinism: the same text imports to the same regions
    CHECK(parse("<region> sample=a.wav key=60 amp_velcurve_30=0.3 amp_velcurve_90=0.8\n").regions == parse("<region> sample=a.wav key=60 amp_velcurve_90=0.8 amp_velcurve_30=0.3\n").regions);
  }
  {  // opcodes outside the subset are listed with file:line and the opcode name; none are allow-listed
    const char* bad[] = {"script", "set_cc1", "set_hdcc1", "lfo01_freq", "eg01_time1", "amplitude_oncc1",
                         "effect1", "output", "group", "off_by", "polyphony", "note_offset", "octave_offset",
                         "amp_veltrack_oncc7", "sample_quality", "delay", "rt_decay", "amp_random", "ampeg_vel2delay", "amplitude_curvecc3", "xfin_lovel", "cutoff2", "fil2_type", "pitch_keytrack"};
    for (const char* op : bad) {
      auto r = parse(std::string("<region> sample=a.wav key=60 ") + op + "=1\n");
      bool found = false;
      for (const auto& i : r.issues)
        if (i.kind == SfzIssueKind::kUnsupportedOpcode && i.opcode == op && i.line == 1) found = true;
      CHECK(found);
      CHECK(r.allowlisted.empty());
      CHECK(classify_opcode(op) == OpcodeClass::kUnsupported);
    }
    for (const char* h : {"effect", "midi", "sample"}) {
      auto r = parse(std::string("<") + h + ">\nfoo=1\n<region> sample=a.wav\n");
      bool found = false;
      for (const auto& i : r.issues)
        if (i.kind == SfzIssueKind::kUnsupportedHeader && i.opcode == std::string("<") + h + ">") found = true;
      CHECK(found);
    }
    // Scripting in <control>: script is not allow-listed there either.
    auto c = parse("<control>\nscript=foo.lua\n<region> sample=a.wav\n");
    CHECK(has(c, SfzIssueKind::kUnsupportedOpcode, "'script'"));
    // Reason classes: only the three unsupported kinds are "unsupported features".
    CHECK(is_unsupported_feature(SfzIssueKind::kUnsupportedOpcode) && is_unsupported_feature(SfzIssueKind::kUnsupportedHeader) &&
          is_unsupported_feature(SfzIssueKind::kUnsupportedValue));
    for (SfzIssueKind k : {SfzIssueKind::kInvalidValue, SfzIssueKind::kParse, SfzIssueKind::kMissingSample,
                           SfzIssueKind::kMissingSampleOpcode})
      CHECK(!is_unsupported_feature(k));
  }
  {  // the opcode table classifies concrete names the way the patterns say
    const auto& t = sfz_opcode_table();
    std::set<std::string> patterns;
    for (const auto& e : t) {
      CHECK(patterns.insert(e.pattern).second);  // no duplicates
      CHECK(e.cls != OpcodeClass::kUnsupported);
      std::string name = e.pattern;
      if (name.size() > 3 && name.compare(name.size() - 3, 3, "<N>") == 0) name = name.substr(0, name.size() - 3) + "7";
      else if (name.back() == '*') name = name.substr(0, name.size() - 1) + "x";
      CHECK(classify_opcode(name) == e.cls);
    }
    CHECK(classify_opcode("xfin_locc") == OpcodeClass::kUnsupported);
    CHECK(classify_opcode("xfin_locc1234") == OpcodeClass::kUnsupported);
    CHECK(classify_opcode("xfin_locc1a") == OpcodeClass::kUnsupported);
    CHECK(classify_opcode("label_cc") == OpcodeClass::kUnsupported);
    CHECK(classify_opcode("hint_") == OpcodeClass::kUnsupported);
    CHECK(classify_opcode("cutoff2") == OpcodeClass::kUnsupported);
    // The README carries exactly this table (one authoritative list, checked here).
    std::ifstream in(FORMATLIB_README);
    CHECK(in.good());
    std::string line;
    bool inside = false;
    std::set<std::string> doc;
    while (std::getline(in, line)) {
      if (line.find("<!-- sfz-opcode-table:begin -->") != std::string::npos) { inside = true; continue; }
      if (line.find("<!-- sfz-opcode-table:end -->") != std::string::npos) break;
      if (!inside || line.rfind("| `", 0) != 0) continue;
      const auto e1 = line.find('`', 3);
      const auto c1 = line.find("| ", e1);
      const auto c2 = line.find(' ', c1 + 2);
      doc.insert(line.substr(3, e1 - 3) + " " + line.substr(c1 + 2, c2 - c1 - 2));
    }
    std::set<std::string> impl;
    for (const auto& e : t) impl.insert(std::string(e.pattern) + (e.cls == OpcodeClass::kSupported ? " supported" : " allow-listed"));
    if (doc != impl) {
      for (const auto& x : impl) if (!doc.count(x)) std::printf("README table lacks: %s\n", x.c_str());
      for (const auto& x : doc) if (!impl.count(x)) std::printf("README table has extra: %s\n", x.c_str());
    }
    CHECK(doc == impl);
  }
  fs::remove_all(g_dir);
  if (g_fail == 0) std::printf("sfz_test OK\n");
  return g_fail == 0 ? 0 : 1;
}

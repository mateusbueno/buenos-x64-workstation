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
      CHECK(r.ok() && r.allowlisted.empty() && r.regions.size() == 1);  // Story 13.5: supported (not inert-by-allow-list) and without effect alone
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
    auto r = parse("<region> sample=a.wav key=60 lfo01_freq=1 ampeg_release_smoothcc4=2 amp_veltrack_oncc7=50\n"
                   "<effect>\n<region> sample=*sine key=1\n<region> sample=a.wav key=2 trigger=bogus\n"
                   "<region> sample=a.wav key=3 loop_mode=loop_bidir\n");
    CHECK(!r.ok());
    CHECK(has(r, SfzIssueKind::kUnsupportedOpcode, "'lfo01_freq'") && has(r, SfzIssueKind::kUnsupportedOpcode, "'ampeg_release_smoothcc4'") &&
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
  {  // Story 5.4 / 13.5: ampeg_dynamic (every VSCO2 file) is accepted and never stored; with ampeg_*_oncc it is rejected (the envelope would have to follow the CC)
    auto r = parse("<group>\nampeg_dynamic=1 ampeg_release=0.8\n<region> sample=a.wav key=60\n");
    CHECK(r.ok() && r.regions.size() == 1 && r.regions[0].ampeg.release == 0.8f && r.allowlisted.empty());
    CHECK(classify_opcode("ampeg_dynamic") == OpcodeClass::kSupported);
    CHECK(parse("<region> sample=a.wav key=60 ampeg_vel2release=1\n").ok());  // Story 13.4: supported, and it does not make ampeg_dynamic matter
    CHECK(has(parse("<region> sample=a.wav key=60 ampeg_vel2hold=1\n"), SfzIssueKind::kUnsupportedOpcode, "'ampeg_vel2hold'"));
    // Story 13.5: no longer inert once an envelope _oncc exists, at any level and in any order
    for (const char* text : {"<group>\nampeg_dynamic=1 ampeg_release_oncc72=2\n<region> sample=a.wav key=60\n",
                             "<region> sample=a.wav key=60 ampeg_release_oncc72=2 ampeg_dynamic=1\n",
                             "<region> sample=a.wav key=60 ampeg_dynamic=1 ampeg_attackcc4=0.5\n",
                             "<global>ampeg_dynamic=1\n<group>ampeg_decay_oncc3=1\n<region> sample=a.wav key=60\n"}) {
      auto d = parse(text);
      CHECK(!d.ok() && has(d, SfzIssueKind::kUnsupportedValue, "ampeg_dynamic=1 with ampeg_*_oncc"));
      bool named = false;
      for (const auto& i : d.issues) named = named || (i.opcode == "ampeg_dynamic" && is_unsupported_feature(i.kind));
      CHECK(named);
    }
    CHECK(parse("<region> sample=a.wav key=60 ampeg_dynamic=0 ampeg_release_oncc72=2\n").ok());
    for (const char* on : {"true", "on", "1", "2"})
      CHECK(!parse(std::string("<region> sample=a.wav key=60 ampeg_dynamic=") + on + " ampeg_release_oncc72=2\n").ok());
    {
      auto bad = parse("<region> sample=a.wav key=60 ampeg_dynamic=maybe ampeg_release_oncc72=2\n");
      CHECK(bad.ok() && bad.ignored_values.size() == 1);  // unparseable: logged, flag off
    }
    CHECK(parse("<region> sample=a.wav key=60 ampeg_dynamic=1 amplitude_oncc72=2 pan_oncc1=3\n").ok());  // a non-envelope entry does not need the continuous form
    CHECK(parse("<group>ampeg_dynamic=1\n<region> sample=a.wav key=60 ampeg_dynamic=0 ampeg_release_oncc72=2\n").ok());
  }
  {  // Story 13.5: CC gates (locc/hicc), CC defaults (set_cc) and CC modulation (*_oncc, *_curvecc) read at voice start
    const std::string R = "<region> sample=a.wav key=60 ";
    {  // gates: CC units, hicc below 127 covers up to hicc + 1 (exclusive), a missing end is the sfizz default, a gate covering everything is no gate
      auto r = parse(R + "locc64=64 hicc64=100 locc70=10 hicc70=10 lohdcc1=0.5 hihdcc1=0.75 locc2=0 hicc2=127 locc80=5 hicc80=127\n");
      CHECK(r.ok() && r.regions.size() == 1);
      if (r.regions.size() == 1) {
        const CcMap& m = r.regions[0].cc;
        CHECK(m.gate_count == 4 && m.mod_count == 0);
        CHECK(m.gates[0].cc == 1 && m.gates[0].lo == 63.5f && m.gates[0].hi == 95.25f);
        CHECK(m.gates[1].cc == 64 && m.gates[1].lo == 64.f && m.gates[1].hi == std::nextafter(101.f, 0.f));
        CHECK(m.gates[2].cc == 70 && m.gates[2].lo == 10.f && m.gates[2].hi == std::nextafter(11.f, 0.f));
        CHECK(m.gates[3].cc == 80 && m.gates[3].lo == 5.f && m.gates[3].hi == 127.f);
        CHECK(has_cc_extension(r.regions[0]) && validate_region(r.regions[0], 1000) == Error::kNone);
      }
      auto lo_only = parse(R + "locc7=20\n");
      CHECK(lo_only.ok() && lo_only.regions[0].cc.gate_count == 1 && lo_only.regions[0].cc.gates[0].lo == 20.f && lo_only.regions[0].cc.gates[0].hi == 127.f);
      auto hi_only = parse(R + "hicc7=20\n");
      CHECK(hi_only.ok() && hi_only.regions[0].cc.gates[0].lo == 0.f && hi_only.regions[0].cc.gates[0].hi == std::nextafter(21.f, 0.f));
      CHECK(parse(R + "lorealcc9=0.25 hirealcc9=0.5\n").regions[0].cc.gates[0] == (CcGate{9, 31.75f, 63.5f}));  // SFZ 1 aliases
      CHECK(!has_cc_extension(parse(R + "key=61\n").regions[0]) && parse(R + "key=61\n").regions[0] == parse(R + "key=61 locc5=0\n").regions[0]);
      // inheritance: a group's gate holds for its regions, the region's own value wins
      auto g = parse("<group> locc64=64\n<region> sample=a.wav key=60\n<region> sample=a.wav key=61 locc64=10\n");
      CHECK(g.ok() && g.regions[0].cc.gates[0].lo == 64.f && g.regions[1].cc.gates[0].lo == 10.f);
      // values: out of range clamped and logged, a note name or text is invalid
      auto c = parse(R + "locc7=200\n");
      CHECK(c.ok() && c.regions[0].cc.gates[0].lo == 127.f && c.ignored_values.size() == 1);
      CHECK(has(parse(R + "locc7=abc\n"), SfzIssueKind::kInvalidValue, "'abc'"));
      // controllers above 127 are not gates (sfizz never refreshes them after load); leading zeros are one controller spelled two ways
      for (const char* op : {"locc131=1", "hicc140=1", "lohdcc200=0.5"}) {
        auto b = parse(R + op + "\n");
        bool named = false;
        for (const auto& i : b.issues) named = named || (i.kind == SfzIssueKind::kUnsupportedValue && i.opcode == std::string(op).substr(0, std::string(op).find('=')));
        CHECK(!b.ok() && named);
      }
      CHECK(has(parse(R + "locc064=1\n"), SfzIssueKind::kInvalidValue, "non-canonical controller number"));
      CHECK(has(parse(R + "locc70=50 hicc70=40\n"), SfzIssueKind::kInvalidValue, "inverted CC gate"));  // an inverted gate never plays: an error, not a silent region
      CHECK(parse(R + "locc70=40 hicc70=40\n").ok());
      std::string many = R;
      for (int i = 0; i <= int(kMaxCcGates); ++i) many += " locc" + std::to_string(i + 1) + "=5";
      CHECK(has(parse(many + "\n"), SfzIssueKind::kUnsupportedValue, "CC gates"));
      many = R;
      for (int i = 0; i < int(kMaxCcGates); ++i) many += " locc" + std::to_string(i + 1) + "=5";
      CHECK(parse(many + "\n").ok());
    }
    {  // modulation entries: forms, aliases, ordering, depth ranges, depth 0
      auto r = parse(R + "tune_oncc94=-100 amplitude_cc93=50 amplitude_curvecc93=7 pan_oncc1=10 volume_oncc2=-3 gain_cc2=-4 pitch_oncc95=5 offset_oncc73=4000 "
                         "ampeg_release_oncc100=2 ampeg_attackcc64=0.1 ampeg_start_cc3=10 ampeg_sustain_oncc4=-20 ampeg_delay_oncc5=0.5 ampeg_hold_oncc6=0.25 ampeg_decay_curvecc7=2 "
                         "ampeg_decay_oncc7=1\n");
      CHECK(r.ok() && r.regions.size() == 1);
      if (r.regions.size() == 1) {
        const CcMap& m = r.regions[0].cc;
        CHECK(m.gate_count == 0 && m.mod_count == 13);
        // ascending by (target, cc): amplitude 93; volume 2 (gain_cc2 wins over volume_oncc2: same target and controller, last wins); pan 1; tune 94, 95; offset 73; EG
        auto at = [&](unsigned i) { return m.mods[i]; };
        CHECK(at(0) == (CcMod{CcTarget::kAmplitude, 93, 7, 50.f}));
        CHECK(at(1) == (CcMod{CcTarget::kVolume, 2, 0, -4.f}));
        CHECK(at(2) == (CcMod{CcTarget::kPan, 1, 0, 10.f}));
        CHECK(at(3) == (CcMod{CcTarget::kTune, 94, 0, -100.f}) && at(4) == (CcMod{CcTarget::kTune, 95, 0, 5.f}));
        CHECK(at(5) == (CcMod{CcTarget::kOffset, 73, 0, 4000.f}));
        CHECK(at(6) == (CcMod{CcTarget::kEgDelay, 5, 0, 0.5f}) && at(7) == (CcMod{CcTarget::kEgStart, 3, 0, 10.f}) && at(8) == (CcMod{CcTarget::kEgAttack, 64, 0, 0.1f}));
        CHECK(at(9) == (CcMod{CcTarget::kEgHold, 6, 0, 0.25f}) && at(10) == (CcMod{CcTarget::kEgDecay, 7, 2, 1.f}) && at(11) == (CcMod{CcTarget::kEgSustain, 4, 0, -20.f}));
        CHECK(at(12) == (CcMod{CcTarget::kEgRelease, 100, 0, 2.f}));
        CHECK(has_cc_extension(r.regions[0]) && validate_region(r.regions[0], 1000) == Error::kNone);
      }
      // pseudo-controllers
      auto p = parse(R + "pan_oncc131=100 pan_oncc135=10 pan_oncc136=5 pan_oncc137=2 pan_oncc140=1 pan_oncc141=1 amplitude_oncc127=1\n");
      CHECK(p.ok() && p.regions[0].cc.mod_count == 7);
      // a curve index alone has no effect on an additive target (sfizz: depth 0); amplitude keeps the entry (a multiplicative connection of depth 0 silences)
      CHECK(!has_cc_extension(parse(R + "pan_curvecc1=3\n").regions[0]) && !has_cc_extension(parse(R + "volume_oncc1=0\n").regions[0]));
      CHECK(!has_cc_extension(parse(R + "amplitude_curvecc1=3\n").regions[0]));  // a curve alone is no connection
      auto z = parse(R + "amplitude_oncc1=0\n");
      CHECK(z.ok() && z.regions[0].cc.mod_count == 1 && z.regions[0].cc.mods[0].depth == 0.f);
      // the same entry from a higher level, overridden in the region
      auto g = parse("<group> pan_oncc1=10 volume_oncc2=1\n<region> sample=a.wav key=60 pan_oncc1=20\n");
      CHECK(g.ok() && g.regions[0].cc.mod_count == 2 && g.regions[0].cc.mods[0].depth == 1.f && g.regions[0].cc.mods[1].depth == 20.f);
      // ranges: clamped and logged
      auto o = parse(R + "amplitude_oncc1=99999 pan_oncc2=500 tune_oncc3=-99999 ampeg_attack_oncc4=500 offset_oncc5=-5 volume_oncc6=100 pan_curvecc2=300\n");
      CHECK(o.ok() && o.ignored_values.size() == 7);
      if (o.ok()) {
        const CcMap& m = o.regions[0].cc;
        CHECK(m.mods[0].depth == 10000.f && m.mods[3].depth == -12000.f && m.mods[1].depth == 48.f);
        CHECK(m.mods[2].depth == 200.f && m.mods[2].curve == 0);  // curve 300 falls back to 0
      }
      // offset has no curve in sfizz: accepted, ignored, never silent
      auto oc = parse(R + "offset_oncc1=100 offset_curvecc1=3\n");
      CHECK(oc.ok() && oc.regions[0].cc.mods[0].curve == 0 && oc.warnings.size() == 1 && oc.warnings[0].opcode == "offset_curvecc1");
      // not implemented: still named
      for (const char* op : {"pan_stepcc1=1", "tune_smoothcc1=1", "cutoff_oncc131=1", "width_oncc1=1", "amp_veltrack_oncc7=5", "delay_oncc1=1", "end_oncc1=1"}) {
        auto b = parse(R + op + "\n");
        bool named = false;
        for (const auto& i : b.issues) named = named || (i.kind == SfzIssueKind::kUnsupportedOpcode && i.opcode == std::string(op).substr(0, std::string(op).find('=')));
        CHECK(!b.ok() && named);
      }
      // controllers that are not sources
      for (const char* op : {"pan_oncc128=1", "pan_oncc129=1", "pan_oncc130=1", "pan_oncc132=1", "pan_oncc133=1", "pan_oncc134=1", "pan_oncc138=1", "pan_oncc142=1", "pan_oncc511=1"}) {
        auto b = parse(R + op + "\n");
        bool named = false;
        for (const auto& i : b.issues) named = named || (i.kind == SfzIssueKind::kUnsupportedValue && i.opcode == std::string(op).substr(0, std::string(op).find('=')));
        CHECK(!b.ok() && named);
      }
      CHECK(has(parse(R + "pan_oncc01=1\n"), SfzIssueKind::kInvalidValue, "non-canonical controller number"));
      CHECK(has(parse(R + "pan_oncc1=abc\n"), SfzIssueKind::kInvalidValue, "'abc'"));
      std::string many = R;
      for (int i = 0; i <= int(kMaxCcMods); ++i) many += " pan_oncc" + std::to_string(i) + "=1";
      CHECK(has(parse(many + "\n"), SfzIssueKind::kUnsupportedValue, "CC modulation entries"));
      many = R;
      for (int i = 0; i < int(kMaxCcMods); ++i) many += " pan_oncc" + std::to_string(i) + "=1";
      CHECK(parse(many + "\n").ok());
    }
    {  // set_cc / set_hdcc: seeds for controllers a region reads; the rest is dropped and logged
      auto r = parse("<control>\nset_cc64=90 set_hdcc1=0.5 set_cc7=100 set_cc10=20 set_cc99=5 set_realcc2=1\n"
                     "<region> sample=a.wav key=60 locc64=64 pan_oncc1=3 xfin_locc10=0 xfin_hicc10=64 locc2=1\n");
      CHECK(r.ok() && r.regions.size() == 1);
      CHECK(r.cc_defaults.size() == 6);
      CHECK(r.cc_setup.defaults.size() == 4 && r.cc_setup.defaults[0] == (CcDefault{1, 63.5f}) && r.cc_setup.defaults[1] == (CcDefault{2, 127.f}) &&
            r.cc_setup.defaults[2] == (CcDefault{10, 20.f}) && r.cc_setup.defaults[3] == (CcDefault{64, 90.f}));  // 7 and 99 are read by no region
      bool dropped = false;
      for (const auto& v : r.ignored_values) dropped = dropped || (v.opcode == "set_cc99" && v.message.find("no region reads") != std::string::npos);
      CHECK(dropped);
      CHECK(r.cc_setup.curves.empty());
      auto same = parse("<control>\nset_cc7=100\n<region> sample=a.wav key=60 locc7=10\n");  // equal to the engine's value: kept, so binding the bank restores it
      CHECK(same.ok() && same.cc_setup.defaults.size() == 1 && same.cc_setup.defaults[0] == (CcDefault{7, 100.f}));
      CHECK(has(parse("<control>\nset_cc200=1\n<region> sample=a.wav\n"), SfzIssueKind::kUnsupportedValue, "controller"));
      CHECK(has(parse("<control>\nset_cc064=1\n<region> sample=a.wav\n"), SfzIssueKind::kInvalidValue, "non-canonical"));
      CHECK(has(parse("<region> sample=a.wav set_cc1=1\n"), SfzIssueKind::kUnsupportedOpcode, "only valid in <control>"));
      auto clamped = parse("<control>\nset_cc64=500\n<region> sample=a.wav key=1 locc64=1\n");
      CHECK(clamped.ok() && clamped.cc_setup.defaults.size() == 1 && clamped.cc_setup.defaults[0].value == 127.f);
    }
    {  // <curve> blocks: sfizz's index rules, only the curves a region names are kept
      auto r = parse("<curve>\nv000=0 v064=0.9 v127=1\n<curve>\nv000=1 v127=0\n<region> sample=a.wav key=60 amplitude_oncc1=100 amplitude_curvecc1=7 pan_oncc2=5 pan_curvecc2=8\n");
      CHECK(r.ok() && r.curves.size() == 2);
      CHECK(r.cc_setup.curves.size() == 2 && r.cc_setup.curves[0].index == 7 && r.cc_setup.curves[0].points.size() == 3 && r.cc_setup.curves[0].points[1] == (CurvePoint{64, 0.9f}));
      CHECK(r.cc_setup.curves[1].index == 8 && r.cc_setup.curves[1].points.size() == 2);
      // an explicit index closes implicit indexing; later explicit blocks win; the predefined slots can be replaced
      auto e = parse("<curve>\nv000=0 v127=1\n<curve>\ncurve_index=9 v000=0 v127=0.5\n<curve>\nv000=0 v127=0.25\n<curve>\ncurve_index=9 v000=0 v127=0.75\n<curve>\ncurve_index=0 v127=0.1 v128=3\n"
                     "<region> sample=a.wav key=60 pan_oncc1=5 pan_curvecc1=7 pan_oncc2=5 pan_curvecc2=9 pan_oncc3=5 pan_curvecc3=0 pan_oncc4=5 pan_curvecc4=10 pan_oncc5=5 pan_curvecc5=3\n");
      CHECK(e.ok() && e.cc_setup.curves.size() == 3);
      if (e.cc_setup.curves.size() == 3) {
        CHECK(e.cc_setup.curves[0].index == 0 && e.cc_setup.curves[0].points.size() == 1 && e.cc_setup.curves[0].points[0] == (CurvePoint{127, 0.1f}));  // v128 does not exist
        CHECK(e.cc_setup.curves[1].index == 7 && e.cc_setup.curves[1].points[1].y == 1.f);   // the implicit one before any explicit index got slot 7
        CHECK(e.cc_setup.curves[2].index == 9 && e.cc_setup.curves[2].points[1].y == 0.75f);
      }
      // a curve nothing uses is not stored; a library with <curve> blocks and no CC opcode keeps its bundle
      auto u = parse("<curve>\ncurve_index=3 v000=0 v127=1\n<region> sample=a.wav key=60 pan_oncc1=5\n");
      CHECK(u.ok() && u.cc_setup.curves.empty() && !u.cc_setup.any());
      CHECK(!parse("<curve>\ncurve_index=3 v000=0 v127=1\n<region> sample=a.wav key=60\n").cc_setup.any());
      // an unusable curve_index spells index 0 (sfizz reads it with a default of 0)
      auto bad = parse("<curve>\ncurve_index=999 v000=0 v127=0.5\n<region> sample=a.wav key=60 pan_oncc1=5 pan_curvecc1=0\n");
      CHECK(bad.ok() && bad.cc_setup.curves.size() == 1 && bad.cc_setup.curves[0].index == 0 && bad.ignored_values.size() == 1);
    }
    {  // regions removed by drop_never_note_triggered no longer keep a default or a curve alive
      auto r = parse("<control>\nset_cc64=90\n<curve>\ncurve_index=3 v000=0 v127=1\n<region> sample=a.wav hikey=-1 locc64=1 pan_oncc1=3 pan_curvecc1=3\n<region> sample=a.wav key=62\n");
      CHECK(r.ok() && r.cc_setup.defaults.size() == 1 && r.cc_setup.curves.size() == 1);
      CHECK(drop_never_note_triggered(r) == 1 && !r.cc_setup.any() && r.regions.size() == 1);
    }
    {  // a region without any of the opcodes is exactly what it was
      auto r = parse(R + "pan=10\n");
      Region want; want.key_lo = want.key_hi = want.key_center = 60; want.pan = 10.f;
      CHECK(r.ok() && r.regions[0] == want && r.regions[0].cc == CcMap{} && !r.cc_setup.any());
    }
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
  {  // Story 13.6: choke groups (group, off_by, off_mode, off_time) and polyphony limits (polyphony, note_polyphony, note_selfmask)
    const std::string R = "<region> sample=a.wav key=60 ";
    {  // defaults: a region without the opcodes has the default VoiceGroup (no table record)
      auto r = parse(R + "\n");
      CHECK(r.ok() && r.regions.size() == 1 && r.regions[0].vg == VoiceGroup{} && !has_vg_extension(r.regions[0]));
    }
    {  // group / off_by / off_mode / off_time with their aliases; inheritance from <group>
      auto r = parse("<group> group=2 off_by=1 off_mode=normal\n<region> sample=a.wav key=60\n<region> sample=a.wav key=61 group=3 offby=2 offmode=fast\n"
                     "<region> sample=a.wav key=62 polyphony_group=-4 off_time=0.5\n<region> sample=a.wav key=63 off_mode=time\n<region> sample=a.wav key=64 off_time=0.5 off_mode=fast\n");
      CHECK(r.ok() && r.regions.size() == 5);
      if (r.regions.size() == 5) {
        VoiceGroup g0; g0.group = 2; g0.has_off_by = true; g0.off_by = 1; g0.off_mode = OffMode::kNormal;
        CHECK(r.regions[0].vg == g0);
        CHECK(r.regions[1].vg.group == 3 && r.regions[1].vg.off_by == 2 && r.regions[1].vg.off_mode == OffMode::kFast && r.regions[1].vg.off_time == 0.f);
        CHECK(r.regions[2].vg.group == -4 && r.regions[2].vg.off_by == 1 && r.regions[2].vg.off_mode == OffMode::kTime && r.regions[2].vg.off_time == 0.5f);  // off_time implies time
        CHECK(r.regions[3].vg.off_mode == OffMode::kTime && r.regions[3].vg.off_time == 0.006f);  // off_mode=time alone: sfizz's 6 ms
        CHECK(r.regions[4].vg.off_mode == OffMode::kFast && r.regions[4].vg.off_time == 0.f);     // a later off_mode replaces the one off_time implied (over the group's normal)
        for (const Region& g : r.regions) CHECK(validate_region(g, 1000) == Error::kNone);
      }
    }
    {  // off_mode fast after off_time in the same region: canonical (no stray off_time), and an out-of-range off_time is clamped with a log entry
      auto r = parse(R + "off_time=0.5 off_mode=fast\n");
      CHECK(r.ok() && r.regions[0].vg.off_mode == OffMode::kFast && r.regions[0].vg.off_time == 0.f);
      auto c = parse(R + "off_time=500\n");
      CHECK(c.ok() && c.regions[0].vg.off_time == 100.f && c.ignored_values.size() == 1);
    }
    {  // polyphony: a region limit, and with a non-zero group the whole group's limit (the last assignment wins for every region of the group)
      auto r = parse("<region> sample=a.wav key=60 group=1 polyphony=2\n<region> sample=a.wav key=61 group=1\n<region> sample=a.wav key=62 group=1 polyphony=3\n"
                     "<region> sample=a.wav key=63 polyphony=4\n<region> sample=a.wav key=64 group=2\n");
      CHECK(r.ok() && r.regions.size() == 5);
      if (r.regions.size() == 5) {
        CHECK(r.regions[0].vg.polyphony == 2 && r.regions[0].vg.group_polyphony == 3);
        CHECK(r.regions[1].vg.polyphony == 0 && r.regions[1].vg.group_polyphony == 3);
        CHECK(r.regions[2].vg.polyphony == 3 && r.regions[2].vg.group_polyphony == 3);
        CHECK(r.regions[3].vg.polyphony == 4 && r.regions[3].vg.group_polyphony == 0);  // group 0: only the region
        CHECK(r.regions[4].vg.group == 2 && r.regions[4].vg.group_polyphony == 0 && r.regions[4].vg.polyphony == 0);
      }
      // polyphony in <group> next to group=N (in <group> or <master>) is the group's limit too
      auto g = parse("<master>group=5\n<group>polyphony=1\n<region> sample=a.wav key=60\n<region> sample=a.wav key=61\n");
      CHECK(g.ok() && g.regions.size() == 2 && g.regions[0].vg.polyphony == 1 && g.regions[1].vg.group_polyphony == 1);
      auto g2 = parse("<group>group=5 polyphony=1\n<region> sample=a.wav key=60\n");
      CHECK(g2.ok() && g2.regions[0].vg.group == 5 && g2.regions[0].vg.group_polyphony == 1);
      // 256 is sfizz's default: no limit stored
      CHECK(parse(R + "polyphony=256 note_polyphony=256\n").regions[0].vg == VoiceGroup{});
      CHECK(parse(R + "polyphony=255 group=1\n").regions[0].vg.polyphony == 255);
    }
    {  // region sets: a polyphony that limits all the regions of a header is rejected, naming the opcode at its own line
      for (const char* text : {"<global>polyphony=8\n<region> sample=a.wav key=60\n", "<master>polyphony=8\n<region> sample=a.wav key=60\n",
                               "<master>polyphony=8 group=2\n<group>group=3\n<region> sample=a.wav key=60\n",   // a region under it leaves group 2
                               "<master>polyphony=8 group=2\n<region> sample=a.wav key=60 group=4\n",
                               "<group>polyphony=2\n<region> sample=a.wav key=60\n", "<group>polyphony=2\n<region> sample=a.wav key=60 group=3\n",
                               "<global>polyphony=8 group=2\n<group>group=3\n<region> sample=a.wav key=60\n"}) {
        auto d = parse(text);
        CHECK(!d.ok() && has(d, SfzIssueKind::kUnsupportedValue, "region set"));
        bool named = false;
        for (const auto& i : d.issues) named = named || (i.opcode == "polyphony" && is_unsupported_feature(i.kind) && i.line >= 1);
        CHECK(named);
      }
      {  // the Salamander pedal pattern: <master> group=N polyphony=P, every region under it keeps N: the group's limit stands for the set's
        auto m = parse("<master>\nlokey=-1 hikey=-1 group=1 polyphony=1 off_time=0.1\n<group>\nvolume=-20\n<region> sample=a.wav lorand=0 hirand=0.5\n<region> sample=a.wav lorand=0.5 hirand=1\n");
        CHECK(m.ok() && m.regions.size() == 2);
        for (const auto& g : m.regions) CHECK(g.vg.group == 1 && g.vg.polyphony == 1 && g.vg.group_polyphony == 1 && g.vg.off_mode == OffMode::kTime && g.vg.off_time == 0.1f);
      }
      {  // Salamander's pedal.txt: the same under a <global>; and a group set at a broader header than the polyphony
        auto m = parse("<global>\nmaster_label=pedalAction\nlokey=-1 hikey=-1 group=1 polyphony=1 off_time=0.1\n<group>\n<region> sample=a.wav\n");
        CHECK(m.ok() && m.regions.size() == 1 && m.regions[0].vg.group_polyphony == 1 && m.regions[0].vg.polyphony == 1);
        auto n = parse("<global>group=3\n<group>polyphony=2\n<region> sample=a.wav key=60\n");
        CHECK(n.ok() && n.regions.size() == 1 && n.regions[0].vg.group == 3 && n.regions[0].vg.group_polyphony == 2);
      }
      {  // a deeper polyphony would hide the header's region-set limit: rejected, naming the header's opcode, at every header level
        for (const char* text : {"<group>group=1 polyphony=2\n<region> sample=a.wav key=60 polyphony=5\n",
                                 "<master>group=1 polyphony=2\n<group>polyphony=3\n<region> sample=a.wav key=60\n",
                                 "<master>group=1 polyphony=2\n<region> sample=a.wav key=60 polyphony=5\n",
                                 "<global>group=1 polyphony=2\n<region> sample=a.wav key=60 polyphony=5\n",
                                 "<global>group=1 polyphony=2\n<master>polyphony=4\n<region> sample=a.wav key=60\n"}) {
          auto d = parse(text);
          CHECK(!d.ok() && has(d, SfzIssueKind::kUnsupportedValue, "later polyphony overrides"));
          bool named = false;
          for (const auto& i : d.issues) named = named || (i.opcode == "polyphony" && i.line == 1);
          CHECK(named);
        }
        CHECK(parse("<group>group=1\n<region> sample=a.wav key=60 polyphony=5\n").ok());  // no header limit: a region limit is fine
      }
      CHECK(!parse("<region> sample=a.wav key=60 polyphony=0\n").ok());  // sfizz 0: not represented
      CHECK(parse("<region> sample=a.wav key=60 polyphony=-1\n").ok() && parse("<region> sample=a.wav key=60 polyphony=999\n").ignored_values.size() == 1);
    }
    {  // note_polyphony / note_selfmask
      CHECK(parse(R + "note_polyphony=1 note_selfmask=off\n" + R + "note_polyphony=1 note_selfmask=mask\n").regions[1].vg.note_selfmask);
      auto r = parse(R + "note_polyphony=1\n" + R + "note_polyphony=2 note_selfmask=off\n" + R + "note_polyphony=0\n");
      CHECK(r.ok() && r.regions.size() == 3);
      if (r.regions.size() == 3) {
        CHECK(r.regions[0].vg.note_polyphony == 1 && r.regions[0].vg.note_selfmask);
        CHECK(r.regions[1].vg.note_polyphony == 2 && !r.regions[1].vg.note_selfmask);
        CHECK(r.regions[2].vg.note_polyphony == 0 && r.ignored_values.size() == 1);
      }
    }
    {  // bad values
      CHECK(!parse(R + "off_mode=slow\n").ok() && has(parse(R + "off_mode=slow\n"), SfzIssueKind::kUnsupportedValue, "off_mode"));
      CHECK(!parse(R + "note_selfmask=maybe\n").ok());
      CHECK(!parse(R + "group=abc\n").ok() && !parse(R + "off_by=1.5\n").ok());
      CHECK(has(parse(R + "group=9999999999\n"), SfzIssueKind::kUnsupportedValue, "group"));
      CHECK(!parse(R + "off_time=abc\n").ok());
    }
    for (const char* op : {"group", "polyphony_group", "off_by", "offby", "off_mode", "offmode", "off_time", "polyphony", "note_polyphony", "note_selfmask"})
      CHECK(classify_opcode(op) == OpcodeClass::kSupported);
    // determinism
    CHECK(parse(R + "group=1 off_by=2 polyphony=3\n").regions == parse(R + "polyphony=3 off_by=2 group=1\n").regions);
  }
  {  // Story 13.7: bend_up/bend_down/bend_step/bend_smooth and on_locc/on_hicc controller triggers
    const std::string R = "<region> sample=a.wav key=60 ";
    {  // no bend opcode: no bend and no trigger; the region is exactly what it was
      auto r = parse(R + "pan=10\n");
      Region want; want.key_lo = want.key_hi = want.key_center = 60; want.pan = 10.f;
      CHECK(r.ok() && r.regions[0] == want && !r.regions[0].bend.enabled && r.regions[0].trig == CcTriggers{} && !has_bend_trigger_extension(r.regions[0]));
    }
    {  // any one bend opcode enables the bend; the others keep sfizz's defaults; aliases
      auto r = parse(R + "bend_up=400\n<region> sample=a.wav key=61 benddown=-300 bend_step=25 bend_smooth=40\n<region> sample=a.wav key=62 bendup=1200 bend_down=-1200\n"
                         "<region> sample=a.wav key=63 bend_smooth=0\n");
      CHECK(r.ok() && r.regions.size() == 4);
      if (r.regions.size() == 4) {
        const PitchBend a = r.regions[0].bend, b = r.regions[1].bend, c = r.regions[2].bend, d = r.regions[3].bend;
        CHECK(a.enabled && a.up == 400.f && a.down == -200.f && a.step == 1.f && a.smooth_ms == 0);
        CHECK(b.enabled && b.up == 200.f && b.down == -300.f && b.step == 25.f && b.smooth_ms == 40);
        CHECK(c.enabled && c.up == 1200.f && c.down == -1200.f);
        CHECK(d.enabled && d.smooth_ms == 0 && d.up == 200.f);  // given, even at its default: the region follows the wheel
        CHECK(has_bend_trigger_extension(r.regions[0]) && has_bend_trigger_extension(r.regions[3]));
      }
      // inherited from a header, like any opcode
      auto g = parse("<group>\nbend_up=700 bend_down=-500\n<region> sample=a.wav key=60\n<region> sample=a.wav key=61 bend_up=100\n");
      CHECK(g.ok() && g.regions[0].bend.up == 700.f && g.regions[0].bend.down == -500.f && g.regions[1].bend.up == 100.f && g.regions[1].bend.down == -500.f);
    }
    {  // value rules
      auto lo = parse(R + "bend_step=0.2\n");  // below 1: sfizz enforces the lower bound, logged
      CHECK(lo.ok() && lo.regions[0].bend.step == 1.f && lo.ignored_values.size() == 1 && lo.ignored_values[0].opcode == "bend_step");
      auto hi = parse(R + "bend_step=99999 bend_smooth=70000\n");
      CHECK(hi.ok() && hi.regions[0].bend.step == kMaxBendStep && hi.regions[0].bend.smooth_ms == 65535 && hi.ignored_values.size() == 2);
      auto ok = parse(R + "bend_up=12000 bend_down=-12000\n");
      CHECK(ok.ok() && ok.regions[0].bend.up == 12000.f && ok.regions[0].bend.down == -12000.f);
      auto nz = parse(R + "bend_up=-0\n");
      CHECK(nz.ok() && !std::signbit(nz.regions[0].bend.up));
      CHECK(has(parse(R + "bend_up=12001\n"), SfzIssueKind::kUnsupportedValue, "bend_up"));   // sfizz keeps these; the importer does not
      CHECK(has(parse(R + "bend_down=-24000\n"), SfzIssueKind::kUnsupportedValue, "bend_down"));
      CHECK(has(parse(R + "bend_up=abc\n"), SfzIssueKind::kInvalidValue, "bend_up"));
      CHECK(has(parse(R + "bend_smooth=x\n"), SfzIssueKind::kInvalidValue, "bend_smooth"));
      CHECK(has(parse(R + "lobend=0\n"), SfzIssueKind::kUnsupportedOpcode, "lobend"));
      CHECK(has(parse(R + "hibend=0\n"), SfzIssueKind::kUnsupportedOpcode, "hibend"));
    }
    {  // controller triggers: a hikey=-1 region with on_locc/on_hicc is kept and never starts from a note
      auto r = parse("<master>\nlokey=-1 hikey=-1 group=1 polyphony=1 off_time=0.1\n<group>\nvolume=-20 on_locc64=127 on_hicc64=127 locc64=127 hicc64=127\n"
                     "<region> sample=a.wav lorand=0 hirand=0.5\n<region> sample=a.wav lorand=0.5 hirand=1\n<group>\non_locc64=0 on_hicc64=0 locc64=0 hicc64=0\n<region> sample=a.wav\n");
      CHECK(r.ok() && r.regions.size() == 3 && r.never_note_triggered.size() == 3);
      if (r.regions.size() == 3) {
        for (std::size_t i = 0; i < 3; ++i) CHECK(!r.never_note_triggered[i] && r.regions[i].trig.no_note && r.regions[i].trig.count == 1 && r.regions[i].trig.t[0].cc == 64);
        CHECK(r.regions[0].trig.t[0].lo == 127.f && r.regions[0].trig.t[0].hi == 127.f);       // on_hicc=127 is not widened
        CHECK(r.regions[2].trig.t[0].lo == 0.f && r.regions[2].trig.t[0].hi == std::nextafter(1.0f, 0.0f));  // on_hicc=0 covers [0, 1)
        CHECK(r.regions[0].cc.gate_count == 1 && r.regions[2].cc.gate_count == 1);              // the locc/hicc gates of the same controller stay gates
        for (const auto& iv : r.ignored_values) CHECK(iv.opcode == "lokey");                     // hikey=-1 is not logged: nothing is dropped (lokey=-1 is the 13.2 default fallback)
        CHECK(has_bend_trigger_extension(r.regions[0]));
      }
      CHECK(drop_never_note_triggered(r) == 0 && r.regions.size() == 3);
    }
    {  // hikey=-1 without a trigger is still dropped (one log line per opcode); with a trigger on one region only that region stays
      auto r = parse("<group>\nhikey=-1\n<region> sample=a.wav\n<region> sample=a.wav on_locc1=10\n<region> sample=a.wav key=60\n");
      CHECK(r.ok() && r.regions.size() == 3);
      if (r.regions.size() == 3) {
        CHECK(r.never_note_triggered[0] && !r.never_note_triggered[1] && !r.never_note_triggered[2]);
        CHECK(r.regions[1].trig.no_note && r.regions[1].trig.count == 1 && !r.regions[2].trig.no_note && r.regions[2].trig.count == 0);
        CHECK(r.ignored_values.size() == 1 && r.ignored_values[0].opcode == "hikey");  // the group's hikey=-1, once, for the region that is dropped
      }
      CHECK(drop_never_note_triggered(r) == 1 && r.regions.size() == 2 && r.regions[0].trig.count == 1);
    }
    {  // a trigger on a region that also plays from a key range keeps both ways; a lone on_hicc / on_locc fills the other end; start_* aliases
      auto r = parse(R + "on_locc1=30 on_hicc1=60\n<region> sample=a.wav key=61 on_hicc2=50\n<region> sample=a.wav key=62 start_locc3=100 start_hicc3=100\n");
      CHECK(r.ok() && r.regions.size() == 3);
      if (r.regions.size() == 3) {
        CHECK(!r.regions[0].trig.no_note && r.regions[0].trig.count == 1 && r.regions[0].trig.t[0].lo == 30.f && r.regions[0].trig.t[0].hi == std::nextafter(61.0f, 0.0f));
        CHECK(r.regions[1].trig.t[0].cc == 2 && r.regions[1].trig.t[0].lo == 0.f && r.regions[1].trig.t[0].hi == std::nextafter(51.0f, 0.0f));
        CHECK(r.regions[2].trig.t[0].cc == 3 && r.regions[2].trig.t[0].lo == 100.f && r.regions[2].trig.t[0].hi == std::nextafter(101.0f, 0.0f));
      }
      // several controllers, ascending by number whatever the order in the file
      auto m = parse(R + "on_locc20=1 on_locc5=2 on_hicc5=9 on_locc127=0\n");
      CHECK(m.ok() && m.regions[0].trig.count == 3 && m.regions[0].trig.t[0].cc == 5 && m.regions[0].trig.t[1].cc == 20 && m.regions[0].trig.t[2].cc == 127);
      // a trigger controller feeds the set_cc seed (a controller some region reads)
      auto sd = parse("<control>\nset_cc20=90\n<region> sample=a.wav key=60 on_locc20=1\n");
      CHECK(sd.ok() && sd.cc_setup.defaults.size() == 1 && sd.cc_setup.defaults[0].cc == 20 && sd.cc_setup.defaults[0].value == 90.f);
    }
    {  // rejected forms, with the opcode named
      CHECK(has(parse(R + "on_locc131=1\n"), SfzIssueKind::kUnsupportedValue, "on_locc131"));      // a pseudo-controller cannot start a voice
      CHECK(has(parse(R + "on_hicc200=1\n"), SfzIssueKind::kUnsupportedValue, "on_hicc200"));
      CHECK(has(parse(R + "on_lohdcc64=0.5\n"), SfzIssueKind::kUnsupportedOpcode, "on_lohdcc64"));
      CHECK(has(parse(R + "on_hihdcc64=0.5\n"), SfzIssueKind::kUnsupportedOpcode, "on_hihdcc64"));
      CHECK(has(parse(R + "on_locc1=1 on_locc2=1 on_locc3=1 on_locc4=1 on_locc5=1\n"), SfzIssueKind::kUnsupportedValue, "more than 4 controller triggers"));
      CHECK(has(parse(R + "on_locc1=90 on_hicc1=10\n"), SfzIssueKind::kInvalidValue, "inverted controller trigger"));
      CHECK(has(parse(R + "on_locc064=1\n"), SfzIssueKind::kInvalidValue, "non-canonical"));
      auto clamped = parse(R + "on_locc1=200\n");  // out of range: clamped to 127 like locc, logged
      CHECK(clamped.ok() && clamped.regions[0].trig.t[0].lo == 127.f && clamped.ignored_values.size() == 1);
    }
    {  // a controller voice is an attack: any other trigger mode is refused; -0 values are stored as +0 so the bundle writer accepts them
      for (const char* t : {"release", "first", "legato", "release_key"})
        CHECK(has(parse(R + "on_locc1=10 trigger=" + t + "\n"), SfzIssueKind::kUnsupportedValue, "controller voice always starts as an attack"));
      CHECK(parse(R + "on_locc1=10 trigger=attack\n").ok());
      auto z = parse(R + "on_locc1=-0 on_hicc1=-0 bend_down=-0 bend_up=-0.0\n");
      CHECK(z.ok() && z.regions[0].trig.count == 1 && !std::signbit(z.regions[0].trig.t[0].lo) && !std::signbit(z.regions[0].trig.t[0].hi) &&
            !std::signbit(z.regions[0].bend.down) && !std::signbit(z.regions[0].bend.up));
      CHECK(validate_region(z.regions[0], 1000) == Error::kNone);
    }
    {  // a dropped region's trigger controller or curve does not keep a default alive; a kept one does
      auto r = parse("<control>\nset_cc64=90\n<region> sample=a.wav hikey=-1\n<region> sample=a.wav key=62\n");
      CHECK(r.ok() && drop_never_note_triggered(r) == 1);
    }
  }
  {  // opcodes outside the subset are listed with file:line and the opcode name; none are allow-listed
    const char* bad[] = {"script", "lfo01_freq", "eg01_time1", "cutoff_oncc1", "width_oncc1", "volume_stepcc1", "pan_smoothcc2",
                         "effect1", "output", "note_offset", "octave_offset",
                         "amp_veltrack_oncc7", "sample_quality", "delay", "rt_decay", "amp_random", "ampeg_vel2delay", "amp_veltrack_curvecc3", "xfin_lovel", "cutoff2", "fil2_type", "pitch_keytrack"};
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

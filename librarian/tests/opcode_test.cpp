// Story 5.1 tests: one fixture per SFZ opcode (parsed region table vs the bundle's instrument
// table after a write/read round trip), reason codes, allow-list, batch import and the JSON Lines
// report. Fixtures are generated in code (small synthetic WAVs + SFZ text), nothing is committed.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "test_util.hpp"
#include "velora/format/sfz_import.hpp"
#include "velora/librarian/batch.hpp"
#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;
using namespace velora::librarian;
namespace fmt = velora::format;

namespace {

using Bytes = std::vector<std::uint8_t>;
void w16(Bytes& b, std::uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void w32(Bytes& b, std::uint32_t v) { w16(b, v & 0xFFFF); w16(b, v >> 16); }
void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

void write_file(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}
void write_bytes(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Bytes make_wav(const fmt::PcmFormat& f, const std::vector<std::int32_t>& pcm) {
  Bytes payload;
  for (std::int32_t v : pcm) {
    const auto u = static_cast<std::uint32_t>(v);
    for (unsigned i = 0; i < f.bits_per_sample / 8; ++i) payload.push_back(std::uint8_t(u >> (8 * i)));
  }
  Bytes b;
  tag(b, "RIFF"); w32(b, std::uint32_t(36 + payload.size())); tag(b, "WAVE");
  tag(b, "fmt "); w32(b, 16); w16(b, f.is_float ? 3 : 1); w16(b, f.channels); w32(b, f.sample_rate);
  w32(b, f.sample_rate * f.channels * f.bits_per_sample / 8); w16(b, f.channels * f.bits_per_sample / 8);
  w16(b, f.bits_per_sample);
  tag(b, "data"); w32(b, std::uint32_t(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
const fmt::PcmFormat k16m{1, 16, false, 48000};
void write_sample(const fs::path& p, std::uint64_t frames, std::uint64_t seed = 1) {
  write_bytes(p, make_wav(k16m, vt::make_int(k16m, frames, vt::Kind::kMixed, seed)));
}

struct Env {
  fs::path dir, src, root;
  explicit Env(const char* name) {
    dir = fs::temp_directory_path() / (std::string("velora_opcode_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    src = dir / "src";
    root = dir / "data";
    fs::create_directories(src / "samples");
    fs::create_directories(root);
    write_sample(src / "samples/a.wav", 9000, 1);  // 9000 frames
    write_sample(src / "samples/b.wav", 4000, 2);
  }
  ~Env() { std::error_code ec; fs::remove_all(dir, ec); }
  ImportOptions opts(const std::string& sfz) const {
    ImportOptions o;
    o.sfz_path = (src / sfz).string();
    o.root = root.string();
    return o;
  }
  fs::path lib() const { return root / "lib"; }
};

std::set<std::string> names(const fs::path& dir) {
  std::set<std::string> s;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) s.insert(it->path().filename().string());
  return s;
}

bool mentions(const ImportResult& r, const std::string& needle) {
  for (const auto& s : r.reasons)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

bool offends(const ImportResult& r, const std::string& opcode, std::size_t line) {
  for (const auto& o : r.offenders)
    if (o.opcode == opcode && o.line == line && !o.file.empty()) return true;
  return false;
}

// Nothing of a skipped file reached the data root: no library directory, no staging garbage.
bool nothing_written(const Env& e) {
  if (!fs::exists(e.lib())) return true;
  const auto top = names(e.lib());
  if (!(top.empty() || top == std::set<std::string>{".staging"})) return false;
  return !fs::exists(e.lib() / ".staging") || names(e.lib() / ".staging") <= std::set<std::string>{".lock"};
}

// ---------------------------------------------------------------------------------------------
// One fixture per opcode: parsed table == bundle table, field by field, plus the expected values.

#define CHECK_F(field) do { if (!((a.field) == (p.field))) { std::printf("FAIL [%s] field %s differs\n", what, #field); ++g_fail; } } while (0)

void compare_regions(const char* what, const fmt::Region& a, const fmt::Region& p) {
  CHECK_F(sample); CHECK_F(key_lo); CHECK_F(key_hi); CHECK_F(key_center); CHECK_F(vel_lo); CHECK_F(vel_hi);
  CHECK_F(transpose); CHECK_F(tune_cents); CHECK_F(volume_db); CHECK_F(pan); CHECK_F(offset);
  CHECK_F(seq_length); CHECK_F(seq_position); CHECK_F(lorand); CHECK_F(hirand); CHECK_F(loop_mode);
  CHECK_F(has_loop_points); CHECK_F(loop_start); CHECK_F(loop_end); CHECK_F(ampeg); CHECK_F(trigger);
  CHECK_F(sw.lokey); CHECK_F(sw.hikey); CHECK_F(sw.last); CHECK_F(sw.down); CHECK_F(sw.up);
  CHECK_F(sw.previous); CHECK_F(sw.default_key);
  CHECK_F(xf.in_count); CHECK_F(xf.out_count); CHECK_F(xf.curve);
  for (unsigned i = 0; i < fmt::kMaxCcCrossfades; ++i) { CHECK_F(xf.in[i]); CHECK_F(xf.out[i]); }
  CHECK_F(filter.type); CHECK_F(filter.cutoff_hz); CHECK_F(filter.resonance_db);
  CHECK_F(filter);
  CHECK_F(sw); CHECK_F(xf);
  if (!(a == p)) { std::printf("FAIL [%s] regions differ\n", what); ++g_fail; }
}

struct OpcodeCase {
  const char* name;
  std::string text;  // region opcodes, placed in a <region> with sample=samples/a.wav
  std::function<bool(const fmt::Region&)> expect;
  bool extension;    // true: needs the format 2.1 extension table
};

void run_opcode_case(const OpcodeCase& c) {
  Env e(("op" + std::to_string(::getpid())).c_str());
  write_file(e.src / "T.sfz", "<region> sample=samples/a.wav key=60 " + c.text + "\n");
  const auto parsed = fmt::parse_sfz_file((e.src / "T.sfz").string());
  CHECK(parsed.ok());
  if (!parsed.ok()) {
    for (const auto& i : parsed.issues) std::printf("  [%s] %s\n", c.name, i.to_string().c_str());
    return;
  }
  const ImportResult r = import_sfz(e.opts("T.sfz"));
  CHECK(r.ok());
  if (!r.ok()) { for (const auto& s : r.reasons) std::printf("  [%s] %s\n", c.name, s.c_str()); return; }
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok());
  if (!b.ok() || b.value().instruments()[0].regions.size() != 1) { CHECK(false); return; }
  fmt::Region want = parsed.regions[0];
  std::string why;
  CHECK(fmt::resolve_loop_points(want, parsed.loops[0], 9000, &why));  // the importer's loop defaults
  const fmt::Region& got = b.value().instruments()[0].regions[0];
  compare_regions(c.name, got, want);
  if (!c.expect(got)) { std::printf("FAIL [%s] unexpected value\n", c.name); ++g_fail; }
  CHECK(b.value().format_minor() == (c.extension ? 1u : 0u));
  CHECK(b.value().format_version() == 2u);
  CHECK(fmt::has_extension(got) == c.extension);
  CHECK(b.value().verify_all() == fmt::Error::kNone);
}

void test_per_opcode() {
  using R = fmt::Region;
  const std::vector<OpcodeCase> cases = {
      // keyswitches
      {"sw_lokey", "sw_lokey=c1", [](const R& r) { return r.sw.lokey == 24 && r.sw.hikey == -1; }, true},
      {"sw_hikey", "sw_hikey=e1", [](const R& r) { return r.sw.hikey == 28 && r.sw.lokey == -1; }, true},
      {"sw_lokey+sw_hikey", "sw_lokey=24 sw_hikey=35", [](const R& r) { return r.sw.lokey == 24 && r.sw.hikey == 35; }, true},
      {"sw_last", "sw_last=26", [](const R& r) { return r.sw.last == 26 && r.sw.down == -1; }, true},
      {"sw_down", "sw_down=d#1", [](const R& r) { return r.sw.down == 27; }, true},
      {"sw_up", "sw_up=36", [](const R& r) { return r.sw.up == 36; }, true},
      {"sw_previous", "sw_previous=0", [](const R& r) { return r.sw.previous == 0; }, true},
      {"sw_default", "sw_default=127", [](const R& r) { return r.sw.default_key == 127; }, true},
      // CC crossfades
      {"xfin_locc/xfin_hicc", "xfin_locc1=10 xfin_hicc1=90",
       [](const R& r) { return r.xf.in_count == 1 && r.xf.in[0] == fmt::CcRange{1, 10, 90} && r.xf.out_count == 0; }, true},
      {"xfout_locc/xfout_hicc", "xfout_locc74=20 xfout_hicc74=127",
       [](const R& r) { return r.xf.out_count == 1 && r.xf.out[0] == fmt::CcRange{74, 20, 127} && r.xf.in_count == 0; }, true},
      {"xfin_hicc only", "xfin_hicc11=64", [](const R& r) { return r.xf.in[0] == fmt::CcRange{11, 0, 64}; }, true},
      {"four xfin + four xfout", "xfin_hicc1=5 xfin_hicc2=6 xfin_hicc3=7 xfin_hicc4=8 xfout_locc9=1 xfout_hicc9=2 xfout_hicc10=3 xfout_hicc11=4 xfout_hicc12=5",
       [](const R& r) { return r.xf.in_count == 4 && r.xf.out_count == 4 && r.xf.out[3] == fmt::CcRange{12, 0, 5}; }, true},
      {"two controllers sorted", "xfin_hicc64=9 xfin_hicc2=8",
       [](const R& r) { return r.xf.in_count == 2 && r.xf.in[0].cc == 2 && r.xf.in[1].cc == 64; }, true},
      {"xf_cccurve=gain", "xf_cccurve=gain", [](const R& r) { return r.xf.curve == fmt::XfCurve::kGain; }, true},
      {"xf_cccurve=power", "xf_cccurve=power xfin_hicc1=3", [](const R& r) { return r.xf.curve == fmt::XfCurve::kPower; }, true},
      {"xf_cccurve default", "xfin_hicc1=3", [](const R& r) { return r.xf.curve == fmt::XfCurve::kPower; }, true},
      // loops
      {"loop_mode=no_loop", "loop_mode=no_loop", [](const R& r) { return r.loop_mode == fmt::LoopMode::kNoLoop && !r.has_loop_points; }, false},
      {"loop_mode=one_shot", "loop_mode=one_shot", [](const R& r) { return r.loop_mode == fmt::LoopMode::kOneShot && !r.has_loop_points; }, false},
      {"loop_mode=loop_continuous", "loop_mode=loop_continuous",
       [](const R& r) { return r.loop_mode == fmt::LoopMode::kLoopContinuous && r.has_loop_points && r.loop_start == 0 && r.loop_end == 8999; }, false},
      {"loop_mode=loop_sustain", "loop_mode=loop_sustain loop_start=100 loop_end=800",
       [](const R& r) { return r.loop_mode == fmt::LoopMode::kLoopSustain && r.loop_start == 100 && r.loop_end == 800; }, false},
      {"loop_start", "loop_start=250", [](const R& r) { return r.has_loop_points && r.loop_start == 250 && r.loop_end == 8999; }, false},
      {"loop_end", "loop_end=4000", [](const R& r) { return r.has_loop_points && r.loop_start == 0 && r.loop_end == 4000; }, false},
      // filters
      {"fil_type=lpf_1p", "fil_type=lpf_1p cutoff=1000", [](const R& r) { return r.filter == fmt::Filter{fmt::FilterType::kLpf1p, 1000.f, 0.f}; }, true},
      {"fil_type=lpf_2p", "fil_type=lpf_2p cutoff=1000", [](const R& r) { return r.filter.type == fmt::FilterType::kLpf2p; }, true},
      {"fil_type=hpf_1p", "fil_type=hpf_1p cutoff=1000", [](const R& r) { return r.filter.type == fmt::FilterType::kHpf1p; }, true},
      {"fil_type=hpf_2p", "fil_type=hpf_2p cutoff=1000", [](const R& r) { return r.filter.type == fmt::FilterType::kHpf2p; }, true},
      {"fil_type=bpf_2p", "fil_type=bpf_2p cutoff=1000", [](const R& r) { return r.filter.type == fmt::FilterType::kBpf2p; }, true},
      {"cutoff", "cutoff=2500.25", [](const R& r) { return r.filter == fmt::Filter{fmt::FilterType::kLpf2p, 2500.25f, 0.f}; }, true},
      {"resonance", "cutoff=500 resonance=12.5", [](const R& r) { return r.filter.resonance_db == 12.5f && r.filter.cutoff_hz == 500.f; }, true},
      {"fil_type without cutoff", "fil_type=hpf_2p resonance=4", [](const R& r) { return r.filter == fmt::Filter{}; }, false},
  };
  for (const auto& c : cases) run_opcode_case(c);
}

// Everything at once, inherited through the levels, in two regions.
void test_combined_and_inheritance() {
  Env e("combined");
  write_file(e.src / "T.sfz",
             "<global>\nsw_lokey=c1 sw_hikey=e1 sw_default=c1 cutoff=900 fil_type=bpf_2p resonance=2\n"
             "<group>\nsw_last=c1 xfin_locc1=0 xfin_hicc1=64 xf_cccurve=gain\n"
             "<region> sample=samples/a.wav key=60 xfout_locc1=65 xfout_hicc1=127\n"
             "<group>\nsw_last=d1 sw_down=36\n"
             "<region> sample=samples/b.wav key=61 loop_mode=loop_continuous\n"
             "<region> sample=samples/b.wav key=62\n");
  const auto parsed = fmt::parse_sfz_file((e.src / "T.sfz").string());
  CHECK(parsed.ok() && parsed.regions.size() == 3);
  const ImportResult r = import_sfz(e.opts("T.sfz"));
  CHECK(r.ok());
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok());
  if (!b.ok() || !parsed.ok()) return;
  const auto& g = b.value().instruments()[0].regions;
  CHECK(g.size() == 3 && b.value().format_minor() == 1);
  if (g.size() != 3) return;
  CHECK(g[0].sw.lokey == 24 && g[0].sw.last == 24 && g[0].xf.in_count == 1 && g[0].xf.out_count == 1);
  CHECK(g[0].xf.curve == fmt::XfCurve::kGain && g[0].filter == (fmt::Filter{fmt::FilterType::kBpf2p, 900.f, 2.f}));
  CHECK(g[1].sw.last == 26 && g[1].sw.down == 36 && g[1].xf == fmt::Crossfade{} && g[1].loop_mode == fmt::LoopMode::kLoopContinuous);
  CHECK(g[2].sw.last == 26 && g[2].filter.type == fmt::FilterType::kBpf2p);
  // The opcode and the bundle agree for each region (loops resolved against sample lengths).
  for (std::size_t i = 0; i < 3; ++i) {
    fmt::Region want = parsed.regions[i];
    std::string why;
    CHECK(fmt::resolve_loop_points(want, parsed.loops[i], parsed.regions[i].sample == 0 ? 9000 : 4000, &why));
    compare_regions("combined", g[i], want);
  }
  // Re-import is a no-op (deterministic bundle id, extension included).
  const ImportResult again = import_sfz(e.opts("T.sfz"));
  CHECK(again.ok() && again.bundle_id == r.bundle_id && !again.bundle_created);
}

// A bundle without the new features is exactly the Epic 1 format: version word 2, minor 0.
void test_unextended_stays_epic1() {
  Env e("epic1");
  fs::copy_file(std::string(LIBRARIAN_FIXTURE_DIR) + "/salamander_like.sfz", e.src / "Piano.sfz");
  for (const char* n : {"A0v1", "C1v1", "A0v2", "C1v2", "rel1", "rel2", "rel3"})
    write_sample(e.src / "samples" / (std::string(n) + ".wav"), 5000, 7 + std::strlen(n));
  const ImportResult r = import_sfz(e.opts("Piano.sfz"));
  CHECK(r.ok());
  const std::string bytes = slurp(r.bundle_dir + "/bundle.vlb");
  CHECK(bytes.size() > 128);
  const auto u32 = [&](std::size_t o) {
    return std::uint32_t(std::uint8_t(bytes[o])) | std::uint32_t(std::uint8_t(bytes[o + 1])) << 8 |
           std::uint32_t(std::uint8_t(bytes[o + 2])) << 16 | std::uint32_t(std::uint8_t(bytes[o + 3])) << 24;
  };
  CHECK(u32(4) == 2u);  // major 2, minor 0: the Epic 1 version word
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok() && b.value().format_minor() == 0 && b.value().format_version() == 2);
  if (b.ok())
    for (const auto& g : b.value().instruments()[0].regions) CHECK(!fmt::has_extension(g));
}

// ---------------------------------------------------------------------------------------------
// Reason codes.

void test_unsupported_features() {
  Env e("unsup");
  struct Case { const char* what; std::string sfz; const char* opcode; std::size_t line; };
  const std::vector<Case> cases = {
      {"scripting", "<control>\nscript=song.lua\n<region> sample=samples/a.wav key=60\n", "script", 2},
      {"effect header", "<region> sample=samples/a.wav key=60\n<effect>\nbus=main\ntype=reverb\n", "<effect>", 2},
      {"effect send", "<region> sample=samples/a.wav key=60 effect1=50\n", "effect1", 1},
      {"lfo", "<group>\nlfo01_freq=3\n<region> sample=samples/a.wav key=60\n", "lfo01_freq", 2},
      {"eg", "<region> sample=samples/a.wav key=60\neg01_time1=0.1\n", "eg01_time1", 2},
      {"amplitude_oncc", "<region> sample=samples/a.wav key=60 amplitude_oncc1=100\n", "amplitude_oncc1", 1},
      {"curve header", "<curve>\ncurve_index=1\n<region> sample=samples/a.wav key=60\n", "<curve>", 1},
      {"set_cc", "<control>\nset_cc1=100\n<region> sample=samples/a.wav key=60\n", "set_cc1", 2},
      {"unsupported fil_type", "<region> sample=samples/a.wav key=60 fil_type=lpf_4p cutoff=100\n", "fil_type", 1},
      {"unsupported loop_mode", "<region> sample=samples/a.wav key=60 loop_mode=loop_bidir\n", "loop_mode", 1},
      {"five xfin controllers", "<region> sample=samples/a.wav xfin_hicc1=1 xfin_hicc2=1 xfin_hicc3=1 xfin_hicc4=1 xfin_hicc5=1\n", "xfin_locc<N>", 1},
  };
  for (const auto& c : cases) {
    write_file(e.src / "Bad.sfz", c.sfz);
    const ImportResult r = import_sfz(e.opts("Bad.sfz"));
    if (r.status != Status::kSkipped || r.reason_code != ReasonCode::kUnsupportedFeatures || !offends(r, c.opcode, c.line)) {
      std::printf("FAIL [%s] status=%s reason=%s\n", c.what, to_string(r.status), to_string(r.reason_code));
      ++g_fail;
    }
    CHECK(r.failing_path.empty());
    CHECK(nothing_written(e));
  }
  // All offenders are listed, with file:line, in one report; sounding neighbours do not matter.
  write_file(e.src / "Bad.sfz",
             "<control>\nscript=x.lua\n<group>\nlfo01_freq=1\n<region> sample=samples/a.wav\namp_veltrack=10\n<effect>\n");
  const ImportResult r = import_sfz(e.opts("Bad.sfz"));
  CHECK(r.reason_code == ReasonCode::kUnsupportedFeatures && r.offenders.size() == 4);
  CHECK(offends(r, "script", 2) && offends(r, "lfo01_freq", 4) && offends(r, "amp_veltrack", 6) && offends(r, "<effect>", 7));
  CHECK(mentions(r, "Bad.sfz:2: unsupported opcode 'script'"));
  CHECK(!r.offenders.empty() && r.offenders[0].file.find("Bad.sfz") != std::string::npos);
  // Audio format outside the supported set (8-bit, not resampled) is a feature, not damage.
  Bytes eight = make_wav({1, 16, false, 48000}, std::vector<std::int32_t>(100, 3));
  eight[34] = 8;
  write_bytes(e.src / "samples/eight.wav", eight);
  write_file(e.src / "Bad.sfz", "<region> sample=samples/eight.wav key=60\n");
  const ImportResult a = import_sfz(e.opts("Bad.sfz"));
  CHECK(a.status == Status::kSkipped && a.reason_code == ReasonCode::kUnsupportedFeatures && offends(a, "sample", 1));
  write_file(e.src / "samples/song.flac", "fLaC................................................");
  write_file(e.src / "Bad.sfz", "<region> sample=samples/song.flac key=60\n");
  const ImportResult f = import_sfz(e.opts("Bad.sfz"));
  CHECK(f.status == Status::kSkipped && f.reason_code == ReasonCode::kUnsupportedFeatures && offends(f, "sample", 1));
  CHECK(nothing_written(e));
}

void test_allowlist() {
  Env e("allow");
  write_file(e.src / "T.sfz",
             "<control>\nlabel_cc1=Mod hint_ram_based=1 label_key36=Soft image=pic.png\n"
             "<global>\nglobal_label=g\n<group>\ngroup_label=x sw_label=KS\n"
             "<region> sample=samples/a.wav key=60 region_label=r label_cc74=Cutoff hint_anything=2\n");
  const ImportResult r = import_sfz(e.opts("T.sfz"));
  CHECK(r.ok() && r.allowlisted.size() == 10);
  std::set<std::string> ops;
  for (const auto& a : r.allowlisted) { ops.insert(a.opcode); CHECK(a.line >= 1 && !a.file.empty()); }
  CHECK(ops.count("label_cc1") && ops.count("image") && ops.count("hint_anything") && ops.count("sw_label"));
  // Same instrument with and without the labels: identical regions and sample content.
  write_file(e.src / "T2.sfz", "<region> sample=samples/a.wav key=60\n");
  auto b1 = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  const ImportResult r2 = import_sfz(e.opts("T2.sfz"));
  auto b2 = fmt::BundleReader::open_file(r2.bundle_dir + "/bundle.vlb");
  CHECK(b1.ok() && b2.ok() && b1.value().instruments()[0].regions == b2.value().instruments()[0].regions);
  // The report counts them.
  FileReport fr{(e.src / "T.sfz").string(), r};
  const std::string line = report_file_line(fr);
  CHECK(line.find("\"allowlisted_count\":10") != std::string::npos && line.find("\"opcode\":\"label_cc1\"") != std::string::npos);
  CHECK(line.find("\"status\":\"imported\"") != std::string::npos && line.find("\"reason\":\"none\"") != std::string::npos);
}

void test_damaged() {
  Env e("damaged");
  // Unparsable value, structural error, missing sample, missing include, region without sample.
  struct Case { const char* what; std::string sfz; };
  const std::vector<Case> cases = {
      {"bad number", "<region> sample=samples/a.wav key=abc\n"},
      {"opcode outside header", "key=60\n<region> sample=samples/a.wav\n"},
      {"missing include", "#include \"gone.sfz\"\n<region> sample=samples/a.wav key=60\n"},
      {"region without sample", "<region> key=60\n"},
      {"inverted loop", "<region> sample=samples/a.wav loop_start=900 loop_end=100\n"},
      {"inverted keyswitch range", "<region> sample=samples/a.wav sw_lokey=40 sw_hikey=30\n"},
      {"inverted crossfade range", "<region> sample=samples/a.wav xfin_locc1=90 xfin_hicc1=10\n"},
      {"non-canonical controller number", "<region> sample=samples/a.wav xfin_hicc007=10\n"},
      {"controller value out of range", "<region> sample=samples/a.wav xfin_hicc1=300\n"},
      {"path escape", "<region> sample=../outside.wav\n"},
      {"loop beyond sample", "<region> sample=samples/a.wav loop_end=99999\n"},
  };
  for (const auto& c : cases) {
    write_file(e.src / "Bad.sfz", c.sfz);
    const ImportResult r = import_sfz(e.opts("Bad.sfz"));
    if (r.status != Status::kSkipped || r.reason_code != ReasonCode::kDamaged || r.failing_path.empty()) {
      std::printf("FAIL [%s] status=%s reason=%s failing='%s'\n", c.what, to_string(r.status), to_string(r.reason_code), r.failing_path.c_str());
      ++g_fail;
    }
    CHECK(nothing_written(e));
  }
  // Missing sample: the failing path is the sample file.
  write_file(e.src / "Bad.sfz", "<region> sample=samples/nope.wav key=60\n");
  ImportResult r = import_sfz(e.opts("Bad.sfz"));
  CHECK(r.reason_code == ReasonCode::kDamaged && r.failing_path == (e.src / "samples/nope.wav").lexically_normal().string());
  CHECK(mentions(r, "Bad.sfz:1"));
  // The SFZ itself cannot be opened.
  r = import_sfz(e.opts("absent.sfz"));
  CHECK(r.reason_code == ReasonCode::kDamaged && r.failing_path.find("absent.sfz") != std::string::npos);
  // Truncated WAV: the data chunk claims more bytes than the file has.
  Bytes cut = make_wav(k16m, vt::make_int(k16m, 3000, vt::Kind::kMixed, 5));
  cut.resize(cut.size() - 1000);
  write_bytes(e.src / "samples/cut.wav", cut);
  write_file(e.src / "Bad.sfz", "<region> sample=samples/a.wav key=60\n<region> sample=samples/cut.wav key=61\n");
  r = import_sfz(e.opts("Bad.sfz"));
  CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kDamaged);
  CHECK(r.failing_path == (e.src / "samples/cut.wav").string() && mentions(r, "cut.wav"));
  CHECK(nothing_written(e));
  // A WAV header with no payload at all, a garbage .wav, an empty file.
  write_bytes(e.src / "samples/short.wav", Bytes(20, 0));
  write_file(e.src / "samples/garbage.wav", "this is not a wav file at all, but long enough to have a header");
  write_file(e.src / "samples/empty.wav", "");
  for (const char* n : {"short", "garbage", "empty"}) {
    write_file(e.src / "Bad.sfz", std::string("<region> sample=samples/") + n + ".wav key=60\n");
    r = import_sfz(e.opts("Bad.sfz"));
    if (r.status != Status::kSkipped || r.reason_code != ReasonCode::kDamaged ||
        r.failing_path != (e.src / "samples" / (std::string(n) + ".wav")).string()) {
      std::printf("FAIL [%s.wav] status=%s reason=%s failing='%s'\n", n, to_string(r.status), to_string(r.reason_code), r.failing_path.c_str());
      ++g_fail;
    }
  }
  // Unreadable WAV (permissions); skipped when the test runs as root.
  write_sample(e.src / "samples/locked.wav", 100, 9);
  ::chmod((e.src / "samples/locked.wav").c_str(), 0);
  if (::access((e.src / "samples/locked.wav").c_str(), R_OK) != 0) {
    write_file(e.src / "Bad.sfz", "<region> sample=samples/locked.wav key=60\n");
    r = import_sfz(e.opts("Bad.sfz"));
    CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kDamaged && r.failing_path == (e.src / "samples/locked.wav").string());
  }
  ::chmod((e.src / "samples/locked.wav").c_str(), 0644);
  // A damaged file leaves an earlier import of the same source untouched.
  write_file(e.src / "Keep.sfz", "<region> sample=samples/a.wav key=60\n");
  const ImportResult first = import_sfz(e.opts("Keep.sfz"));
  CHECK(first.ok());
  write_file(e.src / "Keep.sfz", "<region> sample=samples/gone.wav key=60\n");
  const ImportResult second = import_sfz(e.opts("Keep.sfz"));
  CHECK(second.reason_code == ReasonCode::kDamaged);
  CHECK(fs::read_symlink(e.lib() / first.library_id / "current").string() == first.bundle_id);
  CHECK(names(e.lib() / first.library_id) == (std::set<std::string>{"current", first.bundle_id}));
  CHECK(names(e.lib() / ".staging") <= std::set<std::string>{".lock"});
  // Damage dominates: unsupported constructs next to a missing sample are reported as damaged.
  write_file(e.src / "Keep.sfz", "<region> sample=samples/gone.wav key=60 lfo01_freq=1\n");
  const ImportResult mixed = import_sfz(e.opts("Keep.sfz"));
  CHECK(mixed.reason_code == ReasonCode::kDamaged && offends(mixed, "lfo01_freq", 1));
}

// ---------------------------------------------------------------------------------------------
// Batch and report.

void test_batch() {
  Env e("batch");
  // Samples must be inside each SFZ's directory: copy them next to the SFZs.
  fs::create_directories(e.src / "lib/samples");
  fs::create_directories(e.src / "lib/sub/samples");
  fs::copy_file(e.src / "samples/a.wav", e.src / "lib/samples/a.wav");
  fs::copy_file(e.src / "samples/b.wav", e.src / "lib/sub/samples/b.wav");
  write_file(e.src / "lib/good1.sfz", "<region> sample=samples/a.wav key=60 sw_last=24 cutoff=800\n");
  write_file(e.src / "lib/bad_unsup.sfz", "<control>\nscript=x.lua\n<region> sample=samples/a.wav key=60\n");
  write_file(e.src / "lib/bad_damaged.sfz", "<region> sample=samples/missing.wav key=60\n");
  write_file(e.src / "lib/sub/good2.sfz", "<control>\nlabel_cc1=x\n<region> sample=samples/b.wav key=62\n");
  write_file(e.src / "lib/readme.txt", "not an sfz");
  write_file(e.src / "lib/UPPER.SFZ", "<region> sample=samples/a.wav key=64 xfin_hicc1=64\n");

  BatchOptions bo;
  bo.inputs = {(e.src / "lib").string(), (e.src / "lib/good1.sfz").string()};  // duplicate is dropped
  bo.root = e.root.string();
  const BatchResult b = import_batch(bo);
  CHECK(b.files.size() == 5);  // UPPER, bad_damaged, bad_unsup, good1, sub/good2 (sorted)
  CHECK(b.imported == 3 && b.skipped == 2 && b.failed == 0 && b.unchanged == 0 && !b.all_ok());
  std::map<std::string, const FileReport*> by;
  for (const auto& f : b.files) by[fs::path(f.sfz_path).filename().string()] = &f;
  CHECK(by.size() == 5);
  if (by.size() == 5) {
    CHECK(by["good1.sfz"]->result.ok() && by["good2.sfz"]->result.ok() && by["UPPER.SFZ"]->result.ok());
    CHECK(by["bad_unsup.sfz"]->result.reason_code == ReasonCode::kUnsupportedFeatures);
    CHECK(by["bad_damaged.sfz"]->result.reason_code == ReasonCode::kDamaged);
    CHECK(by["good2.sfz"]->result.allowlisted.size() == 1);
    CHECK(file_status(by["good1.sfz"]->result) == std::string("imported"));
  }
  // Exactly the three good libraries exist (plus .staging); the others wrote nothing.
  std::size_t libs = 0;
  for (const auto& n : names(e.lib())) if (n != ".staging") ++libs;
  CHECK(libs == 3);
  // Order is stable (sorted) and the batch is repeatable: a second run changes nothing.
  CHECK(b.files[0].sfz_path < b.files[1].sfz_path);
  const BatchResult again = import_batch(bo);
  CHECK(again.imported == 0 && again.unchanged == 3 && again.skipped == 2);

  // Report: one JSON line per file plus a summary, stable key order.
  const std::string jl = report_jsonl(b);
  std::vector<std::string> lines;
  { std::istringstream is(jl); std::string l; while (std::getline(is, l)) lines.push_back(l); }
  CHECK(lines.size() == 6);
  for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
    CHECK(lines[i].rfind("{\"type\":\"file\",\"file\":\"", 0) == 0);
    CHECK(lines[i].find("\"status\":\"") != std::string::npos && lines[i].back() == '}');
  }
  CHECK(lines.back() == "{\"type\":\"summary\",\"files\":5,\"imported\":3,\"unchanged\":0,\"skipped\":2,\"failed\":0}");
  bool saw_unsup = false, saw_damaged = false;
  for (const auto& l : lines) {
    if (l.find("bad_unsup.sfz\",\"status\":\"skipped\",\"reason\":\"unsupported_features\",\"failing_path\":\"\",\"offenders\":[{\"opcode\":\"script\"") != std::string::npos &&
        l.find("\"line\":2") != std::string::npos)
      saw_unsup = true;
    if (l.find("bad_damaged.sfz\",\"status\":\"skipped\",\"reason\":\"damaged\",\"failing_path\":\"") != std::string::npos &&
        l.find("missing.wav\"") != std::string::npos)
      saw_damaged = true;
  }
  CHECK(saw_unsup && saw_damaged);

  // JSON escaping of awkward file names.
  FileReport odd;
  odd.sfz_path = "we\"ird\\name\n.sfz";
  odd.result.status = Status::kSkipped;
  odd.result.reason_code = ReasonCode::kDamaged;
  odd.result.failing_path = "tab\there";
  const std::string ol = report_file_line(odd);
  CHECK(ol.find("\"file\":\"we\\\"ird\\\\name\\n.sfz\"") != std::string::npos && ol.find("tab\\there") != std::string::npos);
  CHECK(ol.find('\n') == std::string::npos);

  // A directory without SFZ files is reported, not silently ignored.
  fs::create_directories(e.src / "emptydir");
  BatchOptions eo;
  eo.inputs = {(e.src / "emptydir").string()};
  eo.root = e.root.string();
  const BatchResult eb = import_batch(eo);
  CHECK(eb.files.size() == 1 && eb.skipped == 1 && eb.files[0].result.reason_code == ReasonCode::kDamaged && !eb.all_ok());
  CHECK(import_batch(BatchOptions{}).files.empty());
}


void test_warnings_and_misc() {
  Env e("warn");
  // fil_type / resonance without cutoff: accepted as "no filter", never silently.
  write_file(e.src / "W.sfz",
             "<group>\nfil_type=hpf_2p\n"
             "<region> sample=samples/a.wav key=60 resonance=6\n"
             "<region> sample=samples/b.wav key=61\n"
             "<region> sample=samples/a.wav key=62 cutoff=500 fil_type=lpf_1p resonance=1\n");
  const ImportResult r = import_sfz(e.opts("W.sfz"));
  CHECK(r.ok() && r.warnings.size() == 2);  // the group-level fil_type is reported once, not per region
  if (r.warnings.size() == 2) {
    CHECK(r.warnings[0].code == "ignored_opcode" && r.warnings[0].opcode == "fil_type" && r.warnings[0].line == 2);
    CHECK(r.warnings[1].opcode == "resonance" && r.warnings[1].line == 3 && !r.warnings[1].file.empty());
  }
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok() && b.value().instruments()[0].regions[0].filter == fmt::Filter{} &&
        b.value().instruments()[0].regions[2].filter.type == fmt::FilterType::kLpf1p);
  const std::string line = report_file_line(FileReport{(e.src / "W.sfz").string(), r});
  CHECK(line.find("\"warnings\":[{\"code\":\"ignored_opcode\",\"opcode\":\"fil_type\"") != std::string::npos);
  CHECK(line.find("\"warning_count\":2") != std::string::npos);
  // No warning when cutoff is present.
  write_file(e.src / "W2.sfz", "<region> sample=samples/a.wav key=60 cutoff=500 fil_type=lpf_1p resonance=1\n");
  CHECK(import_sfz(e.opts("W2.sfz")).warnings.empty());
  // Both spellings of one controller are rejected, with a clear message.
  write_file(e.src / "W3.sfz", "<region> sample=samples/a.wav key=60 xfin_hicc7=1 xfin_hicc07=2\n");
  const ImportResult c = import_sfz(e.opts("W3.sfz"));
  CHECK(c.reason_code == ReasonCode::kDamaged && mentions(c, "non-canonical controller number") && mentions(c, "xfin_hicc7"));
  // Two missing samples: each issue keeps its own path; failing_path is the first one.
  write_file(e.src / "W4.sfz", "<region> sample=samples/m1.wav key=60\n<group>\nsample=samples/m2.wav\n<region> key=61\n<region> key=62\n");
  const auto parsed = fmt::parse_sfz_file((e.src / "W4.sfz").string());
  std::vector<std::string> paths;
  for (const auto& i : parsed.issues) if (i.kind == fmt::SfzIssueKind::kMissingSample) paths.push_back(i.path);
  CHECK(paths.size() == 2 && paths[0].find("m1.wav") != std::string::npos && paths[1].find("m2.wav") != std::string::npos);
  // add_sample failures that are not about the WAV are never "damaged WAV".
  ReasonCode rc;
  CHECK(status_for_add_sample_error(fmt::Error::kInvalidArgument, &rc) == Status::kInvalidArgument && rc == ReasonCode::kNone);
  CHECK(status_for_add_sample_error(fmt::Error::kEncoderFailure, &rc) == Status::kIo && rc == ReasonCode::kNone);
  CHECK(status_for_add_sample_error(fmt::Error::kUnsupportedFormat, &rc) == Status::kSkipped && rc == ReasonCode::kUnsupportedFeatures);
  CHECK(status_for_add_sample_error(fmt::Error::kTruncated, &rc) == Status::kSkipped && rc == ReasonCode::kDamaged);
  // A failed (not skipped) file in the report: nonexistent data root.
  BatchOptions bo;
  bo.inputs = {(e.src / "W2.sfz").string()};
  bo.root = (e.dir / "no_such_root").string();
  const BatchResult fb = import_batch(bo);
  CHECK(fb.files.size() == 1 && fb.failed == 1 && fb.skipped == 0 && !fb.all_ok());
  const std::string jl = report_jsonl(fb);
  CHECK(jl.find("\"status\":\"failed\"") != std::string::npos && jl.find("\"reason\":\"none\"") != std::string::npos);
  CHECK(jl.find("\"detail\":\"invalid argument\"") != std::string::npos && jl.find("\"failed\":1}") != std::string::npos);
  // Non-UTF-8 bytes in a path are escaped, so the report stays valid UTF-8.
  FileReport odd;
  odd.sfz_path = std::string("bad\xff") + "name\xc3\x28" + "ok\xc3\xa9" + "\xed\xa0\x80.sfz";
  odd.result.status = Status::kSkipped;
  odd.result.reason_code = ReasonCode::kDamaged;
  const std::string ol = report_file_line(odd);
  CHECK(ol.find("bad\\ufffdname\\ufffd(ok\xc3\xa9\\ufffd\\ufffd\\ufffd.sfz") != std::string::npos);
  for (unsigned char ch : ol) CHECK(ch != 0xff);
}

void test_walk_and_dedupe() {
  Env e("walk");
  fs::create_directories(e.src / "lib/samples");
  fs::copy_file(e.src / "samples/a.wav", e.src / "lib/samples/a.wav");
  write_file(e.src / "lib/a.sfz", "<region> sample=samples/a.wav key=60\n");
  fs::create_symlink(e.src / "lib/a.sfz", e.src / "lib/link.sfz");
  BatchOptions bo;
  bo.root = e.root.string();
  bo.inputs = {(e.src / "lib/a.sfz").string(), (e.src / "lib/./a.sfz").string(), (e.src / "lib/link.sfz").string(),
               (e.src / "lib/../lib/a.sfz").string()};
  CHECK(expand_inputs(bo.inputs).size() == 1);
  const BatchResult b = import_batch(bo);
  CHECK(b.files.size() == 1 && b.imported == 1);
  // The directory form lists the symlink and the file once each only when they are different files:
  // here both resolve to a.sfz.
  bo.inputs = {(e.src / "lib").string(), (e.src / "lib/a.sfz").string()};
  CHECK(expand_inputs(bo.inputs).size() == 1);
  // A directory that cannot be listed completely is a failed entry naming it; the other files import.
  if (::geteuid() != 0) {
    fs::create_directories(e.src / "lib/zlocked");
    write_file(e.src / "lib/zlocked/hidden.sfz", "<region> sample=../samples/a.wav key=60\n");
    ::chmod((e.src / "lib/zlocked").c_str(), 0);
    BatchOptions wo;
    wo.root = e.root.string();
    wo.inputs = {(e.src / "lib").string()};
    const BatchResult w = import_batch(wo);
    ::chmod((e.src / "lib/zlocked").c_str(), 0755);
    bool named = false;
    for (const auto& f : w.files)
      if (!f.result.ok() && f.result.status == Status::kIo && f.sfz_path.find("lib") != std::string::npos &&
          !f.result.reasons.empty() && f.result.reasons[0].find("cannot list directory") != std::string::npos)
        named = true;
    CHECK(named && w.failed >= 1 && !w.all_ok());
    bool good = false;
    for (const auto& f : w.files) if (f.sfz_path.find("a.sfz") != std::string::npos && f.result.ok()) good = true;
    CHECK(good);  // files found before the error are not dropped
  }
}

void test_batch_cli() {
  Env e("batchcli");
  fs::create_directories(e.src / "lib/samples");
  fs::copy_file(e.src / "samples/a.wav", e.src / "lib/samples/a.wav");
  write_file(e.src / "lib/good.sfz", "<region> sample=samples/a.wav key=60\n");
  write_file(e.src / "lib/bad.sfz", "<region> sample=samples/a.wav key=60 lfo01_freq=2\n");
  const std::string exe = LIBRARIAN_EXE;
  const std::string base = "'" + exe + "' import '" + (e.src / "lib").string() + "' --root '" + e.root.string() + "'";
  const std::string rep = (e.dir / "report.jsonl").string();
  int rc = std::system((base + " --report '" + rep + "' >/dev/null 2>&1").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1);  // one file was not imported
  const std::string text = slurp(rep);
  CHECK(text.find("\"status\":\"imported\"") != std::string::npos && text.find("\"reason\":\"unsupported_features\"") != std::string::npos);
  CHECK(text.find("\"opcode\":\"lfo01_freq\"") != std::string::npos && text.find("{\"type\":\"summary\",\"files\":2,\"imported\":1") != std::string::npos);
  std::size_t libs = 0;
  for (const auto& n : names(e.lib())) if (n != ".staging") ++libs;
  CHECK(libs == 1);
  // Text output on stdout: batch (lone directory) format with per-file lines and a summary.
  {
    Env t("batchtxt");
    fs::create_directories(t.src / "lib/samples");
    fs::copy_file(t.src / "samples/a.wav", t.src / "lib/samples/a.wav");
    write_file(t.src / "lib/good.sfz", "<region> sample=samples/a.wav key=60\n");
    write_file(t.src / "lib/bad.sfz", "<region> sample=samples/a.wav lfo01_freq=2\n");
    const std::string tb = "'" + exe + "' import '" + (t.src / "lib").string() + "' --root '" + t.root.string() + "'";
    const std::string o1 = (t.dir / "o1.txt").string(), e1 = (t.dir / "e1.txt").string();
    rc = std::system((tb + " >'" + o1 + "' 2>'" + e1 + "'").c_str());
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1);
    const std::string out1 = slurp(o1), err1 = slurp(e1);
    CHECK(out1.find((t.src / "lib/good.sfz").string() + ": published library_id ") != std::string::npos);
    CHECK(out1.find(" bundle_id ") != std::string::npos && out1.find(" samples 1 regions 1\n") != std::string::npos);
    CHECK(out1.find("2 file(s): 1 imported, 0 unchanged, 1 skipped, 0 failed\n") != std::string::npos);
    CHECK(out1.find("bad.sfz") == std::string::npos);  // skipped files are on stderr
    CHECK(err1.find("bad.sfz") != std::string::npos && err1.find("unsupported_features") != std::string::npos &&
          err1.find("unsupported opcode 'lfo01_freq'") != std::string::npos);
    // Second run: the good file is unchanged.
    rc = std::system((tb + " >'" + o1 + "' 2>/dev/null").c_str());
    CHECK(slurp(o1).find(": unchanged library_id ") != std::string::npos &&
          slurp(o1).find("2 file(s): 0 imported, 1 unchanged, 1 skipped, 0 failed\n") != std::string::npos);
    // A single plain file keeps the Story 1.13 output, exactly.
    const std::string sb = "'" + exe + "' import '" + (t.src / "lib/good.sfz").string() + "' --root '" + t.root.string() + "'";
    rc = std::system((sb + " >'" + o1 + "' 2>/dev/null").c_str());
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
    std::istringstream is(slurp(o1));
    std::vector<std::string> ls;
    for (std::string l; std::getline(is, l);) ls.push_back(l);
    CHECK(ls.size() == 5 && ls[0].rfind("library_id ", 0) == 0 && ls[1].rfind("bundle_id ", 0) == 0 &&
          ls[2] == "samples 1" && ls[3] == "regions 1" && ls[4] == "unchanged");
    // An unwritable report: reasons and summary are still printed, exit code 1.
    const std::string rb = tb + " --report '" + (t.dir / "nodir/r.jsonl").string() + "'";
    rc = std::system((rb + " >'" + o1 + "' 2>'" + e1 + "'").c_str());
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1);
    CHECK(slurp(e1).find("cannot write report") != std::string::npos && slurp(e1).find("bad.sfz") != std::string::npos);
    CHECK(slurp(o1).find("2 file(s):") != std::string::npos);
  }
  // Report on stdout.
  const std::string out = (e.dir / "stdout.txt").string();
  rc = std::system((base + " --report - >'" + out + "' 2>/dev/null").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1);
  const std::string so = slurp(out);
  CHECK(so.rfind("{\"type\":\"file\"", 0) == 0 && so.find("\"unchanged\":1") != std::string::npos);
  // Several explicit files; all good = exit 0.
  const std::string two = "'" + exe + "' import '" + (e.src / "lib/good.sfz").string() + "' '" + (e.src / "lib/good.sfz").string() +
                          "' --root '" + e.root.string() + "' >/dev/null";
  CHECK(std::system(two.c_str()) == 0);
  // An unwritable report path is an error, not a silent loss.
  rc = std::system(("'" + exe + "' import '" + (e.src / "lib/good.sfz").string() + "' --root '" + e.root.string() + "' --report '" +
                    (e.dir / "nodir/r.jsonl").string() + "' >/dev/null 2>&1").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1);
  // Usage errors stay exit code 2.
  rc = std::system(("'" + exe + "' import '" + (e.src / "lib/good.sfz").string() + "' --root '" + e.root.string() + "' --bogus x >/dev/null 2>&1").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
}

}  // namespace

int main() {
  test_per_opcode();
  test_combined_and_inheritance();
  test_unextended_stays_epic1();
  test_unsupported_features();
  test_allowlist();
  test_damaged();
  test_warnings_and_misc();
  test_walk_and_dedupe();
  test_batch();
  test_batch_cli();
  if (g_fail) std::printf("%d check(s) failed\n", g_fail);
  else std::printf("librarian_opcode_test: all passed\n");
  return g_fail ? 1 : 0;
}

// librarian tests (Story 1.13): synthetic SFZ + generated WAVs, no committed binaries.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "test_util.hpp"
#include "velora/format/sfz_import.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;
using namespace velora;
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

// WAV from interleaved exchange-convention PCM.
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

void write_sample(const fs::path& p, const fmt::PcmFormat& f, std::uint64_t frames, std::uint64_t seed) {
  write_bytes(p, make_wav(f, vt::make_int(f, frames, vt::Kind::kMixed, seed)));
}

struct Env {
  fs::path dir, src, root;
  explicit Env(const char* name) {
    dir = fs::temp_directory_path() / (std::string("velora_librarian_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    src = dir / "src";
    root = dir / "data";
    fs::create_directories(src);
    fs::create_directories(root);
  }
  ~Env() { std::error_code ec; fs::remove_all(dir, ec); }
  ImportOptions opts(const char* sfz = "Piano.sfz") const {
    ImportOptions o;
    o.sfz_path = (src / sfz).string();
    o.root = root.string();
    return o;
  }
  fs::path lib() const { return root / "lib"; }
};

const fmt::PcmFormat k16s{2, 16, false, 48000};
const fmt::PcmFormat k24m{1, 24, false, 44100};
const fmt::PcmFormat kf32{2, 32, true, 96000};

// A small but feature-complete library: define, include, default_path, velocity layers,
// round robin, loop, release trigger, ampeg, note names, inheritance.
void make_demo_library(const Env& e, std::uint64_t seed = 1) {
  write_sample(e.src / "samples/a.wav", k16s, 9000, seed);
  write_sample(e.src / "samples/b.wav", k24m, 5000, seed + 10);
  write_bytes(e.src / "samples/rel.wav", make_wav(kf32, vt::make_float(2, 3000, seed + 20)));
  write_file(e.src / "inc/common.sfz",
             "<global>\nampeg_release=0.4\nvolume=-2\n");
  write_file(e.src / "Piano.sfz",
             "// demo\n"
             "#define $LOW 60\n"
             "<control>\n"
             "default_path=samples/\n"
             "#include \"inc/common.sfz\"\n"
             "<master>\n"
             "pan=10\n"
             "<group>\n"
             "lovel=0 hivel=63\n"
             "loop_mode=loop_continuous loop_start=100 loop_end=1500\n"
             "<region> sample=a.wav key=$LOW seq_length=2 seq_position=1 tune=-5 volume=-3\n"
             "<region> sample=a.wav key=$LOW seq_length=2 seq_position=2 offset=10\n"
             "<group>\n"
             "lovel=64 hivel=127 ampeg_attack=0.01 ampeg_decay=0.2 ampeg_sustain=80\n"
             "<region> sample=b.wav lokey=c4 hikey=e4 pitch_keycenter=62 lorand=0.25 hirand=0.75 transpose=2 pan=-20\n"
             "<region> sample=b.wav lokey=f4 hikey=g#4 pitch_keycenter=db5 loop_mode=one_shot\n"
             "<group>\n"
             "trigger=release\n"
             "<region> sample=rel.wav key=72 ampeg_release=1.5 ampeg_hold=0.002 ampeg_delay=0.001 ampeg_start=5\n");
}

// Opens the bundle `current` names and fully verifies it. Returns its bundle_id ("" if absent).
std::string current_bundle(const Env& e, const std::string& library_id, bool* bad = nullptr) {
  const fs::path link = e.lib() / library_id / "current";
  std::error_code ec;
  if (!fs::is_symlink(link, ec)) return {};
  const std::string target = fs::read_symlink(link, ec).string();
  auto r = fmt::BundleReader::open_file((e.lib() / library_id / target / "bundle.vlb").string());
  if (!r || r.value().verify_all() != fmt::Error::kNone || fmt::to_hex(r.value().bundle_id()) != target) {
    if (bad) *bad = true;
    return "?";
  }
  return target;
}

std::set<std::string> list_names(const fs::path& dir) {
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

// ---------------------------------------------------------------------------------------------

void test_happy_import() {
  Env e("happy");
  make_demo_library(e);
  const auto parsed = fmt::parse_sfz_file((e.src / "Piano.sfz").string());
  CHECK(parsed.ok());
  CHECK(parsed.regions.size() == 5 && parsed.source_region_count == 5 && parsed.samples.size() == 3);
  CHECK(parsed.included_files.size() == 2);

  const ImportResult r = import_sfz(e.opts());
  CHECK(r.ok());
  if (!r.ok()) { for (auto& s : r.reasons) std::printf("  %s\n", s.c_str()); return; }
  CHECK(r.bundle_created && r.current_flipped && r.region_count == 5 && r.sample_count == 3);
  CHECK(r.bundle_dir == (e.lib() / r.library_id / r.bundle_id).string());
  CHECK(current_bundle(e, r.library_id) == r.bundle_id);

  // Region table equals the parsed region table, field by field.
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok());
  if (!b.ok()) return;
  const auto& rd = b.value();
  CHECK(rd.format_version() == fmt::kFormatVersion && rd.instruments().size() == 1);
  CHECK(rd.instruments()[0].key == "Piano.sfz" && rd.instruments()[0].name == "Piano");
  const auto& reg = rd.instruments()[0].regions;
  CHECK(reg.size() == parsed.regions.size());
  for (std::size_t i = 0; i < reg.size() && i < parsed.regions.size(); ++i) {
    const fmt::Region &a = reg[i], &p = parsed.regions[i];
    CHECK(a.sample == p.sample && a.key_lo == p.key_lo && a.key_hi == p.key_hi && a.key_center == p.key_center);
    CHECK(a.vel_lo == p.vel_lo && a.vel_hi == p.vel_hi && a.transpose == p.transpose);
    CHECK(a.tune_cents == p.tune_cents && a.volume_db == p.volume_db && a.pan == p.pan);
    CHECK(a.offset == p.offset && a.seq_length == p.seq_length && a.seq_position == p.seq_position);
    CHECK(a.lorand == p.lorand && a.hirand == p.hirand && a.loop_mode == p.loop_mode);
    CHECK(a.has_loop_points == p.has_loop_points && a.loop_start == p.loop_start && a.loop_end == p.loop_end);
    CHECK(a.ampeg == p.ampeg && a.trigger == p.trigger);
    CHECK(a == p);
  }
  // And the values themselves (inheritance global -> master -> group -> region).
  CHECK(reg[0].key_lo == 60 && reg[0].key_hi == 60 && reg[0].key_center == 60);
  CHECK(reg[0].vel_lo == 0 && reg[0].vel_hi == 63 && reg[0].seq_length == 2 && reg[0].seq_position == 1);
  CHECK(reg[0].tune_cents == -5.f && reg[0].volume_db == -3.f && reg[0].pan == 10.f);
  CHECK(reg[0].loop_mode == fmt::LoopMode::kLoopContinuous && reg[0].has_loop_points &&
        reg[0].loop_start == 100 && reg[0].loop_end == 1500);
  CHECK(reg[0].ampeg.release == 0.4f);
  CHECK(reg[1].seq_position == 2 && reg[1].offset == 10 && reg[1].volume_db == -2.f);
  CHECK(reg[2].key_lo == 60 && reg[2].key_hi == 64 && reg[2].key_center == 62 && reg[2].transpose == 2);
  CHECK(reg[2].vel_lo == 64 && reg[2].vel_hi == 127 && reg[2].pan == -20.f);
  CHECK(reg[2].lorand == 0.25f && reg[2].hirand == 0.75f && !reg[2].has_loop_points);
  CHECK(reg[2].loop_mode == fmt::LoopMode::kNoLoop);
  CHECK(reg[2].ampeg.attack == 0.01f && reg[2].ampeg.decay == 0.2f && reg[2].ampeg.sustain == 80.f);
  CHECK(reg[3].key_lo == 65 && reg[3].key_hi == 68 && reg[3].key_center == 73 && reg[3].loop_mode == fmt::LoopMode::kOneShot);
  CHECK(reg[4].trigger == fmt::TriggerMode::kRelease && reg[4].key_center == 72);
  CHECK(reg[4].ampeg.release == 1.5f && reg[4].ampeg.hold == 0.002f && reg[4].ampeg.delay == 0.001f &&
        reg[4].ampeg.start == 5.f && reg[4].sample == 2);
  // Samples: three different formats, all stored as they are.
  CHECK(rd.samples().size() == 3);
  CHECK(rd.samples()[0].format == k16s && rd.samples()[1].format == k24m && rd.samples()[2].format == kf32);
  CHECK(rd.samples()[0].name == "samples/a.wav");

  // The resolver opens the published bundle through the layout, without scanning.
  host::LibraryResolver resolver(host::make_posix_fs(), e.root.string());
  const auto rr = resolver.resolve(r.library_id, "Piano.sfz");
  CHECK(rr.ok());
  if (rr.ok()) CHECK(rr.instrument_info().regions.size() == 5);
  CHECK(resolver.resolve(r.library_id, "Other.sfz").status == host::ResolveStatus::kMissingInstrument);
  // Nothing but .staging (empty) and the library directory under lib/.
  CHECK(list_names(e.lib()) == (std::set<std::string>{".staging", r.library_id}));
  CHECK(list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));
  CHECK(list_names(e.lib() / r.library_id) == (std::set<std::string>{"current", r.bundle_id}));
}

// Story 13.3: end and the first / legato / release_key triggers import into the bundle (format 2.2) and read back unchanged.
void test_end_and_trigger_modes() {
  Env e("endtrig");
  make_demo_library(e);
  write_file(e.src / "Piano.sfz",
             "<region> sample=samples/a.wav key=60 end=100\n"
             "<region> sample=samples/a.wav key=61 end=0\n"
             "<region> sample=samples/a.wav key=62 trigger=first\n"
             "<region> sample=samples/a.wav key=62 trigger=legato end=999999999\n"
             "<region> sample=samples/a.wav key=62 trigger=release_key\n"
             "<region> sample=samples/a.wav key=63\n");
  const auto r = import_sfz(e.opts());
  CHECK(r.ok());
  if (!r.ok()) return;
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok());
  if (!b.ok()) return;
  CHECK(b.value().format_minor() == 2);
  const auto& reg = b.value().instruments()[0].regions;
  CHECK(reg.size() == 6);
  if (reg.size() != 6) return;
  CHECK(reg[0].has_end && reg[0].end == 100 && reg[1].has_end && reg[1].end == 0 && !reg[5].has_end);
  CHECK(reg[2].trigger == fmt::TriggerMode::kFirst && reg[3].trigger == fmt::TriggerMode::kLegato && reg[3].end == 999999999);
  CHECK(reg[4].trigger == fmt::TriggerMode::kReleaseKey && reg[5].trigger == fmt::TriggerMode::kAttack);
}

// Story 13.5: CC gates, set_cc, *_oncc and <curve> import into the bundle (format 2.4), read back unchanged, and a library without them stays out of minor 4.
void test_cc_setup() {
  Env e("ccsetup");
  make_demo_library(e);
  write_file(e.src / "Piano.sfz",
             "<control>\nset_cc64=90 set_cc7=100 set_cc99=5\n"
             "<curve>\ncurve_index=9 v000=0 v064=0.9 v127=1\n<curve>\ncurve_index=11 v000=1 v127=0\n"
             "<region> sample=samples/a.wav key=60 locc64=64 hicc64=100\n"
             "<region> sample=samples/a.wav key=61 amplitude_oncc93=100 amplitude_curvecc93=9 ampeg_release_oncc100=2\n"
             "<region> sample=samples/a.wav key=62 pan_oncc131=50 tune_cc94=-100 offset_oncc73=3\n");
  const auto r = import_sfz(e.opts());
  CHECK(r.ok());
  if (!r.ok()) return;
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok());
  if (!b.ok()) return;
  CHECK(b.value().format_minor() == 4);
  const auto& ins = b.value().instruments()[0];
  CHECK(ins.regions.size() == 3);
  if (ins.regions.size() != 3) return;
  CHECK(ins.regions[0].cc.gate_count == 1 && ins.regions[0].cc.gates[0].cc == 64 && ins.regions[0].cc.gates[0].lo == 64.f);
  CHECK(ins.regions[1].cc.mod_count == 2 && ins.regions[1].cc.mods[0].curve == 9 && ins.regions[1].cc.mods[1].target == fmt::CcTarget::kEgRelease);
  CHECK(ins.regions[2].cc.mod_count == 3);
  // cc7 equals the engine default and cc99 is read by nothing: only cc64 is stored; only curve 9 is named by a region.
  CHECK(ins.cc_setup.defaults.size() == 1 && ins.cc_setup.defaults[0].cc == 64 && ins.cc_setup.defaults[0].value == 90.f);
  CHECK(ins.cc_setup.curves.size() == 1 && ins.cc_setup.curves[0].index == 9 && ins.cc_setup.curves[0].points.size() == 3);
  // The same library without any of it is a minor 0 bundle, exactly as before.
  Env plain("ccplain");
  make_demo_library(plain);
  write_file(plain.src / "Piano.sfz", "<control>\nset_cc99=5\n<curve>\ncurve_index=9 v000=0 v127=1\n<region> sample=samples/a.wav key=60\n");
  const auto p = import_sfz(plain.opts());
  CHECK(p.ok());
  if (!p.ok()) return;
  auto pb = fmt::BundleReader::open_file(p.bundle_dir + "/bundle.vlb");
  CHECK(pb.ok() && pb.value().format_minor() == 0 && !pb.value().instruments()[0].cc_setup.any());
}

void test_category() {
  Env e("category");
  make_demo_library(e);
  ImportOptions o = e.opts();
  o.category = "Piano";
  const auto r = import_sfz(o);
  CHECK(r.ok());
  std::ifstream in(fs::path(r.bundle_dir) / "category");
  std::string line;
  std::getline(in, line);
  CHECK(line == "category.piano");  // the stable key (Story 5.5), whatever spelling was passed
  CHECK(r.suggested_category == fmt::Category::kPiano);  // from Piano.sfz; a suggestion only
  // Re-import with another category refreshes it; the bundle stays the same.
  o.category = "Keys";
  const auto r2 = import_sfz(o);
  CHECK(r2.ok() && r2.bundle_id == r.bundle_id && !r2.bundle_created);
  std::ifstream in2(fs::path(r.bundle_dir) / "category");
  std::getline(in2, line);
  CHECK(line == "category.keys");
  o.category = "bad/name";
  CHECK(import_sfz(o).status == Status::kInvalidArgument);
  o.category = "none";
  CHECK(import_sfz(o).status == Status::kInvalidArgument);
}

void test_unsupported_opcode() {
  Env e("unsup");
  make_demo_library(e);
  const auto first = import_sfz(e.opts());
  CHECK(first.ok());
  // Same source path, now with unsupported opcodes in two places (and one unsupported header).
  write_file(e.src / "Piano.sfz",
             "<group>\nlovel=1\n"
             "<region> sample=samples/a.wav key=60 amp_random=3\n"
             "<region> sample=samples/a.wav key=61 lfo01_freq=3\n"
             "<effect>\ntype=lofi\n"
             "<region> sample=samples/b.wav key=62\n");
  const auto r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped);
  CHECK(mentions(r, "Piano.sfz:3: unsupported opcode 'amp_random'"));
  CHECK(mentions(r, "Piano.sfz:4: unsupported opcode 'lfo01_freq'"));
  CHECK(mentions(r, "Piano.sfz:5: unsupported header <effect>"));
  CHECK(r.reasons.size() == 3);
  CHECK(r.library_id.empty() && r.bundle_id.empty());
  // Previous `current` intact, nothing new published.
  CHECK(current_bundle(e, first.library_id) == first.bundle_id);
  CHECK(list_names(e.lib() / first.library_id) == (std::set<std::string>{"current", first.bundle_id}));
  // trigger=bogus is an unsupported value, not silently accepted.
  write_file(e.src / "Piano.sfz", "<region> sample=samples/a.wav key=60 trigger=bogus\n");
  const auto r2 = import_sfz(e.opts());
  CHECK(r2.status == Status::kSkipped && mentions(r2, "Piano.sfz:1: unsupported value 'bogus'"));
  // A bad number is reported with its location too.
  write_file(e.src / "Piano.sfz", "<region> sample=samples/a.wav\npan=abc\n");
  const auto r3 = import_sfz(e.opts());
  CHECK(r3.status == Status::kSkipped && mentions(r3, "Piano.sfz:2: invalid value 'abc' for opcode 'pan'"));
}

void test_missing_sample_and_includes() {
  Env e("missing");
  make_demo_library(e);
  write_file(e.src / "Piano.sfz", "<region> sample=samples/nope.wav key=60\n");
  auto r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "nope.wav") && mentions(r, "Piano.sfz:1"));
  CHECK(!fs::exists(e.lib()));  // nothing at all was created

  write_file(e.src / "Piano.sfz", "<region> key=60\n");
  r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "without a sample"));

  write_file(e.src / "Piano.sfz", "#include \"gone.sfz\"\n<region> sample=samples/a.wav key=60\n");
  r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "gone.sfz"));

  write_file(e.src / "Piano.sfz", "#include \"Piano.sfz\"\n<region> sample=samples/a.wav key=60\n");
  r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "include"));
  write_file(e.src / "x.sfz", "#include \"y.sfz\"\n");
  write_file(e.src / "y.sfz", "#include \"x.sfz\"\n");
  write_file(e.src / "Piano.sfz", "#include \"x.sfz\"\n<region> sample=samples/a.wav key=60\n");
  r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "include"));

  r = import_sfz([&] { auto o = e.opts("absent.sfz"); return o; }());
  CHECK(r.status == Status::kSkipped && mentions(r, "cannot open"));

  // A region whose offset or loop points lie beyond its sample is rejected, not clipped.
  write_file(e.src / "Piano.sfz", "<region> sample=samples/a.wav key=60 loop_start=0 loop_end=99999\n");
  r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && mentions(r, "Piano.sfz:1") && mentions(r, "beyond"));
}

void test_unsupported_audio() {
  Env e("audio");
  make_demo_library(e);
  // 8-bit PCM, 22.05 kHz, and a non-WAV file behind a .wav name.
  Bytes eight = make_wav({1, 16, false, 48000}, std::vector<std::int32_t>(100, 3));
  eight[34] = 8;  // bits per sample
  write_bytes(e.src / "samples/eight.wav", eight);
  write_sample(e.src / "samples/odd.wav", {1, 16, false, 22050}, 500, 5);
  write_file(e.src / "samples/fake.wav", "this is not audio");
  write_file(e.src / "Piano.sfz",
             "<region> sample=samples/eight.wav key=60\n<region> sample=samples/odd.wav key=61\n"
             "<region> sample=samples/fake.wav key=62\n<region> sample=samples/a.wav key=63\n");
  const auto r = import_sfz(e.opts());
  CHECK(r.status == Status::kSkipped && r.reasons.size() == 3);
  CHECK(mentions(r, "eight.wav") && mentions(r, "odd.wav") && mentions(r, "fake.wav"));
  CHECK(mentions(r, "unsupported audio"));
  CHECK(!fs::exists(e.lib() / ".staging") || list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));
  CHECK(list_names(e.lib()) == (std::set<std::string>{".staging"}));
}

void test_reimport_and_change() {
  Env e("reimport");
  make_demo_library(e);
  const auto a = import_sfz(e.opts());
  CHECK(a.ok() && a.bundle_created && a.current_flipped);
  std::vector<Stage> seen;
  ImportOptions rec = e.opts();
  rec.fault.on_stage = [&](Stage s) { seen.push_back(s); return HookAction::kContinue; };
  const auto again = import_sfz(rec);
  // Already current: no link is created, so the link hook does not fire.
  CHECK(std::find(seen.begin(), seen.end(), Stage::kLinkCreated) == seen.end());
  CHECK(std::find(seen.begin(), seen.end(), Stage::kFlipped) != seen.end());
  CHECK(again.ok() && again.library_id == a.library_id && again.bundle_id == a.bundle_id);
  CHECK(!again.bundle_created && !again.current_flipped);
  CHECK(list_names(e.lib() / a.library_id) == (std::set<std::string>{"current", a.bundle_id}));
  CHECK(list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));

  // One sample changes: same library_id (source fingerprint is the SFZ's canonical path), new
  // bundle_id, `current` flips, the old bundle directory stays.
  write_sample(e.src / "samples/a.wav", k16s, 9000, 99);
  const auto c = import_sfz(e.opts());
  CHECK(c.ok() && c.library_id == a.library_id && c.bundle_id != a.bundle_id);
  CHECK(c.bundle_created && c.current_flipped);
  CHECK(current_bundle(e, a.library_id) == c.bundle_id);
  CHECK(list_names(e.lib() / a.library_id) == (std::set<std::string>{"current", a.bundle_id, c.bundle_id}));
  // Reverting to the old content re-uses the old directory and flips back.
  make_demo_library(e);
  const auto back = import_sfz(e.opts());
  CHECK(back.ok() && back.bundle_id == a.bundle_id && !back.bundle_created && back.current_flipped);
  CHECK(current_bundle(e, a.library_id) == a.bundle_id);

  // A different source path is a different library.
  fs::copy(e.src, e.dir / "src2", fs::copy_options::recursive);
  ImportOptions o = e.opts();
  o.sfz_path = (e.dir / "src2" / "Piano.sfz").string();
  const auto other = import_sfz(o);
  CHECK(other.ok() && other.library_id != a.library_id);
  // A damaged existing bundle directory is never reused or overwritten silently.
  const fs::path bundle = e.lib() / other.library_id / other.bundle_id / "bundle.vlb";
  fs::resize_file(bundle, fs::file_size(bundle) - 5);
  const auto bad = import_sfz(o);
  CHECK(bad.status == Status::kIo && mentions(bad, "damaged"));
}

// Invariants that must hold after a crash at any point.
void check_crash_invariants(const Env& e, const std::string& library_id, const std::string& old_id,
                            const std::string& new_id, const char* what) {
  bool bad = false;
  const std::string cur = current_bundle(e, library_id, &bad);
  if (bad || (cur != old_id && cur != new_id)) {
    std::printf("FAIL crash invariant (%s): current='%s' old='%s' new='%s'\n", what, cur.c_str(),
                old_id.c_str(), new_id.c_str());
    ++g_fail;
  }
  // lib/<id>/ only ever holds complete bundle directories, `current` and temp links.
  std::error_code ec;
  for (fs::directory_iterator it(e.lib() / library_id, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n == "current" || n.rfind(".current.tmp", 0) == 0) continue;
    auto r = fmt::BundleReader::open_file((it->path() / "bundle.vlb").string());
    if (!r || r.value().verify_all() != fmt::Error::kNone || fmt::to_hex(r.value().bundle_id()) != n) {
      std::printf("FAIL partial bundle under lib/<id>/ after crash (%s): %s\n", what, n.c_str());
      ++g_fail;
    }
  }
}

// Runs the import in a child that _exit()s at `at` (a real kill); returns true if it did.
bool import_killed_at(const ImportOptions& base, Stage at) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    ImportOptions o = base;
    o.fault.on_stage = [at](Stage s) {
      if (s == at) ::_exit(77);
      return HookAction::kContinue;
    };
    const ImportResult r = import_sfz(o);
    ::_exit(r.ok() ? 0 : 1);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 77;
}

void test_kill_at_every_stage() {
  for (bool with_previous : {false, true}) {
    for (int s = 0; s <= static_cast<int>(Stage::kFlipped); ++s) {
      const Stage at = static_cast<Stage>(s);
      Env e(with_previous ? "killprev" : "killnew");
      make_demo_library(e);
      std::string old_id;
      std::string lib_id;
      if (with_previous) {
        const auto first = import_sfz(e.opts());
        CHECK(first.ok());
        old_id = first.bundle_id;
        lib_id = first.library_id;
        write_sample(e.src / "samples/b.wav", k24m, 5000, 77);  // new content
      }
      const std::string what = std::string(with_previous ? "update" : "first") + "@" + to_string(at);
      CHECK(import_killed_at(e.opts(), at));
      // The target library id is deterministic: learn it from a clean import into another root.
      Env probe("killprobe");
      fs::copy(e.src, probe.src, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
      ImportOptions po = probe.opts();
      po.sfz_path = (e.src / "Piano.sfz").string();  // same canonical source path => same library_id
      const auto pr = import_sfz(po);
      CHECK(pr.ok());
      if (lib_id.empty()) lib_id = pr.library_id;
      CHECK(pr.library_id == lib_id);
      if (fs::exists(e.lib() / lib_id)) check_crash_invariants(e, lib_id, old_id, pr.bundle_id, what.c_str());
      const std::string cur = current_bundle(e, lib_id);
      if (at <= Stage::kLinkCreated) CHECK(cur == old_id);  // before the flip: previous (or none)
      if (at == Stage::kFlipped) CHECK(cur == pr.bundle_id);
      // Staging leftovers never appear under lib/<id>/.
      for (const auto& n : list_names(e.lib() / lib_id))
        CHECK(n == "current" || n == old_id || n == pr.bundle_id || n.rfind(".current.tmp", 0) == 0);
      // Recovery on the next run: it succeeds, ends on the new bundle and leaves no garbage.
      const auto rec = import_sfz(e.opts());
      CHECK(rec.ok() && rec.bundle_id == pr.bundle_id && rec.library_id == lib_id);
      CHECK(current_bundle(e, lib_id) == pr.bundle_id);
      CHECK(list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));
      for (const auto& n : list_names(e.lib() / lib_id))
        CHECK(n == "current" || n == old_id || n == pr.bundle_id);
    }
  }
}

void test_abort_hook() {
  // The in-process abort simulates a kill: it must not clean up after itself.
  Env e("abort");
  make_demo_library(e);
  ImportOptions o = e.opts();
  o.fault.on_stage = [](Stage s) { return s == Stage::kFsynced ? HookAction::kAbort : HookAction::kContinue; };
  const auto r = import_sfz(o);
  CHECK(r.status == Status::kAborted);
  CHECK(list_names(e.lib() / ".staging").size() == 2);  // lock + the orphaned staging dir
  CHECK(!fs::exists(e.lib() / r.library_id));            // nothing published
  const auto again = import_sfz(e.opts());
  CHECK(again.ok() && again.reconciled == 1);
  CHECK(list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));
  CHECK(reconcile_staging(e.root.string()) == 0);
}

void test_corrupt_staged_sample() {
  for (bool previous : {false, true}) {
    Env e("corrupt");
    make_demo_library(e);
    std::string old_id, lib_id;
    if (previous) {
      const auto f = import_sfz(e.opts());
      old_id = f.bundle_id;
      lib_id = f.library_id;
      write_sample(e.src / "samples/a.wav", k16s, 9000, 123);
    }
    ImportOptions o = e.opts();
    o.fault.corrupt_staged_before_verify = true;
    const auto r = import_sfz(o);
    CHECK(r.status == Status::kVerifyFailed);
    CHECK(list_names(e.lib() / ".staging") == (std::set<std::string>{".lock"}));
    if (previous) {
      CHECK(current_bundle(e, lib_id) == old_id);
      CHECK(list_names(e.lib() / lib_id) == (std::set<std::string>{"current", old_id}));
    } else {
      CHECK(list_names(e.lib()) == (std::set<std::string>{".staging"}));
    }
  }
}

void test_cross_device_and_lock() {
  Env e("xdev");
  make_demo_library(e);
  ImportOptions o = e.opts();
  o.fault.pretend_cross_device = true;
  const auto r = import_sfz(o);
  CHECK(r.status == Status::kCrossDevice && mentions(r, "filesystem"));
  CHECK(list_names(e.lib()) == (std::set<std::string>{".staging"}));
  // A second importer while the lock is held is refused.
  ImportOptions held = e.opts();
  Status inner = Status::kOk;
  held.fault.on_stage = [&](Stage s) {
    if (s == Stage::kStaged) inner = import_sfz(e.opts()).status;
    return HookAction::kContinue;
  };
  // flock is per open file description, so the nested attempt in the same process conflicts too.
  CHECK(import_sfz(held).ok());
  CHECK(inner == Status::kBusy);
}

void test_cli() {
  Env e("cli");
  make_demo_library(e);
  const std::string exe = LIBRARIAN_EXE, texe = LIBRARIAN_TEST_EXE;
  const std::string base = "'" + exe + "' import '" + (e.src / "Piano.sfz").string() + "' --root '" + e.root.string() + "'";
  CHECK(std::system((base + " >/dev/null").c_str()) == 0);
  CHECK(std::system((base + " --category Keys >/dev/null").c_str()) == 0);
  CHECK(std::system(("'" + exe + "' >/dev/null 2>&1").c_str()) != 0);
  CHECK(std::system(("'" + exe + "' import --root '" + e.root.string() + "' >/dev/null 2>&1").c_str()) != 0);  // no file
  CHECK(std::system(("'" + exe + "' import '" + (e.src / "Piano.sfz").string() + "' --root '" + (e.dir / "typo").string() + "' >/dev/null 2>&1").c_str()) != 0);
  CHECK(!fs::exists(e.dir / "typo"));  // a mistyped root is not created
  // The shipped CLI has no fault hook: the env var is ignored.
  CHECK(std::system(("VELORA_LIBRARIAN_FAULT=renamed " + base + " >/dev/null 2>&1").c_str()) == 0);
  // Real kill of the binary after rename, before the flip: exit 137, current stays absent... then
  // recovers.
  Env k("cli2");
  make_demo_library(k);
  const std::string kb = "'" + exe + "' import '" + (k.src / "Piano.sfz").string() + "' --root '" + k.root.string() + "'";
  const std::string tb = "'" + texe + "' import '" + (k.src / "Piano.sfz").string() + "' --root '" + k.root.string() + "'";
  const int rc = std::system(("VELORA_LIBRARIAN_FAULT=renamed " + tb + " >/dev/null 2>&1").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 137);
  std::size_t links = 0;
  for (const auto& n : list_names(k.lib())) if (n != ".staging" && n != kServiceLockFileName) ++links;  // the CLI holds .service.lock (Story 5.6)
  CHECK(links == 1);  // the library dir exists with a complete bundle but no `current`
  for (const auto& n : list_names(k.lib())) if (n != ".staging" && n != kServiceLockFileName) CHECK(!fs::exists(k.lib() / n / "current"));
  CHECK(std::system((kb + " >/dev/null").c_str()) == 0);
  // Unsupported opcode: non-zero exit and reason on stderr.
  write_file(e.src / "Piano.sfz", "<region> sample=samples/a.wav amp_random=1\n");
  CHECK(std::system((base + " 2>/dev/null").c_str()) != 0);
}

void test_loops_and_paths() {
  Env e("loops");
  make_demo_library(e);  // a.wav = 9000 frames
  auto import_text = [&](const std::string& text) {
    write_file(e.src / "L.sfz", text);
    return import_sfz(e.opts("L.sfz"));
  };
  auto regions_of = [&](const ImportResult& r) {
    auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
    return b.ok() ? b.value().instruments()[0].regions : std::vector<fmt::Region>{};
  };
  {  // loop_start only: loops to the last frame
    const auto r = import_text("<region> sample=samples/a.wav key=60 loop_start=500\n");
    CHECK(r.ok());
    const auto g = regions_of(r);
    CHECK(g.size() == 1 && g[0].has_loop_points && g[0].loop_start == 500 && g[0].loop_end == 8999);
  }
  {  // loop_end only: starts at 0
    const auto r = import_text("<region> sample=samples/a.wav key=61 loop_end=700\n");
    const auto g = regions_of(r);
    CHECK(r.ok() && g.size() == 1 && g[0].has_loop_points && g[0].loop_start == 0 && g[0].loop_end == 700);
  }
  {  // loop mode without points loops the whole sample; one_shot/no_loop without points do not
    const auto r = import_text("<region> sample=samples/a.wav key=62 loop_mode=loop_continuous\n"
                               "<region> sample=samples/a.wav key=63 loop_mode=loop_sustain\n"
                               "<region> sample=samples/a.wav key=64 loop_mode=one_shot\n"
                               "<region> sample=samples/a.wav key=65\n");
    const auto g = regions_of(r);
    CHECK(r.ok() && g.size() == 4);
    if (g.size() == 4) {
      CHECK(g[0].has_loop_points && g[0].loop_start == 0 && g[0].loop_end == 8999);
      CHECK(g[1].has_loop_points && g[1].loop_end == 8999);
      CHECK(!g[2].has_loop_points && !g[3].has_loop_points);
    }
  }
  {  // genuine errors
    auto r = import_text("<region> sample=samples/a.wav key=60 loop_start=900 loop_end=100\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "loop_start > loop_end") && mentions(r, "L.sfz:1"));
    r = import_text("<region> sample=samples/a.wav key=60 loop_start=20000\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "loop_start"));
    r = import_text("<region> sample=samples/a.wav key=60 loop_end=9000\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "beyond"));
  }
  {  // bad values are reported once, even when many regions inherit them, and at levels without regions
    const auto r = import_text("<group>\nlovel=999\n<region> sample=samples/a.wav key=1\n<region> sample=samples/a.wav key=2\n"
                               "<master>\npan=500\n");
    CHECK(r.status == Status::kSkipped && r.reasons.size() == 2);
    CHECK(mentions(r, "L.sfz:2: invalid value '999'") && mentions(r, "L.sfz:6: invalid value '500'"));
  }
  {  // sample paths cannot leave the SFZ directory
    write_sample(e.dir / "outside.wav", k16s, 100, 3);
    auto r = import_text("<region> sample=../outside.wav key=1\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "leaves the SFZ directory") && mentions(r, "L.sfz:1"));
    r = import_text("<region> sample=" + (e.dir / "outside.wav").string() + " key=1\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "leaves the SFZ directory"));
    r = import_text("<control>\ndefault_path=../\n<region> sample=outside.wav key=1\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "L.sfz:2") && mentions(r, "leaves"));
    r = import_text("<control>\ndefault_path=samples/\n<region> sample=" + (e.src / "samples/a.wav").string() + " key=1\n");
    CHECK(r.status == Status::kSkipped && mentions(r, "combined with default_path"));
    // An absolute path inside the SFZ directory is fine.
    r = import_text("<region> sample=" + (e.src / "samples/a.wav").string() + " key=1\n");
    CHECK(r.ok());
  }
}

void test_base_dir_and_root() {
  Env e("base");
  make_demo_library(e);
  fs::create_directories(e.src / "sub");
  fs::copy_file(e.src / "Piano.sfz", e.src / "sub/Piano.sfz");
  // sub/Piano.sfz refers to samples/ relative to itself.
  fs::create_directories(e.src / "sub/samples");
  for (const char* n : {"a.wav", "b.wav", "rel.wav"}) fs::copy_file(e.src / "samples" / n, e.src / "sub/samples" / n);
  fs::create_directories(e.src / "sub/inc");
  fs::copy_file(e.src / "inc/common.sfz", e.src / "sub/inc/common.sfz");
  ImportOptions o = e.opts("sub/Piano.sfz");
  o.base_dir = e.src.string();
  const auto r = import_sfz(o);
  CHECK(r.ok());
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  CHECK(b.ok() && b.value().instruments()[0].key == "sub/Piano.sfz");
  // Default base: the SFZ's own directory.
  const auto d = import_sfz(e.opts("sub/Piano.sfz"));
  auto bd = fmt::BundleReader::open_file(d.bundle_dir + "/bundle.vlb");
  CHECK(d.ok() && bd.ok() && bd.value().instruments()[0].key == "Piano.sfz");
  // SFZ outside the base directory.
  o.base_dir = (e.src / "sub").string();
  o.sfz_path = (e.src / "Piano.sfz").string();
  CHECK(import_sfz(o).status == Status::kInvalidArgument);
  // The data root must exist.
  ImportOptions bad = e.opts();
  bad.root = (e.dir / "typo").string();
  CHECK(import_sfz(bad).status == Status::kInvalidArgument && !fs::exists(e.dir / "typo"));
}

void test_salamander_like_fixture() {
  Env e("fixture");
  fs::copy_file(std::string(LIBRARIAN_FIXTURE_DIR) + "/salamander_like.sfz", e.src / "Piano.sfz");
  for (const char* n : {"A0v1", "C1v1", "A0v2", "C1v2", "rel1", "rel2", "rel3"})
    write_sample(e.src / "samples" / (std::string(n) + ".wav"), k16s, 5000, 7 + std::strlen(n));
  const auto r = import_sfz(e.opts());
  CHECK(r.ok() && r.region_count == 7 && r.source_region_count == 7 && r.sample_count == 7);
  auto b = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  if (b.ok() && b.value().instruments()[0].regions.size() == 7) {
    const auto& g = b.value().instruments()[0].regions;
    CHECK(g[0].vel_lo == 1 && g[0].vel_hi == 40 && g[0].tune_cents == 3.f && g[0].ampeg.release == 0.6f);
    CHECK(g[2].vel_lo == 41 && g[2].vel_hi == 80);
    CHECK(g[4].trigger == fmt::TriggerMode::kRelease && g[4].volume_db == -12.f && g[4].ampeg.release == 0.1f);
    CHECK(g[6].seq_length == 2 && g[6].seq_position == 2);
  } else {
    CHECK(false);
  }
}

}  // namespace

int main() {
  test_happy_import();
  test_category();
  test_unsupported_opcode();
  test_end_and_trigger_modes();
  test_cc_setup();
  test_missing_sample_and_includes();
  test_unsupported_audio();
  test_reimport_and_change();
  test_kill_at_every_stage();
  test_abort_hook();
  test_corrupt_staged_sample();
  test_cross_device_and_lock();
  test_loops_and_paths();
  test_base_dir_and_root();
  test_salamander_like_fixture();
  test_cli();
  if (g_fail) std::printf("%d check(s) failed\n", g_fail);
  else std::printf("librarian_test: all passed\n");
  return g_fail ? 1 : 0;
}

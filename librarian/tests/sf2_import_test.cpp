// Story 5.3 tests: SF2 import through the librarian. Fixtures come from the test-only SF2 writer
// (formatlib/tests/sf2_builder.hpp); nothing is committed. Covers one SF2 = one library with one
// instrument per preset, bit-exact samples through the bundle, library identity, damaged and
// unsupported files (skipped, nothing written, batch continues), the ignored_sf2_feature warnings
// in the report, the CLI and the shared publish protocol (verify failure, abort, re-import).
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "sf2_builder.hpp"
#include "test_util.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/sf2_import.hpp"
#include "velora/librarian/batch.hpp"
#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;
using namespace velora::librarian;
namespace fmt = velora::format;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Env {
  fs::path dir, src, root;
  explicit Env(const char* name) {
    dir = fs::temp_directory_path() / (std::string("velora_sf2_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    src = dir / "src";
    root = dir / "data";
    fs::create_directories(src);
    fs::create_directories(root);
  }
  ~Env() { std::error_code ec; fs::remove_all(dir, ec); }
  void put(const std::string& name, const Bytes& b) const {
    fs::create_directories((src / name).parent_path());
    std::ofstream(src / name, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  }
  ImportOptions opts(const std::string& name) const {
    ImportOptions o;
    o.sfz_path = (src / name).string();
    o.root = root.string();
    return o;
  }
  fs::path lib() const { return root / "lib"; }
};

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
std::set<std::string> names(const fs::path& dir) {
  std::set<std::string> s;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) s.insert(it->path().filename().string());
  return s;
}
bool nothing_written(const Env& e) {
  if (!fs::exists(e.lib())) return true;
  const auto top = names(e.lib());
  if (!(top.empty() || top == std::set<std::string>{".staging"})) return false;
  return !fs::exists(e.lib() / ".staging") || names(e.lib() / ".staging") <= std::set<std::string>{".lock"};
}

// The library_id the importer must mint: UUIDv5 of SHA-256("velora.source.v1" || 0 || canonical path).
std::string expected_library_id(const fs::path& file) {
  std::error_code ec;
  const std::string id = fs::weakly_canonical(fs::absolute(file, ec), ec).generic_string();
  fmt::Sha256 h;
  const std::string tag = "velora.source.v1";
  h.update(tag.data(), tag.size());
  const std::uint8_t zero = 0;
  h.update(&zero, 1);
  h.update(id.data(), id.size());
  const auto d = h.finish();
  return fmt::uuid_to_string(fmt::library_id_from_fingerprint(d));
}

void test_gm_import(bool bits24) {
  Env e(bits24 ? "gm24" : "gm16");
  sf2b::Fixture f = sf2b::make_gm(bits24);
  e.put("gm.sf2", f.b.build());
  const ImportResult r = import_sf2(e.opts("gm.sf2"));
  CHECK(r.ok());
  for (const auto& m : r.reasons) std::printf("  %s\n", m.c_str());
  if (!r.ok()) return;
  CHECK(r.sf2 && r.preset_count == 6 && r.bundle_created && r.current_flipped);
  CHECK(r.library_id == expected_library_id(e.src / "gm.sf2"));
  CHECK(r.region_count == 3 + 3 + 1 + 2 + 3 + 1);
  CHECK(r.sample_count == 2 + 2 + 1 + 2 + 1 + 1);  // shared samples are stored once per preset
  // Only the default velocity -> filter cutoff modulator is dropped, once for each of the two presets that have a filter.
  CHECK(r.warnings.size() == 2 && r.ignored_generators == 0 && r.ignored_modulators == 2);
  for (const auto& w : r.warnings) CHECK(w.code == "ignored_sf2_feature" && w.opcode.find("velocity -> initialFilterFc") != std::string::npos);

  // The published bundle: resolved through `current`, one instrument per preset, regions and PCM exact.
  const fs::path cur = e.lib() / r.library_id / "current" / "bundle.vlb";
  auto rd = fmt::BundleReader::open_file(cur.string());
  CHECK(rd.ok());
  if (!rd) return;
  CHECK(fmt::to_hex(rd.value().bundle_id()) == r.bundle_id);
  CHECK_ERR(rd.value().verify_all(), fmt::Error::kNone);
  const fmt::Sf2ParseResult p = fmt::parse_sf2_file((e.src / "gm.sf2").string());
  CHECK(p.ok());
  CHECK(rd.value().instruments().size() == 6);
  const char* keys[] = {"0:0", "0:1", "0:48", "8:0", "128:0", "1:5"};
  for (std::size_t i = 0; i < 6 && i < rd.value().instruments().size(); ++i) {
    const auto& ins = rd.value().instruments()[i];
    CHECK(ins.key == keys[i] && ins.name == p.presets[i].name);
    CHECK(ins.regions == p.presets[i].regions);
    CHECK(ins.sample_count == p.presets[i].samples.size());
    for (std::size_t k = 0; k < ins.sample_count; ++k) {
      const auto& si = rd.value().samples()[ins.first_sample + k];
      const auto& src = f.b.samples[p.presets[i].samples[k].shdr];
      std::vector<std::int32_t> got(si.frame_count);
      CHECK(si.format.bits_per_sample == (bits24 ? 24 : 16) && si.format.sample_rate == src.rate && si.format.channels == 1);
      CHECK_ERR(rd.value().read_frames(ins.first_sample + k, 0, si.frame_count, got), fmt::Error::kNone);
      CHECK(got == src.pcm);
    }
  }
  CHECK(rd.value().format_minor() == 1);  // the filter zones need the 2.1 extension table

  // The same file again: identical bundle, nothing new.
  const ImportResult again = import_sf2(e.opts("gm.sf2"));
  CHECK(again.ok() && !again.bundle_created && !again.current_flipped && again.bundle_id == r.bundle_id &&
        again.library_id == r.library_id);
  // Changed content of the same file: same library, new bundle.
  sf2b::Fixture g = sf2b::make_gm(bits24);
  g.b.samples[0].pcm[10] ^= 0x40;
  e.put("gm.sf2", g.b.build());
  const ImportResult changed = import_sf2(e.opts("gm.sf2"));
  CHECK(changed.ok() && changed.bundle_created && changed.library_id == r.library_id && changed.bundle_id != r.bundle_id);
  // import_file dispatches on the extension, case-insensitively.
  e.put("UP.SF2", f.b.build());
  const ImportResult up = import_file(e.opts("UP.SF2"));
  CHECK(up.ok() && up.sf2 && up.library_id != r.library_id);
  CHECK(is_sf2_path("a/b.Sf2") && !is_sf2_path("a.sfz") && !is_sf2_path("sf2"));
}

void test_single_instrument() {
  Env e("single");
  sf2b::Builder b;
  sf2b::add_sample(b, "solo", 4800, 100, 44100, 69, 0);
  sf2b::ZS z;
  z.modes = 1;
  sf2b::Instrument in;
  in.name = "Solo";
  in.zones = {sf2b::izone(0, z.gens())};
  b.instruments = {in};
  sf2b::add_preset(b, "Solo", 0, 0, {sf2b::pzone(0)});
  e.put("solo.sf2", b.build());
  const ImportResult r = import_sf2(e.opts("solo.sf2"));
  CHECK(r.ok() && r.preset_count == 1 && r.region_count == 1 && r.sample_count == 1 && r.warnings.empty());
  auto rd = fmt::BundleReader::open_file((e.lib() / r.library_id / "current" / "bundle.vlb").string());
  CHECK(rd.ok() && rd.value().instruments().size() == 1 && rd.value().instruments()[0].key == "0:0");
  if (rd.ok()) CHECK(rd.value().format_minor() == 0);  // no extension feature: the Epic 1 byte layout
}

void test_skipped_files() {
  sf2b::Fixture f = sf2b::make_gm(false);
  Bytes good = f.b.build();
  {  // damaged: bad RIFF size
    Env e("dmg");
    Bytes bad = good;
    sf2b::put32(bad, f.b.layout.riff_size, 0x7fffff00u);
    e.put("bad.sf2", bad);
    const ImportResult r = import_sf2(e.opts("bad.sf2"));
    CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kDamaged);
    CHECK(r.failing_path == (e.src / "bad.sf2").string() && r.reasons.size() == 1 && r.reasons[0].find("RIFF size") != std::string::npos);
    CHECK(nothing_written(e) && r.library_id.empty());
  }
  {  // damaged: a sample offset outside sdta (end beyond the data)
    Env e("dmg2");
    Bytes bad = good;
    sf2b::put32(bad, f.b.layout.shdr(0) + 24, 0x00ffffffu);
    e.put("bad.sf2", bad);
    const ImportResult r = import_sf2(e.opts("bad.sf2"));
    CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kDamaged && nothing_written(e));
    CHECK(r.reasons[0].find("beyond the sample data") != std::string::npos);
  }
  {  // a file that is not an SF2 at all, a truncated one, an unreadable one
    Env e("dmg3");
    e.put("junk.sf2", Bytes(100, 7));
    Bytes cut(good.begin(), good.begin() + 4000);
    e.put("cut.sf2", cut);
    CHECK(import_sf2(e.opts("junk.sf2")).reason_code == ReasonCode::kDamaged);
    CHECK(import_sf2(e.opts("cut.sf2")).reason_code == ReasonCode::kDamaged);
    const ImportResult missing = import_sf2(e.opts("nothere.sf2"));
    CHECK(missing.status == Status::kSkipped && missing.reason_code == ReasonCode::kDamaged);
    CHECK(nothing_written(e));
  }
  {  // unsupported: ROM sample, 22 kHz sample, SoundFont 3
    Env e("uns");
    sf2b::Fixture g = sf2b::make_gm(false);
    g.b.samples[0].rate = 22050;
    e.put("rate.sf2", g.b.build());
    const ImportResult r = import_sf2(e.opts("rate.sf2"));
    CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kUnsupportedFeatures);
    CHECK(r.offenders.size() == 1 && r.offenders[0].opcode == "sample" && r.offenders[0].message.find("22050") != std::string::npos);
    sf2b::Fixture h = sf2b::make_gm(false);
    h.b.ifil_major = 3;
    e.put("v3.sf2", h.b.build());
    const ImportResult v = import_sf2(e.opts("v3.sf2"));
    CHECK(v.reason_code == ReasonCode::kUnsupportedFeatures && v.offenders.size() == 1 && v.offenders[0].opcode == "ifil");
    sf2b::Fixture k = sf2b::make_gm(false);
    k.b.samples[4].type = 0x8001;
    e.put("rom.sf2", k.b.build());
    CHECK(import_sf2(e.opts("rom.sf2")).reason_code == ReasonCode::kUnsupportedFeatures);
    CHECK(nothing_written(e));
  }
  {  // no region at all
    Env e("empty");
    sf2b::Builder b;
    sf2b::add_sample(b, "s", 100, 5, 48000, 60, 0);
    sf2b::Instrument in;
    in.name = "I";
    in.zones = {sf2b::izone(0)};
    b.instruments = {in};
    sf2b::add_preset(b, "P", 0, 0, {});
    e.put("e.sf2", b.build());
    const ImportResult r = import_sf2(e.opts("e.sf2"));
    CHECK(r.status == Status::kSkipped && r.reason_code == ReasonCode::kDamaged && r.reasons[0].find("no regions") != std::string::npos);
    CHECK(nothing_written(e));
  }
  {  // argument errors
    Env e("args");
    ImportOptions o = e.opts("x.sf2");
    o.root = (e.dir / "norot").string();
    CHECK(import_sf2(o).status == Status::kInvalidArgument);
    o = e.opts("x.sf2");
    o.category = "a/b";
    CHECK(import_sf2(o).status == Status::kInvalidArgument);
    o.sfz_path.clear();
    CHECK(import_sf2(o).status == Status::kInvalidArgument);
  }
}

void test_warning_report() {
  Env e("warn");
  sf2b::Builder b;
  sf2b::add_sample(b, "s", 2000, 40, 48000, 60, 0);
  sf2b::ZS z;
  z.extra = {sf2b::g(sf2b::modLfoToPitch, 100), sf2b::g(sf2b::attackModEnv, -1200), sf2b::g(sf2b::sustainModEnv, 100),
             sf2b::g(sf2b::modEnvToFilterFc, 800)};
  z.mods = {sf2b::Mod{0x0093, 5, 100, 0, 0}};
  sf2b::Instrument weird, plain;
  weird.name = "Weird";
  weird.zones = {sf2b::izone(0, z.gens(), z.mods)};
  plain.name = "Plain";
  plain.zones = {sf2b::izone(0)};
  b.instruments = {weird, plain};
  sf2b::add_preset(b, "Weird", 0, 7, {sf2b::pzone(0)});
  sf2b::add_preset(b, "Plain", 0, 8, {sf2b::pzone(1)});
  e.put("w.sf2", b.build());
  const ImportResult r = import_sf2(e.opts("w.sf2"));
  CHECK(r.ok() && r.preset_count == 2 && r.region_count == 2);  // imports, never skipped for them
  CHECK(r.warnings.size() == 5 && r.ignored_generators == 4 && r.ignored_modulators == 1);
  std::set<std::string> items;
  for (const auto& w : r.warnings) {
    CHECK(w.code == "ignored_sf2_feature" && w.preset == "0:7" && w.file == (e.src / "w.sf2").string() && w.line == 0);
    items.insert(w.opcode);
  }
  CHECK(items == (std::set<std::string>{"modLfoToPitch", "attackModEnv", "sustainModEnv", "modEnvToFilterFc",
                                        "modulator src=0x0093 dest=modLfoToPitch amount=100 amtsrc=0x0000 trans=0"}));
  // The JSON Lines report.
  BatchOptions bo;
  bo.inputs = {(e.src / "w.sf2").string()};
  bo.root = e.root.string();
  const BatchResult br = import_batch(bo);
  const std::string text = report_jsonl(br);
  CHECK(br.imported + br.unchanged == 1 && br.skipped == 0);
  CHECK(text.find("\"code\":\"ignored_sf2_feature\",\"preset\":\"0:7\",\"opcode\":\"modLfoToPitch\"") != std::string::npos);
  CHECK(text.find("\"warning_count\":5") != std::string::npos);
  CHECK(text.find(",\"format\":\"sf2\",\"presets\":2,\"ignored_generators\":4,\"ignored_modulators\":1}") != std::string::npos);
  CHECK(text.find("\"preset\":\"0:8\"") == std::string::npos);  // the plain preset has no warning
}

void test_batch_and_cli() {
  Env e("batch");
  sf2b::Fixture f = sf2b::make_gm(false);
  Bytes good = f.b.build();
  e.put("lib/a_good.sf2", good);
  Bytes bad = good;
  sf2b::put32(bad, f.b.layout.shdr(1) + 20, 0x00ffffffu);  // start after end
  e.put("lib/b_damaged.sf2", bad);
  sf2b::Fixture g = sf2b::make_gm(true);
  e.put("lib/c_good24.SF2", g.b.build());
  sf2b::Fixture u = sf2b::make_gm(false);
  u.b.samples[0].rate = 8000;
  e.put("lib/d_rate.sf2", u.b.build());
  e.put("lib/sub/e_junk.sf2", Bytes(33, 1));
  BatchOptions bo;
  bo.inputs = {(e.src / "lib").string()};
  bo.root = e.root.string();
  const BatchResult br = import_batch(bo);
  CHECK(br.files.size() == 5 && br.imported == 2 && br.skipped == 3 && br.failed == 0 && !br.all_ok());
  // The batch continued past the damaged file: the good ones after it are published.
  std::size_t libs = 0;
  for (const auto& n : names(e.lib())) if (n != ".staging") ++libs;
  CHECK(libs == 2);
  const std::string text = report_jsonl(br);
  CHECK(text.find("b_damaged.sf2\",\"status\":\"skipped\",\"reason\":\"damaged\",\"failing_path\":\"") != std::string::npos);
  CHECK(text.find("d_rate.sf2\",\"status\":\"skipped\",\"reason\":\"unsupported_features\"") != std::string::npos);
  CHECK(text.find("{\"type\":\"summary\",\"files\":5,\"imported\":2,\"unchanged\":0,\"skipped\":3,\"failed\":0}") != std::string::npos);
  // An SFZ next to an SF2 is still an SFZ library: both kinds in one batch.
  // CLI: text and report.
  const std::string exe = LIBRARIAN_EXE;
  Env c("cli");
  c.put("one.sf2", good);
  const std::string base = "'" + exe + "' import '" + (c.src / "one.sf2").string() + "' --root '" + c.root.string() + "'";
  const std::string out = (c.dir / "out.txt").string();
  int rc = std::system((base + " >'" + out + "' 2>/dev/null").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
  const std::string so = slurp(out);
  CHECK(so.find("library_id " + expected_library_id(c.src / "one.sf2")) != std::string::npos);
  CHECK(so.find("samples 9\n") != std::string::npos && so.find("regions 13\n") != std::string::npos && so.find("published\n") != std::string::npos);
  rc = std::system((base + " --report - >'" + out + "' 2>/dev/null").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0 && slurp(out).find("\"status\":\"unchanged\"") != std::string::npos);
  // The damaged one exits 1 with the reason on stderr.
  c.put("two.sf2", bad);
  const std::string err = (c.dir / "err.txt").string();
  rc = std::system(("'" + exe + "' import '" + (c.src / "two.sf2").string() + "' --root '" + c.root.string() + "' >/dev/null 2>'" + err + "'").c_str());
  CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 1 && slurp(err).find("damaged") != std::string::npos && slurp(err).find("two.sf2") != std::string::npos);
}

void test_protocol() {
  sf2b::Fixture f = sf2b::make_gm(false);
  {  // a corrupted staged bundle is caught by verification; nothing is published
    Env e("verify");
    e.put("gm.sf2", f.b.build());
    ImportOptions o = e.opts("gm.sf2");
    o.fault.corrupt_staged_before_verify = true;
    const ImportResult r = import_sf2(o);
    CHECK(r.status == Status::kVerifyFailed);
    CHECK(nothing_written(e));
  }
  {  // abort right after the rename: the bundle is there, `current` is not
    Env e("abort");
    e.put("gm.sf2", f.b.build());
    ImportOptions o = e.opts("gm.sf2");
    o.fault.on_stage = [](Stage s) { return s == Stage::kRenamed ? HookAction::kAbort : HookAction::kContinue; };
    const ImportResult r = import_sf2(o);
    CHECK(r.status == Status::kAborted);
    const fs::path libdir = e.lib() / expected_library_id(e.src / "gm.sf2");
    CHECK(fs::exists(libdir) && !fs::exists(libdir / "current"));
    // Recovery: the next run completes the flip.
    const ImportResult again = import_sf2(e.opts("gm.sf2"));
    CHECK(again.ok() && again.current_flipped && !again.bundle_created);
  }
  // The verification re-reads the source: a file that changes while it is imported is caught by each of its checks.
  {
    struct Change { const char* name; void (*mutate)(sf2b::Fixture&); const char* needle; };
    const Change changes[] = {
        {"chg_count", [](sf2b::Fixture& g) { g.b.presets.pop_back(); }, "changed while it was imported"},
        {"chg_name", [](sf2b::Fixture& g) { g.b.presets[0].name = "Renamed"; }, "wrong key or name"},
        {"chg_regions", [](sf2b::Fixture& g) { g.b.instruments[0].zones[1].gens.push_back(sf2b::g(sf2b::fineTune, 7)); }, "region table"},
        {"chg_pcm", [](sf2b::Fixture& g) { g.b.samples[0].pcm[5] ^= 1; }, "not bit-identical"},
    };
    for (const Change& c : changes) {
      Env e(c.name);
      e.put("gm.sf2", f.b.build());
      ImportOptions o = e.opts("gm.sf2");
      o.fault.on_stage = [&](Stage st) {
        if (st == Stage::kStaged) {
          sf2b::Fixture g = sf2b::make_gm(false);
          c.mutate(g);
          e.put("gm.sf2", g.b.build());
        }
        return HookAction::kContinue;
      };
      const ImportResult r = import_sf2(o);
      bool named = false;
      for (const auto& why : r.reasons) named = named || why.find(c.needle) != std::string::npos;
      if (!named || r.status != Status::kVerifyFailed) std::printf("  change '%s': status %s\n", c.name, to_string(r.status));
      CHECK(r.status == Status::kVerifyFailed && named && nothing_written(e));
    }
  }
  {  // the category rides in the atomic publish
    Env e("cat");
    e.put("gm.sf2", f.b.build());
    ImportOptions o = e.opts("gm.sf2");
    o.category = "Piano";
    const ImportResult r = import_sf2(o);
    CHECK(r.ok() && slurp(e.lib() / r.library_id / "current" / "category") == "Piano\n");
  }
  {  // a lock held by another import
    Env e("lock");
    e.put("gm.sf2", f.b.build());
    ImportOptions o = e.opts("gm.sf2");
    bool inner_busy = false;
    o.fault.on_stage = [&](Stage s) {
      if (s == Stage::kStaged) inner_busy = import_sf2(e.opts("gm.sf2")).status == Status::kBusy;
      return HookAction::kContinue;
    };
    CHECK(import_sf2(o).ok());
    CHECK(inner_busy);
  }
}

}  // namespace

int main() {
  test_gm_import(false);
  test_gm_import(true);
  test_single_instrument();
  test_skipped_files();
  test_warning_report();
  test_batch_and_cli();
  test_protocol();
  if (g_fail) std::printf("%d check(s) failed\n", g_fail);
  else std::printf("librarian_sf2_test: all passed\n");
  return g_fail ? 1 : 0;
}

// Live Set load (Story 4.3): resolve, make resident, build ONE generation, reserve the voice pool, publish only when everything is
// resident. The library content is synthetic: small bundles built with formatlib's BundleWriter (a looping constant or a looping sine
// per instrument) laid out in a temp library root as the resolver expects (<root>/lib/<library_id>/current -> <bundle_id>/bundle.vlb).
// Rendering is offline and deterministic: the block renderer is driven directly, one block at a time.
//
// As in the engine/core part router tests, a part is told apart in the output by the level of its instrument (a constant): the level
// of a note is the sum of the levels of the parts that sound it.
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/fm_engine.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/va_engine.hpp"
#include "velora/core/ep_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/tier_profile.hpp"

namespace fs = std::filesystem;
using namespace velora;
using host::LiveSetLoadStatus;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                                       \
  do {                                                                                                              \
    const double va_ = (a), vb_ = (b);                                                                              \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                         \
      std::printf("FAIL %s:%d: %s = %.9g, expected %.9g (+-%g)\n", __FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
      ++g_fail;                                                                                                     \
    }                                                                                                               \
  } while (0)

namespace {

namespace ls = host::liveset;
namespace lsp = ls::lsp;
using fixtures::uuid_n;

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-liveload-XXXXXX").string();
    if (!mkdtemp(path.data())) {
      std::printf("mkdtemp failed\n");
      std::exit(1);
    }
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
};

// An instrument of the synthetic library: a looping constant (`level` > 0, `sine` false) or a looping 100 Hz sine at key 60.
struct Spec {
  std::string key;
  float level = 0.1f;
  bool sine = false;
};

// Installs one library with the given instruments under `root` and returns its library_id (canonical UUID string).
std::string install_library(const std::string& root, std::uint32_t index, const std::vector<Spec>& specs) {
  const std::string fp = "velora-liveset-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  for (const Spec& sp : specs) {
    auto ins = w.add_instrument(sp.key, sp.key);
    CHECK(ins.ok());
    std::vector<std::int32_t> pcm(4800);
    for (std::size_t i = 0; i < pcm.size(); ++i)
      pcm[i] = format::float_to_bits(sp.sine ? 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 480.0)) : sp.level);
    CHECK(w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, 4800, pcm}) == format::Error::kNone);
    format::Region r;
    r.sample = 0;
    r.key_lo = 0;
    r.key_hi = 127;
    r.key_center = 60;
    r.loop_mode = format::LoopMode::kLoopContinuous;
    CHECK(w.add_region(ins.value(), r) == format::Error::kNone);
  }
  auto bytes = w.finalize();
  CHECK(bytes.ok());
  auto probe = format::BundleReader::open_memory(bytes.value());
  CHECK(probe.ok());
  const std::string lib_id = format::uuid_to_string(lib);
  const std::string bundle_id = format::to_hex(probe.value().bundle_id());
  const fs::path dir = fs::path(root) / "lib" / lib_id / bundle_id;
  fs::create_directories(dir);
  std::ofstream f(dir / "bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  f.close();
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

// Wraps the RAM provider: counts loads and can fail the n-th one.
class TestHeads final : public host::HeadProvider {
 public:
  host::LoadStatus load(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out) override {
    ++calls;
    if (calls == fail_on) return host::LoadStatus::kBundleInconsistent;
    return ram.load(r, out);
  }
  host::RamHeadProvider ram;
  int calls = 0;
  int fail_on = 0;  // 1-based call number to fail; 0 = never
};

struct Rig {
  TmpDir tmp;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;  // the tier's generators and voice pool (v1: 8 parts, 640 voices)
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::LibraryResolver resolver;
  TestHeads heads;
  host::LiveSetLoader loader;
  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::vector<std::vector<float>> blocks;

  static core::SampleEngine::Config cfg(std::uint32_t voices = 340) {
    core::SampleEngine::Config c;
    c.max_voices = voices;  // the 256 voice floor plus the 64 voice fade headroom, with slack (the tier's sample_voice_capacity)
    return c;
  }
  explicit Rig(bool with_sample = true, host::LiveSetLoaderOptions opt = {})
      : Rig(with_sample, opt, host::v1_profile().sample_voice_capacity) {}
  // A rig of a tier (Story 4.1): the plane takes the profile, the loader its options, the sample engine its voice capacity -- exactly what engine-host does.
  explicit Rig(const host::TierProfile& tier) : Rig(true, host::loader_options_for(tier), tier.sample_voice_capacity, &tier) {}
  Rig(bool with_sample, host::LiveSetLoaderOptions opt, std::uint32_t voices, const host::TierProfile* tier = nullptr)
      : sample(cfg(voices)),
        organ((tier ? *tier : host::v1_profile()).part_count, (tier ? *tier : host::v1_profile()).organ_voice_capacity),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        resolver(host::make_posix_fs(), tmp.path),
        loader(*br, plane, resolver, heads, with_sample ? &sample : nullptr, opt) {
    if (tier) plane.set_tier_profile(*tier);
    if (with_sample) CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }
  void run(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) {
      br->process_block(l, r);
      blocks.emplace_back(l, l + core::kBlockFrames);
    }
  }
  void note_on(std::uint32_t key, std::uint32_t ch = 0) { CHECK(br->submit({core::make_note_on(0, ch, key, 65535), 0})); }
  void note_off(std::uint32_t key, std::uint32_t ch = 0) { CHECK(br->submit({core::make_note_off(0, ch, key, 0), 0})); }
  static double unity() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }
  double level() const {
    double s = 0;
    for (std::uint32_t i = core::kBlockFrames - 32; i < core::kBlockFrames; ++i) s += blocks.back()[i];
    return s / 32.0 / unity();
  }
  double play(std::uint32_t key) {
    note_on(key);
    run(6);
    const double v = level();
    note_off(key);
    run(80);
    return v;
  }
  double frequency(std::uint32_t nblocks) const {
    std::vector<float> x;
    for (std::size_t i = blocks.size() - nblocks; i < blocks.size(); ++i) x.insert(x.end(), blocks[i].begin(), blocks[i].end());
    std::uint32_t n = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
      if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return static_cast<double>(n) * 48000.0 / static_cast<double>(x.size());
  }
};

lsp::LiveSet base_set(std::uint64_t seed, std::uint32_t parts = 8) {
  return ls::make_default_live_set(uuid_n(seed), "Test set", parts);
}
void sample_part(lsp::LiveSet& s, int p, const std::string& lib, const std::string& key, int lo = 0, int hi = 127) {
  lsp::Part* part = s.mutable_parts(p);
  fixtures::timbre(part, lib, key);
  part->set_low_key(static_cast<std::uint32_t>(lo));
  part->set_high_key(static_cast<std::uint32_t>(hi));
  part->set_volume(1.0f);
}

// --- the happy paths ------------------------------------------------------------------------------------------

void test_layer_and_states() {
  Rig rig;
  const std::string piano = install_library(rig.tmp.path, 1, {{"piano.sfz", 0.1f}});
  const std::string pad = install_library(rig.tmp.path, 2, {{"pad.sfz", 0.01f}});
  lsp::LiveSet s = base_set(10);
  sample_part(s, 0, piano, "piano.sfz");
  sample_part(s, 1, pad, "pad.sfz");
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.revision == 1 && rig.plane.revision() == 1);
  CHECK(res.parts.size() == 8);
  CHECK(res.parts[0].state == core::PartState::kReady && res.parts[0].resolve == host::ResolveStatus::kOk);
  CHECK(res.parts[1].state == core::PartState::kReady);
  CHECK(res.parts[2].state == core::PartState::kEmptyTimbre);  // a sample part with no timbre
  CHECK(res.resident_bytes > 0);
  // The whole Live Set's reservation: two instruments, a sample floor of 256 voices, no organ.
  CHECK(rig.br->arbiter().reserved());
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) ==
        256ull * rig.sample.cost_units_per_voice());
  CHECK(rig.br->arbiter().reservation().organ_rotary == 0);
  CHECK(rig.br->arbiter().reservation().fx > 0);  // the fixed topology is always reserved
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.11, 1e-3);  // layer: both sound
  CHECK(rig.sample.active_voices() == 0);
  // The generation carries the part model.
  const core::Snapshot* g = rig.br->held_snapshot();
  CHECK(g && g->live_set && g->parts[0].sample_bank && g->parts[2].sample_bank == nullptr);
}

void test_split_b2_c3() {
  Rig rig;
  const std::string bass = install_library(rig.tmp.path, 1, {{"bass.sfz", 0.1f}});
  const std::string piano = install_library(rig.tmp.path, 2, {{"piano.sfz", 0.01f}});
  lsp::LiveSet s = base_set(11);
  sample_part(s, 0, bass, "bass.sfz", 0, 47);     // up to B2
  sample_part(s, 1, piano, "piano.sfz", 48, 127);  // from C3
  CHECK(rig.loader.load(s).ok());
  rig.run(2);
  CHECK_NEAR(rig.play(47), 0.1, 1e-3);
  CHECK_NEAR(rig.play(48), 0.01, 1e-3);
  CHECK_NEAR(rig.play(36), 0.1, 1e-3);
  CHECK_NEAR(rig.play(72), 0.01, 1e-3);
}

void test_keyboard_off_and_zone_pitch() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"sine.sfz", 0, true}, {"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(12);
  sample_part(s, 0, lib, "dc.sfz");
  sample_part(s, 1, lib, "dc.sfz");
  s.mutable_parts(1)->set_keyboard_on(false);
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(rig.heads.calls == 1);  // the same instrument in two parts is made resident once
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);  // part 1 has the keyboard off

  // Octave and transpose move the pitch, the zone is tested on the key as played.
  Rig r2;
  const std::string lib2 = install_library(r2.tmp.path, 1, {{"sine.sfz", 0, true}});
  lsp::LiveSet t = base_set(13);
  sample_part(t, 0, lib2, "sine.sfz", 48, 95);
  t.mutable_parts(0)->set_octave(1);
  t.mutable_parts(0)->set_transpose(2);
  CHECK(r2.loader.load(t).ok());
  r2.run(2);
  r2.note_on(60);
  r2.run(220);
  CHECK_NEAR(r2.frequency(200), 100.0 * std::pow(2.0, 14.0 / 12.0), 5.0);
  r2.note_off(60);
  r2.run(80);
  r2.note_on(47);
  r2.run(10);
  CHECK(r2.sample.active_voices() == 0);  // below the zone
}

void test_part_volume_output() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.2f}});
  lsp::LiveSet s = base_set(14);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(0)->set_volume(0.5f);
  CHECK(rig.loader.load(s).ok());
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);
}

void test_missing_library_and_instrument() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(15);
  sample_part(s, 0, lib, "dc.sfz");
  sample_part(s, 1, uuid_n(777), "ghost.sfz");   // a library that is not installed
  sample_part(s, 2, lib, "no-such-instrument.sfz");  // an installed library without that instrument
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());  // a part that does not resolve is silent, the Live Set still loads
  CHECK(res.parts[1].state == core::PartState::kMissingLibrary && res.parts[1].resolve == host::ResolveStatus::kMissingLibrary);
  CHECK(res.parts[2].state == core::PartState::kMissingLibrary && res.parts[2].resolve == host::ResolveStatus::kMissingInstrument);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);  // never a substitute: only part 0 sounds
  const core::Snapshot* g = rig.br->held_snapshot();
  CHECK(g && g->parts[1].state == core::PartState::kMissingLibrary && g->parts[1].sample_bank == nullptr);
}

void test_fm_part_silent_organ_ready() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(16);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(1)->mutable_engine()->set_engine_type_id(3);  // FM: reserved, no engine compiled in
  s.mutable_parts(2)->mutable_engine()->set_engine_type_id(4);  // VA
  lsp::Part* organ = s.mutable_parts(3);
  organ->mutable_engine()->set_engine_type_id(2);
  fixtures::entry(organ->mutable_engine()->mutable_params(), 100, 5);  // drawbar_16 of part 3
  fixtures::entry(organ->mutable_engine()->mutable_params(), 102, 7);  // drawbar_8
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts[1].state == core::PartState::kEngineNotAvailable);
  CHECK(res.parts[2].state == core::PartState::kEngineNotAvailable);
  CHECK(res.parts[3].state == core::PartState::kReady);  // an organ part needs no timbre
  // The organ's reservation joins the sample floor (the whole Live Set).
  CHECK(rig.br->arbiter().reservation().organ_rotary == core::OrganEngine::reservation_cost_units(1));
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) > 0);
  rig.run(2);
  // Engine sound parameters reached the generation's initial values.
  const core::params::ParamDef* d16 = core::params::find(core::params::kDrawbar16);
  const core::params::ParamDef* d8 = core::params::find(core::params::kDrawbar8);
  CHECK(rig.br->last_values().load(core::params::perf_slot(*d16, 3)) == 5.0f);
  CHECK(rig.br->last_values().load(core::params::perf_slot(*d8, 3)) == 7.0f);
  CHECK(rig.br->last_values().load(core::params::perf_slot(*d8, 0)) == static_cast<float>(d8->def));
  rig.note_on(60);
  rig.run(20);
  CHECK(rig.sample.active_voices() >= 1);
  CHECK(rig.organ.active_voices() >= 1);   // the organ part plays the same key
  rig.note_off(60);
  rig.run(200);
  CHECK(rig.sample.active_voices() == 0 && rig.organ.active_voices() == 0);
}

void test_no_sample_part_floor_zero() {
  Rig rig;
  lsp::LiveSet s = base_set(17);
  for (int p = 0; p < 8; ++p) s.mutable_parts(p)->mutable_engine()->set_engine_type_id(p == 0 ? 2 : 3);
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(rig.br->arbiter().reserved());
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) == 0);  // no sample part: the floor is zero
  CHECK(rig.br->arbiter().reservation().organ_rotary > 0);
  // And a sample-only set on the same engines needs no organ reservation.
  Rig two;
  const std::string lib = install_library(two.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet t = base_set(18);
  sample_part(t, 0, lib, "dc.sfz");
  CHECK(two.loader.load(t).ok());
  CHECK(two.br->arbiter().reservation().organ_rotary == 0);
}

void test_without_sample_engine() {
  Rig rig(/*with_sample=*/false);
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(19);
  sample_part(s, 0, lib, "dc.sfz");
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts[0].state == core::PartState::kEngineNotAvailable);  // no sample engine compiled in: silent, nothing substituted
  rig.run(2);
  rig.note_on(60);
  rig.run(10);
  CHECK(std::fabs(rig.level()) < 1e-9);
}

void test_second_load_replaces_first() {
  Rig rig;
  const std::string a = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  const std::string b = install_library(rig.tmp.path, 2, {{"dc.sfz", 0.3f}});
  lsp::LiveSet s1 = base_set(20), s2 = base_set(21);
  sample_part(s1, 0, a, "dc.sfz");
  sample_part(s2, 0, b, "dc.sfz");
  CHECK(rig.loader.load(s1).ok());
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);
  const host::LiveSetLoadResult r2 = rig.loader.load(s2);
  CHECK(r2.ok() && r2.revision == 2);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.3, 1e-3);
  rig.run(1);
  CHECK(rig.plane.reclaim() >= 1);  // the first generation (and its instrument) is freed on the control thread once RT let go of it
}

// --- all or nothing --------------------------------------------------------------------------------------------

void test_atomic_publication() {
  Rig rig;
  const std::string a = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  const std::string b = install_library(rig.tmp.path, 2, {{"dc.sfz", 0.3f}});
  const std::string c = install_library(rig.tmp.path, 3, {{"dc.sfz", 0.5f}});
  // Nothing was ever published: a failing first load leaves the engine without a generation.
  lsp::LiveSet bad = base_set(30);
  sample_part(bad, 0, a, "dc.sfz");
  sample_part(bad, 1, b, "dc.sfz");
  rig.heads.fail_on = 2;  // the second part's instrument fails to become resident
  host::LiveSetLoadResult res = rig.loader.load(bad);
  CHECK(res.status == LiveSetLoadStatus::kInstrumentLoadFailed && res.failed_part == 1);
  CHECK(res.load_status == host::LoadStatus::kBundleInconsistent);
  CHECK(rig.plane.revision() == 0 && rig.br->snapshots().current() == nullptr);
  CHECK(!rig.br->arbiter().reserved());
  // A good Live Set loads...
  lsp::LiveSet good = base_set(31);
  sample_part(good, 0, c, "dc.sfz");
  rig.heads.fail_on = 0;
  CHECK(rig.loader.load(good).ok());
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.5, 1e-3);
  const std::uint64_t rev = rig.plane.revision();
  const auto reservation = rig.br->arbiter().reservation();
  const std::size_t gens = rig.plane.live_generations();
  // ...and a failing load after it publishes nothing and the old generation keeps playing.
  rig.heads.calls = 0;
  rig.heads.fail_on = 2;
  res = rig.loader.load(bad);
  CHECK(res.status == LiveSetLoadStatus::kInstrumentLoadFailed && res.failed_part == 1);
  CHECK(rig.plane.revision() == rev && rig.plane.live_generations() == gens);
  CHECK(rig.br->arbiter().reserved() && rig.br->arbiter().reservation().floors == reservation.floors &&
        rig.br->arbiter().reservation().fx == reservation.fx);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.5, 1e-3);
  CHECK(rig.br->held_snapshot()->revision == rev);
}

void test_rejections_publish_nothing() {
  {
    host::LiveSetLoaderOptions o;
    o.arbiter_budget = 100;
    Rig rig(true, o);
    const std::string a = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
    lsp::LiveSet s = base_set(40);
    sample_part(s, 0, a, "dc.sfz");
    const host::LiveSetLoadResult res = rig.loader.load(s);
    CHECK(res.status == LiveSetLoadStatus::kArbiterRejected);
    CHECK(res.reserve_status != core::VoiceArbiter::ReserveStatus::kOk);
    CHECK(rig.plane.revision() == 0 && rig.br->snapshots().current() == nullptr && !rig.br->arbiter().reserved());
  }
  {
    Rig rig;
    rig.plane.set_fx_budget(1);  // the effects of any Live Set cost more than this
    const std::string a = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
    lsp::LiveSet s = base_set(41);
    sample_part(s, 0, a, "dc.sfz");
    const host::LiveSetLoadResult res = rig.loader.load(s);
    CHECK(res.status == LiveSetLoadStatus::kFxBudgetExceeded);
    CHECK(rig.plane.revision() == 0 && !rig.br->arbiter().reserved());
  }
  {
    // A sample engine too small for the floor: the reservation is refused (floor + 64 headroom must fit the capacity).
    core::BlockRenderer small_br(8);
    core::SampleEngine::Config c;
    c.max_voices = 64;
    core::SampleEngine small(c);
    CHECK(small_br.bind_engine(small));
    host::ControlEventQueue q;
    host::ControlPlane plane(small_br.snapshots(), small_br.last_values(), q, [] { return std::uint64_t{0}; });
    TmpDir tmp;
    const std::string a = install_library(tmp.path, 1, {{"dc.sfz", 0.1f}});
    host::LibraryResolver resolver(host::make_posix_fs(), tmp.path);
    host::RamHeadProvider heads;
    host::LiveSetLoader loader(small_br, plane, resolver, heads, &small);
    lsp::LiveSet s = base_set(42);
    sample_part(s, 0, a, "dc.sfz");
    const host::LiveSetLoadResult res = loader.load(s);
    CHECK(res.status == LiveSetLoadStatus::kArbiterRejected && res.reserve_status == core::VoiceArbiter::ReserveStatus::kFloorExceedsCapacity);
    CHECK(plane.revision() == 0);
  }
}

// --- late failures, a full ledger, quiescence -------------------------------------------------------------------

void test_late_failure_restores_reservation() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet a = base_set(60);
  sample_part(a, 0, lib, "dc.sfz");
  CHECK(rig.loader.load(a).ok());
  const core::VoiceArbiter::LiveSetConfig before = rig.br->arbiter().config();
  const core::VoiceArbiter::Reservation res_before = rig.br->arbiter().reservation();
  CHECK(before.floor_count == 1 && before.organ_rotary_cost == 0);
  // The second Live Set (an organ, no sample part) reserves something different; its publication fails after the reservation.
  lsp::LiveSet b = base_set(61);
  for (int p = 0; p < 8; ++p) b.mutable_parts(p)->mutable_engine()->set_engine_type_id(p == 0 ? 2 : 3);
  rig.plane.inject_publish_fault_for_tests(host::ControlPlane::PublishStatus::kInternal);
  const host::LiveSetLoadResult r = rig.loader.load(b);
  CHECK(r.status == LiveSetLoadStatus::kInternal);
  const core::VoiceArbiter::LiveSetConfig& after = rig.br->arbiter().config();
  CHECK(rig.br->arbiter().reserved());
  CHECK(after.floor_count == before.floor_count && after.floors[0].voices == before.floors[0].voices &&
        after.floors[0].engine_type_id == before.floors[0].engine_type_id);
  CHECK(after.organ_rotary_cost == before.organ_rotary_cost && after.budget == before.budget && after.fx_instance_count == before.fx_instance_count &&
        after.waive_sample_floor == before.waive_sample_floor && after.waive_organ_reservation == before.waive_organ_reservation);
  const core::VoiceArbiter::Reservation res_after = rig.br->arbiter().reservation();
  CHECK(res_after.floors == res_before.floors && res_after.fx == res_before.fx && res_after.organ_rotary == res_before.organ_rotary &&
        res_after.shared == res_before.shared);
  CHECK(rig.plane.revision() == 1);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);  // the first Live Set plays on
  // With nothing reserved before, a late failure leaves nothing reserved.
  Rig fresh;
  const std::string lib2 = install_library(fresh.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet c = base_set(62);
  sample_part(c, 0, lib2, "dc.sfz");
  fresh.plane.inject_publish_fault_for_tests(host::ControlPlane::PublishStatus::kBusy);
  CHECK(fresh.loader.load(c).status == LiveSetLoadStatus::kBusy);
  CHECK(!fresh.br->arbiter().reserved() && fresh.plane.revision() == 0);
}

void test_full_ledger_is_busy() {
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(63);
  sample_part(s, 0, lib, "dc.sfz");
  // Every generation stays referenced by a note that started on it: 32 loads fill the 32 ledger slots.
  for (std::uint32_t i = 0; i < core::SnapshotExchange::kLedgerSlots; ++i) {
    CHECK(rig.loader.load(s).ok());
    rig.run(2);
    rig.note_on(30 + i);
    rig.run(2);
  }
  const std::uint64_t rev = rig.plane.revision();
  const core::VoiceArbiter::Reservation before = rig.br->arbiter().reservation();
  const host::LiveSetLoadResult r = rig.loader.load(s);
  CHECK(r.status == LiveSetLoadStatus::kBusy);
  CHECK(rig.plane.revision() == rev && rig.br->arbiter().reservation().floors == before.floors);
  // Releasing the notes frees generations again.
  for (std::uint32_t i = 0; i < core::SnapshotExchange::kLedgerSlots; ++i) rig.note_off(30 + i);
  rig.run(100);
  CHECK(rig.loader.load(s).ok());
}

bool g_rendering = false;

void test_refused_while_rendering() {
  host::LiveSetLoaderOptions o;
  o.is_rendering = [] { return g_rendering; };
  Rig rig(true, o);
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(64);
  sample_part(s, 0, lib, "dc.sfz");
  g_rendering = true;
  const host::LiveSetLoadResult r = rig.loader.load(s);
  CHECK(r.status == LiveSetLoadStatus::kNotQuiesced);
  CHECK(rig.plane.revision() == 0 && !rig.br->arbiter().reserved() && rig.heads.calls == 0);
  g_rendering = false;
  CHECK(rig.loader.load(s).ok());
}

// --- the tier ---------------------------------------------------------------------------------------------------

void test_tier_part_count() {
  // v1: a Pro-sized (16 part) file is refused with the typed format error, and what plays keeps playing.
  Rig rig;
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet cur = base_set(60);
  sample_part(cur, 0, lib, "dc.sfz");
  CHECK(rig.loader.load(cur).ok());
  rig.run(2);
  const std::uint64_t rev = rig.plane.revision();
  lsp::LiveSet pro = fixtures::full(16);
  host::LiveSetLoadResult res = rig.loader.load(pro);
  CHECK(res.status == LiveSetLoadStatus::kInvalidLiveSet);
  bool saw = false;
  for (const ls::Issue& i : res.report.errors) saw = saw || i.code == ls::IssueCode::kBadPartCount;
  CHECK(saw);
  CHECK(rig.plane.revision() == rev && rig.br->arbiter().reserved());
  CHECK_NEAR(rig.play(60), 0.1, 1e-3);  // the current set keeps playing
  // A tier whose part count the build cannot carry (a profile above the structural capacity of 16) is a typed refusal, nothing published.
  host::LiveSetLoaderOptions o;
  o.part_count = core::params::kPartCount + 1;
  Rig big_rig(true, o);
  res = big_rig.loader.load(base_set(61, core::params::kPartCount + 1));
  CHECK(res.status == LiveSetLoadStatus::kPartCountUnsupported);
  CHECK(big_rig.plane.revision() == 0 && !big_rig.br->arbiter().reserved());
  // Pro: 16 parts load; an 8 part file on a Pro tier is invalid.
  Rig pro_rig(host::pro_profile());
  res = pro_rig.loader.load(base_set(50, 8));
  CHECK(res.status == LiveSetLoadStatus::kInvalidLiveSet);
  CHECK(pro_rig.plane.revision() == 0);
}

// Story 4.1: a 16 part Live Set on the Pro profile loads and EVERY part renders (parts 8..15 included): part j owns the key zone [20 + 4j, 23 + 4j] and a
// constant of its own level, so the level of a note tells which part sounded it.
void test_pro_sixteen_parts_render();
// Story 4.1: on Pro the organ, FM and VA engines are built for the profile (16 organ parts, FM / VA pools large enough for the doubled floors): parts at
// index >= 8 of each engine load AND sound. Each part owns a key zone; the loudest sample of a note says it sounded.
void test_pro_engine_parts_above_seven_sound() {
  const host::TierProfile& pro = host::pro_profile();
  Rig rig(pro);
  core::FmEngine fm(48000.0f, pro.fm_voice_capacity);
  core::VaEngine va(48000.0f, pro.va_voice_capacity);
  CHECK(rig.br->bind_engine(fm) && rig.br->bind_engine(va));
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(90, 16);
  sample_part(s, 14, lib, "dc.sfz", 50, 53);
  s.mutable_parts(10)->mutable_engine()->set_engine_type_id(2);  // organ
  s.mutable_parts(10)->set_low_key(40);
  s.mutable_parts(10)->set_high_key(43);
  s.mutable_parts(9)->mutable_engine()->set_engine_type_id(3);  // FM
  s.mutable_parts(9)->set_low_key(20);
  s.mutable_parts(9)->set_high_key(23);
  s.mutable_parts(12)->mutable_engine()->set_engine_type_id(4);  // VA
  s.mutable_parts(12)->set_low_key(30);
  s.mutable_parts(12)->set_high_key(33);
  for (int j : {0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 13, 15}) s.mutable_parts(j)->set_keyboard_on(false);
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  for (int j : {9, 10, 12, 14}) CHECK(res.parts[static_cast<std::size_t>(j)].state == core::PartState::kReady);
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kFm)) == pro.fm_floor_voices * fm.cost_units_per_voice());
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kVa)) == pro.va_floor_voices * va.cost_units_per_voice());
  rig.run(2);
  auto peak_of = [&](std::uint32_t key) {
    rig.blocks.clear();
    rig.note_on(key);
    rig.run(12);
    float pk = 0;
    for (const auto& b : rig.blocks)
      for (float v : b) pk = std::fmax(pk, std::fabs(v));
    rig.note_off(key);
    rig.run(200);
    return pk;
  };
  CHECK(peak_of(21) > 1e-4f);  // FM, part 9
  CHECK(peak_of(41) > 1e-4f);  // organ, part 10
  CHECK(peak_of(31) > 1e-4f);  // VA, part 12
  CHECK(peak_of(51) > 1e-4f);  // sample, part 14
}

void test_pro_sixteen_parts_render() {
  Rig rig(host::pro_profile());
  std::vector<Spec> specs;
  for (std::uint32_t j = 0; j < 16; ++j) specs.push_back({"p" + std::to_string(j) + ".sfz", 0.004f * static_cast<float>(j + 1)});
  const std::string lib = install_library(rig.tmp.path, 1, specs);
  lsp::LiveSet s = base_set(70, 16);
  for (int j = 0; j < 16; ++j) sample_part(s, j, lib, "p" + std::to_string(j) + ".sfz", 20 + 4 * j, 23 + 4 * j);
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts.size() == 16);
  for (const host::PartLoadReport& p : res.parts) CHECK(p.state == core::PartState::kReady);
  CHECK(rig.plane.part_cap() == 16);
  rig.run(2);
  for (std::uint32_t j = 0; j < 16; ++j) CHECK_NEAR(rig.play(20 + 4 * j + 1), 0.004 * static_cast<double>(j + 1), 1e-3);
  CHECK(rig.sample.active_voices() == 0);
  const core::Snapshot* g = rig.br->held_snapshot();
  CHECK(g && g->live_set && g->parts[15].sample_bank != nullptr && g->parts[15].state == core::PartState::kReady);
}

// Story 4.1: the profile reaches the arbiter reservation (budget and floors) and the loader's head budget.
void test_tier_budgets_reach_the_loader() {
  Rig v1(host::v1_profile());
  const std::string lib1 = install_library(v1.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet a = base_set(80);
  sample_part(a, 0, lib1, "dc.sfz");
  CHECK(v1.loader.load(a).ok());
  Rig pro(host::pro_profile());
  const std::string lib2 = install_library(pro.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet b = base_set(81, 16);
  sample_part(b, 0, lib2, "dc.sfz");
  CHECK(pro.loader.load(b).ok());
  const std::uint64_t cost = v1.sample.cost_units_per_voice();
  CHECK(v1.br->arbiter().reservation().budget == host::v1_profile().arbiter_budget);
  CHECK(pro.br->arbiter().reservation().budget == host::pro_profile().arbiter_budget);
  CHECK(pro.br->arbiter().reservation().budget == 2 * v1.br->arbiter().reservation().budget);
  CHECK(v1.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) == 256ull * cost);
  CHECK(pro.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) == 512ull * cost);
  CHECK(v1.plane.fx_budget() == host::v1_profile().fx_budget && pro.plane.fx_budget() == host::pro_profile().fx_budget);
  CHECK(pro.plane.fx_budget() > v1.plane.fx_budget());
  // The head budget is the profile's: a profile whose budget the Live Set's heads exceed refuses it early with the typed error, nothing published.
  host::TierProfile tiny = host::v1_profile();
  tiny.head_budget_bytes = 1000;
  Rig small(tiny);
  const std::string lib3 = install_library(small.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet c = base_set(82);
  sample_part(c, 0, lib3, "dc.sfz");
  const host::LiveSetLoadResult r = small.loader.load(c);
  CHECK(r.status == LiveSetLoadStatus::kHeadBudgetExceeded && r.head_budget == 1000 && r.head_bytes > 1000);
  CHECK(small.plane.revision() == 0);
}

// --- files ---------------------------------------------------------------------------------------------------------

void test_load_files() {
  const std::string dir = VELORA_LIVESET_CORPUS_DIR;
  for (const char* name : {"minimal.liveset", "full-8-parts.liveset", "fm-va-parts.liveset", "missing-library.liveset",
                           "newer-schema-unknown-fields.liveset"}) {
    Rig rig;
    const host::LiveSetLoadResult res = rig.loader.load_file(dir + "/" + name);
    if (!res.ok()) std::printf("  %s: %s (%s)\n", name, host::to_string(res.status), res.detail.c_str());
    CHECK(res.ok());  // libraries that are not installed make parts silent, they do not fail the load
    if (std::string(name) == "missing-library.liveset") {
      CHECK(res.parts[0].state == core::PartState::kMissingLibrary && res.parts[3].state == core::PartState::kMissingLibrary);
      CHECK(res.parts[1].state == core::PartState::kEmptyTimbre);
    }
    if (std::string(name) == "fm-va-parts.liveset")
      CHECK(res.parts[0].state == core::PartState::kEngineNotAvailable && res.parts[1].state == core::PartState::kEngineNotAvailable);
    if (std::string(name) == "full-8-parts.liveset") {
      CHECK(res.parts[2].state == core::PartState::kReady);  // the organ part
      // Part model: the split, the octave / transpose and keyboard flags of the file reach the generation.
      rig.run(2);
      const core::Snapshot* g = rig.br->held_snapshot();
      CHECK(g && g->parts[0].high_key == 59 && g->parts[1].low_key == 60 && g->parts[1].octave == 1 && g->parts[1].transpose == -2);
      CHECK(g && g->parts[6].keyboard_on == 0 && g->parts[1].output == core::params::kPartOutputAux);
    }
  }
  {
    Rig rig;
    CHECK(rig.loader.load_file(dir + "/does-not-exist.liveset").status == LiveSetLoadStatus::kReadFailed);
    TmpDir t;
    {
      std::ofstream f(t.path + "/garbage.liveset", std::ios::binary);
      f << "this is not a live set";
    }
    const host::LiveSetLoadResult res = rig.loader.load_file(t.path + "/garbage.liveset");
    CHECK(res.status == LiveSetLoadStatus::kReadFailed || res.status == LiveSetLoadStatus::kInvalidLiveSet);
    CHECK(rig.plane.revision() == 0);
  }
}

}  // namespace

void test_fm_part_floor_reservation() {
  Rig rig;
  core::FmEngine fm;
  CHECK(rig.br->bind_engine(fm));

  // 1. Live Set with an FM part: FM floor is reserved (128 voices * 4 = 512 cost units)
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(25);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(1)->mutable_engine()->set_engine_type_id(3);  // FM
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts[1].state == core::PartState::kReady);
  const std::uint32_t fm_id = static_cast<std::uint32_t>(core::EngineTypeId::kFm);
  CHECK(rig.br->arbiter().floor_cost(fm_id) == 128 * 4);
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) > 0);

  // 2. Live Set with NO FM part: FM floor is zero
  lsp::LiveSet s_no_fm = base_set(26);
  sample_part(s_no_fm, 0, lib, "dc.sfz");
  const host::LiveSetLoadResult res2 = rig.loader.load(s_no_fm);
  CHECK(res2.ok());
  CHECK(rig.br->arbiter().floor_cost(fm_id) == 0);
}

void test_va_part_floor_reservation() {
  Rig rig;
  core::VaEngine va;
  CHECK(rig.br->bind_engine(va));

  // 1. Live Set with a VA part: VA floor is reserved (64 voices * 4 = 256 cost units)
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(27);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(1)->mutable_engine()->set_engine_type_id(4);  // VA
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts[1].state == core::PartState::kReady);
  const std::uint32_t va_id = static_cast<std::uint32_t>(core::EngineTypeId::kVa);
  CHECK(rig.br->arbiter().floor_cost(va_id) == 64 * 4);
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) > 0);

  // 2. Live Set with NO VA part: VA floor is zero
  lsp::LiveSet s_no_va = base_set(28);
  sample_part(s_no_va, 0, lib, "dc.sfz");
  const host::LiveSetLoadResult res2 = rig.loader.load(s_no_va);
  CHECK(res2.ok());
  CHECK(rig.br->arbiter().floor_cost(va_id) == 0);
}

void test_ep_part_floor_reservation() {
  Rig rig;
  core::EpEngine ep(48000.0f, 128);
  CHECK(rig.br->bind_engine(ep));

  // 1. Live Set with an EP part: EP floor is reserved (64 voices * 4 = 256 cost units)
  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(29);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(1)->mutable_engine()->set_engine_type_id(5);  // EP
  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  CHECK(res.parts[1].state == core::PartState::kReady);
  const std::uint32_t ep_id = static_cast<std::uint32_t>(core::EngineTypeId::kModeledEP);
  CHECK(rig.br->arbiter().floor_cost(ep_id) == 64 * 4);
  CHECK(rig.br->arbiter().floor_cost(static_cast<std::uint32_t>(core::EngineTypeId::kSample)) > 0);

  // 2. Live Set with NO EP part: EP floor is zero
  lsp::LiveSet s_no_ep = base_set(30);
  sample_part(s_no_ep, 0, lib, "dc.sfz");
  const host::LiveSetLoadResult res2 = rig.loader.load(s_no_ep);
  CHECK(res2.ok());
  CHECK(rig.br->arbiter().floor_cost(ep_id) == 0);
}

void test_ep_part_unbound_engine_not_available() {
  Rig rig;  // Only sample engine is bound by default in Rig

  const std::string lib = install_library(rig.tmp.path, 1, {{"dc.sfz", 0.1f}});
  lsp::LiveSet s = base_set(31);
  sample_part(s, 0, lib, "dc.sfz");
  s.mutable_parts(1)->mutable_engine()->set_engine_type_id(5);  // EP, not bound!
  auto* pe = s.mutable_parts(1)->mutable_engine()->add_params();
  pe->set_param_id(1301);
  pe->set_value(0.85f);

  const host::LiveSetLoadResult res = rig.loader.load(s);
  CHECK(res.ok());
  // EP engine is not bound -> kEngineNotAvailable
  CHECK(res.parts[1].state == core::PartState::kEngineNotAvailable);
  // Stored parameters are preserved in the LiveSet message
  CHECK(s.parts(1).engine().params(0).param_id() == 1301);
  CHECK(s.parts(1).engine().params(0).value() == 0.85f);
}

int main() {
  test_layer_and_states();
  test_split_b2_c3();
  test_keyboard_off_and_zone_pitch();
  test_part_volume_output();
  test_missing_library_and_instrument();
  test_fm_part_silent_organ_ready();
  test_fm_part_floor_reservation();
  test_va_part_floor_reservation();
  test_ep_part_floor_reservation();
  test_ep_part_unbound_engine_not_available();
  test_no_sample_part_floor_zero();
  test_without_sample_engine();
  test_second_load_replaces_first();
  test_atomic_publication();
  test_rejections_publish_nothing();
  test_late_failure_restores_reservation();
  test_full_ledger_is_busy();
  test_refused_while_rendering();
  test_tier_part_count();
  test_pro_sixteen_parts_render();
  test_pro_engine_parts_above_seven_sound();
  test_tier_budgets_reach_the_loader();
  test_load_files();
  if (g_fail) {
    std::printf("live_set_loader_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("live_set_loader_tests: ok\n");
  return 0;
}

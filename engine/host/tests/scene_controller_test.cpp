// Scene recall (Story 4.4): a parameter-only snapshot swap. The library content is synthetic (a looping constant per instrument, laid out as the
// resolver expects), so a part is told apart in the output by its level. The block renderer is driven directly, one block at a time.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/scene_controller.hpp"

namespace fs = std::filesystem;
using namespace velora;
namespace p = core::params;

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
using fixtures::entry;
using fixtures::uuid_n;
using host::SceneResult;

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-scene-XXXXXX").string();
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

// A library with looping constants, one instrument per level.
std::string install_library(const std::string& root, std::uint32_t index, const std::vector<std::pair<std::string, float>>& specs) {
  const std::string fp = "velora-scene-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  for (const auto& sp : specs) {
    auto ins = w.add_instrument(sp.first, sp.first);
    CHECK(ins.ok());
    std::vector<std::int32_t> pcm(4800, format::float_to_bits(sp.second));
    if (sp.second < 0)  // a negative level asks for a looping 100 Hz sine at key 60
      for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = format::float_to_bits(0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 480.0)));
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

// The RAM provider that counts every call and can call back into the test (a recall in the middle of a load).
class CountingHeads final : public host::HeadProvider {
 public:
  host::LoadStatus load(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out) override {
    ++calls;
    if (on_load) on_load();
    if (calls == fail_on) return host::LoadStatus::kBundleInconsistent;
    return ram.load(r, out);
  }
  host::RamHeadProvider ram;
  int calls = 0;
  int fail_on = 0;
  std::function<void()> on_load;
};

struct Rig {
  TmpDir tmp;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::SceneController scenes;
  host::LibraryResolver resolver;
  CountingHeads heads;
  host::LiveSetLoader loader;
  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::vector<std::vector<float>> blocks;

  static core::SampleEngine::Config cfg() {
    core::SampleEngine::Config c;
    c.max_voices = 340;
    return c;
  }
  static host::LiveSetLoaderOptions opts(host::SceneController* s, std::uint64_t budget) {
    host::LiveSetLoaderOptions o;
    o.scenes = s;
    if (budget) o.arbiter_budget = budget;
    return o;
  }
  explicit Rig(std::uint64_t budget = 0)
      : sample(cfg()),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        scenes(plane),
        resolver(host::make_posix_fs(), tmp.path),
        loader(*br, plane, resolver, heads, &sample, opts(&scenes, budget)) {
    CHECK(br->bind_engine(sample));
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
  void pedal(bool down) { CHECK(br->submit({core::make_control_change(0, 0, 64, down ? 0xFFFFFFFFu : 0u), 0})); }
  void live_move(std::uint16_t id, std::uint32_t part, double v) {
    CHECK(br->submit({core::make_param_event(*p::find(id), part, v), 0}));
  }
  double live(std::uint16_t id, std::uint32_t part) { return br->last_values().load(p::perf_slot(*p::find(id), part)); }
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
  const core::Snapshot* cur() const { return plane.current_snapshot(); }
  double frequency(std::uint32_t nblocks) const {
    std::vector<float> x;
    for (std::size_t i = blocks.size() - nblocks; i < blocks.size(); ++i) x.insert(x.end(), blocks[i].begin(), blocks[i].end());
    std::uint32_t n = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
      if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return static_cast<double>(n) * 48000.0 / static_cast<double>(x.size());
  }
};

lsp::ScenePart* scene_part(lsp::Scene* s, int part) {
  while (s->parts_size() <= part) s->add_parts();
  return s->mutable_parts(part);
}

// The Live Set most tests use. Parts: 0 sample "a" (0.1), 1 sample "b" (0.01), 2 organ (keyboard off in the base), 3.. empty sample parts.
// Scenes: 0 "verse" (part 0 at half volume, part 1 off, organ drawbar_8 = 3, reverb send of part 0), 1 "chorus" (organ on, drawbar_8 = 8 = the base
// value, part 0 zone up to B3), 2 "outro" (names only: everything is the base).
struct Lib {
  std::string a, b;
};
lsp::LiveSet make_set(const Lib& lib, std::uint64_t seed = 40) {
  lsp::LiveSet s = ls::make_default_live_set(uuid_n(seed), "Scenes", 8);
  fixtures::timbre(s.mutable_parts(0), lib.a, "a.sfz");
  fixtures::timbre(s.mutable_parts(1), lib.b, "b.sfz");
  s.mutable_parts(0)->set_volume(1.0f);
  s.mutable_parts(1)->set_volume(1.0f);
  lsp::Part* organ = s.mutable_parts(2);
  organ->clear_timbre();
  organ->mutable_engine()->set_engine_type_id(2);
  organ->set_keyboard_on(false);
  entry(organ->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
  entry(organ->mutable_engine()->mutable_params(), p::kDrawbar16, 1);

  lsp::Scene* verse = s.add_scenes();
  verse->set_name("verse");
  scene_part(verse, 0)->set_volume(0.5f);
  scene_part(verse, 0)->set_send_reverb(0.3f);
  scene_part(verse, 1)->set_keyboard_on(false);
  entry(scene_part(verse, 2)->mutable_engine_params(), p::kDrawbar8, 3);
  lsp::Scene* chorus = s.add_scenes();
  chorus->set_name("chorus");
  scene_part(chorus, 2)->set_keyboard_on(true);
  entry(scene_part(chorus, 2)->mutable_engine_params(), p::kDrawbar8, 8);
  scene_part(chorus, 0)->set_high_key(59);
  lsp::Scene* outro = s.add_scenes();
  outro->set_name("outro");
  return s;
}

struct Fixture {
  Rig rig;
  Lib lib;
  explicit Fixture(std::uint64_t budget = 0) : rig(budget) {
    lib.a = install_library(rig.tmp.path, 1, {{"a.sfz", 0.1f}});
    lib.b = install_library(rig.tmp.path, 2, {{"b.sfz", 0.01f}});
  }
  bool load(const lsp::LiveSet& s) {
    const host::LiveSetLoadResult res = rig.loader.load(s);
    rig.run(2);
    return res.ok();
  }
};

// --- recall: parameter-only, no heads, same banks --------------------------------------------------------------
void test_recall_is_parameter_only() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  CHECK(rig.scenes.has_live_set() && rig.scenes.scene_count() == 3 && rig.scenes.current_scene() == -1);
  ws::v1::State st;
  rig.plane.fill_state(&st);
  CHECK(st.live_set_loaded() && st.current_scene() == -1 && st.scene_count() == 3);

  const int heads_before = rig.heads.calls;
  const core::Snapshot* before = rig.cur();
  const std::uint64_t rev_before = rig.plane.revision();
  std::array<std::shared_ptr<core::SampleBank>, 8> banks;
  for (int i = 0; i < 8; ++i) banks[static_cast<std::size_t>(i)] = before->parts[static_cast<std::size_t>(i)].sample_bank;
  CHECK(banks[0] && banks[1] && !banks[2]);
  // Delete the libraries: a recall that tried to resolve or load anything would fail.
  std::error_code ec;
  fs::remove_all(fs::path(rig.tmp.path) / "lib", ec);

  const host::SceneOutcome o = rig.scenes.recall_scene(0);
  CHECK(o.result == SceneResult::kOk && o.published && o.scene == 0 && o.revision == rev_before + 1);
  CHECK(rig.heads.calls == heads_before);  // no head or instrument was loaded
  const core::Snapshot* after = rig.cur();
  CHECK(after != before && after->live_set && after->scene_index == 0 && after->revision == rev_before + 1);
  for (std::size_t i = 0; i < 8; ++i) {
    const core::PartCfg &a = before->parts[i], &b = after->parts[i];
    CHECK(a.sample_bank == b.sample_bank);  // pointer equality: the very same prepared instrument
    CHECK(a.timbre_ref == b.timbre_ref && a.engine_type == b.engine_type && a.state == b.state);
  }
  // What the scene stores changed; the rest is the base.
  CHECK(after->parts[0].volume == 0.5f && before->parts[0].volume == 1.0f);
  CHECK(after->parts[1].keyboard_on == 0 && before->parts[1].keyboard_on == 1);
  CHECK(after->parts[3].keyboard_on == before->parts[3].keyboard_on && after->parts[3].low_key == 0 && after->parts[3].high_key == 127);
  CHECK(after->perf_init[p::perf_slot(*p::find(p::kDrawbar8), 2)] == 3.0f);
  CHECK(after->perf_init[p::perf_slot(*p::find(p::kDrawbar16), 2)] == 1.0f);  // not stored by the scene: the base value
  CHECK_NEAR(after->perf_init[p::perf_slot(*p::find(p::kSendReverbLevel), 0)], 0.3, 1e-6);
  CHECK(after->perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)] == before->perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)]);  // no tempo in a scene
  CHECK(after->program_generation != 0 && after->perf_force[p::perf_slot(*p::find(p::kDrawbar8), 2)] == 1 &&
        after->perf_force[p::perf_slot(*p::find(p::kDrawbar16), 2)] == 0);
  rig.plane.fill_state(&st);
  CHECK(st.current_scene() == 0 && st.scene_count() == 3);
  ws::v1::PerformanceValues pv;
  rig.plane.fill_live_values(&pv);
  CHECK(pv.current_scene() == 0);
  // The sound: part 0 at half volume, part 1 off.
  rig.run(4);
  CHECK_NEAR(rig.play(60), 0.05, 1e-3);
  // The base comes back with the base scene ("outro" stores nothing).
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);
  rig.run(4);
  CHECK_NEAR(rig.play(60), 0.11, 1e-3);
  CHECK(rig.heads.calls == heads_before);
}

// --- performance values: applied once, at the new generation -------------------------------------------------------
void test_performance_values_applied_once() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 8, 1e-6);
  // A live knob move the scene does NOT store (drawbar_16) and one it stores (drawbar_8).
  rig.live_move(p::kDrawbar16, 2, 6);
  rig.live_move(p::kDrawbar8, 2, 5);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar16, 2), 6, 1e-6);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // verse: drawbar_8 = 3
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 3, 1e-6);   // overridden
  CHECK_NEAR(rig.live(p::kDrawbar16, 2), 6, 1e-6);  // not stored: left alone
  // After the recall the live value is again the player's: a later structural change does not re-apply the scene.
  rig.live_move(p::kDrawbar8, 2, 7);
  rig.run(2);
  ws::v1::ClientMessage m;
  m.set_request_id(1);
  m.mutable_set_part_output()->set_part(4);
  m.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  bool changed = false;
  CHECK(rig.plane.handle(m, changed).has_ack() && changed);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 7, 1e-6);
  // The same scene again re-applies its values: a reset of the stored performance values to the scene's.
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 3, 1e-6);
  CHECK_NEAR(rig.live(p::kDrawbar16, 2), 6, 1e-6);
  // A scene that stores a value EQUAL to the base one still overrides a live move (chorus stores drawbar_8 = 8, the base value).
  rig.live_move(p::kDrawbar8, 2, 2);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 2, 1e-6);
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 8, 1e-6);
  // Two recalls before RT adopts either: RT sees only the last one, and what that scene stores is applied.
  rig.live_move(p::kDrawbar8, 2, 4);
  rig.run(2);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 8, 1e-6);
  // The send the verse stores is applied too.
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kSendReverbLevel, 0), 0.3, 1e-6);
  // A scene that does not store a value a PREVIOUS scene overrode brings the base back (a scene is a full snapshot).
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kSendReverbLevel, 0), 0, 1e-6);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 8, 1e-6);
}

// --- notes across a recall -----------------------------------------------------------------------------------------
void test_notes_continue_across_recall() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  rig.note_on(60);
  rig.run(8);
  CHECK_NEAR(rig.level(), 0.11, 1e-3);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // part 0 -> half volume, part 1 -> keyboard off
  rig.run(8);
  // The held note keeps sounding on BOTH parts (part 1's keyboard is off for new notes only); part 0 is at its new volume.
  CHECK_NEAR(rig.level(), 0.05 + 0.01, 1e-3);
  CHECK(rig.sample.active_voices() == 2);
  rig.note_on(62);  // a new note follows the new keyboard states: part 0 only
  rig.run(8);
  CHECK(rig.sample.active_voices() == 3);
  rig.note_off(62);
  rig.note_off(60);
  rig.run(80);
  CHECK(rig.sample.active_voices() == 0);  // released normally

  // Sustained by the pedal.
  rig.pedal(true);
  rig.note_on(64);
  rig.run(4);
  rig.note_off(64);
  rig.run(4);
  CHECK(rig.sample.active_voices() == 1);   // still sounding (part 1's keyboard is off in the verse)
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);  // back to the base
  rig.run(20);
  CHECK(rig.sample.active_voices() == 1);   // the recall did not release or cut it
  rig.pedal(false);
  rig.run(80);
  CHECK(rig.sample.active_voices() == 0);

  // A held note and a keyboard that turns ON (chorus: the organ): the held note is not retargeted, only new notes reach the organ.
  rig.note_on(60);
  rig.run(8);
  CHECK(rig.organ.active_voices() == 0);
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  rig.run(8);
  CHECK(rig.organ.active_voices() == 0);
  rig.note_on(67);
  rig.run(8);
  CHECK(rig.organ.active_voices() >= 1);
  rig.note_off(60);
  rig.note_off(67);
  rig.run(200);
  CHECK(rig.sample.active_voices() == 0 && rig.organ.active_voices() == 0);
}

// A stored zone only changes where NEW notes go: the note-off of a held note still reaches the part it started on.
void test_zone_change_note_off_consistency() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  rig.note_on(72);  // above B3: part 0 and part 1 both take it
  rig.run(6);
  CHECK(rig.sample.active_voices() == 2);
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);  // part 0's zone becomes 0..59: 72 is outside it now
  rig.run(6);
  CHECK(rig.sample.active_voices() == 2);
  rig.note_off(72);
  rig.run(80);
  CHECK(rig.sample.active_voices() == 0);  // part 0's voice was released although its zone no longer holds the key
  CHECK(rig.br->router().held_keys() == 0);
}

// The sound of notes up to their release does not change at all when the scene changes nothing audible.
std::vector<float> render_script(bool recall) {
  Fixture f;
  lsp::LiveSet s = make_set(f.lib);
  // A scene that changes only silent parts (5 and 6 have no timbre): zone, octave, keyboard flag and a sound parameter.
  lsp::Scene* q = s.add_scenes();
  q->set_name("bystanders");
  lsp::ScenePart* p5 = scene_part(q, 5);
  p5->set_keyboard_on(false);
  p5->set_low_key(10);
  p5->set_high_key(20);
  p5->set_octave(2);
  entry(p5->mutable_engine_params(), p::kDrawbar8, 2);
  scene_part(q, 6)->set_transpose(5);
  CHECK(f.load(s));
  Rig& rig = f.rig;
  rig.note_on(60);
  rig.run(3);
  rig.note_on(64);
  rig.run(3);
  rig.pedal(true);
  rig.note_off(60);  // pedal-sustained
  rig.run(3);
  if (recall) CHECK(rig.scenes.recall_scene(3).result == SceneResult::kOk);
  rig.run(5);
  rig.note_on(67);
  rig.run(3);
  rig.pedal(false);  // the pedal note releases here
  rig.run(2);
  rig.note_off(64);  // releasing ...
  rig.run(1);
  if (recall) CHECK(rig.scenes.recall_scene(3).result == SceneResult::kOk);  // ... at the moment of the recall
  rig.run(10);
  rig.note_off(67);
  rig.run(100);
  std::vector<float> all;
  for (const auto& b : rig.blocks) all.insert(all.end(), b.begin(), b.end());
  return all;
}

void test_unaffected_audio_is_sample_identical() {
  const std::vector<float> a = render_script(false), b = render_script(true);
  CHECK(a.size() == b.size() && a.size() > 1000);
  CHECK(a == b);  // every sample, including the release tails
  double peak = 0;
  for (float x : a) peak = std::max(peak, static_cast<double>(std::fabs(x)));
  CHECK(peak > 0.05);  // the script really sounds
}

// A changed volume changes the level without a step: the part fader ramps.
void test_volume_change_is_gap_free() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  rig.note_on(60);
  rig.run(10);
  const double before = rig.level();
  const std::size_t from = rig.blocks.size();
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);  // base: same volume, no change
  rig.run(4);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // part 0 volume 1 -> 0.5
  rig.run(8);
  std::vector<float> x;
  for (std::size_t i = from; i < rig.blocks.size(); ++i) x.insert(x.end(), rig.blocks[i].begin(), rig.blocks[i].end());
  double max_step = 0;
  for (std::size_t i = 1; i < x.size(); ++i) max_step = std::max(max_step, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  // Work in level() units (the output divided by unity()): the whole change, and the largest step in the same units.
  const double jump = std::fabs(before - rig.level());
  CHECK(jump > 0.04);
  CHECK(max_step / Rig::unity() < jump / 16.0);  // spread over at least 16 samples: no hard step
  rig.note_off(60);
  rig.run(100);
}

// --- stepping ----------------------------------------------------------------------------------------------------
void test_next_previous_stop_at_the_ends() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  host::SceneOutcome o = rig.scenes.previous_scene();  // none recalled yet
  CHECK(o.result == SceneResult::kAtFirst && !o.published && o.scene == -1);
  o = rig.scenes.next_scene();
  CHECK(o.result == SceneResult::kOk && o.scene == 0 && o.published);
  o = rig.scenes.next_scene();
  CHECK(o.result == SceneResult::kOk && o.scene == 1);
  o = rig.scenes.next_scene();
  CHECK(o.result == SceneResult::kOk && o.scene == 2);
  const std::uint64_t rev = rig.plane.revision();
  o = rig.scenes.next_scene();  // the last scene: stop, no wrap
  CHECK(o.result == SceneResult::kAtLast && !o.published && o.scene == 2 && rig.plane.revision() == rev);
  o = rig.scenes.next_scene();
  CHECK(o.result == SceneResult::kAtLast && rig.scenes.current_scene() == 2);
  o = rig.scenes.previous_scene();
  CHECK(o.result == SceneResult::kOk && o.scene == 1);
  o = rig.scenes.previous_scene();
  CHECK(o.result == SceneResult::kOk && o.scene == 0);
  o = rig.scenes.previous_scene();  // the first scene: stop
  CHECK(o.result == SceneResult::kAtFirst && o.scene == 0 && !o.published);
  // Stepping follows the scene order after a jump.
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);
  CHECK(rig.scenes.previous_scene().scene == 1);
  // A Live Set without scenes: nothing to step to.
  Fixture g;
  lsp::LiveSet s = ls::make_default_live_set(uuid_n(41), "No scenes", 8);
  CHECK(g.load(s));
  CHECK(g.rig.scenes.scene_count() == 0);
  CHECK(g.rig.scenes.next_scene().result == SceneResult::kAtLast);
  CHECK(g.rig.scenes.previous_scene().result == SceneResult::kAtFirst);
  CHECK(g.rig.scenes.recall_scene(0).result == SceneResult::kNoSuchScene);
}

// --- hostile cases -----------------------------------------------------------------------------------------------
void test_missing_scene_and_no_live_set() {
  Fixture f;
  // Nothing loaded.
  CHECK(f.rig.scenes.recall_scene(0).result == SceneResult::kNoLiveSet);
  CHECK(f.rig.scenes.next_scene().result == SceneResult::kNoLiveSet && f.rig.scenes.previous_scene().result == SceneResult::kNoLiveSet);
  CHECK(f.rig.plane.revision() == 0);
  CHECK(f.load(make_set(f.lib)));
  const std::uint64_t rev = f.rig.plane.revision();
  for (std::uint32_t idx : {3u, 4u, 7u, 8u, 100u, 0xFFFFFFFFu}) {
    const host::SceneOutcome o = f.rig.scenes.recall_scene(idx);
    CHECK(o.result == SceneResult::kNoSuchScene && !o.published && o.revision == rev);
  }
  CHECK(f.rig.plane.revision() == rev && f.rig.scenes.current_scene() == -1);
  // A Live Set loaded WITHOUT the controller is not a scene source: its generation is not the controller's.
  Fixture g;
  host::LiveSetLoader plain(*g.rig.br, g.rig.plane, g.rig.resolver, g.rig.heads, &g.rig.sample, {});
  CHECK(plain.load(make_set(g.lib)).ok());
  CHECK(g.rig.scenes.recall_scene(0).result == SceneResult::kNoLiveSet);
  // A failed load leaves the previous Live Set's scenes recallable.
  Fixture h;
  CHECK(h.load(make_set(h.lib)));
  h.rig.heads.fail_on = h.rig.heads.calls + 1;
  lsp::LiveSet other = make_set(h.lib, 77);
  other.mutable_parts(0)->mutable_timbre()->set_instrument_key("b.sfz");  // not the same library: resolves, then the provider fails
  fixtures::timbre(other.mutable_parts(0), h.lib.b, "b.sfz");
  CHECK(!h.rig.loader.load(other).ok());
  CHECK(h.rig.scenes.recall_scene(1).result == SceneResult::kOk);
  CHECK(h.rig.scenes.current_scene() == 1);
}

void test_recall_during_load() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  host::SceneOutcome seen;
  seen.result = SceneResult::kOk;
  int fired = 0;
  f.rig.heads.on_load = [&] {
    ++fired;
    seen = f.rig.scenes.recall_scene(0);
    CHECK(f.rig.scenes.next_scene().result == SceneResult::kLoadInProgress);
  };
  const std::uint64_t rev = f.rig.plane.revision();
  lsp::LiveSet other = make_set(f.lib, 78);
  const host::LiveSetLoadResult res = f.rig.loader.load(other);
  CHECK(res.ok() && fired >= 1);
  CHECK(seen.result == SceneResult::kLoadInProgress && !seen.published);
  CHECK(f.rig.plane.revision() == rev + 1);  // only the load published
  f.rig.heads.on_load = nullptr;
  CHECK(!f.rig.scenes.load_in_progress());
  CHECK(f.rig.scenes.current_scene() == -1);  // the new Live Set starts with no scene
  CHECK(f.rig.scenes.recall_scene(0).result == SceneResult::kOk);
}

// A Live Set load starts over: no scene is recalled, the new scenes are the loaded Live Set's.
void test_new_live_set_resets_scene() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  CHECK(f.rig.scenes.recall_scene(1).result == SceneResult::kOk);
  lsp::LiveSet s = make_set(f.lib, 79);
  s.mutable_scenes()->RemoveLast();
  CHECK(f.load(s));
  CHECK(f.rig.scenes.current_scene() == -1 && f.rig.scenes.scene_count() == 2);
  CHECK(f.rig.scenes.recall_scene(2).result == SceneResult::kNoSuchScene);
  ws::v1::State st;
  f.rig.plane.fill_state(&st);
  CHECK(st.current_scene() == -1 && st.scene_count() == 2);
}

// The scene format has no timbre. Hand-encoded unknown fields are preserved by the format and do nothing; forbidden ids are rejected.
void test_scene_cannot_change_a_timbre() {
  Fixture f;
  lsp::LiveSet s = make_set(f.lib);
  // Unknown fields on the Scene and on a ScenePart (what a "timbre" or an engine type would look like if someone tried): preserved, inert.
  fixtures::unknown_varint(s.mutable_scenes(0), 90, 2);
  fixtures::unknown_varint(scene_part(s.mutable_scenes(0), 0), 91, 2);
  auto* ufs = scene_part(s.mutable_scenes(0), 1)->GetReflection()->MutableUnknownFields(scene_part(s.mutable_scenes(0), 1));
  ufs->AddLengthDelimited(92, "\x0a\x24" + f.lib.b + "\x12\x05" "b.sfz");  // a TimbreRef-shaped payload
  std::string bytes;
  CHECK(ls::serialize(s, bytes) == ls::Status::kOk);
  ls::LoadResult lr;
  ls::ReadOptions ro;
  CHECK(ls::parse(bytes, ro, lr) == ls::Status::kOk);
  std::string again;
  CHECK(ls::serialize(lr.live_set, again) == ls::Status::kOk && again == bytes);  // the unknown fields survive a rewrite
  CHECK(f.load(lr.live_set));
  const core::Snapshot* base = f.rig.cur();
  std::array<std::shared_ptr<core::SampleBank>, 8> banks;
  for (std::size_t i = 0; i < 8; ++i) banks[i] = base->parts[i].sample_bank;
  const std::uint32_t timbre0 = base->parts[0].timbre_ref;
  for (std::uint32_t k = 0; k < 3; ++k) {
    CHECK(f.rig.scenes.recall_scene(k).result == SceneResult::kOk);
    const core::Snapshot* g = f.rig.cur();
    for (std::size_t i = 0; i < 8; ++i) CHECK(g->parts[i].sample_bank == banks[i] && g->parts[i].engine_type == base->parts[i].engine_type);
    CHECK(g->parts[0].timbre_ref == timbre0);
  }
  // Ids the schema forbids in a scene's parameter lists (the timbre reference, the part output, the insert type) are rejected at load.
  for (std::uint32_t id : {static_cast<std::uint32_t>(p::kTimbreRef), static_cast<std::uint32_t>(p::kPartOutput), static_cast<std::uint32_t>(p::kInsertType)}) {
    lsp::LiveSet bad = make_set(f.lib, 80);
    entry(scene_part(bad.mutable_scenes(1), 0)->mutable_engine_params(), id, 1);
    Fixture g;
    const host::LiveSetLoadResult res = g.rig.loader.load(bad);
    CHECK(res.status == host::LiveSetLoadStatus::kInvalidLiveSet);
  }
  // And even a scene that skipped validation cannot reach them: apply_scene writes covered fields only.
  lsp::Scene hostile;
  entry(scene_part(&hostile, 0)->mutable_engine_params(), p::kTimbreRef, 99);
  entry(scene_part(&hostile, 0)->mutable_envelope(), p::kPartOutput, 1);
  entry(hostile.mutable_master(), p::kTempoBpm, 200);
  core::Snapshot work = *base;
  host::apply_scene(hostile, *base, work);
  CHECK(work.parts[0].timbre_ref == base->parts[0].timbre_ref && work.parts[0].output == base->parts[0].output);
  CHECK(work.parts[0].sample_bank == base->parts[0].sample_bank);
  CHECK(work.perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)] == base->perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)]);
}

// Effects: an unchanged chain keeps its instances (RT does not cross-fade); a changed one gets new ones; the reservation covers the worst scene.
void test_inserts_in_scenes() {
  Fixture f;
  lsp::LiveSet s = make_set(f.lib);
  ws::v1::InsertSlot* base_slot = s.mutable_parts(0)->add_inserts();
  base_slot->set_type(ws::v1::INSERT_TYPE_EQ);
  // Scene 1 (chorus) replaces part 0's chain with two heavier effects; the others keep the base chain.
  lsp::InsertChain* chain = scene_part(s.mutable_scenes(1), 0)->mutable_inserts();
  chain->add_slots()->set_type(ws::v1::INSERT_TYPE_ROTARY);
  chain->add_slots()->set_type(ws::v1::INSERT_TYPE_EP_AMP);
  CHECK(f.load(s));
  Rig& rig = f.rig;
  // The reservation covers the heaviest scene: the same as a base that holds those effects.
  Fixture ref;
  lsp::LiveSet heavy = make_set(ref.lib);
  heavy.mutable_parts(0)->add_inserts()->set_type(ws::v1::INSERT_TYPE_ROTARY);
  heavy.mutable_parts(0)->add_inserts()->set_type(ws::v1::INSERT_TYPE_EP_AMP);
  heavy.clear_scenes();
  CHECK(ref.load(heavy));
  CHECK(rig.br->arbiter().reservation().fx == ref.rig.br->arbiter().reservation().fx);

  const core::Snapshot* g0 = rig.cur();
  const auto chain0 = g0->parts[0].chain;
  CHECK(chain0 && g0->parts[0].insert_count == 1);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // verse: no insert override
  CHECK(rig.cur()->parts[0].chain == chain0);                    // the very same instances
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);  // chorus: two other effects
  const core::Snapshot* g1 = rig.cur();
  CHECK(g1->parts[0].insert_count == 2 && g1->parts[0].chain && g1->parts[0].chain != chain0);
  CHECK(g1->parts[1].chain == g0->parts[1].chain);
  rig.run(4);
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);  // outro: the base chain comes back, as a new set of instances
  const core::Snapshot* g2 = rig.cur();
  CHECK(g2->parts[0].insert_count == 1 && g2->parts[0].inserts[0].type == static_cast<std::uint8_t>(p::kInsertTypeEq) && g2->parts[0].chain != g1->parts[0].chain);
  // The FX budget: a scene whose effects cost more than the budget is refused and changes nothing.
  rig.plane.set_fx_budget(rig.plane.fx_cost());
  const std::uint64_t rev = rig.plane.revision();
  const host::SceneOutcome o = rig.scenes.recall_scene(1);
  CHECK(o.result == SceneResult::kFxBudgetExceeded && !o.published && rig.plane.revision() == rev && rig.scenes.current_scene() == 2);
  // ... but a scene that does not raise the cost goes through.
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
}

// A recall storm with notes held: generations are reclaimed, none leak.
void test_recall_storm_no_leaks() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  rig.note_on(48);
  rig.note_on(55);
  rig.run(4);
  std::size_t max_live = 0;
  std::uint32_t last_key = 55;
  for (int i = 0; i < 1000; ++i) {
    const host::SceneOutcome o = rig.scenes.recall_scene(static_cast<std::uint32_t>(i % 3));
    CHECK(o.result == SceneResult::kOk && o.scene == i % 3);
    if (i % 50 == 25) {  // keep striking and releasing keys so generations are pinned and unpinned all the time
      rig.note_off(last_key);
      last_key = static_cast<std::uint32_t>(40 + (i / 50) % 20);
      rig.note_on(last_key);
    }
    rig.run(1);
    max_live = std::max(max_live, rig.plane.live_generations());
  }
  CHECK(max_live <= 6);  // two held notes plus the current generation and the one RT still holds
  CHECK(rig.plane.revision() == 1001);
  rig.note_off(48);
  for (int k = 40; k < 60; ++k) rig.note_off(static_cast<std::uint32_t>(k));
  rig.note_off(55);
  rig.run(300);
  rig.plane.reclaim();
  CHECK(rig.plane.live_generations() == 1);
  CHECK(rig.sample.active_voices() == 0 && rig.organ.active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // and it keeps working
}

// A full ledger answers BUSY and changes nothing.
void test_full_ledger_is_busy() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  bool busy = false;
  for (std::uint32_t i = 0; i < 40 && !busy; ++i) {
    rig.note_on(30 + i);  // each held note pins the generation it started on
    rig.run(1);
    const std::int32_t at = rig.scenes.current_scene();
    const std::uint64_t rev = rig.plane.revision();
    const host::SceneOutcome o = rig.scenes.recall_scene(i % 2);
    if (o.result == SceneResult::kBusy) {
      busy = true;
      CHECK(!o.published && rig.plane.revision() == rev && rig.scenes.current_scene() == at);
      CHECK(rig.plane.live_generations() == core::SnapshotExchange::kLedgerSlots);
      // the same through ws.v1: the typed busy error
      ws::v1::ClientMessage m;
      m.set_request_id(9);
      m.mutable_recall_scene()->set_index(1);
      bool changed = true;
      const ws::v1::ServerMessage rep = rig.plane.handle(m, changed);
      CHECK(!changed && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_BUSY && rep.request_id() == 9 && rep.revision() == rev);
      CHECK(rep.error().details().at("reason") == "ledger_full");
    } else {
      CHECK(o.result == SceneResult::kOk);
    }
  }
  CHECK(busy);
  for (std::uint32_t i = 0; i < 40; ++i) rig.note_off(30 + i);
  rig.run(200);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // freed generations make room again
}


// Every field family a scene may store, on audible parts: the generation, the forced performance slots, the sound; and the reset to the base.
void test_every_stored_field_and_reset() {
  Fixture f;
  const std::string sine = install_library(f.rig.tmp.path, 3, {{"s.sfz", -1.0f}});
  lsp::LiveSet s = make_set(f.lib);
  fixtures::timbre(s.mutable_parts(0), sine, "s.sfz");
  s.clear_scenes();
  lsp::Scene* full = s.add_scenes();
  full->set_name("everything");
  lsp::ScenePart* sp = scene_part(full, 0);
  sp->set_low_key(40);
  sp->set_octave(1);
  sp->set_transpose(2);
  sp->set_output(ws::v1::PART_OUTPUT_AUX);
  sp->set_send_reverb(0.2f);
  sp->set_send_delay(0.4f);
  entry(sp->mutable_envelope(), p::kKeyClick, 0.9f);
  entry(sp->mutable_eq(), p::kLeakage, 0.1f);
  entry(full->mutable_master(), p::kSendReverbMaster, 0.6f);
  entry(full->mutable_master(), p::kReverbLevel, 0.5f);
  lsp::Scene* none = s.add_scenes();
  none->set_name("omits them all");
  CHECK(f.load(s));
  Rig& rig = f.rig;
  const core::Snapshot* base = rig.cur();
  const auto slot = [](std::uint16_t id, std::uint32_t part) { return p::perf_slot(*p::find(id), part); };
  const std::size_t reverb_level_index = static_cast<std::size_t>(p::find(p::kReverbLevel)->master_index);
  const float base_reverb_level = base->master[reverb_level_index];
  CHECK(base->parts[0].low_key == 0 && base->parts[0].octave == 0 && base->parts[0].transpose == 0 && base->parts[0].output == p::kPartOutputMain);
  // the base sound: 100 Hz
  rig.note_on(60);
  rig.run(220);
  CHECK_NEAR(rig.frequency(200), 100.0, 5.0);
  rig.note_off(60);
  rig.run(80);

  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  const core::Snapshot* g = rig.cur();
  CHECK(g->parts[0].low_key == 40 && g->parts[0].high_key == 127 && g->parts[0].octave == 1 && g->parts[0].transpose == 2);
  CHECK(g->parts[0].output == p::kPartOutputAux);
  CHECK_NEAR(g->perf_init[slot(p::kSendReverbLevel, 0)], 0.2, 1e-6);
  CHECK_NEAR(g->perf_init[slot(p::kSendDelayLevel, 0)], 0.4, 1e-6);
  CHECK_NEAR(g->perf_init[slot(p::kKeyClick, 0)], 0.9, 1e-6);
  CHECK_NEAR(g->perf_init[slot(p::kLeakage, 0)], 0.1, 1e-6);
  CHECK_NEAR(g->perf_init[slot(p::kSendReverbMaster, 0)], 0.6, 1e-6);
  CHECK_NEAR(g->master[reverb_level_index], 0.5, 1e-6);
  for (std::uint16_t id : {p::kSendReverbLevel, p::kSendDelayLevel, p::kKeyClick, p::kLeakage, p::kSendReverbMaster})
    CHECK(g->perf_force[slot(id, 0)] == 1);
  CHECK(g->perf_force[slot(p::kSendDelayLevel, 1)] == 0 && g->perf_force[slot(p::kKeyClick, 1)] == 0);  // other parts: nothing stored
  rig.run(4);
  CHECK_NEAR(rig.live(p::kSendDelayLevel, 0), 0.4, 1e-6);
  CHECK_NEAR(rig.live(p::kKeyClick, 0), 0.9, 1e-6);
  // the sound: key 60 as played, an octave and two semitones up
  rig.note_on(60);
  rig.run(220);
  CHECK_NEAR(rig.frequency(200), 100.0 * std::pow(2.0, 14.0 / 12.0), 6.0);
  rig.note_off(60);
  rig.run(80);
  rig.note_on(36);  // below the new zone (40): part 0 does not take it, only part 1 (the whole keyboard) does
  rig.run(10);
  CHECK(rig.sample.active_voices() == 1);
  rig.note_off(36);
  rig.run(80);

  // A scene that omits them all brings every base value back.
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  g = rig.cur();
  CHECK(g->parts[0].low_key == 0 && g->parts[0].octave == 0 && g->parts[0].transpose == 0 && g->parts[0].output == p::kPartOutputMain);
  CHECK(g->perf_init[slot(p::kSendDelayLevel, 0)] == base->perf_init[slot(p::kSendDelayLevel, 0)]);
  CHECK(g->perf_init[slot(p::kKeyClick, 0)] == base->perf_init[slot(p::kKeyClick, 0)]);
  CHECK(g->perf_init[slot(p::kLeakage, 0)] == base->perf_init[slot(p::kLeakage, 0)]);
  CHECK(g->perf_init[slot(p::kSendReverbMaster, 0)] == base->perf_init[slot(p::kSendReverbMaster, 0)]);
  CHECK(g->master[reverb_level_index] == base_reverb_level);
  for (std::uint16_t id : {p::kSendReverbLevel, p::kSendDelayLevel, p::kKeyClick, p::kLeakage, p::kSendReverbMaster}) CHECK(g->perf_force[slot(id, 0)] == 0);
  rig.run(4);
  CHECK_NEAR(rig.live(p::kSendDelayLevel, 0), 0, 1e-6);
  CHECK_NEAR(rig.live(p::kKeyClick, 0), base->perf_init[slot(p::kKeyClick, 0)], 1e-6);
  rig.note_on(60);
  rig.run(220);
  CHECK_NEAR(rig.frequency(200), 100.0, 5.0);
  rig.note_off(60);
  rig.run(80);
}

// RT skips a scene's program generation (two recalls before it adopts either): what the first stored is still forced, to the newest value.
void test_skipped_program_generation_keeps_forced_slots() {
  Fixture f;
  CHECK(f.load(make_set(f.lib)));
  Rig& rig = f.rig;
  rig.live_move(p::kDrawbar8, 2, 5);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 5, 1e-6);
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);  // verse stores drawbar_8 = 3
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);  // outro stores nothing: drawbar_8 is the base value 8, equal to RT's last applied
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 8, 1e-6);  // without the carried mask the live 5 would survive
  // Once adopted, nothing is carried any more: a later recall forces only what it stores.
  rig.live_move(p::kDrawbar8, 2, 4);
  rig.run(2);
  CHECK(rig.scenes.recall_scene(2).result == SceneResult::kOk);
  CHECK(rig.cur()->perf_force[p::perf_slot(*p::find(p::kDrawbar8), 2)] == 0);
  rig.run(2);
  CHECK_NEAR(rig.live(p::kDrawbar8, 2), 4, 1e-6);
}

// ws.v1 level: the FX budget refusal, and BUSY with the reason (load in progress / ledger full).
void test_ws_level_refusals() {
  Fixture f;
  lsp::LiveSet s = make_set(f.lib);
  scene_part(s.mutable_scenes(1), 0)->mutable_inserts()->add_slots()->set_type(ws::v1::INSERT_TYPE_ROTARY);
  CHECK(f.load(s));
  Rig& rig = f.rig;
  rig.plane.set_fx_budget(rig.plane.fx_cost());
  ws::v1::ClientMessage m;
  m.set_request_id(31);
  m.mutable_recall_scene()->set_index(1);
  bool changed = true;
  const std::uint64_t rev = rig.plane.revision();
  ws::v1::ServerMessage rep = rig.plane.handle(m, changed);
  CHECK(!changed && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED && rep.request_id() == 31 && rep.revision() == rev);
  CHECK(rep.error().details().at("result") == "fx budget exceeded");
  CHECK(rig.plane.revision() == rev && rig.scenes.current_scene() == -1);
  // No program generation was consumed by the refusal.
  rig.plane.set_fx_budget(core::kMaxFxCost);
  const std::uint64_t pg_before = rig.plane.next_program_generation();
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  CHECK(rig.cur()->program_generation == pg_before + 1);

  // A recall that arrives during a load (here from the head provider, as the load runs on this very thread).
  ws::v1::ServerMessage during;
  bool during_changed = true;
  f.rig.heads.on_load = [&] { during = rig.plane.handle(m, during_changed); };
  lsp::LiveSet other = make_set(f.lib, 90);
  CHECK(rig.loader.load(other).ok());
  f.rig.heads.on_load = nullptr;
  CHECK(!during_changed && during.has_error() && during.error().code() == ws::v1::ERROR_CODE_BUSY);
  CHECK(during.error().details().at("reason") == "load_in_progress" && during.error().details().at("result") == "live set load in progress");
}

// The worst scene is what the arbiter reserves for: a Live Set whose worst scene does not fit is refused at load, the old one stays.
void test_worst_scene_over_the_budget_is_refused_at_load() {
  std::uint64_t budget = 0;
  {
    Fixture probe;
    CHECK(probe.load(make_set(probe.lib)));
    const auto r = probe.rig.br->arbiter().reservation();
    budget = r.fx + r.organ_rotary + r.floors;  // exactly what the base needs
  }
  Fixture f(budget);
  CHECK(f.load(make_set(f.lib)));  // fits
  const std::uint64_t rev = f.rig.plane.revision();
  const auto before = f.rig.br->arbiter().reservation();
  lsp::LiveSet heavy = make_set(f.lib, 91);
  lsp::InsertChain* chain = scene_part(heavy.mutable_scenes(1), 0)->mutable_inserts();
  chain->add_slots()->set_type(ws::v1::INSERT_TYPE_ROTARY);
  chain->add_slots()->set_type(ws::v1::INSERT_TYPE_EP_AMP);
  const host::LiveSetLoadResult res = f.rig.loader.load(heavy);
  CHECK(res.status == host::LiveSetLoadStatus::kArbiterRejected);
  CHECK(f.rig.plane.revision() == rev);
  const auto after = f.rig.br->arbiter().reservation();
  CHECK(after.fx == before.fx && after.floors == before.floors && after.organ_rotary == before.organ_rotary);
  CHECK(f.rig.scenes.recall_scene(1).result == SceneResult::kOk);  // the old Live Set's scenes still work
  CHECK(f.rig.scenes.current_scene() == 1 && f.rig.scenes.scene_count() == 3);
}

// Story 7.4 AC 3: Recall Scene 2 with Arp On, Up-Down, 1/16T, then Scene 1 with Arp Off.
// Arpeggiator starts and stops as a parameter-only snapshot swap, and the tempo does not change.
void test_arpeggiator_scene_recall() {
  Fixture f;
  lsp::LiveSet s = make_set(f.lib, 95);
  s.set_tempo(128.5f);
  // Base Live Set: Part 2 has Arp Off (Up, 1/8, 1 octave, 50% gate, 50% swing, latch off)
  s.mutable_parts(2)->mutable_arp()->set_enabled(false);
  s.mutable_parts(2)->mutable_arp()->set_pattern(lsp::ARP_PATTERN_UP);
  s.mutable_parts(2)->mutable_arp()->set_rate(lsp::ARP_RATE_EIGHTH);

  // Scene 0 (verse): keeps Arp Off on part 2
  scene_part(s.mutable_scenes(0), 2)->mutable_arp()->set_enabled(false);

  // Scene 1 (chorus): Part 2 has Arp On, Up-Down, 1/16T, 2 octaves, Gate 60%, Swing 55%, Latch On
  lsp::ArpSettings* a1 = scene_part(s.mutable_scenes(1), 2)->mutable_arp();
  a1->set_enabled(true);
  a1->set_pattern(lsp::ARP_PATTERN_UP_DOWN);
  a1->set_rate(lsp::ARP_RATE_SIXTEENTH_TRIPLET);
  a1->set_octave_range(2);
  a1->set_gate(60.0f);
  a1->set_swing(55.0f);
  a1->set_hold(true);

  CHECK(f.load(s));
  Rig& rig = f.rig;

  // Base load check: part 2 arp is Off, tempo is 128.5 BPM
  CHECK(!rig.cur()->parts[2].arp.enabled);
  CHECK(rig.br->tempo_bpm() == 128.5f);

  // Recall Scene 0 (verse): Arp Off on part 2, tempo unchanged
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  rig.run(2);
  CHECK(!rig.cur()->parts[2].arp.enabled);
  CHECK(!rig.br->arp(2).enabled());
  CHECK(rig.br->tempo_bpm() == 128.5f);

  // Recall Scene 1 (chorus): Arp On, Up-Down, 1/16T on part 2, tempo unchanged
  const std::uint64_t pg_before = rig.cur()->program_generation;
  CHECK(rig.scenes.recall_scene(1).result == SceneResult::kOk);
  CHECK(rig.cur()->program_generation == pg_before + 1);
  rig.run(2);
  CHECK(rig.cur()->parts[2].arp.enabled);
  CHECK(rig.cur()->parts[2].arp.mode == core::ArpMode::kUpDown);
  CHECK(rig.cur()->parts[2].arp.rate == core::ArpRate::k1_16T);
  CHECK(rig.cur()->parts[2].arp.octaves == 2);
  CHECK(rig.cur()->parts[2].arp.gate_percent == 60.0f);
  CHECK(rig.cur()->parts[2].arp.swing_percent == 55.0f);
  CHECK(rig.cur()->parts[2].arp.latch == true);
  CHECK(rig.br->arp(2).enabled());
  CHECK(rig.br->arp(2).config().mode == core::ArpMode::kUpDown);
  CHECK(rig.br->arp(2).config().rate == core::ArpRate::k1_16T);
  CHECK(rig.br->tempo_bpm() == 128.5f);

  // Recall Scene 0 (verse): Arp stops, tempo unchanged
  CHECK(rig.scenes.recall_scene(0).result == SceneResult::kOk);
  rig.run(2);
  CHECK(!rig.cur()->parts[2].arp.enabled);
  CHECK(!rig.br->arp(2).enabled());
  CHECK(rig.br->tempo_bpm() == 128.5f);
}

// --- ws.v1 over a real Unix domain socket -----------------------------------------------------------------------
struct Client {
  int fd = -1;
  bool open(const std::string& path) {
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    return fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  bool read_exact(std::string& out, std::size_t n, int timeout_ms) {
    out.clear();
    while (out.size() < n) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, timeout_ms) <= 0) return false;
      char buf[4096];
      const ssize_t k = ::recv(fd, buf, std::min(sizeof buf, n - out.size()), 0);
      if (k <= 0) return false;
      out.append(buf, static_cast<std::size_t>(k));
    }
    return true;
  }
  bool send(const ws::v1::ClientMessage& m) {
    std::string b;
    m.SerializeToString(&b);
    const std::uint32_t n = static_cast<std::uint32_t>(b.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    const std::string s = std::string(h, 4) + b;
    return ::send(fd, s.data(), s.size(), 0) == static_cast<ssize_t>(s.size());
  }
  bool recv(ws::v1::ServerMessage& m, int timeout_ms = 3000) {
    std::string h, b;
    if (!read_exact(h, 4, timeout_ms)) return false;
    const std::uint32_t n = (std::uint8_t(h[0]) << 24) | (std::uint8_t(h[1]) << 16) | (std::uint8_t(h[2]) << 8) | std::uint8_t(h[3]);
    return read_exact(b, n, timeout_ms) && m.ParseFromString(b);
  }
  bool call(const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply) {
    if (!send(m)) return false;
    for (;;) {  // skip pushed State messages (request_id 0)
      if (!recv(reply)) return false;
      if (reply.request_id() == m.request_id()) return true;
    }
  }
};

ws::v1::ClientMessage hello_msg() {
  ws::v1::ClientMessage m;
  m.set_request_id(1);
  m.mutable_hello()->set_version(1);
  m.mutable_hello()->set_client_name("scene-test");
  return m;
}

void test_ws_v1_scene_commands() {
  Fixture f;
  const std::string path = f.rig.tmp.path + "/e.sock";
  Rig& rig = f.rig;
  // Phase 1: no Live Set loaded.
  {
    host::ControlServer server(rig.plane, path);
    std::string err;
    CHECK(server.start(err));
    Client c;
    ws::v1::ServerMessage rep;
    CHECK(c.open(path) && c.call(hello_msg(), rep));
    ws::v1::ClientMessage m;
    m.set_request_id(2);
    m.mutable_recall_scene()->set_index(0);
    CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_LIVE_SET && rep.request_id() == 2);
    m.set_request_id(3);
    m.mutable_next_scene();
    CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_LIVE_SET);
    server.stop();
  }
  CHECK(f.load(make_set(f.lib)));
  CHECK(rig.plane.revision() == 1);
  std::atomic<bool> stop{false};
  std::thread rt([&] {  // a real "RT" thread keeps rendering while clients recall scenes
    float l[64], r[64];
    while (!stop.load()) {
      rig.br->process_block(l, r);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  {
    host::ControlServer server(rig.plane, path);
    std::string err;
    CHECK(server.start(err));
    Client sub;
    ws::v1::ServerMessage rep;
    CHECK(sub.open(path) && sub.call(hello_msg(), rep));
    ws::v1::ClientMessage m;
    m.set_request_id(20);
    m.mutable_subscribe()->set_enable(true);
    CHECK(sub.call(m, rep));
    ws::v1::ServerMessage pushed;
    CHECK(sub.recv(pushed) && pushed.state().live_set_loaded() && pushed.state().current_scene() == -1 && pushed.state().scene_count() == 3);

    // Typed errors and results.
    Client c;
    CHECK(c.open(path) && c.call(hello_msg(), rep));
    m.Clear();
    m.set_request_id(21);
    m.mutable_recall_scene()->set_index(7);
    CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_SUCH_SCENE && rep.revision() == 1);
    CHECK(rep.error().details().at("scene_count") == "3");
    m.set_request_id(22);
    m.mutable_recall_scene()->set_index(1);
    CHECK(c.call(m, rep) && rep.has_scene_reply() && rep.scene_reply().result() == ws::v1::SCENE_STEP_RESULT_OK &&
          rep.scene_reply().current_scene() == 1 && rep.revision() == 2 && rep.request_id() == 22);
    m.set_request_id(23);
    m.mutable_next_scene();
    CHECK(c.call(m, rep) && rep.scene_reply().current_scene() == 2 && rep.revision() == 3);
    m.set_request_id(24);
    m.mutable_next_scene();
    CHECK(c.call(m, rep) && rep.has_scene_reply() && rep.scene_reply().result() == ws::v1::SCENE_STEP_RESULT_AT_LAST && rep.revision() == 3);
    m.set_request_id(25);
    m.mutable_previous_scene();
    CHECK(c.call(m, rep) && rep.scene_reply().result() == ws::v1::SCENE_STEP_RESULT_OK && rep.scene_reply().current_scene() == 1);
    m.set_request_id(26);
    m.mutable_get_performance_values();
    CHECK(c.call(m, rep) && rep.has_performance_values() && rep.performance_values().current_scene() == 1);
    // the subscriber saw the three published generations
    for (std::uint64_t want : {2u, 3u, 4u}) {
      CHECK(sub.recv(pushed) && pushed.state().revision() == want);
    }
    CHECK(pushed.state().current_scene() == 1);

    // Concurrent clients are serialized by the control thread: every recall gets its own revision.
    constexpr int kClients = 4, kPer = 60;
    std::vector<std::vector<std::uint64_t>> revs(kClients);
    std::atomic<int> failures{0};
    std::vector<std::thread> th;
    for (int i = 0; i < kClients; ++i) {
      th.emplace_back([&, i] {
        Client cc;
        ws::v1::ServerMessage r2;
        if (!cc.open(path) || !cc.call(hello_msg(), r2)) { failures++; return; }
        for (int k = 0; k < kPer; ++k) {
          ws::v1::ClientMessage q;
          q.set_request_id(static_cast<std::uint64_t>(1000 * (i + 1) + k));
          q.mutable_recall_scene()->set_index(static_cast<std::uint32_t>((i + k) % 3));
          if (!cc.call(q, r2) || !r2.has_scene_reply() || r2.request_id() != q.request_id() ||
              r2.scene_reply().current_scene() != static_cast<std::int32_t>((i + k) % 3)) { failures++; return; }
          revs[static_cast<std::size_t>(i)].push_back(r2.revision());
        }
      });
    }
    for (auto& t : th) t.join();
    CHECK(failures.load() == 0);
    std::set<std::uint64_t> all;
    for (auto& v : revs) {
      for (std::size_t k = 1; k < v.size(); ++k) CHECK(v[k] > v[k - 1]);
      all.insert(v.begin(), v.end());
    }
    CHECK(all.size() == static_cast<std::size_t>(kClients * kPer));
    CHECK(*all.begin() == 5 && *all.rbegin() == 4 + kClients * kPer);
    server.stop();
  }
  stop.store(true);
  rt.join();
}

}  // namespace

int main() {
  test_recall_is_parameter_only();
  test_performance_values_applied_once();
  test_notes_continue_across_recall();
  test_zone_change_note_off_consistency();
  test_unaffected_audio_is_sample_identical();
  test_volume_change_is_gap_free();
  test_next_previous_stop_at_the_ends();
  test_missing_scene_and_no_live_set();
  test_recall_during_load();
  test_new_live_set_resets_scene();
  test_scene_cannot_change_a_timbre();
  test_inserts_in_scenes();
  test_recall_storm_no_leaks();
  test_full_ledger_is_busy();
  test_every_stored_field_and_reset();
  test_skipped_program_generation_keeps_forced_slots();
  test_ws_level_refusals();
  test_worst_scene_over_the_budget_is_refused_at_load();
  test_arpeggiator_scene_recall();
  test_ws_v1_scene_commands();
  if (g_fail) {
    std::printf("scene_controller_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("scene_controller_tests: ok\n");
  return 0;
}

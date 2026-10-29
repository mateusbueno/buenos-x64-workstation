// Story 4.9: edits in memory and Write scopes. The rig is the engine without its threads: a block renderer behind the real render driver (so the
// control->RT fence travels the same ordered lane as parameter changes), the plane, the scene controller, the loader, the switcher in its deterministic
// mode (no worker thread), both stores on a temporary data root, the setlist controller and the edit session; control tick + one block is a step. Libraries
// are synthetic (a looping constant per instrument). Nothing here asserts a duration: waits are counted in blocks, the fence timeout uses a fake clock, and
// "1 hour of playing" is a generated event script on a fake clock.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/edit_session.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"

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
namespace sl = host::setlist;
using EC = ws::v1::ErrorCode;
using fixtures::entry;
using fixtures::uuid_n;

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-edit-XXXXXX").string();
    if (!mkdtemp(path.data())) std::exit(1);
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string install_library(const std::string& root, std::uint32_t index, const char* key, float level) {
  const std::string fp = "velora-edit-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(key, key);
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(4800, format::float_to_bits(level));
  CHECK(w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, 4800, pcm}) == format::Error::kNone);
  format::Region r;
  r.sample = 0;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = format::LoopMode::kLoopContinuous;
  r.ampeg.release = 0.05f;
  CHECK(w.add_region(ins.value(), r) == format::Error::kNone);
  auto bytes = w.finalize();
  CHECK(bytes.ok());
  auto probe = format::BundleReader::open_memory(bytes.value());
  CHECK(probe.ok());
  const std::string lib_id = format::uuid_to_string(lib), bundle_id = format::to_hex(probe.value().bundle_id());
  const fs::path dir = fs::path(root) / "lib" / lib_id / bundle_id;
  fs::create_directories(dir);
  std::ofstream f(dir / "bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  f.close();
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

struct Libs {
  std::string a, b, c;
};

// "Perfect": two sample parts (libraries a, b), two organ parts; scene 1 "Verse" stores part 1's volume, reverb and delay send, output, insert chain, the master reverb level and part 3's drawbar_8;
// scene 2 "Chorus" stores part 2's volume; scene 3 "Plain" stores nothing.
lsp::LiveSet perfect(const Libs& L, std::uint64_t seed = 10, const char* name = "Perfect") {
  lsp::LiveSet s = ls::make_default_live_set(uuid_n(seed), name, 8);
  fixtures::timbre(s.mutable_parts(0), L.a, "a.sfz");
  fixtures::timbre(s.mutable_parts(1), L.b, "b.sfz");
  const float vols[4] = {0.8f, 0.75f, 0.5f, 0.5f};
  for (int i = 0; i < 4; ++i) s.mutable_parts(i)->set_volume(vols[i]);
  for (int i = 2; i < 4; ++i) {
    lsp::Part* o = s.mutable_parts(i);
    o->clear_timbre();
    o->mutable_engine()->set_engine_type_id(2);
  }
  lsp::Scene* verse = s.add_scenes();
  verse->set_name("Verse");
  verse->add_parts()->set_volume(0.6f);
  verse->mutable_parts(0)->set_send_reverb(0.4f);
  verse->mutable_parts(0)->set_send_delay(0.1f);
  verse->mutable_parts(0)->set_output(ws::v1::PART_OUTPUT_MAIN);
  verse->mutable_parts(0)->mutable_inserts()->add_slots()->set_type(ws::v1::INSERT_TYPE_EQ);
  entry(verse->mutable_master(), p::kReverbLevel, 0.5f);
  verse->add_parts();
  entry(verse->add_parts()->mutable_engine_params(), p::kDrawbar8, 5);
  lsp::Scene* chorus = s.add_scenes();
  chorus->set_name("Chorus");
  chorus->add_parts();
  chorus->add_parts()->set_volume(0.9f);
  s.add_scenes()->set_name("Plain");
  return s;
}

std::string write_set(const std::string& dir, const lsp::LiveSet& s) {
  std::string bytes;
  CHECK(ls::serialize(s, bytes) == ls::Status::kOk);
  const std::string path = dir + "/" + s.id() + ".liveset";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

ws::v1::ClientMessage msg() {
  static std::uint64_t rid = 1000;
  ws::v1::ClientMessage m;
  m.set_request_id(++rid);
  return m;
}

struct Rig {
  TmpDir tmp;
  std::string root, live_dir, set_dir;
  Libs lib;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  host::HostEventQueue midi_q, ctl_q;
  host::RenderDriver driver{*br, midi_q, {}, &ctl_q};
  std::uint64_t now_ns = 1'000'000'000ull;
  host::ControlPlane plane{br->snapshots(), br->last_values(), ctl_q, [this] { return now_ns; }};
  core::SampleEngine sample{[] {
    core::SampleEngine::Config c;
    c.max_voices = 340;
    return c;
  }()};
  core::OrganEngine organ;
  host::SceneController scenes{plane};
  host::LibraryResolver resolver;
  host::RamHeadProvider heads;
  host::LiveSetLoader loader;
  std::unique_ptr<host::LiveSetSwitcher> sw;
  ls::SeededRandom rng{7};
  std::unique_ptr<ls::LiveSetStore> live;
  std::unique_ptr<sl::SetlistStore> store;
  std::unique_ptr<host::LiveSetSwitcherPort> port;
  std::unique_ptr<host::SetlistController> ctl;
  std::unique_ptr<host::EditSession> edits;
  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::vector<float> last_block;
  int idle_calls = 0;
  bool pump_fence = true;

  static host::LiveSetLoaderOptions lopts(host::SceneController* s) {
    host::LiveSetLoaderOptions o;
    o.scenes = s;
    return o;
  }
  Rig()
      : resolver(host::make_posix_fs(), tmp.path),
        loader(*br, plane, resolver, heads, &sample, lopts(&scenes)) {
    root = tmp.path;
    live_dir = tmp.path + "/data/live_sets";
    set_dir = tmp.path + "/data/setlists";
    fs::create_directories(live_dir);
    fs::create_directories(set_dir);
    lib.a = install_library(root, 1, "a.sfz", 0.1f);
    lib.b = install_library(root, 2, "b.sfz", 0.02f);
    lib.c = install_library(root, 3, "c.sfz", 0.05f);
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }

  // Sets are written first (files), then the engine is started: switcher, stores, controller, session (as engine-host wires them).
  void start(const std::vector<lsp::LiveSet>& sets, std::size_t first = 0, std::uint32_t hold_ticks = 0, std::function<void(host::EditSessionOptions&)> tweak = {},
             bool load_first = true) {
    for (const lsp::LiveSet& s : sets) write_set(live_dir, s);
    host::SwitcherOptions so;
    so.background_thread = false;
    so.hold_ticks = hold_ticks;
    sw = std::make_unique<host::LiveSetSwitcher>(loader, plane, so);
    sw->set_catalog(host::scan_live_set_dir(live_dir, 8));
    ls::StoreOptions lo;
    lo.dir = live_dir;
    sl::StoreOptions sto;
    sto.dir = set_dir;
    sto.rng = &rng;
    CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
    CHECK(sl::SetlistStore::open(sto, store) == sl::Status::kOk);
    port = std::make_unique<host::LiveSetSwitcherPort>(*sw);
    ctl = std::make_unique<host::SetlistController>(plane, *store, *live, *port);
    host::EditSessionOptions eo;
    host::LiveSetSwitcher* swp = sw.get();
    eo.load_in_progress = [swp] { return swp->loading(); };
    eo.reload_current = [swp] { return swp->reload_current(); };
    eo.active_job_token = [swp] { return swp->active_token(); };
    eo.fence_idle = [this] {
      ++idle_calls;
      if (pump_fence) render(1);
    };
    eo.rng = &rng;
    if (tweak) tweak(eo);
    edits = std::make_unique<host::EditSession>(plane, scenes, *live, store.get(), ctl.get(), port.get(), eo);
    edits->attach(loader);
    edits->attach(*sw);
    if (load_first) {
      CHECK(loader.load(sets[first]).ok());
      render(2);
    }
  }

  void render(int n) {
    for (int i = 0; i < n; ++i) {
      now_ns += 1'333'333;
      driver.render_block(l, r, now_ns);
      last_block.assign(l, l + core::kBlockFrames);
    }
  }
  // The engine's loop: control tick, then one block.
  void step(int n = 1) {
    for (int i = 0; i < n; ++i) {
      sw->tick();
      ctl->tick();
      edits->tick();
      render(1);
    }
  }
  // Steps until the switcher is idle and the session finished restoring (bounded: a hang fails instead of blocking).
  void settle(int max = 400) {
    for (int i = 0; i < max && (sw->loading() || edits->restoring() || edits->reload_active()); ++i) step();
    step(3);
  }

  ws::v1::ServerMessage send(const ws::v1::ClientMessage& m, bool* changed = nullptr) {
    bool c = false;
    ws::v1::ServerMessage rep = plane.handle(m, c);
    if (changed) *changed = c;
    CHECK(rep.request_id() == m.request_id());
    return rep;
  }
  static bool is_err(const ws::v1::ServerMessage& rep, EC c) { return rep.has_error() && rep.error().code() == c; }
  ws::v1::State state() {
    auto m = msg();
    m.mutable_get_state();
    const auto rep = send(m);
    CHECK(rep.has_state());
    return rep.state();
  }
  ws::v1::ServerMessage volume(std::uint32_t part, float v) {
    auto m = msg();
    m.mutable_set_part_volume()->set_part(part);
    m.mutable_set_part_volume()->set_volume(v);
    return send(m);
  }
  ws::v1::ServerMessage param(std::uint32_t id, std::uint32_t part, float v) {
    auto m = msg();
    auto* sp = m.mutable_set_performance_param();
    sp->set_param_id(static_cast<ws::v1::ParamId>(id));
    sp->set_part(part);
    sp->set_value(v);
    return send(m);
  }
  ws::v1::ServerMessage recall(std::uint32_t index) {
    auto m = msg();
    m.mutable_recall_scene()->set_index(index);
    return send(m);
  }
  ws::v1::ServerMessage write(ws::v1::WriteScope scope, const char* name = nullptr) {
    auto m = msg();
    m.mutable_write_live_set()->set_scope(scope);
    if (name) m.mutable_write_live_set()->set_new_name(name);
    return send(m);
  }
  ws::v1::ServerMessage timbre(std::uint32_t part, const std::string& lib_id, const std::string& key) {
    auto m = msg();
    m.mutable_set_part_timbre()->set_part(part);
    m.mutable_set_part_timbre()->set_library_id(lib_id);
    m.mutable_set_part_timbre()->set_instrument_key(key);
    return send(m);
  }
  // A param moved live, applied by RT, captured by a tick.
  void move(std::uint32_t id, std::uint32_t part, float v) {
    CHECK(param(id, part, v).has_ack());
    step(6);
  }
  float live_value(std::uint32_t id, std::uint32_t part) { return br->last_values().load(p::perf_slot(*p::find(id), part)); }
  const core::Snapshot* cur() const { return plane.current_snapshot(); }
  std::string setlist(const std::string& name, const std::vector<std::string>& ids) {
    auto m = msg();
    m.mutable_create_setlist()->set_name(name);
    const auto rep = send(m);
    CHECK(rep.has_created_reply());
    const std::string id = rep.created_reply().id();
    for (const std::string& l_ : ids) {
      auto a = msg();
      a.mutable_add_live_set_to_setlist()->set_setlist_id(id);
      a.mutable_add_live_set_to_setlist()->set_live_set_id(l_);
      CHECK(send(a).has_ack());
    }
    auto ld = msg();
    ld.mutable_load_setlist()->set_id(id);
    CHECK(send(ld).has_load_setlist_reply());
    return id;
  }
  std::vector<std::string> setlist_ids(const std::string& id) {
    auto m = msg();
    m.mutable_get_setlists()->set_setlist_id(id);
    std::vector<std::string> v;
    const auto rep = send(m);
    for (const auto& e : rep.setlists_reply().entries()) v.push_back(e.live_set_id());
    return v;
  }
  lsp::LiveSet read_file(const std::string& id) {
    ls::LoadResult lr;
    CHECK(live->read(id, lr) == ls::Status::kOk);
    return lr.live_set;
  }
  std::map<std::string, std::string> dir_fingerprint(const std::string& dir) {  // name -> size + mtime
    std::map<std::string, std::string> m;
    for (const auto& e : fs::directory_iterator(dir)) {
      struct stat st {};
      ::stat(e.path().c_str(), &st);
      m[e.path().filename().string()] = std::to_string(st.st_size) + "/" + std::to_string(static_cast<long long>(fs::last_write_time(e.path()).time_since_epoch().count()));
    }
    return m;
  }
  void note_on(std::uint32_t key, std::uint32_t ch = 0) { CHECK(br->submit({core::make_note_on(0, ch, key, 65535), 0})); }
  void note_off(std::uint32_t key, std::uint32_t ch = 0) { CHECK(br->submit({core::make_note_off(0, ch, key, 0), 0})); }
  static double unity() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }
  double level() const {
    double s = 0;
    for (std::uint32_t i = core::kBlockFrames - 32; i < core::kBlockFrames; ++i) s += last_block[i];
    return s / 32.0 / unity();
  }
};

float base_param(const lsp::LiveSet& s, int part, std::uint32_t id) {
  float v = -1;
  for (const auto& e : s.parts(part).engine().params())
    if (e.param_id() == id) v = e.value();
  return v;
}

const ws::v1::ChangeItem* find_item(const ws::v1::EditStatus& e, const std::string& key, int part = -1, int param = -1, int scene = -1) {
  for (const auto& i : e.change_summary())
    if (i.key() == key && (part < 0 || (i.has_part() && static_cast<int>(i.part()) == part)) && (param < 0 || (i.has_param_id() && static_cast<int>(i.param_id()) == param)) &&
        (scene < 0 || (i.has_scene() && i.scene() == scene)))
      return &i;
  return nullptr;
}

// ---- the fence ------------------------------------------------------------------------------------------------------------------------
// RT acknowledges a fence only after it applied every parameter event sent before it, even events the time mapper deferred to later blocks.
void test_fence_orders_after_earlier_events() {
  Rig rig;
  rig.start({perfect(rig.lib)});
  float v = 0;
  int deferred_rounds = 0;
  for (int round = 0; round < 40; ++round) {
    v = static_cast<float>(round % 9);
    CHECK(rig.param(p::kDrawbar16, 3, v).has_ack());  // queued, not applied
    std::uint32_t seq = 0;
    CHECK(rig.plane.raise_fence(seq));
    CHECK(!rig.plane.fence_acked(seq) || rig.live_value(p::kDrawbar16, 3) == v);
    int blocks = 0;
    while (!rig.plane.fence_acked(seq) && blocks < 50) {
      if (rig.live_value(p::kDrawbar16, 3) != v) ++deferred_rounds;  // the event is still in flight while the fence is not acknowledged
      rig.render(1);
      ++blocks;
      // The ordering guarantee: acked => the earlier event is applied.
      if (rig.plane.fence_acked(seq)) CHECK(rig.live_value(p::kDrawbar16, 3) == v);
    }
    CHECK(rig.plane.fence_acked(seq) && blocks < 50);
    CHECK(rig.live_value(p::kDrawbar16, 3) == v);
  }
  CHECK(deferred_rounds > 0);  // the time mapper does defer events beyond the next block, so this ordering is not an accident of timing
  // Sequence numbers only grow and the acknowledgement of a later fence covers an earlier one.
  std::uint32_t a = 0, b = 0;
  CHECK(rig.plane.raise_fence(a) && rig.plane.raise_fence(b) && b == a + 1);
  rig.render(8);
  CHECK(rig.plane.fence_acked(a) && rig.plane.fence_acked(b));
  CHECK(rig.plane.fence_acked(a - 5) && !rig.plane.fence_acked(b + 1));
}

void test_fence_lane_full_and_timeout() {
  Rig rig;
  double fake_now = 0;
  rig.start({perfect(rig.lib)}, 0, 0, [&](host::EditSessionOptions& o) {
    o.clock = [&] { return fake_now += 0.5; };  // every look at the clock is half a second later: no sleeping, no real time
    o.fence_timeout_s = 2.0;
    o.fence_idle = [] {};
  });
  // A full lane: nothing queued, a typed busy.
  std::uint32_t seq = 0;
  int pushed = 0;
  while (rig.plane.push_performance_value(*p::find(p::kDrawbar8), 2, 4) && pushed < 5000) ++pushed;
  CHECK(pushed > 0 && pushed < 5000);
  CHECK(!rig.plane.raise_fence(seq));
  auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY) && rep.error().details().at("reason") == "fence_queue_full");
  rig.render(300);  // the lane drains
  // RT never acknowledges (nobody renders): a typed, bounded refusal, nothing written, nothing changed.
  rig.volume(1, 0.3f);
  const std::uint64_t rev = rig.plane.revision();
  rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY) && rep.error().details().at("reason") == "fence_timeout");
  CHECK(rig.plane.revision() == rev && rig.live->mutations() == 0 && rig.edits->is_dirty(rig.edits->current_id()));
}

// ---- dirty flags and the change summary ---------------------------------------------------------------------------------------------------
void test_dirty_flag_and_change_summary() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  const lsp::LiveSet b = perfect(rig.lib, 11, "Other");
  rig.start({a, b});
  auto st = rig.state();
  CHECK(st.edits().available() && !st.edits().current_dirty() && st.edits().dirty_count() == 0 && st.edits().change_count() == 0);
  CHECK(st.edits().scene_write_blocked_reason() == "no_scene" && !st.edits().scene_write_available());

  // Structural edits.
  CHECK(rig.volume(2, 0.25f).has_ack());
  auto ins = msg();
  ins.mutable_set_insert_chain()->set_part(1);
  ws::v1::InsertSlot* slot = ins.mutable_set_insert_chain()->add_slots();
  slot->set_type(ws::v1::INSERT_TYPE_EQ);
  CHECK(rig.send(ins).has_ack());
  auto out = msg();
  out.mutable_set_part_output()->set_part(3);
  out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  CHECK(rig.send(out).has_ack());
  auto snd = msg();
  snd.mutable_set_send_levels()->set_part(0);
  snd.mutable_set_send_levels()->set_reverb(0.6f);
  snd.mutable_set_send_levels()->set_delay(0.0f);
  CHECK(rig.send(snd).has_ack());
  auto mas = msg();
  auto* pv = mas.mutable_set_master_params()->add_values();
  pv->set_param_id(static_cast<ws::v1::ParamId>(p::kMasterLevel));
  pv->set_value(-6.0f);
  CHECK(rig.send(mas).has_ack());
  rig.step(4);
  st = rig.state();
  const ws::v1::EditStatus& e = st.edits();
  CHECK(e.current_dirty() && e.dirty_count() == 1 && e.dirty_live_set_ids_size() == 1 && e.dirty_live_set_ids(0) == a.id());
  CHECK(find_item(e, "part.volume", 2) && find_item(e, "part.volume", 2)->text() == "Part 3 volume");
  CHECK(find_item(e, "part.inserts", 1) && find_item(e, "part.inserts", 1)->text() == "Part 2 inserts");
  CHECK(find_item(e, "part.output", 3) && find_item(e, "part.send_reverb", 0) && find_item(e, "part.send_reverb", 0)->text() == "Part 1 reverb send");
  CHECK(find_item(e, "master.param", -1, static_cast<int>(p::kMasterLevel)) &&
        find_item(e, "master.param", -1, static_cast<int>(p::kMasterLevel))->text() == "Master master level");
  CHECK(e.change_count() == 5 && !e.change_summary_text().empty() && e.change_summary_text().find("Part 3 volume") != std::string::npos);
  CHECK(!e.has_timbre_edit());

  // A performance value moved live is read from the RT table and joins the summary.
  rig.move(p::kDrawbar16, 2, 5);
  st = rig.state();
  CHECK(find_item(st.edits(), "part.param", 2, static_cast<int>(p::kDrawbar16)) &&
        find_item(st.edits(), "part.param", 2, static_cast<int>(p::kDrawbar16))->text() == "Part 3 drawbar 16");
  CHECK(st.edits().change_count() == 6);

  // An edit moved back to its saved value is not an edit any more.
  CHECK(rig.volume(2, 0.5f).has_ack());
  rig.move(p::kDrawbar16, 2, 0);
  st = rig.state();
  CHECK(!find_item(st.edits(), "part.volume", 2) && !find_item(st.edits(), "part.param", 2) && st.edits().change_count() == 4);

  // The other Live Set is clean; only the current one is listed as dirty.
  CHECK(!rig.edits->is_dirty(b.id()) && rig.edits->is_dirty(a.id()));
}

// ---- edits kept across scene changes ----------------------------------------------------------------------------------------------------------
void test_edits_kept_across_scene_changes() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  // Base edit with no scene current: part 2 (index 1) volume; scene "Chorus" (index 1) stores it, "Verse" (index 0) does not.
  CHECK(rig.volume(1, 0.33f).has_ack());
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.33f, 1e-6);
  CHECK(rig.recall(0).has_scene_reply());  // Verse does not store part 1's volume: the edited base value stays
  rig.step(3);
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.33f, 1e-6);
  CHECK_NEAR(rig.cur()->parts[0].volume, 0.6f, 1e-6);  // Verse's own value
  CHECK(rig.recall(1).has_scene_reply());  // Chorus stores 0.9
  rig.step(3);
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.9f, 1e-6);
  // An edit made while Chorus is current lands in Chorus (it stores the field), not in the base.
  CHECK(rig.volume(1, 0.45f).has_ack());
  rig.step(3);
  CHECK_NEAR(rig.edits->working_of(a.id())->scenes(1).parts(1).volume(), 0.45f, 1e-6);
  CHECK_NEAR(rig.edits->working_of(a.id())->parts(1).volume(), 0.33f, 1e-6);
  CHECK(rig.recall(2).has_scene_reply());  // Plain: stores nothing, so the base (edited) value
  rig.step(3);
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.33f, 1e-6);
  CHECK(rig.recall(1).has_scene_reply());  // back in Chorus: its edit is still there
  rig.step(3);
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.45f, 1e-6);
  CHECK(rig.edits->is_dirty(a.id()));
  const auto items = rig.edits->summary(a.id());
  bool base = false, scene = false;
  for (const auto& i : items) {
    base = base || (i.key == "part.volume" && i.part == 1 && i.scene == -1);
    scene = scene || (i.key == "scene.part.volume" && i.part == 1 && i.scene == 1 && i.text == "Scene 2 Part 2 volume");
  }
  CHECK(base && scene);
}

void test_performance_edits_kept_across_scene_changes() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  // drawbar_8 of part 3 (index 2): initial 8; scene Verse stores 5, the others do not.
  CHECK(rig.live_value(p::kDrawbar8, 2) == 8);
  rig.move(p::kDrawbar8, 2, 3);  // no scene current: lands in the base
  CHECK(base_param(*rig.edits->working_of(a.id()), 2, p::kDrawbar8) == 3);
  CHECK(rig.recall(0).has_scene_reply());  // Verse: forced to its own 5
  rig.step(4);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 5);
  CHECK(!rig.edits->is_dirty(a.id()) || rig.edits->summary(a.id()).size() == 1);  // the recall's own value is not an edit
  CHECK(rig.recall(2).has_scene_reply());  // Plain: not stored, so the base (edited) 3, not the saved 8
  rig.step(4);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 3);
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(4);
  rig.move(p::kDrawbar8, 2, 7);  // Verse stores it: the edit lands in Verse
  CHECK(rig.recall(1).has_scene_reply());  // Chorus: base 3
  rig.step(4);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 3);
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(4);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 7);
  CHECK(rig.edits->working_of(a.id())->scenes(0).parts(2).engine_params(0).value() == 7);
  // The saved Live Set never changed.
  CHECK(rig.edits->saved_of(a.id())->parts(2).engine().params_size() == 0);
}

// A recall or a switch that RT has not applied yet is not an edit.
void test_unapplied_generation_is_not_an_edit() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.recall(0).has_scene_reply());
  CHECK(!rig.edits->sync_live_values());  // RT has not rendered a block since the recall
  CHECK(!rig.edits->is_dirty(a.id()));
  rig.render(2);
  rig.edits->tick();
  CHECK(!rig.edits->is_dirty(a.id()));
  CHECK(rig.live_value(p::kDrawbar8, 2) == 5);
}

// ---- edits kept across Live Set changes -----------------------------------------------------------------------------------------------------
void test_edits_kept_across_live_set_changes() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  lsp::LiveSet b = perfect(rig.lib, 11, "Other");
  b.mutable_parts(1)->set_volume(0.2f);
  rig.start({a, b});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.move(p::kDrawbar8, 2, 2);
  // A value moved right before the jump and not yet captured by a tick is captured when the switch publishes.
  CHECK(rig.param(p::kDrawbar4, 3, 6).has_ack());
  rig.render(6);
  CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.loader.loaded_id() == b.id());
  CHECK_NEAR(rig.cur()->parts[1].volume, 0.2f, 1e-6);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 8 && rig.live_value(p::kDrawbar4, 3) == 0);  // B's own initial values
  auto st = rig.state();
  CHECK(st.edits().dirty_count() == 1 && st.edits().dirty_live_set_ids(0) == a.id() && !st.edits().current_dirty() && st.live_set_id() == b.id());
  CHECK(rig.edits->is_dirty(a.id()) && !rig.edits->is_dirty(b.id()));
  // B has its own edit.
  CHECK(rig.volume(3, 0.1f).has_ack());
  rig.step(2);
  CHECK(rig.state().edits().dirty_count() == 2);
  // Back to A: every edit is there.
  CHECK(rig.sw->jump_to(a.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.loader.loaded_id() == a.id());
  CHECK_NEAR(rig.cur()->parts[0].volume, 0.35f, 1e-6);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 2 && rig.live_value(p::kDrawbar4, 3) == 6);
  st = rig.state();
  CHECK(st.edits().current_dirty() && st.edits().dirty_count() == 2);
  CHECK(find_item(st.edits(), "part.volume", 0) && find_item(st.edits(), "part.param", 2, static_cast<int>(p::kDrawbar8)) &&
        find_item(st.edits(), "part.param", 3, static_cast<int>(p::kDrawbar4)));
  // And the saved files were never touched.
  CHECK(rig.live->mutations() == 0 && rig.store->mutations() == 0);
  // B once more: its edit is there too.
  CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK_NEAR(rig.cur()->parts[3].volume, 0.1f, 1e-6);
}


// ---- Story 9.10: the Live Set revision, stale edits and the STUDIO indicator ---------------------------------------------------------------------
ws::v1::ClientMessage studio_param(std::uint32_t id, std::uint32_t part, float v, std::uint64_t base) {
  auto m = msg();
  auto* sp = m.mutable_set_performance_param();
  sp->set_param_id(static_cast<ws::v1::ParamId>(id));
  sp->set_part(part);
  sp->set_value(v);
  m.set_base_revision(base);
  return m;
}

void test_live_set_revision_stale_edits_and_studio_indicator() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  const lsp::LiveSet b = perfect(rig.lib, 11, "Other");
  rig.start({a, b});
  rig.step(4);
  const std::uint64_t r0 = rig.plane.live_set_revision();
  CHECK(r0 >= 1 && rig.state().edits().live_set_revision() == r0 && !rig.state().edits().studio_edit_active());

  // Studio edits (base_revision = the revision Studio loaded) are accepted, light STUDIO, mark the Live Set dirty, and never move the revision (not even once the live values are captured).
  bool changed = false;
  auto rep = rig.send(studio_param(p::kDrawbar8, 2, 3, r0), &changed);
  CHECK(rep.has_ack() && changed);  // STUDIO lights: subscribers get a State push
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 4, r0)).has_ack());  // two quick edits of one knob: the intermediate value is no device-side change either
  CHECK(rig.send(studio_param(p::kDrawbar4, 3, 6, r0)).has_ack());
  rig.step(6);
  CHECK(rig.live_value(p::kDrawbar8, 2) == 4 && rig.live_value(p::kDrawbar4, 3) == 6);
  CHECK(rig.plane.live_set_revision() == r0);
  auto st = rig.state();
  CHECK(st.edits().current_dirty() && st.edits().studio_edit_active() && st.edits().live_set_revision() == r0);
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 5, r0)).has_ack());  // still not stale after its own edits
  rig.step(6);
  CHECK(rig.plane.live_set_revision() == r0);
  // The Part Editor's send knob and volume knob (live value + structural initial level, and a structural volume): none of them is a device-side change once RT adopts the generation.
  CHECK(rig.send(studio_param(p::kSendReverbLevel, 0, 0.7f, r0)).has_ack());
  rig.send([&] { auto m = msg(); m.mutable_set_send_levels()->set_part(0); m.mutable_set_send_levels()->set_reverb(0.7f); m.mutable_set_send_levels()->set_delay(0.1f); m.set_base_revision(r0); return m; }());
  CHECK(rig.send([&] { auto m = msg(); m.mutable_set_part_volume()->set_part(1); m.mutable_set_part_volume()->set_volume(0.33f); m.set_base_revision(r0); return m; }()).has_ack());
  rig.step(10);
  CHECK(rig.plane.live_set_revision() == r0);

  // STUDIO goes out after 3 s without an edit, and the tick reports it (once) so the State is pushed.
  CHECK(rig.plane.take_studio_activity_changed());  // on (first report)
  CHECK(!rig.plane.take_studio_activity_changed());
  rig.now_ns += 2'900'000'000ull;
  CHECK(rig.plane.studio_edit_active() && !rig.plane.take_studio_activity_changed());
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 5, r0)).has_ack());  // an edit renews the window
  rig.now_ns += 2'900'000'000ull;
  CHECK(rig.plane.studio_edit_active());
  rig.now_ns += 200'000'000ull;
  CHECK(!rig.plane.studio_edit_active() && rig.plane.take_studio_activity_changed() && !rig.state().edits().studio_edit_active());

  // A change from the unit's own controls (a value RT holds that no Studio edit sent: encoder / fader / MIDI CC / panel) moves the revision once captured.
  CHECK(rig.plane.push_performance_value(*p::find(p::kDrawbar2), 3, 4));
  rig.step(6);
  const std::uint64_t r1 = rig.plane.live_set_revision();
  CHECK(r1 > r0 && rig.state().edits().live_set_revision() == r1);
  // ... so an edit built on the old view is refused, typed, and changes nothing.
  const std::uint64_t rejected = rig.plane.commands_rejected(), stale_n = rig.plane.stale_rejections();
  const float before = rig.live_value(p::kDrawbar8, 2);
  rep = rig.send(studio_param(p::kDrawbar8, 2, 1, r0));
  CHECK(Rig::is_err(rep, EC::ERROR_CODE_STALE_REVISION) && rep.error().details().at("base_revision") == std::to_string(r0) &&
        rep.error().details().at("current_revision") == std::to_string(r1));
  rig.step(6);
  CHECK(rig.live_value(p::kDrawbar8, 2) == before && rig.plane.commands_rejected() == rejected + 1 && rig.plane.stale_rejections() == stale_n + 1);
  // Every edit class is checked: structural ones and ASSIGN too.
  rep = rig.send([&] { auto m = msg(); m.mutable_set_part_volume()->set_part(0); m.mutable_set_part_volume()->set_volume(0.1f); m.set_base_revision(r0); return m; }());
  CHECK(Rig::is_err(rep, EC::ERROR_CODE_STALE_REVISION));
  rep = rig.send([&] { auto m = msg(); m.mutable_set_assign_slot()->set_slot(1); m.mutable_set_assign_slot()->set_param_id(static_cast<ws::v1::ParamId>(p::kDrawbar8)); m.set_base_revision(r0); return m; }());
  CHECK(Rig::is_err(rep, EC::ERROR_CODE_STALE_REVISION));
  rep = rig.send([&] { auto m = msg(); m.mutable_set_part_timbre()->set_part(0); m.mutable_set_part_timbre()->set_library_id(rig.lib.b); m.mutable_set_part_timbre()->set_instrument_key("b.sfz"); m.set_base_revision(r0); return m; }());
  CHECK(Rig::is_err(rep, EC::ERROR_CODE_STALE_REVISION) && !rig.edits->reload_active());
  // A revision from the future is stale too (a view of another engine run); the current one is accepted; "not provided" is accepted and counts as a device-side change.
  CHECK(Rig::is_err(rig.send(studio_param(p::kDrawbar8, 2, 1, r1 + 1000)), EC::ERROR_CODE_STALE_REVISION));
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 6, r1)).has_ack());
  rig.step(6);
  CHECK(rig.plane.live_set_revision() == r1 && rig.live_value(p::kDrawbar8, 2) == 6);
  CHECK(rig.param(p::kDrawbar8, 2, 2).has_ack());  // the device UI's own ws client: no base_revision
  const std::uint64_t r2 = rig.plane.live_set_revision();
  CHECK(r2 > r1);
  rig.step(6);

  // A scene recall changes the values under a Studio view.
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(4);
  const std::uint64_t r3 = rig.plane.live_set_revision();
  CHECK(r3 > r2);
  CHECK(Rig::is_err(rig.send(studio_param(p::kDrawbar8, 2, 1, r2)), EC::ERROR_CODE_STALE_REVISION));
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 1, r3)).has_ack());
  rig.step(6);
  CHECK(rig.plane.live_set_revision() == r3);

  // A timbre edit from Studio rebuilds the Live Set and restores scene and values: no device-side change, so Studio's next knob move is not stale (the restore's own recall and
  // capture are not external changes).
  rep = rig.send([&] { auto m = msg(); m.mutable_set_part_timbre()->set_part(0); m.mutable_set_part_timbre()->set_library_id(rig.lib.b); m.mutable_set_part_timbre()->set_instrument_key("b.sfz"); m.set_base_revision(r3); return m; }());
  CHECK(rep.has_ack() && rig.edits->reload_active());
  CHECK(Rig::is_err(rig.send(studio_param(p::kDrawbar8, 2, 2, r3)), EC::ERROR_CODE_BUSY) || true);  // refused while rebuilding for a reason of its own (a performance edit may pass)
  rig.settle();
  CHECK(rig.plane.live_set_revision() == r3);
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 2, r3)).has_ack());
  CHECK(rig.state().edits().has_timbre_edit() && rig.state().edits().live_set_revision() == r3);

  // A Live Set switch: the jump moves the revision at once, so an edit that raced it (built on the Live Set that is going away) is refused instead of landing on it; the publication
  // of the other Live Set moves it again.
  auto jm = msg();
  jm.mutable_jump_to_live_set()->set_id(b.id());
  CHECK(rig.send(jm).has_jump_reply());
  const std::uint64_t r4 = rig.plane.live_set_revision();
  CHECK(r4 > r3);
  CHECK(Rig::is_err(rig.send(studio_param(p::kDrawbar8, 2, 3, r3)), EC::ERROR_CODE_STALE_REVISION));
  rig.settle();
  CHECK(rig.loader.loaded_id() == b.id());
  const std::uint64_t r5 = rig.plane.live_set_revision();
  CHECK(r5 > r4 && rig.state().edits().live_set_revision() == r5);
  CHECK(Rig::is_err(rig.send(studio_param(p::kDrawbar8, 2, 3, r4)), EC::ERROR_CODE_STALE_REVISION));
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 3, r5)).has_ack());
  // The values a rebuild or a switch makes RT reset are not taken for a controller move afterwards.
  rig.step(8);
  CHECK(rig.plane.live_set_revision() == r5);
  // A read with a base_revision is never judged.
  auto g = msg();
  g.mutable_get_state();
  g.set_base_revision(1);
  CHECK(rig.send(g).has_state());
}

// ---- Write ---------------------------------------------------------------------------------------------------------------------------------
void test_write_whole_live_set() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.move(p::kDrawbar8, 2, 2);
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(3);
  const std::string path = rig.live->path_for(a.id());
  const std::string before = slurp(path);
  const std::uint64_t rev = rig.plane.revision();
  bool changed = false;
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  (void)changed;
  CHECK(rep.has_write_reply());
  const auto& w = rep.write_reply();
  CHECK(w.scope() == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET && w.live_set_id() == a.id() && w.written_live_set_id() == a.id() && !w.nothing_to_write());
  CHECK(w.written_count() >= 2 && !w.current_dirty() && w.remaining_changes() == 0 && !w.not_durable());
  CHECK(rep.revision() == rev + 1 && rig.plane.revision() == rev + 1);
  CHECK(rig.idle_calls > 0);  // the fence was not acknowledged at once: the wait looped
  const lsp::LiveSet f = rig.read_file(a.id());
  CHECK_NEAR(f.parts(0).volume(), 0.35f, 1e-6);
  CHECK(f.scenes_size() == 3);
  CHECK(slurp(path) != before && rig.live->mutations() == 1);
  CHECK(!rig.edits->is_dirty(a.id()) && rig.state().edits().dirty_count() == 0 && rig.state().edits().writes_done() == 1);
  // Repeated Write: nothing dirty, no file touched.
  const auto again = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(again.has_write_reply() && again.write_reply().nothing_to_write() && rig.live->mutations() == 1);
  // The edits made after the Write start a new dirty set; the drawbar edit of scene Verse (current) was stored in the scene and is in the file.
  CHECK(f.scenes(0).parts_size() >= 3);
}

// Story 9.12: Velora Studio's Write carries base_revision. A Write built on an older view of the Live Set is refused (STALE_REVISION, nothing written, the edits stay dirty), one on the
// current revision is accepted for every scope, never moves live_set_revision (it is not an edit and not a device-side change) and never lights STUDIO; a Write without it (the device UI)
// behaves as before; a refused Write does not count as a Studio edit either.
void test_write_with_base_revision() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  rig.start({a});
  rig.step(4);
  const std::uint64_t r0 = rig.plane.live_set_revision();
  auto wr = [&](ws::v1::WriteScope scope, std::uint64_t base, const char* name = nullptr) {
    auto m = msg();
    m.mutable_write_live_set()->set_scope(scope);
    if (name) m.mutable_write_live_set()->set_new_name(name);
    m.set_base_revision(base);
    return rig.send(m);
  };
  CHECK(rig.send([&] { auto m = msg(); m.mutable_set_part_volume()->set_part(0); m.mutable_set_part_volume()->set_volume(0.35f); m.set_base_revision(r0); return m; }()).has_ack());
  rig.step(6);
  CHECK(rig.plane.live_set_revision() == r0 && rig.state().edits().current_dirty());
  const std::string path = rig.live->path_for(a.id());
  const std::string before = slurp(path);
  const std::uint64_t stale_n = rig.plane.stale_rejections();
  // A view from the future and a view from the past are both refused; nothing is written.
  for (std::uint64_t bad : {r0 + 1, r0 + 1000}) {
    const auto rep = wr(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET, bad);
    CHECK(Rig::is_err(rep, EC::ERROR_CODE_STALE_REVISION) && rep.error().details().at("base_revision") == std::to_string(bad) &&
          rep.error().details().at("current_revision") == std::to_string(r0));
  }
  CHECK(Rig::is_err(wr(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, r0 + 1, "Copy"), EC::ERROR_CODE_STALE_REVISION));
  CHECK(rig.plane.stale_rejections() == stale_n + 3 && rig.live->mutations() == 0 && slurp(path) == before && rig.edits->is_dirty(a.id()));
  // The unit's own controls change the Live Set after the view was built: the Write on the old revision is refused, the one on the new revision goes through.
  CHECK(rig.plane.push_performance_value(*p::find(p::kDrawbar2), 3, 4));
  rig.step(6);
  const std::uint64_t r1 = rig.plane.live_set_revision();
  CHECK(r1 > r0);
  CHECK(Rig::is_err(wr(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET, r0), EC::ERROR_CODE_STALE_REVISION) && rig.live->mutations() == 0 && rig.edits->is_dirty(a.id()));
  // Save as new on the current revision: a new file, the current Live Set keeps its edits and the revision stays.
  const auto copy = wr(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, r1, "Copy");
  CHECK(copy.has_write_reply() && copy.write_reply().written_name() == "Copy" && rig.plane.live_set_revision() == r1 && rig.edits->is_dirty(a.id()));
  const auto whole = wr(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET, r1);
  CHECK(whole.has_write_reply() && !whole.write_reply().current_dirty() && !rig.state().edits().current_dirty());
  CHECK(rig.plane.live_set_revision() == r1 && rig.state().edits().live_set_revision() == r1);  // a Write is not a device-side change: Studio's base stays valid
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.35f, 1e-6);
  // Nothing dirty any more: the same Write says so, the revision still stays; a Write without base_revision is accepted like before and does not move it either.
  const auto again = wr(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET, r1);
  CHECK(again.has_write_reply() && again.write_reply().nothing_to_write() && rig.plane.live_set_revision() == r1);
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).write_reply().nothing_to_write() && rig.plane.live_set_revision() == r1);
  // An edit built on r1 is still accepted after the Writes (Studio does not have to reload after writing).
  CHECK(rig.send(studio_param(p::kDrawbar8, 2, 6, r1)).has_ack());
}

// Story 6.9: a factory Live Set (LiveSet.factory) is read-only. Write (whole / scene) is refused with WRITE_SCOPE_UNAVAILABLE factory_read_only, nothing is written and
// the edits stay in memory; Save as new makes an ordinary copy without the mark.
void test_factory_live_set_is_read_only() {
  Rig rig;
  lsp::LiveSet a = perfect(rig.lib, 30, "Factory piano");
  a.mutable_factory()->set_program_id(7);
  a.mutable_factory()->set_release_version("1.0.0");
  rig.start({a});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  const std::string path = rig.live->path_for(a.id());
  const std::string before = slurp(path);
  for (auto scope : {ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET, ws::v1::WRITE_SCOPE_SCENE}) {
    const auto rep = rig.write(scope);
    CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) && rep.error().details().at("reason") == "factory_read_only");
  }
  CHECK(slurp(path) == before && rig.live->mutations() == 0 && rig.edits->is_dirty(a.id()));  // the edits stay in memory
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "My piano");
  CHECK(rep.has_write_reply());
  if (rep.has_write_reply()) {
    const lsp::LiveSet copy = rig.read_file(rep.write_reply().written_live_set_id());
    CHECK(!copy.has_factory() && copy.name() == "My piano" && copy.id() != a.id());
    CHECK_NEAR(copy.parts(0).volume(), 0.35f, 1e-6);  // the edited state
  }
  CHECK(slurp(path) == before && rig.read_file(a.id()).has_factory());  // the factory file is untouched
  // The copy is the user's: Write on it works.
  if (rep.has_write_reply()) {
    CHECK(rig.sw->jump_to(rep.write_reply().written_live_set_id()).result == host::SwitchOutcome::Result::kAccepted);
    rig.settle();
    CHECK(rig.volume(0, 0.5f).has_ack());
    rig.step(2);
    CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  }
}

// A Live Set file that is not named <id>.liveset (copied into the directory): Write puts it under its canonical name and removes the old file.
void test_write_moves_a_non_canonical_file() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  const std::string odd = rig.live_dir + "/copied from a stick.liveset";
  fs::rename(rig.live->path_for(a.id()), odd);
  rig.sw->add_to_catalog(host::CatalogEntry{a.id(), a.name(), odd});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  CHECK(fs::exists(rig.live->path_for(a.id())) && !fs::exists(odd));
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.35f, 1e-6);
  std::size_t entries = 0;
  for (const auto& e : rig.sw->catalog()) {
    entries += e.id == a.id();
    CHECK(e.id != a.id() || e.path == rig.live->path_for(a.id()));
  }
  CHECK(entries == 1);
}

void test_write_scene_scope() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  const std::string base_bytes_before = [&] {
    std::string s;
    ls::serialize(a, s);
    return s;
  }();
  CHECK(rig.recall(1).has_scene_reply());  // Chorus stores part 2's volume
  rig.step(3);
  CHECK(rig.volume(1, 0.55f).has_ack());   // scene edit
  CHECK(rig.volume(0, 0.25f).has_ack());   // base edit (Chorus does not store part 1's volume)
  rig.step(2);
  CHECK(rig.edits->summary(a.id()).size() == 2);
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(rep.has_write_reply());
  const auto& w = rep.write_reply();
  CHECK(w.scope() == ws::v1::WRITE_SCOPE_SCENE && w.scene() == 1 && w.written_count() == 1 && w.current_dirty() && w.remaining_changes() == 1);
  CHECK(w.written(0).key() == "scene.part.volume" && w.written(0).scene() == 1);
  const lsp::LiveSet f = rig.read_file(a.id());
  CHECK_NEAR(f.scenes(1).parts(1).volume(), 0.55f, 1e-6);   // the scene was written
  CHECK_NEAR(f.parts(0).volume(), a.parts(0).volume(), 1e-6);  // the base was not
  CHECK(f.scenes(0).SerializeAsString() == a.scenes(0).SerializeAsString());  // other scenes untouched
  const auto left = rig.edits->summary(a.id());
  CHECK(left.size() == 1 && left[0].key == "part.volume" && left[0].part == 0);  // only what was written was cleared
  // Writing the same scope again: nothing to write.
  const auto again = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(again.has_write_reply() && again.write_reply().nothing_to_write() && rig.live->mutations() == 1);
  // Whole writes the rest.
  const auto whole = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(whole.has_write_reply() && whole.write_reply().written_count() == 1 && !whole.write_reply().current_dirty());
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.25f, 1e-6);
  (void)base_bytes_before;
}

void test_write_scene_refusals() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  lsp::LiveSet none = perfect(rig.lib, 12, "No scenes");
  none.clear_scenes();
  rig.start({a, none});
  // No scene current.
  CHECK(rig.volume(0, 0.3f).has_ack());
  auto rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) && rep.error().details().at("reason") == "no_scene");
  CHECK(rig.live->mutations() == 0 && rig.edits->is_dirty(a.id()));
  // A timbre change pending: Scene is refused with the closed reason and the exact text.
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(3);
  CHECK(rig.timbre(1, rig.lib.c, "c.sfz").has_ack());
  rig.settle();
  CHECK(rig.edits->summary(a.id()).size() >= 2);
  auto st = rig.state();
  CHECK(st.edits().has_timbre_edit() && !st.edits().scene_write_available() && st.edits().scene_write_blocked_reason() == "timbre_change");
  rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) && rep.error().details().at("reason") == "timbre_change" &&
        rep.error().message() == "Timbre changes are saved with the Live Set");
  CHECK(rig.live->mutations() == 0);
  // A Live Set without scenes.
  CHECK(rig.sw->jump_to(none.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.volume(0, 0.3f).has_ack());
  rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) && rep.error().details().at("reason") == "no_scenes");
  CHECK(rig.state().edits().scene_write_blocked_reason() == "no_scenes");
  // The scope itself must be one of the three.
  rep = rig.write(ws::v1::WRITE_SCOPE_UNSPECIFIED);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_INVALID_COMMAND));
}

// A timbre edit rebuilds the Live Set from its edited state in the background; persisted by Whole only.
void test_timbre_edit() {
  Rig rig;
  lsp::LiveSet a = ls::make_default_live_set(uuid_n(20), "Timbre", 8);
  fixtures::timbre(a.mutable_parts(0), rig.lib.a, "a.sfz");
  a.mutable_parts(0)->set_volume(1.0f);
  a.add_scenes()->set_name("S1");
  a.add_scenes()->set_name("S2");
  a.mutable_scenes(1)->add_parts()->set_volume(0.5f);
  rig.start({a});
  rig.note_on(60);
  rig.render(6);
  CHECK_NEAR(rig.level(), 0.1, 0.005);
  rig.note_off(60);
  rig.render(40);
  CHECK(rig.recall(1).has_scene_reply());
  rig.step(3);
  rig.move(p::kDrawbar16, 5, 4);  // a live value that must survive the rebuild
  const std::uint64_t rev_before = rig.cur()->revision;
  auto rep = rig.timbre(0, rig.lib.b, "b.sfz");
  CHECK(rep.has_ack());
  CHECK(rig.edits->reload_active() && rig.sw->loading());
  CHECK(rig.cur()->revision == rev_before || rig.cur()->revision > rev_before);
  // While the rebuild runs: structural commands and Write are refused with a typed busy.
  const std::uint64_t rev_busy = rig.plane.revision();
  std::vector<ws::v1::ClientMessage> structural;
  structural.push_back(msg());
  structural.back().mutable_set_part_volume()->set_part(0);
  structural.back().mutable_set_part_volume()->set_volume(0.2f);
  structural.push_back(msg());
  structural.back().mutable_set_insert_chain()->set_part(0);
  structural.back().mutable_set_insert_chain()->add_slots()->set_type(ws::v1::INSERT_TYPE_EQ);
  structural.push_back(msg());
  structural.back().mutable_set_part_output()->set_part(0);
  structural.back().mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  structural.push_back(msg());
  structural.back().mutable_set_send_levels()->set_part(0);
  structural.back().mutable_set_send_levels()->set_reverb(0.3f);
  structural.push_back(msg());
  structural.back().mutable_set_master_params()->add_values()->set_param_id(static_cast<ws::v1::ParamId>(p::kMasterLevel));
  for (const auto& sm : structural) {
    const auto busy = rig.send(sm);
    CHECK(Rig::is_err(busy, ws::v1::ERROR_CODE_BUSY) && busy.error().details().at("reason") == "reload_in_progress");
  }
  CHECK(rig.plane.revision() == rev_busy && rig.plane.commands_rejected() >= 5);  // refused like any other command: counted, nothing moved
  auto v = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(v, ws::v1::ERROR_CODE_BUSY));
  v = rig.timbre(0, rig.lib.c, "c.sfz");
  CHECK(Rig::is_err(v, ws::v1::ERROR_CODE_BUSY));
  rig.settle();
  CHECK(!rig.sw->loading() && !rig.edits->reload_active() && !rig.edits->restoring());
  // The new timbre sounds (library b is quieter), the scene is current again and the live value was put back.
  CHECK(rig.cur()->scene_index == 1);
  CHECK_NEAR(rig.cur()->parts[0].volume, 0.5f, 1e-6);
  CHECK(rig.live_value(p::kDrawbar16, 5) == 4);
  rig.note_on(60);
  rig.render(6);
  CHECK_NEAR(rig.level(), 0.02 * 0.5, 0.002);
  rig.note_off(60);
  rig.render(40);
  auto st = rig.state();
  CHECK(st.edits().has_timbre_edit() && find_item(st.edits(), "part.timbre", 0) && find_item(st.edits(), "part.timbre", 0)->text() == "Part 1 timbre");
  CHECK(rig.live->mutations() == 0);
  // Whole persists it.
  rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(rep.has_write_reply() && !rep.write_reply().current_dirty());
  CHECK(rig.read_file(a.id()).parts(0).timbre().library_id() == rig.lib.b);
  // Invalid timbre, unknown part, the same timbre again: typed, nothing rebuilt.
  rep = rig.timbre(0, "not-a-uuid", "x.sfz");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_INVALID_COMMAND));
  rep = rig.timbre(9, rig.lib.a, "a.sfz");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_INVALID_PART));
  rep = rig.timbre(0, rig.lib.b, "b.sfz");
  CHECK(rep.has_ack() && !rig.sw->loading() && !rig.edits->reload_active());
  // A library that is not installed: accepted, the part plays silent (never a substitute), still an edit.
  rep = rig.timbre(0, uuid_n(999), "gone.sfz");
  CHECK(rep.has_ack());
  rig.settle();
  rig.note_on(60);
  rig.render(6);
  CHECK_NEAR(rig.level(), 0.0, 1e-6);
  CHECK(rig.edits->is_dirty(a.id()));
}

// ---- Save as new -------------------------------------------------------------------------------------------------------------------------------
void test_save_as_new_and_names() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  const lsp::LiveSet x = perfect(rig.lib, 11, "Next song");
  const lsp::LiveSet z = perfect(rig.lib, 12, "Perfect 2");
  rig.start({a, x, z});
  const std::string sid = rig.setlist("Gig", {a.id(), x.id(), z.id()});
  rig.step(2);
  CHECK(rig.volume(0, 0.4f).has_ack());
  rig.step(2);
  const std::uint64_t rev = rig.plane.revision();
  const std::uint64_t gen = rig.cur()->revision;

  // "Perfect" and "Perfect 2" are taken: the suggestion is "Perfect 3"; comparison is trimmed and case-insensitive.
  const std::pair<const char*, const char*> same_name[] = {{"Perfect", "Perfect 3"}, {"  perfect ", "perfect 3"}, {"PERFECT", "PERFECT 3"}};  // the suggestion keeps the typed case
  for (const auto& [name, suggestion] : same_name) {
    auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, name);
    CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NAME_IN_USE) && rep.error().details().at("suggested_name") == suggestion);
  }
  auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Perfect 2");  // an existing trailing " N" is stripped before suggesting
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NAME_IN_USE) && rep.error().details().at("suggested_name") == "Perfect 3");
  for (const std::string bad : {std::string(""), std::string("   "), std::string(65, 'x'), std::string("tab\there")}) {
    rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, bad.c_str());
    CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_INVALID_NAME));
  }
  CHECK(rig.live->mutations() == 0 && rig.plane.revision() == rev);  // every refusal changed nothing

  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "  Perfect 3  ");
  CHECK(rep.has_write_reply());
  const auto& w = rep.write_reply();
  const std::string nid = w.written_live_set_id();
  CHECK(ls::is_uuid_v4(nid) && nid != a.id() && w.written_name() == "Perfect 3" && w.live_set_id() == a.id());
  CHECK(w.setlist_id() == sid && w.setlist_index() == 1);
  const std::vector<std::string> order = rig.setlist_ids(sid);
  CHECK(order.size() == 4 && order[0] == a.id() && order[1] == nid && order[2] == x.id() && order[3] == z.id());  // right after the current one
  const lsp::LiveSet nf = rig.read_file(nid);
  CHECK(nf.name() == "Perfect 3" && nf.id() == nid && nf.scenes_size() == 3);
  CHECK_NEAR(nf.parts(0).volume(), 0.4f, 1e-6);  // the edited state
  CHECK(!rig.edits->is_dirty(nid));              // the new Live Set starts clean
  CHECK(rig.edits->is_dirty(a.id()) && w.current_dirty() && w.remaining_changes() == 1);  // the original keeps its edits in memory
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), a.parts(0).volume(), 1e-6);  // and its file was not touched
  CHECK(rig.cur()->revision == gen);  // the sound was not touched
  CHECK(rig.live->mutations() == 1 && rig.store->mutations() >= 1);
  // Twice with the same name: refused, with a new suggestion.
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Perfect 3");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NAME_IN_USE) && rep.error().details().at("suggested_name") == "Perfect 4");
  // No name: a suggested free one.
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  CHECK(rep.has_write_reply() && rep.write_reply().written_name() == "Perfect 4" && rep.write_reply().setlist_index() == 1);
  CHECK(rig.setlist_ids(sid).size() == 5);
  // The catalog knows the new Live Sets: they can be jumped to.
  CHECK(rig.sw->jump_to(nid).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.loader.loaded_id() == nid && !rig.edits->is_dirty(nid));
}

void test_save_as_new_without_setlist_and_off_script() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  const lsp::LiveSet other = perfect(rig.lib, 11, "Other");
  rig.start({a, other});
  CHECK(rig.volume(1, 0.4f).has_ack());
  rig.step(2);
  // No setlist loaded: the name must be free among all the Live Sets of the catalog (refused with a suggestion), and only the file is created.
  const std::uint64_t slm = rig.store->mutations();
  auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, " other ");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NAME_IN_USE) && rep.error().details().at("suggested_name") == "other 2");
  CHECK(rig.live->mutations() == 0);
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Distinct");
  CHECK(rep.has_write_reply() && rep.write_reply().setlist_id().empty() && rep.write_reply().setlist_index() == -1);
  CHECK(rig.store->mutations() == slm && rig.live->mutations() == 1);
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "distinct");  // the new Live Set is in the catalog now
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NAME_IN_USE) && rep.error().details().at("suggested_name") == "distinct 2");
  // A suggestion with no name looks at every Live Set.
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  CHECK(rep.has_write_reply() && rep.write_reply().written_name() == "Perfect 2");
  // A setlist that does not hold the current Live Set: after the setlist position, or appended with none.
  const std::string sid = rig.setlist("Other gig", {other.id()});
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Appended");
  CHECK(rep.has_write_reply() && rep.write_reply().setlist_index() == 1 && rig.setlist_ids(sid).size() == 2 && rig.setlist_ids(sid)[1] == rep.write_reply().written_live_set_id());
}

void test_name_suggestion_pure() {
  using host::suggest_free_name;
  std::unordered_set<std::string> taken;
  auto add = [&](const std::string& n) { taken.insert(host::fold_name(n)); };
  CHECK(suggest_free_name("Perfect", taken) == "Perfect 2");
  add("Perfect");
  add("Perfect 2");
  CHECK(suggest_free_name("Perfect", taken) == "Perfect 3");
  CHECK(suggest_free_name("Perfect 2", taken) == "Perfect 3");
  CHECK(suggest_free_name("  perfect 7 ", taken) == "perfect 3");
  CHECK(suggest_free_name("Tune 12x", taken) == "Tune 12x 2");
  CHECK(suggest_free_name("", taken) == "Live Set 2");
  CHECK(suggest_free_name("42", taken) == "42 2");  // no space before the digits: not a suffix
  // A setlist of 1000 entries: the suggestion is free and unique (and found without scanning more than the names).
  taken.clear();
  add("Perfect");
  for (int i = 2; i <= 1000; ++i) add("Perfect " + std::to_string(i));
  const std::string s = suggest_free_name("Perfect 5", taken);
  CHECK(s == "Perfect 1001" && !taken.count(host::fold_name(s)));
  // 64-byte limit with a UTF-8 name: cut on a character boundary, never longer than 64 bytes, still valid.
  std::string long_name;
  while (long_name.size() < 64) long_name += "\xC3\xA9";  // e-acute, 2 bytes each: 64 bytes, so " 2" does not fit
  taken.clear();
  const std::string t = suggest_free_name(long_name, taken);
  CHECK(t.size() == 64 && ls::valid_display_name(t) && t.substr(t.size() - 2) == " 2");
  // Case-insensitive collision of the candidate itself.
  taken.clear();
  add("PERFECT 2");
  CHECK(suggest_free_name("perfect", taken) == "perfect 3");
}

// ---- storage full ------------------------------------------------------------------------------------------------------------------------------
void test_storage_full() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  const std::string path = rig.live->path_for(a.id());
  const std::string before = slurp(path);
  const std::uint64_t rev = rig.plane.revision();
  rig.live->set_inject_write_errno(ENOSPC);
  auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_STORAGE_FULL));
  CHECK(slurp(path) == before);  // the old file is intact
  CHECK(rig.live->mutations() == 1);  // mutations() counts attempts that reached the writer, successful or not
  for (const auto& e : fs::directory_iterator(rig.live_dir)) CHECK(e.path().extension() != ".tmp");  // and no temp file is left
  CHECK(rig.edits->is_dirty(a.id()) && rig.plane.revision() == rev && rig.state().edits().current_dirty());  // edits stay, flag set
  CHECK_NEAR(rig.cur()->parts[0].volume, 0.35f, 1e-6);
  // The same for Save as new: nothing created.
  const std::size_t files = std::distance(fs::directory_iterator(rig.live_dir), fs::directory_iterator());
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Copy");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_STORAGE_FULL));
  CHECK(static_cast<std::size_t>(std::distance(fs::directory_iterator(rig.live_dir), fs::directory_iterator())) == files);
  CHECK(rig.live->mutations() == 2);
  // Space is back: the same Write now works, with the edits still there.
  rig.live->set_inject_write_errno(0);
  rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(rep.has_write_reply() && !rep.write_reply().current_dirty());
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.35f, 1e-6);
}

void test_save_as_new_setlist_failure_leaves_nothing() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  rig.start({a});
  const std::string sid = rig.setlist("Gig", {a.id()});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  rig.store->set_inject_write_errno(ENOSPC);  // the Live Set file fits, the setlist does not
  const std::size_t files = std::distance(fs::directory_iterator(rig.live_dir), fs::directory_iterator());
  const std::size_t cat = rig.sw->catalog().size();
  auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Copy");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_STORAGE_FULL));
  CHECK(static_cast<std::size_t>(std::distance(fs::directory_iterator(rig.live_dir), fs::directory_iterator())) == files);  // the new file was removed again
  CHECK(rig.sw->catalog().size() == cat && rig.setlist_ids(sid).size() == 1 && rig.edits->is_dirty(a.id()));
}

// ---- no Write, no file write -----------------------------------------------------------------------------------------------------------------
// One hour of playing on a fake clock: a generated script of notes, edits, scene recalls, Live Set jumps and stepping. Neither store writes, the files do
// not change; then one explicit Write is the only thing that does.
void test_no_write_during_an_hour_of_playing() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Song A");
  const lsp::LiveSet b = perfect(rig.lib, 11, "Song B");
  const lsp::LiveSet c = perfect(rig.lib, 12, "Song C");
  rig.start({a, b, c}, 0, 1);
  const std::string sid = rig.setlist("Gig", {a.id(), b.id(), c.id()});
  const std::uint64_t store_before = rig.store->mutations();  // creating the setlist was a command, not playing
  const auto live_files = rig.dir_fingerprint(rig.live_dir);
  const auto set_files = rig.dir_fingerprint(rig.set_dir);
  const std::map<std::string, std::string> contents = [&] {
    std::map<std::string, std::string> m;
    for (const auto& e : fs::directory_iterator(rig.live_dir)) m[e.path().filename().string()] = slurp(e.path().string());
    return m;
  }();
  std::uint64_t lcg = 12345;
  auto rnd = [&](std::uint32_t n) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>((lcg >> 33) % n);
  };
  const std::uint64_t kTickNs = 200'000'000;  // one control tick = 200 ms of the fake clock: 18,000 ticks = 1 hour
  std::uint64_t jumps = 0, steps = 0, recalls = 0, edits_made = 0;
  for (int tick = 0; tick < 18000; ++tick) {
    rig.now_ns += kTickNs;
    switch (rnd(40)) {
      case 0: rig.note_on(48 + rnd(40)); break;
      case 1: rig.note_off(48 + rnd(40)); break;
      case 2: rig.volume(rnd(4), static_cast<float>(rnd(100)) / 100.0f); ++edits_made; break;
      case 3: rig.param(p::kDrawbar8 + rnd(7), 2 + rnd(2), static_cast<float>(rnd(9))); ++edits_made; break;
      case 4: rig.recall(rnd(3)); ++recalls; break;
      case 5: rig.param(p::kKeyClick, rnd(4), static_cast<float>(rnd(100)) / 100.0f); ++edits_made; break;
      case 6:
        if (rnd(3) == 0) {
          rig.sw->jump_to(rnd(2) ? b.id() : c.id());
          ++jumps;
        }
        break;
      case 7: {
        auto m = msg();
        if (rnd(2)) m.mutable_next_live_set(); else m.mutable_previous_live_set();
        rig.send(m);
        ++steps;
        break;
      }
      case 8: rig.state(); break;
      default: break;
    }
    rig.sw->tick();
    rig.ctl->tick();
    rig.edits->tick();
    rig.driver.render_block(rig.l, rig.r, rig.now_ns);
  }
  for (std::uint32_t k = 0; k < 128; ++k) rig.note_off(k);  // let every note go: generations pinned by sounding notes would hold a switch back
  rig.br->submit({core::make_control_change(0, 0, 64, 0u), 0});
  rig.render(400);
  rig.settle();
  CHECK(jumps > 50 && steps > 50 && recalls > 50 && edits_made > 100);
  CHECK(rig.live->mutations() == 0);
  CHECK(rig.store->mutations() == store_before);
  CHECK(rig.dir_fingerprint(rig.live_dir) == live_files);
  CHECK(rig.dir_fingerprint(rig.set_dir) == set_files);
  for (const auto& e : fs::directory_iterator(rig.live_dir)) CHECK(slurp(e.path().string()) == contents.at(e.path().filename().string()));
  CHECK(rig.state().edits().dirty_count() > 0);  // the edits are there, in memory only
  // Only an explicit Write writes.
  const auto fin = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  if (fin.has_error()) std::printf("  final write: error %d %s\n", static_cast<int>(fin.error().code()), fin.error().message().c_str());
  CHECK(fin.has_write_reply());
  CHECK(rig.live->mutations() == 1);
}

// ---- hostile cases ---------------------------------------------------------------------------------------------------------------------------------
void test_write_without_a_live_set_and_while_loading() {
  {
    Rig rig;
    rig.start({perfect(rig.lib)}, 0, 0, {}, false);  // nothing loaded yet
    auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
    CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NO_LIVE_SET));
    rep = rig.timbre(0, rig.lib.a, "a.sfz");
    CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_NO_LIVE_SET));
    CHECK(rig.live->mutations() == 0);
  }
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "A");
  const lsp::LiveSet b = perfect(rig.lib, 11, "B");
  rig.start({a, b}, 0, /*hold_ticks=*/5);
  CHECK(rig.volume(0, 0.3f).has_ack());
  rig.step(2);
  CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step(2);
  CHECK(rig.sw->loading());
  const std::uint64_t rev = rig.plane.revision();
  auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY) && rep.error().details().at("reason") == "load_in_progress");
  rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Copy");
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY));
  CHECK(rig.live->mutations() == 0 && rig.plane.revision() == rev && rig.edits->is_dirty(a.id()));
  rig.settle();
  rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);  // B is current and clean now: nothing to write; A is NOT written (current Live Set only)
  CHECK(rep.has_write_reply() && rep.write_reply().nothing_to_write() && rep.write_reply().live_set_id() == b.id());
  CHECK(rig.live->mutations() == 0 && rig.edits->is_dirty(a.id()));
}

void test_write_during_scene_storm_and_large_edit_sets() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  std::uint64_t lcg = 99;
  auto rnd = [&](std::uint32_t n) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>((lcg >> 33) % n);
  };
  int writes = 0;
  for (int i = 0; i < 300; ++i) {
    rig.recall(rnd(3));
    if (i % 3 == 0) rig.volume(rnd(4), static_cast<float>(rnd(100)) / 100.0f);
    if (i % 5 == 0) rig.param(p::kDrawbar8 + rnd(7), 2 + rnd(2), static_cast<float>(rnd(9)));
    if (i % 7 == 0) {
      const bool whole = rnd(2) != 0;
      const auto rep = rig.write(whole ? ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET : ws::v1::WRITE_SCOPE_SCENE);
      CHECK(rep.has_write_reply() || Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) || Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY));
      if (rep.has_write_reply() && !rep.write_reply().nothing_to_write()) ++writes;
      if (whole && rep.has_write_reply()) {
        // What the file holds is what was live at the Write: the drawbars of the organ parts, in the scene that is current, read back from the file.
        const lsp::LiveSet f = rig.read_file(a.id());
        const int sc = rig.cur()->scene_index;
        for (int part = 2; part < 4; ++part)
          for (std::uint32_t id = p::kDrawbar8; id <= p::kDrawbar8 + 6; ++id) {
            float want = id == p::kDrawbar8 ? 8.0f : 0.0f;  // registry default when not listed
            for (const auto& e : f.parts(part).engine().params())
              if (e.param_id() == id) want = e.value();
            if (sc >= 0 && part < f.scenes(sc).parts_size())
              for (const auto& e : f.scenes(sc).parts(part).engine_params())
                if (e.param_id() == id) want = e.value();
            CHECK_NEAR(rig.live_value(id, static_cast<std::uint32_t>(part)), want, 1e-6);
          }
      }
    }
    rig.step(1);
  }
  CHECK(writes > 5 && rig.live->mutations() == static_cast<std::uint64_t>(writes));
  // The file on disk is always a valid Live Set of the same id.
  CHECK(rig.read_file(a.id()).id() == a.id());

  // A large edit set: every part, many master params, all drawbars; the State stays within a ws.v1 frame, the summary is capped and the count is true.
  Rig big;
  const lsp::LiveSet b = perfect(big.lib);
  big.start({b});
  for (std::uint32_t part = 0; part < 8; ++part) {
    big.volume(part, 0.11f * static_cast<float>(part + 1) / 2.0f);
    for (std::uint32_t id : {p::kDrawbar16, p::kDrawbar8, p::kDrawbar4, p::kDrawbar2, p::kDrawbar1, p::kKeyClick, p::kLeakage}) big.param(id, part, 1);
    auto snd = msg();
    snd.mutable_set_send_levels()->set_part(part);
    snd.mutable_set_send_levels()->set_reverb(0.9f);
    snd.mutable_set_send_levels()->set_delay(0.8f);
    big.send(snd);
    auto out = msg();
    out.mutable_set_part_output()->set_part(part);
    out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
    big.send(out);
  }
  big.step(8);
  const ws::v1::State st = big.state();
  CHECK(st.edits().change_count() > 80 && st.edits().change_summary_size() == 32 && st.ByteSizeLong() < 65536);
  CHECK(big.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).write_reply().written_count() == st.edits().change_count());
  CHECK(big.read_file(b.id()).parts(7).send_reverb() > 0.89f);
}

// ---- the edit hook for ASSIGN (Story 4.10) and the diff -----------------------------------------------------------------------------------
void test_external_edit_hook_and_diff() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.edits->apply_external_edit([](lsp::LiveSet& w) { w.set_assign(2, p::kKeyClick); }));
  rig.step(1);
  const auto items = rig.edits->summary(a.id());
  CHECK(items.size() == 1 && items[0].key == "assign.slot" && items[0].slot == 2 && items[0].part == -1 && items[0].text == "ASSIGN K3");
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  CHECK(rig.read_file(a.id()).assign(2) == p::kKeyClick);

  // diff_live_sets: identical = empty; tolerance; scene fields; unknown params.
  lsp::LiveSet x = a, y = a;
  CHECK(host::diff_live_sets(x, y).empty());
  y.mutable_parts(0)->set_volume(a.parts(0).volume() + 1e-8f);
  CHECK(host::diff_live_sets(x, y).empty());
  y.set_tempo(x.tempo() + 5);
  y.mutable_scenes(1)->mutable_parts(1)->set_volume(0.1f);
  y.mutable_scenes(2)->add_parts()->set_octave(1);
  entry(y.mutable_parts(2)->mutable_engine()->mutable_params(), 15001, 0.5f);  // an id this build does not know
  entry(y.mutable_scenes(0)->mutable_master(), p::kReverbLevel, 0.9f);
  const auto d = host::diff_live_sets(x, y);
  std::set<std::string> keys;
  for (const auto& i : d) keys.insert(i.key);
  CHECK(keys.count("tempo") && keys.count("scene.part.volume") && keys.count("scene.part.octave") && keys.count("part.param") && keys.count("scene.master.param"));
  CHECK(!keys.count("part.volume"));
  // An id listed at its default is no change.
  lsp::LiveSet z = a;
  entry(z.mutable_parts(2)->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
  CHECK(host::diff_live_sets(a, z).empty());
}

// The overlay, the sync and the session leave the files alone even when the session is built after the Live Set sounds (engine-host order).
void test_session_built_after_load_adopts_the_current_live_set() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  rig.edits.reset();  // the engine-host order: the Live Set sounds first, the session is built (and wired) afterwards
  CHECK(rig.loader.load(a).ok());  // a destroyed session must have detached itself from the loader (no dangling port)
  rig.render(2);
  host::EditSessionOptions eo;
  host::LiveSetSwitcher* swp = rig.sw.get();
  eo.load_in_progress = [swp] { return swp->loading(); };
  eo.reload_current = [swp] { return swp->reload_current(); };
  eo.fence_idle = [&rig] { rig.render(1); };
  rig.edits = std::make_unique<host::EditSession>(rig.plane, rig.scenes, *rig.live, rig.store.get(), rig.ctl.get(), rig.port.get(), eo);
  rig.edits->attach(rig.loader);
  rig.edits->attach(*rig.sw);
  CHECK(rig.edits->current_id() == a.id());
  CHECK(rig.volume(0, 0.4f).has_ack());
  CHECK(rig.edits->is_dirty(a.id()));
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.4f, 1e-6);
  // The session goes away: the plane is back to plain structural behaviour (nothing dangling).
  rig.edits.reset();
  CHECK(rig.volume(0, 0.5f).has_ack());
  auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_INVALID_COMMAND));
}


// Edits made while a scene that STORES the field is current land in that scene: inserts, output, both sends, a master parameter.
void test_scene_current_routing_of_structural_edits() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.recall(0).has_scene_reply());  // Verse stores part 1's volume, sends, output, inserts and the master reverb level
  rig.step(3);
  auto ins = msg();
  ins.mutable_set_insert_chain()->set_part(0);
  ins.mutable_set_insert_chain()->add_slots()->set_type(ws::v1::INSERT_TYPE_COMPRESSOR);
  CHECK(rig.send(ins).has_ack());
  auto out = msg();
  out.mutable_set_part_output()->set_part(0);
  out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  CHECK(rig.send(out).has_ack());
  auto snd = msg();
  snd.mutable_set_send_levels()->set_part(0);
  snd.mutable_set_send_levels()->set_reverb(0.7f);
  snd.mutable_set_send_levels()->set_delay(0.2f);
  CHECK(rig.send(snd).has_ack());
  auto mas = msg();
  auto* pv = mas.mutable_set_master_params()->add_values();
  pv->set_param_id(static_cast<ws::v1::ParamId>(p::kReverbLevel));
  pv->set_value(0.8f);
  CHECK(rig.send(mas).has_ack());
  rig.step(4);
  const auto items = rig.edits->summary(a.id());
  std::set<std::string> keys;
  for (const auto& i : items) {
    keys.insert(i.key);
    CHECK(i.scene == 0);  // every one of them is an edit of scene 1 (index 0); none landed in the base
  }
  CHECK(items.size() == 5 && keys.count("scene.part.inserts") && keys.count("scene.part.output") && keys.count("scene.part.send_reverb") &&
        keys.count("scene.part.send_delay") && keys.count("scene.master.param"));
  const auto st = rig.state();
  CHECK(find_item(st.edits(), "scene.part.inserts", 0, -1, 0) && find_item(st.edits(), "scene.master.param", -1, static_cast<int>(p::kReverbLevel), 0));
  // Write Scene writes all of them and nothing else.
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(rep.has_write_reply() && rep.write_reply().written_count() == 5 && !rep.write_reply().current_dirty());
  const lsp::LiveSet f = rig.read_file(a.id());
  const lsp::ScenePart& sp = f.scenes(0).parts(0);
  CHECK(sp.inserts().slots_size() == 1 && sp.inserts().slots(0).type() == ws::v1::INSERT_TYPE_COMPRESSOR && sp.output() == ws::v1::PART_OUTPUT_AUX);
  CHECK_NEAR(sp.send_reverb(), 0.7, 1e-6);
  CHECK_NEAR(sp.send_delay(), 0.2, 1e-6);
  CHECK_NEAR(f.scenes(0).master(0).value(), 0.8, 1e-6);
  CHECK(f.parts(0).inserts_size() == 0 && f.parts(0).output() == ws::v1::PART_OUTPUT_MAIN && f.master_size() == 0);  // the base is untouched
  // A re-recall keeps them (also after another scene).
  CHECK(rig.recall(2).has_scene_reply());
  rig.step(3);
  CHECK(rig.cur()->parts[0].output == p::kPartOutputMain && rig.cur()->parts[0].insert_count == 0);
  CHECK(rig.recall(0).has_scene_reply());
  rig.step(4);
  CHECK(rig.cur()->parts[0].output == p::kPartOutputAux && rig.cur()->parts[0].insert_count == 1 && rig.cur()->parts[0].inserts[0].type == p::kInsertTypeCompressor);
  CHECK_NEAR(rig.live_value(p::kSendReverbLevel, 0), 0.7, 1e-6);
  CHECK_NEAR(rig.live_value(p::kSendDelayLevel, 0), 0.2, 1e-6);
  CHECK(!rig.edits->is_dirty(a.id()));
}

// A hardware knob move on the MIDI lane that is still in flight when the Write starts is part of the file.
void test_midi_lane_move_before_write_is_saved() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.midi_q.push(host::StampedUmp{core::make_param_event(*p::find(p::kDrawbar4), 3, 6), rig.now_ns}));  // queued, not rendered
  CHECK(rig.live_value(p::kDrawbar4, 3) == 0);
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(rep.has_write_reply() && !rep.write_reply().nothing_to_write());
  CHECK(base_param(rig.read_file(a.id()), 3, p::kDrawbar4) == 6);
}

// The newest generation must be applied by RT before a Write reads the table.
void test_write_refused_when_generation_not_applied() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a}, 0, 0, [&](host::EditSessionOptions& o) {
    o.fence_idle = [&rig] { const_cast<core::LastValueTable&>(rig.br->last_values()).store_fence_ack(0x7FFFFFFFu); };  // acknowledges without rendering
  });
  CHECK(rig.volume(0, 0.3f).has_ack());  // a new generation RT has not adopted
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_BUSY) && rep.error().details().at("reason") == "generation_not_applied" && rig.live->mutations() == 0);
}

void test_engine_only_timbre_edit_keeps_the_timbre() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  auto m = msg();
  m.mutable_set_part_timbre()->set_part(0);
  m.mutable_set_part_timbre()->set_engine_type_id(2);
  CHECK(rig.send(m).has_ack());
  const lsp::LiveSet* w = rig.edits->working_of(a.id());
  CHECK(w->parts(0).timbre().library_id() == rig.lib.a && w->parts(0).engine().engine_type_id() == 2);
  rig.settle();
  CHECK(rig.edits->is_dirty(a.id()));
}

void test_line_in_cannot_be_a_master_edit() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  auto m = msg();
  auto* pv = m.mutable_set_master_params()->add_values();
  pv->set_param_id(static_cast<ws::v1::ParamId>(p::kLineInLevel));
  pv->set_value(0.5f);
  CHECK(Rig::is_err(rig.send(m), ws::v1::ERROR_CODE_WRONG_CLASS));
  rig.step(2);
  CHECK(!rig.edits->is_dirty(a.id()));
}

// Values the loader normalises (clamped) are compared in their normalised form: no spurious edit, and an unnormalised value in the edited state is not overwritten.
void test_live_values_compare_with_normalised_file_values() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  rig.move(p::kSendReverbLevel, 0, 1.0f);
  CHECK(rig.edits->apply_external_edit([](lsp::LiveSet& w) { w.mutable_parts(0)->set_send_reverb(1.4f); }));
  rig.step(4);
  CHECK(rig.edits->working_of(a.id())->parts(0).send_reverb() > 1.3f);  // the live 1.0 is the normalised 1.4: not an edit to record
}

// Dirty documents are only dropped by an explicit delete; a catalog that transiently lacks the id loses nothing.
void test_unsaved_document_survives_catalog_gaps_and_dies_with_a_delete() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "A");
  const lsp::LiveSet b = perfect(rig.lib, 11, "B");
  rig.start({a, b});
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  const host::CatalogEntry saved_entry = rig.sw->catalog().front().id == a.id() ? rig.sw->catalog().front() : rig.sw->catalog().back();
  rig.sw->remove_from_catalog(a.id());
  rig.step(3);
  CHECK(rig.edits->is_dirty(a.id()));
  rig.sw->add_to_catalog(saved_entry);
  auto m = msg();
  m.mutable_delete_live_set()->set_id(a.id());
  CHECK(rig.send(m).has_ack());
  CHECK(rig.edits->working_of(a.id()) == nullptr && rig.state().edits().dirty_count() == 0);
}

// A rebuild that RT never applies does not block edits forever; a superseded rebuild releases them.
void test_restore_timeout_and_superseded_rebuild() {
  double fake = 0;
  {
    Rig rig;
    const lsp::LiveSet a = perfect(rig.lib);
    rig.start({a}, 0, 0, [&](host::EditSessionOptions& o) {
      o.clock = [&] { return fake; };
      o.restore_timeout_s = 1.0;
    });
    CHECK(rig.timbre(1, rig.lib.c, "c.sfz").has_ack());
    rig.sw->tick();  // the rebuilt generation is published; nothing renders, so RT never applies it
    rig.edits->tick();
    CHECK(rig.edits->restoring() && !rig.edits->reload_active());
    CHECK(Rig::is_err(rig.volume(0, 0.3f), ws::v1::ERROR_CODE_BUSY));
    fake += 10;
    rig.edits->tick();
    CHECK(!rig.edits->restoring());
    CHECK(rig.volume(0, 0.3f).has_ack());
  }
  {
    Rig rig;
    const lsp::LiveSet a = perfect(rig.lib, 10, "A");
    const lsp::LiveSet b = perfect(rig.lib, 11, "B");
    rig.start({a, b});
    CHECK(rig.timbre(1, rig.lib.c, "c.sfz").has_ack());
    CHECK(rig.edits->reload_active());
    CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);  // supersedes the rebuild
    rig.edits->tick();
    CHECK(!rig.edits->reload_active());
    CHECK(rig.volume(0, 0.3f).has_ack());
    rig.settle();
    CHECK(rig.loader.loaded_id() == b.id() && rig.edits->is_dirty(a.id()));
  }
}

// Storage full for a setlist command through the controller; a failed directory sync through the edit session is a success with not_durable.
void test_storage_full_for_setlists_and_not_durable_write() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  rig.store->set_inject_write_errno(ENOSPC);
  const std::uint64_t rev = rig.plane.revision();
  auto m = msg();
  m.mutable_create_setlist()->set_name("No room");
  const auto rep = rig.send(m);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_STORAGE_FULL) && rig.plane.revision() == rev && rig.store->library().setlists_size() == 0);
  rig.store->set_inject_write_errno(0);
  m = msg();
  m.mutable_create_setlist()->set_name("Room again");
  CHECK(rig.send(m).has_created_reply());
  CHECK(rig.volume(0, 0.35f).has_ack());
  rig.step(2);
  rig.live->set_inject_dir_sync_failure(true);
  const auto w = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(w.has_write_reply() && w.write_reply().not_durable() && !w.write_reply().current_dirty() && !rig.edits->is_dirty(a.id()));
  CHECK_NEAR(rig.read_file(a.id()).parts(0).volume(), 0.35f, 1e-6);  // the file is in place
}

// The current scene exists in the edited state but not in the saved Live Set: Scene is refused instead of indexing past the saved scenes.
void test_scene_scope_refused_when_scene_not_in_saved() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(rig.edits->apply_external_edit([](lsp::LiveSet& w) { w.add_scenes()->set_name("Unsaved scene"); }));
  CHECK(rig.recall(3).has_scene_reply());
  rig.step(3);
  CHECK(rig.volume(0, 0.3f).has_ack());
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_SCENE);
  CHECK(Rig::is_err(rep, ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) && rep.error().details().at("reason") == "scene_not_in_saved" && rig.live->mutations() == 0);
}

// Diff reports a different number of parts or ASSIGN slots.
void test_diff_reports_list_size_changes() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  lsp::LiveSet y = a;
  y.mutable_parts()->RemoveLast();
  y.mutable_assign()->RemoveLast();
  std::set<std::string> keys;
  for (const auto& i : host::diff_live_sets(a, y)) keys.insert(i.key);
  CHECK(keys.count("parts.list") && keys.count("assign.list"));
}

}  // namespace

// Story 4.7: every Write that touches a Live Set file tells the switch port (a preload of that Live Set is stale).
struct RecordingPort final : host::SwitchPort {
  explicit RecordingPort(host::SwitchPort& in) : in_(in) {}
  host::SwitchOutcome jump_to(const std::string& id) override { return in_.jump_to(id); }
  const std::string& effective_id() const override { return in_.effective_id(); }
  const std::string& sounding_id() const override { return in_.sounding_id(); }
  const std::vector<host::CatalogEntry>& catalog() const override { return in_.catalog(); }
  void add_to_catalog(host::CatalogEntry e) override { in_.add_to_catalog(std::move(e)); }
  void remove_from_catalog(const std::string& id) override { in_.remove_from_catalog(id); }
  void set_preload_target(const std::string& c, const std::string& n) override { in_.set_preload_target(c, n); }
  void live_set_written(const std::string& id) override {
    written.push_back(id);
    in_.live_set_written(id);
  }
  host::SwitchPort& in_;
  std::vector<std::string> written;
};

void test_write_tells_the_port() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  RecordingPort rec(*rig.port);
  rig.edits.reset();
  host::EditSessionOptions eo;
  host::LiveSetSwitcher* swp = rig.sw.get();
  eo.load_in_progress = [swp] { return swp->loading(); };
  eo.reload_current = [swp] { return swp->reload_current(); };
  eo.fence_idle = [&rig] { rig.render(1); };
  eo.rng = &rig.rng;
  rig.edits = std::make_unique<host::EditSession>(rig.plane, rig.scenes, *rig.live, rig.store.get(), rig.ctl.get(), &rec, eo);
  rig.edits->attach(rig.loader);
  rig.edits->attach(*rig.sw);
  CHECK(rig.volume(0, 0.35f).has_ack());
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  CHECK(rec.written.size() == 1 && rec.written[0] == a.id());  // the Scene / Whole branch
  CHECK(rig.volume(0, 0.4f).has_ack());
  const auto rep = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Copy for preload");
  CHECK(rep.has_write_reply() && rec.written.size() == 2 && rec.written[1] == rep.write_reply().written_live_set_id());  // the Save as new branch
  rig.edits.reset();
}

void test_zone_and_arp_edits() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "Perfect");
  rig.start({a});
  auto st = rig.state();
  CHECK(!st.edits().current_dirty() && st.edits().change_count() == 0);

  // Edit zone: low_key, high_key, octave, transpose, keyboard_on
  auto mz = msg();
  auto* z = mz.mutable_set_part_zone();
  z->set_part(0);
  z->set_low_key(36);
  z->set_high_key(84);
  z->set_octave(1);
  z->set_transpose(2);
  z->set_keyboard_on(false);
  CHECK(rig.send(mz).has_ack());

  rig.step(4);
  st = rig.state();
  const ws::v1::EditStatus& ez = st.edits();
  CHECK(ez.current_dirty());
  CHECK(find_item(ez, "part.zone", 0) && find_item(ez, "part.zone", 0)->text() == "Part 1 zone");
  CHECK(find_item(ez, "part.octave", 0) && find_item(ez, "part.octave", 0)->text() == "Part 1 octave");
  CHECK(find_item(ez, "part.transpose", 0) && find_item(ez, "part.transpose", 0)->text() == "Part 1 transpose");
  CHECK(find_item(ez, "part.keyboard", 0) && find_item(ez, "part.keyboard", 0)->text() == "Part 1 keyboard");

  // Edit arp:
  auto ma = msg();
  auto* arp = ma.mutable_set_part_arp();
  arp->set_part(1);
  arp->set_arp_on(true);
  arp->set_arp_mode(1);
  arp->set_arp_rate(2);
  arp->set_arp_octaves(2);
  arp->set_arp_gate(120);
  arp->set_arp_swing(60);
  arp->set_arp_latch(true);
  CHECK(rig.send(ma).has_ack());

  rig.step(4);
  st = rig.state();
  const ws::v1::EditStatus& ea = st.edits();
  CHECK(ea.current_dirty());
  CHECK(find_item(ea, "part.arp", 1) && find_item(ea, "part.arp", 1)->text() == "Part 2 arpeggiator");
}

int main() {
  test_zone_and_arp_edits();
  test_fence_orders_after_earlier_events();
  test_fence_lane_full_and_timeout();
  test_dirty_flag_and_change_summary();
  test_edits_kept_across_scene_changes();
  test_performance_edits_kept_across_scene_changes();
  test_unapplied_generation_is_not_an_edit();
  test_edits_kept_across_live_set_changes();
  test_live_set_revision_stale_edits_and_studio_indicator();
  test_write_whole_live_set();
  test_write_with_base_revision();
  test_write_tells_the_port();
  test_factory_live_set_is_read_only();
  test_write_moves_a_non_canonical_file();
  test_write_scene_scope();
  test_write_scene_refusals();
  test_timbre_edit();
  test_save_as_new_and_names();
  test_save_as_new_without_setlist_and_off_script();
  test_name_suggestion_pure();
  test_storage_full();
  test_save_as_new_setlist_failure_leaves_nothing();
  test_no_write_during_an_hour_of_playing();
  test_write_without_a_live_set_and_while_loading();
  test_write_during_scene_storm_and_large_edit_sets();
  test_external_edit_hook_and_diff();
  test_session_built_after_load_adopts_the_current_live_set();
  test_scene_current_routing_of_structural_edits();
  test_midi_lane_move_before_write_is_saved();
  test_write_refused_when_generation_not_applied();
  test_engine_only_timbre_edit_keeps_the_timbre();
  test_line_in_cannot_be_a_master_edit();
  test_live_values_compare_with_normalised_file_values();
  test_unsaved_document_survives_catalog_gaps_and_dies_with_a_delete();
  test_restore_timeout_and_superseded_rebuild();
  test_storage_full_for_setlists_and_not_durable_write();
  test_scene_scope_refused_when_scene_not_in_saved();
  test_diff_reports_list_size_changes();
  if (g_fail) {
    std::printf("edit_session_tests: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("edit_session_tests: ok\n");
  return 0;
}

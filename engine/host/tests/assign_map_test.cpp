// Story 4.10: ASSIGN maps with a global default. The rig is the engine without its threads (as in edit_session_test.cpp): a block renderer behind the real
// render driver, the plane, scene controller, loader, switcher in deterministic mode, both stores on a temporary data root, the setlist controller and the edit
// session. Commands: SetAssignSlot / ClearAssignSlot on the current Live Set (an in-memory edit: dirty, change key assign.slot, survives Write and a restart)
// and on the global default (written at once, existing Live Sets untouched), SetEncoderMode / SelectPart, and the encoder routing end to end (steps
// arrive as UMPs on the host event queue, as the panel will deliver them). Nothing here asserts a duration: waits are counted in blocks.
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
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
  bool keep = false;  // a power cycle keeps the data root for the next rig
  explicit TmpDir(const std::string& reuse = "") {
    if (!reuse.empty()) {
      path = reuse;
      keep = true;
      return;
    }
    path = (fs::temp_directory_path() / "velora-assign-XXXXXX").string();
    if (!mkdtemp(path.data())) std::exit(1);
  }
  ~TmpDir() {
    if (keep) return;
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
  std::atomic<std::uint64_t> now_ns{1'000'000'000ull};
  host::ControlPlane plane{br->snapshots(), br->last_values(), ctl_q, [this] { return now_ns.load(); }};
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
  // `reuse_root`: a power cycle on the data root (and the libraries) a previous rig left behind (set `tmp.keep` on the old rig before it dies).
  explicit Rig(const std::string& reuse_root = "", const Libs* libs = nullptr)
      : tmp(reuse_root), resolver(host::make_posix_fs(), tmp.path),
        loader(*br, plane, resolver, heads, &sample, lopts(&scenes)) {
    root = tmp.path;
    live_dir = tmp.path + "/data/live_sets";
    set_dir = tmp.path + "/data/setlists";
    fs::create_directories(live_dir);
    fs::create_directories(set_dir);
    if (libs) {
      lib = *libs;
    } else {
      lib.a = install_library(root, 1, "a.sfz", 0.1f);
      lib.b = install_library(root, 2, "b.sfz", 0.02f);
      lib.c = install_library(root, 3, "c.sfz", 0.05f);
    }
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }

  // Sets are written first (files), then the engine is started: switcher, stores, controller, session (as engine-host wires them).
  void start(const std::vector<lsp::LiveSet>& sets, std::size_t first = 0, std::uint32_t hold_ticks = 0, std::function<void(host::EditSessionOptions&)> tweak = {},
             bool load_first = true, bool write_files = true) {
    if (write_files)
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

// ---- helpers --------------------------------------------------------------------------------------------------------------------------
using AT = ws::v1::AssignTarget;
constexpr AT kCur = ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET;
constexpr AT kDef = ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT;

ws::v1::ServerMessage assign_set(Rig& rig, AT target, std::uint32_t slot, std::uint32_t param) {
  auto m = msg();
  m.mutable_set_assign_slot()->set_target(target);
  m.mutable_set_assign_slot()->set_slot(slot);
  m.mutable_set_assign_slot()->set_param_id(param);
  return rig.send(m);
}
ws::v1::ServerMessage assign_clear(Rig& rig, AT target, std::uint32_t slot) {
  auto m = msg();
  m.mutable_clear_assign_slot()->set_target(target);
  m.mutable_clear_assign_slot()->set_slot(slot);
  return rig.send(m);
}
ws::v1::ServerMessage encoder_mode(Rig& rig, ws::v1::EncoderMode mode) {
  auto m = msg();
  m.mutable_set_encoder_mode()->set_mode(mode);
  return rig.send(m);
}
ws::v1::ServerMessage select_part(Rig& rig, std::uint32_t part) {
  auto m = msg();
  m.mutable_select_part()->set_part(part);
  return rig.send(m);
}
// An encoder step as the panel will deliver it: a UMP on the host event queue (the MIDI lane), stamped with host time. encoder 1..8 = K1..K8.
void turn(Rig& rig, std::uint32_t k, std::int32_t delta) {
  CHECK(rig.midi_q.push(host::StampedUmp{core::make_encoder_step_event(k - 1, delta), rig.now_ns.load()}));
}
std::array<std::uint32_t, 8> map_of(const ws::v1::State& st) {
  std::array<std::uint32_t, 8> a{};
  CHECK(st.encoders().assign_size() == 8);
  for (int k = 0; k < st.encoders().assign_size() && k < 8; ++k) a[static_cast<std::size_t>(k)] = st.encoders().assign(k);
  return a;
}
std::array<std::uint32_t, 8> default_of(const ws::v1::State& st) {
  std::array<std::uint32_t, 8> a{};
  CHECK(st.encoders().assign_default_available() && st.encoders().assign_default_size() == 8);
  for (int k = 0; k < st.encoders().assign_default_size() && k < 8; ++k) a[static_cast<std::size_t>(k)] = st.encoders().assign_default(k);
  return a;
}
std::array<std::uint32_t, 8> file_map(const lsp::LiveSet& s) {
  std::array<std::uint32_t, 8> a{};
  CHECK(s.assign_size() == 8);
  for (int k = 0; k < s.assign_size() && k < 8; ++k) a[static_cast<std::size_t>(k)] = s.assign(k);
  return a;
}
using Map = std::array<std::uint32_t, 8>;

// ---- the current Live Set's map: a dirty in-memory edit that applies at once ----------------------------------------------------------------
void test_current_map_edit_is_dirty_and_live() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(map_of(rig.state()) == Map{});
  CHECK(!rig.state().edits().current_dirty());
  const std::uint64_t rev = rig.plane.revision();
  const auto rep = assign_set(rig, kCur, 1, p::kDrawbar8);
  CHECK(rep.has_ack() && rig.plane.revision() > rev);
  CHECK(rig.cur()->assign[0] == p::kDrawbar8);  // the generation carries it: RT routes from it
  ws::v1::State st = rig.state();
  CHECK(map_of(st) == (Map{p::kDrawbar8}));
  CHECK(st.edits().current_dirty() && st.edits().change_count() == 1);
  const ws::v1::ChangeItem* it = find_item(st.edits(), "assign.slot");
  CHECK(it && it->has_slot() && it->slot() == 0 && !it->has_part() && it->text() == "ASSIGN K1");
  CHECK(st.edits().change_summary_text().find("ASSIGN K1") != std::string::npos);
  CHECK(rig.edits->is_dirty(a.id()));
  CHECK(file_map(rig.read_file(a.id())) == Map{});  // nothing is written until Write
  CHECK(rig.live->mutations() == 0 && rig.store->mutations() == 0);
  // The same parameter in another slot is allowed (ids may repeat on purpose); a replaced slot is one item.
  CHECK(assign_set(rig, kCur, 2, p::kDrawbar8).has_ack());
  CHECK(assign_set(rig, kCur, 2, p::kSendReverbLevel).has_ack());
  CHECK(assign_set(rig, kCur, 8, p::kTempoBpm).has_ack());  // a master-scope parameter
  st = rig.state();
  CHECK(map_of(st) == (Map{p::kDrawbar8, p::kSendReverbLevel, 0, 0, 0, 0, 0, p::kTempoBpm}));
  CHECK(st.edits().change_count() == 3 && find_item(st.edits(), "assign.slot") && st.edits().change_summary(2).slot() == 7);
  CHECK(assign_set(rig, kCur, 3, p::kDrawbar8).has_ack());
  CHECK(rig.cur()->assign[0] == p::kDrawbar8 && rig.cur()->assign[2] == p::kDrawbar8);
  // Clearing makes the slot empty; setting the saved value again (empty everywhere) is no edit at all.
  CHECK(assign_clear(rig, kCur, 3).has_ack() && rig.cur()->assign[2] == 0);
  const std::uint64_t rev2 = rig.plane.revision();
  CHECK(assign_clear(rig, kCur, 3).has_ack() && rig.plane.revision() == rev2);  // already empty: nothing to do
  CHECK(assign_clear(rig, kCur, 1).has_ack() && assign_clear(rig, kCur, 2).has_ack() && assign_clear(rig, kCur, 8).has_ack());
  st = rig.state();
  CHECK(map_of(st) == Map{} && !st.edits().current_dirty() && !rig.edits->is_dirty(a.id()));
  CHECK(rig.live->mutations() == 0);
}

void test_validation_changes_nothing() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(assign_set(rig, kCur, 4, p::kLeakage).has_ack());
  const std::uint64_t rev = rig.plane.revision(), seq = rig.edits->edit_seq();
  const core::Snapshot* gen = rig.cur();
  const Map default_before = rig.store->default_assign();
  auto unchanged = [&] {
    CHECK(rig.plane.revision() == rev && rig.edits->edit_seq() == seq && rig.cur() == gen && rig.cur()->assign[3] == p::kLeakage);
    CHECK(rig.store->default_assign() == default_before && rig.store->mutations() == 0 && rig.live->mutations() == 0);
  };
  for (AT target : {kCur, kDef}) {
    for (std::uint32_t slot : {0u, 9u, 100u, 0xFFFFFFFFu}) {
      const auto rep = assign_set(rig, target, slot, p::kDrawbar8);
      CHECK(Rig::is_err(rep, EC::ERROR_CODE_INVALID_SLOT) && rep.error().details().at("max_slot") == "8");
      CHECK(Rig::is_err(assign_clear(rig, target, slot), EC::ERROR_CODE_INVALID_SLOT));
    }
    CHECK(Rig::is_err(assign_set(rig, target, 1, 9999), EC::ERROR_CODE_UNKNOWN_PARAM));
    CHECK(Rig::is_err(assign_set(rig, target, 1, 0), EC::ERROR_CODE_UNKNOWN_PARAM));   // 0 is "empty": use ClearAssignSlot
    CHECK(Rig::is_err(assign_set(rig, target, 1, 0x10000u + p::kDrawbar8), EC::ERROR_CODE_UNKNOWN_PARAM));  // not truncated into a valid id
    const auto structural = assign_set(rig, target, 1, p::kInsertType);
    CHECK(Rig::is_err(structural, EC::ERROR_CODE_NOT_ASSIGNABLE) && structural.error().details().at("reason") == "structural");
    CHECK(Rig::is_err(assign_set(rig, target, 1, p::kMasterLevel), EC::ERROR_CODE_NOT_ASSIGNABLE));
    const auto device = assign_set(rig, target, 1, p::kLineInLevel);  // a performance parameter the registry marks non-assignable
    CHECK(Rig::is_err(device, EC::ERROR_CODE_NOT_ASSIGNABLE) && device.error().details().at("reason") == "not_assignable");
    unchanged();
  }
  for (AT bad : {ws::v1::ASSIGN_TARGET_UNSPECIFIED, static_cast<AT>(7)}) {
    CHECK(Rig::is_err(assign_set(rig, bad, 1, p::kDrawbar8), EC::ERROR_CODE_INVALID_COMMAND));
    CHECK(Rig::is_err(assign_clear(rig, bad, 1), EC::ERROR_CODE_INVALID_COMMAND));
  }
  unchanged();
  // The mode / part commands refuse what does not exist and change nothing.
  CHECK(Rig::is_err(encoder_mode(rig, ws::v1::ENCODER_MODE_UNSPECIFIED), EC::ERROR_CODE_INVALID_COMMAND));
  CHECK(Rig::is_err(encoder_mode(rig, static_cast<ws::v1::EncoderMode>(9)), EC::ERROR_CODE_INVALID_COMMAND));
  CHECK(Rig::is_err(select_part(rig, 8), EC::ERROR_CODE_INVALID_PART) && Rig::is_err(select_part(rig, 0xFFFFFFFFu), EC::ERROR_CODE_INVALID_PART));
  unchanged();
  rig.step(2);
  CHECK(rig.state().encoders().mode() == ws::v1::ENCODER_MODE_PART && rig.state().encoders().selected_part() == 0 && rig.br->encoder_state().selected_part() == 0);
}

void test_no_live_set_and_no_data_directory() {
  {
    Rig rig;
    rig.start({perfect(rig.lib)}, 0, 0, {}, /*load_first=*/false);
    CHECK(Rig::is_err(assign_set(rig, kCur, 1, p::kDrawbar8), EC::ERROR_CODE_NO_LIVE_SET));
    CHECK(Rig::is_err(assign_clear(rig, kCur, 1), EC::ERROR_CODE_NO_LIVE_SET));
    // The global default needs no Live Set.
    const auto rep = assign_set(rig, kDef, 2, p::kDrawbar8);
    CHECK(rep.has_ack() && rig.store->default_assign()[1] == p::kDrawbar8);
  }
  {
    Rig rig;  // no edit session, no stores: the engine started without a data directory
    CHECK(Rig::is_err(assign_set(rig, kDef, 1, p::kDrawbar8), EC::ERROR_CODE_INVALID_COMMAND));
    CHECK(Rig::is_err(assign_set(rig, kCur, 1, p::kDrawbar8), EC::ERROR_CODE_INVALID_COMMAND));
    // The encoder mode needs no Live Set and no stores, but a generation to be valid.
  }
  {
    Rig rig;  // an edit session without setlist stores: the default is unavailable, the current map works
    const lsp::LiveSet a = perfect(rig.lib);
    write_set(rig.live_dir, a);
    host::SwitcherOptions so;
    so.background_thread = false;
    host::LiveSetSwitcher sw(rig.loader, rig.plane, so);
    ls::StoreOptions lo;
    lo.dir = rig.live_dir;
    std::unique_ptr<ls::LiveSetStore> live;
    CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
    host::EditSession edits(rig.plane, rig.scenes, *live, nullptr, nullptr, nullptr);
    edits.attach(rig.loader);
    CHECK(rig.loader.load(a).ok());
    rig.render(2);
    const auto rep = assign_set(rig, kDef, 1, p::kDrawbar8);
    CHECK(Rig::is_err(rep, EC::ERROR_CODE_INVALID_COMMAND) && rep.error().message().find("data directory") != std::string::npos);
    CHECK(assign_set(rig, kCur, 1, p::kDrawbar8).has_ack() && rig.cur()->assign[0] == p::kDrawbar8);
    CHECK(!rig.state().encoders().assign_default_available());
  }
}

// ---- the map survives scene recalls, timbre rebuilds, Live Set changes ---------------------------------------------------------------------
void test_map_survives_scenes_rebuilds_and_switches() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib, 10, "A");
  lsp::LiveSet b = perfect(rig.lib, 11, "B");
  b.set_assign(0, p::kKeyClick);
  b.set_assign(7, p::kLeakage);
  rig.start({a, b});
  CHECK(assign_set(rig, kCur, 1, p::kDrawbar8).has_ack() && assign_set(rig, kCur, 5, p::kSendDelayLevel).has_ack());
  const Map edited{p::kDrawbar8, 0, 0, 0, p::kSendDelayLevel, 0, 0, 0};
  // Scene recalls copy the generation and never change the map.
  for (std::uint32_t scene : {0u, 1u, 2u, 0u}) {
    CHECK(rig.recall(scene).has_scene_reply());
    rig.step(3);
    CHECK(map_of(rig.state()) == edited);
  }
  // A timbre edit rebuilds the Live Set from its edited state: refused while it runs (BUSY), and the rebuilt generation holds the map.
  CHECK(rig.timbre(1, rig.lib.c, "c.sfz").has_ack());
  const auto busy = assign_set(rig, kCur, 2, p::kLeakage);
  CHECK(Rig::is_err(busy, EC::ERROR_CODE_BUSY));
  CHECK(busy.error().details().at("reason") == "load_in_progress" || busy.error().details().at("reason") == "reload_in_progress");
  rig.settle();
  CHECK(map_of(rig.state()) == edited);
  CHECK(assign_set(rig, kCur, 2, p::kLeakage).has_ack());
  const Map edited2{p::kDrawbar8, p::kLeakage, 0, 0, p::kSendDelayLevel, 0, 0, 0};
  // Jump to B: its own map; A keeps its unsaved edit and comes back with it.
  CHECK(rig.sw->jump_to(b.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.loader.loaded_id() == b.id());
  CHECK(map_of(rig.state()) == (Map{p::kKeyClick, 0, 0, 0, 0, 0, 0, p::kLeakage}));
  CHECK(rig.edits->is_dirty(a.id()) && !rig.state().edits().current_dirty());
  CHECK(assign_set(rig, kCur, 2, p::kDrawbar16).has_ack());  // B is edited too
  CHECK(rig.sw->jump_to(a.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.settle();
  CHECK(rig.loader.loaded_id() == a.id() && map_of(rig.state()) == edited2);
  CHECK(rig.edits->is_dirty(a.id()) && rig.edits->is_dirty(b.id()));
  CHECK(rig.live->mutations() == 0);  // nothing was written by any of this
}

// ---- Write and a power cycle -------------------------------------------------------------------------------------------------------------------
void test_map_survives_write_and_power_cycle() {
  std::string root;
  Libs libs;
  std::string id;
  const Map wanted{p::kDrawbar8, p::kSendReverbLevel, p::kRotarySpeed, p::kRotaryOn, p::kTempoBpm, 0, p::kLeakage, p::kDrawbar16};
  {
    Rig rig;
    root = rig.tmp.path;
    libs = rig.lib;
    rig.tmp.keep = true;
    const lsp::LiveSet a = perfect(rig.lib);
    id = a.id();
    rig.start({a});
    for (std::uint32_t k = 1; k <= 8; ++k)
      if (wanted[k - 1]) CHECK(assign_set(rig, kCur, k, wanted[k - 1]).has_ack());
    CHECK(rig.edits->is_dirty(id) && map_of(rig.state()) == wanted);
    // Scene scope writes scene storage, not the Live Set's map: nothing to write, the edit stays.
    CHECK(rig.recall(0).has_scene_reply());
    rig.step(3);
    const auto sc = rig.write(ws::v1::WRITE_SCOPE_SCENE);
    CHECK(sc.has_write_reply() && sc.write_reply().nothing_to_write() && rig.edits->is_dirty(id));
    CHECK(file_map(rig.read_file(id)) == Map{});
    // Whole Live Set persists the map; the saved file holds it.
    const auto w = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
    CHECK(w.has_write_reply() && !w.write_reply().current_dirty() && w.write_reply().written_count() == 7);
    CHECK(file_map(rig.read_file(id)) == wanted);
    CHECK(!rig.edits->is_dirty(id) && map_of(rig.state()) == wanted);
    // After the Write, going back to the saved value is a new edit against the new baseline.
    CHECK(assign_clear(rig, kCur, 8).has_ack());
    CHECK(rig.state().edits().current_dirty());
    CHECK(assign_set(rig, kCur, 8, p::kDrawbar16).has_ack() && !rig.state().edits().current_dirty());
    // An unwritten edit is lost at power-off: no autosave.
    CHECK(assign_set(rig, kCur, 6, p::kKeyClick).has_ack());
  }
  {
    Rig rig(root, &libs);  // the power cycle: new engine on the same data directory
    rig.start({}, 0, 0, {}, /*load_first=*/false, /*write_files=*/false);
    const std::string path = rig.live_dir + "/" + id + ".liveset";
    CHECK(rig.loader.load_file(path).ok());
    rig.render(2);
    CHECK(file_map(rig.read_file(id)) == wanted);
    CHECK(map_of(rig.state()) == wanted && rig.cur()->assign[7] == p::kDrawbar16 && !rig.state().edits().current_dirty());
    // And the reloaded map drives the encoders: K1 = drawbar_8 (live 8) down by 3.
    CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_ASSIGN).has_ack());
    rig.step(2);
    turn(rig, 1, -3);
    rig.step(2);
    CHECK(rig.live_value(p::kDrawbar8, 0) == 5.0f);
  }
  {
    // Save as new copies the edited map into the new Live Set; the original keeps its edit.
    Rig rig(root, &libs);
    rig.start({}, 0, 0, {}, false, false);
    CHECK(rig.loader.load_file(rig.live_dir + "/" + id + ".liveset").ok());
    rig.render(2);
    CHECK(assign_set(rig, kCur, 6, p::kKeyClick).has_ack());
    const auto w = rig.write(ws::v1::WRITE_SCOPE_SAVE_AS_NEW, "Copy with map");
    CHECK(w.has_write_reply() && w.write_reply().current_dirty());
    Map copy = wanted;
    copy[5] = p::kKeyClick;
    CHECK(file_map(rig.read_file(w.write_reply().written_live_set_id())) == copy);
    CHECK(file_map(rig.read_file(id)) == wanted && rig.edits->is_dirty(id));
    rig.tmp.keep = false;  // the last rig of the cycle removes the data root
  }
}

// ---- the global default ------------------------------------------------------------------------------------------------------------------------------
void test_global_default_edit_and_independence() {
  std::string root;
  Libs libs;
  const Map factory = sl::factory_default_assign();
  std::string old_id;
  {
    Rig rig;
    root = rig.tmp.path;
    libs = rig.lib;
    rig.tmp.keep = true;
    const lsp::LiveSet a = perfect(rig.lib);
    rig.start({a});
    CHECK(default_of(rig.state()) == factory);
    // A Live Set created BEFORE the edit...
    auto m = msg();
    m.mutable_create_live_set()->set_name("Before");
    const auto created = rig.send(m);
    CHECK(created.has_created_reply());
    old_id = created.created_reply().id();
    CHECK(file_map(rig.read_file(old_id)) == factory);
    const std::uint64_t rev = rig.plane.revision(), live_mut = rig.live->mutations(), store_mut = rig.store->mutations();
    const std::string path = rig.set_dir + "/assign_default.pb";
    // ...is not touched by it; the edit is written at once (a settings edit: no Write, the current Live Set is not dirty).
    const auto rep = assign_set(rig, kDef, 3, p::kDrawbar8);
    CHECK(rep.has_ack() && rig.plane.revision() == rev + 1 && rig.store->mutations() == store_mut + 1 && rig.live->mutations() == live_mut);
    Map expect = factory;
    expect[2] = p::kDrawbar8;
    CHECK(rig.store->default_assign() == expect && fs::exists(path));
    {
      sl::AssignResult res;
      CHECK(sl::parse_assign(slurp(path), 1, res) == sl::Status::kOk && res.assign.assign_size() == 8 && res.assign.assign(2) == p::kDrawbar8);
    }
    ws::v1::State st = rig.state();
    CHECK(default_of(st) == expect && !st.edits().current_dirty() && st.edits().dirty_count() == 0);
    CHECK(map_of(st) == Map{} && rig.cur()->assign[2] == 0);  // the sounding Live Set keeps its own map
    CHECK(file_map(rig.read_file(old_id)) == factory);
    CHECK(file_map(rig.read_file(rig.loader.loaded_id())) == Map{});
    // A Live Set created AFTER the edit copies the new default.
    m = msg();
    m.mutable_create_live_set()->set_name("After");
    const auto created2 = rig.send(m);
    CHECK(created2.has_created_reply() && file_map(rig.read_file(created2.created_reply().id())) == expect);
    CHECK(file_map(rig.read_file(old_id)) == factory);
    // Setting the same value again writes nothing; clear works; both are default-only edits.
    const std::uint64_t rev2 = rig.plane.revision(), mut2 = rig.store->mutations();
    CHECK(assign_set(rig, kDef, 3, p::kDrawbar8).has_ack() && rig.plane.revision() == rev2 && rig.store->mutations() == mut2);
    CHECK(assign_clear(rig, kDef, 1).has_ack());
    expect[0] = 0;
    CHECK(rig.store->default_assign() == expect && default_of(rig.state()) == expect);
    // The created Live Set (factory map) plays with the copied map; the encoders drive its slot 3 through it.
    CHECK(rig.sw->jump_to(old_id).result == host::SwitchOutcome::Result::kAccepted);
    rig.settle();
    CHECK(map_of(rig.state()) == factory);
    CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_ASSIGN).has_ack());
    rig.step(2);
    turn(rig, 1, 16);  // K1 = send_delay_level (the DELAY function of the factory default), part 0
    rig.step(2);
    CHECK(rig.live_value(p::kSendDelayLevel, 0) == 0.125f);
    // Storage full: typed error, nothing partial, the default unchanged, no temp file left.
    const std::string before_bytes = slurp(path);
    rig.store->set_inject_write_errno(ENOSPC);
    const std::uint64_t rev3 = rig.plane.revision();
    const auto full = assign_set(rig, kDef, 4, p::kLeakage);
    CHECK(Rig::is_err(full, EC::ERROR_CODE_STORAGE_FULL) && rig.plane.revision() == rev3 && rig.store->default_assign() == expect);
    CHECK(slurp(path) == before_bytes && !fs::exists(path + ".tmp") && !fs::exists(rig.set_dir + "/assign_default.pb.tmp"));
    rig.store->set_inject_write_errno(0);
    // A failed directory sync is a success with the file in place (the store adopts it).
    rig.store->set_inject_dir_sync_failure(true);
    CHECK(assign_set(rig, kDef, 4, p::kLeakage).has_ack());
    rig.store->set_inject_dir_sync_failure(false);
    expect[3] = p::kLeakage;
    CHECK(rig.store->default_assign() == expect);
    // Not the control thread: refused and nothing changed (single writer).
    // (covered by setlist_tests; here only that the command path reports a typed error rather than crashing.)
  }
  {
    Rig rig(root, &libs);  // power cycle: the default is read back from assign_default.pb
    rig.start({}, 0, 0, {}, false, false);
    CHECK(rig.loader.load_file(rig.live_dir + "/" + old_id + ".liveset").ok());
    rig.render(2);
    Map expect = factory;
    expect[2] = p::kDrawbar8;
    expect[0] = 0;
    expect[3] = p::kLeakage;
    CHECK(rig.store->default_assign() == expect && default_of(rig.state()) == expect);
    auto m = msg();
    m.mutable_create_live_set()->set_name("Third");
    const auto created = rig.send(m);
    CHECK(created.has_created_reply() && file_map(rig.read_file(created.created_reply().id())) == expect);
    CHECK(file_map(rig.read_file(old_id)) == factory);
    rig.tmp.keep = false;
  }
}

// ---- encoder routing end to end ------------------------------------------------------------------------------------------------------------------
void test_routing_end_to_end_and_dirty_summary() {
  Rig rig;
  lsp::LiveSet a = perfect(rig.lib);
  a.set_assign(0, p::kDrawbar8);
  a.set_assign(1, p::kSendReverbLevel);
  a.set_assign(2, p::kRotarySpeed);
  rig.start({a});
  CHECK(map_of(rig.state()) == (Map{p::kDrawbar8, p::kSendReverbLevel, p::kRotarySpeed}) && !rig.state().edits().current_dirty());
  // Default PART mode: the steps route to fixed part params, not ASSIGN map.
  turn(rig, 1, -3);
  rig.step(2);
  CHECK(rig.live_value(p::kDrawbar8, 0) == 8.0f && rig.state().encoders().steps_applied() == 1 && !rig.state().edits().current_dirty());
  CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_ALL).has_ack());
  rig.step(2);
  turn(rig, 1, -3);
  rig.step(2);
  CHECK(rig.live_value(p::kDrawbar8, 0) == 8.0f && rig.state().encoders().steps_applied() == 2);
  // ASSIGN: commanded and applied are both published.
  bool changed = false;
  auto m = msg();
  m.mutable_set_encoder_mode()->set_mode(ws::v1::ENCODER_MODE_ASSIGN);
  const std::uint64_t rev = rig.plane.revision();
  CHECK(rig.send(m, &changed).has_ack() && changed && rig.plane.revision() == rev + 1);
  CHECK(rig.state().encoders().mode() == ws::v1::ENCODER_MODE_ASSIGN);
  rig.step(2);
  CHECK(rig.state().encoders().rt_mode() == ws::v1::ENCODER_MODE_ASSIGN);
  m = msg();
  m.mutable_set_encoder_mode()->set_mode(ws::v1::ENCODER_MODE_ASSIGN);
  CHECK(rig.send(m, &changed).has_ack() && !changed && rig.plane.revision() == rev + 1);  // already so: no new revision, no push
  turn(rig, 1, -3);                                                                          // K1 drawbar_8: 8 -> 5
  turn(rig, 2, 32);                                                                          // K2 reverb send: 0 -> 0.25
  turn(rig, 3, 1);                                                                           // K3 rotary speed: slow -> fast
  turn(rig, 4, 7);                                                                           // K4 is empty: nothing happens
  rig.step(3);
  CHECK(rig.live_value(p::kDrawbar8, 0) == 5.0f && rig.live_value(p::kSendReverbLevel, 0) == 0.25f && rig.live_value(p::kRotarySpeed, 0) == 2.0f);
  ws::v1::State st = rig.state();
  CHECK(st.encoders().steps_applied() == 5 && st.encoders().steps_empty_slot() == 1 && st.encoders().steps_rejected() == 0);
  // The live difference is visible to the edit session: the Live Set is dirty with items for what the encoders moved.
  CHECK(st.edits().current_dirty());
  CHECK(find_item(st.edits(), "part.param", 0, p::kDrawbar8) && find_item(st.edits(), "part.send_reverb", 0) && find_item(st.edits(), "part.param", 0, p::kRotarySpeed));
  // Part-scope parameters land on the selected part.
  CHECK(Rig::is_err(select_part(rig, 8), EC::ERROR_CODE_INVALID_PART));
  CHECK(select_part(rig, 3).has_ack());
  rig.step(2);
  CHECK(rig.state().encoders().selected_part() == 3 && rig.state().encoders().rt_selected_part() == 3);
  turn(rig, 1, -2);
  rig.step(2);
  CHECK(rig.live_value(p::kDrawbar8, 3) == 6.0f && rig.live_value(p::kDrawbar8, 0) == 5.0f);
  // No pickup: a scene recall that stores the value moves it; the next step starts from there.
  CHECK(select_part(rig, 0).has_ack());
  CHECK(rig.recall(0).has_scene_reply());  // "Verse" stores the reverb send of part 0 (0.4) and part 2's drawbar_8 (5)
  rig.step(4);
  CHECK(rig.live_value(p::kSendReverbLevel, 0) == 0.4f);
  turn(rig, 2, 1);
  rig.step(2);
  CHECK(std::fabs(rig.live_value(p::kSendReverbLevel, 0) - (0.4f + 1.0f / 128.0f)) < 1e-6f);
  CHECK(rig.state().encoders().ring_values_size() == 8);
  CHECK(rig.state().encoders().ring_led_fills_size() == 8);
  CHECK(rig.state().encoders().ring_values(1) > 0.0f);
  // Writing persists the encoder moves with the map.
  const auto w = rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
  CHECK(w.has_write_reply() && !w.write_reply().current_dirty());
  CHECK(base_param(rig.read_file(a.id()), 3, p::kDrawbar8) == 6.0f);
}

// A step on a part with no engine behind it (part 7 holds no timbre) still moves the live value; the engine ignores it. The map can name a parameter
// the Live Set file holds but this build refuses: the loader leaves it out and the step is an empty slot.
void test_parts_without_engine_and_stale_maps() {
  Rig rig;
  lsp::LiveSet a = perfect(rig.lib);
  a.set_assign(0, p::kDrawbar16);
  a.set_assign(1, 5000);  // unknown to this registry: preserved in the file with a warning, not in the generation
  rig.start({a});
  CHECK(rig.cur()->assign[0] == p::kDrawbar16 && rig.cur()->assign[1] == 0);
  CHECK(file_map(rig.read_file(a.id()))[1] == 5000);
  CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_ASSIGN).has_ack() && select_part(rig, 7).has_ack());
  rig.step(2);
  turn(rig, 1, 4);
  turn(rig, 2, 4);
  rig.step(2);
  CHECK(rig.live_value(p::kDrawbar16, 7) == 4.0f);
  CHECK(rig.state().encoders().steps_applied() == 0 && rig.state().encoders().steps_no_part() == 1 && rig.state().encoders().steps_empty_slot() == 1);
}

// ---- format validation of the flag --------------------------------------------------------------------------------------------------------------------
void test_stores_refuse_non_assignable_ids() {
  Rig rig;
  lsp::LiveSet a = perfect(rig.lib);
  a.set_assign(0, p::kLineInLevel);  // known but no longer assignable: a legacy file still loads (reported as a warning, kept in the file)
  {
    const auto rep = ls::validate(a, 8);
    CHECK(rep.ok() && rep.unknown_params.size() == 1);
  }
  a.set_assign(0, 0x10000u + p::kDrawbar8);  // never narrowed to parameter 102: unknown, preserved with a warning
  {
    const auto rep = ls::validate(a, 8);
    CHECK(rep.ok() && rep.unknown_params.size() == 1);
  }
  a.set_assign(0, p::kInsertType);
  CHECK(!ls::validate(a, 8).ok());
  a.set_assign(0, p::kTempoBpm);
  CHECK(ls::validate(a, 8).ok());
  ::velora::setlist::v1::AssignDefault d;
  d.set_format_version(1);
  for (int k = 0; k < 8; ++k) d.add_assign(0);
  CHECK(sl::validate(d).ok());
  d.set_assign(2, p::kLineInLevel);  // legacy: a warning naming the slot, not an error
  {
    const auto rep = sl::validate(d);
    CHECK(rep.ok() && rep.warnings.size() == 1 && rep.warnings[0].index == 2);
  }
  d.set_assign(2, 0x10000u + p::kDrawbar8);
  CHECK(!sl::validate(d).ok());
  d.set_assign(2, p::kInsertType);
  CHECK(!sl::validate(d).ok());
  d.set_assign(2, p::kSendReverbMaster);
  CHECK(sl::validate(d).ok());
}

// ---- legacy files ----------------------------------------------------------------------------------------------------------------------------
// A Live Set and a default map written before a parameter became non-assignable still load; the slot stays in the file and out of the generation, and
// editing another slot never fails because of it. The commands keep refusing the parameter.
void test_legacy_files_still_load_and_edit() {
  Rig rig;
  lsp::LiveSet a = perfect(rig.lib);
  a.set_assign(0, p::kLineInLevel);
  a.set_assign(1, 0x10000u + p::kDrawbar8);  // 0x10066: must not alias parameter 102
  fs::create_directories(rig.set_dir);
  ::velora::setlist::v1::AssignDefault d;
  d.set_format_version(1);
  for (std::uint32_t id : {static_cast<std::uint32_t>(p::kLineInLevel), 0u, 0u, 0u, 0u, 0u, 0u, 0u}) d.add_assign(id);
  std::string bytes;
  CHECK(sl::serialize(d, bytes) == sl::Status::kOk);
  { std::ofstream f(rig.set_dir + "/assign_default.pb", std::ios::binary | std::ios::trunc); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
  rig.start({a});
  CHECK(rig.cur()->assign[0] == 0 && rig.cur()->assign[1] == 0);  // out of the generation
  CHECK(map_of(rig.state()) == Map{});                             // State shows the generation's map, which differs from the file's here
  CHECK(!rig.state().edits().current_dirty());                     // dirty / diff are computed from the file: nothing changed
  CHECK(file_map(rig.read_file(a.id()))[0] == p::kLineInLevel && file_map(rig.read_file(a.id()))[1] == 0x10000u + p::kDrawbar8);
  CHECK(Rig::is_err(assign_set(rig, kCur, 1, p::kLineInLevel), EC::ERROR_CODE_NOT_ASSIGNABLE));
  CHECK(assign_set(rig, kCur, 3, p::kDrawbar8).has_ack() && rig.cur()->assign[2] == p::kDrawbar8);
  CHECK(rig.write(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET).has_write_reply());
  CHECK(file_map(rig.read_file(a.id()))[0] == p::kLineInLevel && file_map(rig.read_file(a.id()))[2] == p::kDrawbar8);
  // The default: the legacy slot is reported, kept, and does not block the edit of another slot.
  CHECK(default_of(rig.state())[0] == p::kLineInLevel);
  CHECK(Rig::is_err(assign_set(rig, kDef, 2, p::kLineInLevel), EC::ERROR_CODE_NOT_ASSIGNABLE));
  CHECK(assign_set(rig, kDef, 3, p::kDrawbar8).has_ack());
  sl::AssignResult res;
  CHECK(sl::parse_assign(slurp(rig.set_dir + "/assign_default.pb"), 1, res) == sl::Status::kOk && res.assign.assign(0) == p::kLineInLevel && res.assign.assign(2) == p::kDrawbar8);
  CHECK(res.report.ok() && res.report.warnings.size() == 1 && res.report.warnings[0].index == 0);
  CHECK(assign_clear(rig, kDef, 1).has_ack());  // and it can be cleared
  CHECK(rig.store->default_assign()[0] == 0);
}

// ---- refusals leave everything as it was ------------------------------------------------------------------------------------------------------
void test_ledger_full_and_publish_refused_change_nothing() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  CHECK(assign_set(rig, kCur, 4, p::kLeakage).has_ack());
  auto snapshot_of_state = [&] {
    std::string w;
    CHECK(ls::serialize(*rig.edits->working_of(a.id()), w) == ls::Status::kOk);
    return w;
  };
  auto expect_untouched = [&](const ws::v1::ServerMessage& rep, const char* reason, const std::string& work_before, std::uint64_t rev, std::uint64_t seq,
                              const core::Snapshot* gen, const std::vector<ws::v1::ChangeItem>& items_before) {
    CHECK(Rig::is_err(rep, EC::ERROR_CODE_BUSY) && rep.error().details().at("reason") == reason);
    CHECK(snapshot_of_state() == work_before && rig.plane.revision() == rev && rig.edits->edit_seq() == seq && rig.cur() == gen);
    const auto st = rig.state();
    CHECK(st.edits().change_count() == items_before.size() && rig.cur()->assign[1] == 0);
  };
  const std::string work_before = snapshot_of_state();
  const std::uint64_t rev = rig.plane.revision(), seq = rig.edits->edit_seq();
  const core::Snapshot* gen = rig.cur();
  const ws::v1::State st0 = rig.state();
  const std::vector<ws::v1::ChangeItem> items(st0.edits().change_summary().begin(), st0.edits().change_summary().end());
  // The publish is refused (fault hook): working copy, dirty state, edit sequence and revision are exactly as before.
  rig.plane.inject_publish_fault_for_tests(host::ControlPlane::PublishStatus::kBusy);
  expect_untouched(assign_set(rig, kCur, 2, p::kDrawbar8), "publish_refused", work_before, rev, seq, gen, items);
  CHECK(assign_set(rig, kCur, 2, p::kDrawbar8).has_ack());  // the fault was one shot: the same command works
  CHECK(assign_clear(rig, kCur, 2).has_ack());
  // The ledger is full (every generation pinned by a sounding note).
  const std::uint64_t rev2 = rig.plane.revision(), seq2 = rig.edits->edit_seq();
  const core::Snapshot* gen2 = rig.cur();
  const std::string work2 = snapshot_of_state();
  bool full = false;
  for (std::uint32_t i = 0; i < 40 && !full; ++i) {
    rig.note_on(30 + i);
    rig.render(1);
    full = rig.plane.live_generations() == core::SnapshotExchange::kLedgerSlots;
    if (!full) CHECK(rig.recall(i % 2).has_scene_reply());
    rig.render(1);
  }
  CHECK(rig.plane.live_generations() == core::SnapshotExchange::kLedgerSlots);
  const std::uint64_t rev3 = rig.plane.revision(), seq3 = rig.edits->edit_seq();
  const core::Snapshot* gen3 = rig.cur();
  const std::string work3 = snapshot_of_state();
  (void)rev2; (void)seq2; (void)gen2; (void)work2;
  expect_untouched(assign_set(rig, kCur, 2, p::kDrawbar8), "ledger_full", work3, rev3, seq3, gen3, items);
  for (std::uint32_t i = 0; i < 40; ++i) rig.note_off(30 + i);
  rig.step(200);
  CHECK(assign_set(rig, kCur, 2, p::kDrawbar8).has_ack());
}

// SetEncoderMode / SelectPart with the real-time queue full: BUSY, the commanded values and the revision stay.
void test_encoder_commands_with_a_full_queue() {
  Rig rig;
  rig.start({perfect(rig.lib)});
  while (rig.ctl_q.push(host::StampedUmp{core::make_select_part_event(0), rig.now_ns.load()})) {}
  const std::uint64_t rev = rig.plane.revision();
  CHECK(Rig::is_err(encoder_mode(rig, ws::v1::ENCODER_MODE_ASSIGN), EC::ERROR_CODE_BUSY));
  CHECK(Rig::is_err(select_part(rig, 4), EC::ERROR_CODE_BUSY));
  CHECK(rig.plane.revision() == rev && rig.plane.commanded_encoder_mode() == ws::v1::ENCODER_MODE_PART && rig.plane.commanded_selected_part() == 0);
  rig.render(8);  // RT drains the lane
  CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_ASSIGN).has_ack() && select_part(rig, 4).has_ack());
  rig.step(3);
  ws::v1::State st = rig.state();
  CHECK(st.encoders().mode() == ws::v1::ENCODER_MODE_ASSIGN && st.encoders().rt_mode() == ws::v1::ENCODER_MODE_ASSIGN && st.encoders().rt_selected_part() == 4);
  // A repeated command while RT has not applied it yet publishes the State again (changed), without a new revision.
  const std::uint64_t rev2 = rig.plane.revision();
  bool changed = false;
  auto m = msg();
  m.mutable_set_encoder_mode()->set_mode(ws::v1::ENCODER_MODE_ASSIGN);
  CHECK(rig.send(m, &changed).has_ack() && !changed && rig.plane.revision() == rev2);  // RT agrees: nothing to publish
  CHECK(encoder_mode(rig, ws::v1::ENCODER_MODE_PART).has_ack());
  m = msg();
  m.mutable_set_encoder_mode()->set_mode(ws::v1::ENCODER_MODE_PART);
  CHECK(rig.send(m, &changed).has_ack() && changed);  // commanded PART twice, RT still shows ASSIGN (no block rendered yet)
  // A bad event is visible in the State.
  CHECK(rig.ctl_q.push(host::StampedUmp{core::make_select_part_event(9), rig.now_ns.load()}));
  rig.step(3);
  CHECK(rig.state().encoders().events_rejected() == 1);
}

// ---- concurrency over a socket ---------------------------------------------------------------------------------------------------------------------
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
  bool read_exact(std::string& out, std::size_t n) {
    out.clear();
    while (out.size() < n) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, 5000) <= 0) return false;
      char buf[4096];
      const ssize_t k = ::recv(fd, buf, std::min(sizeof buf, n - out.size()), 0);
      if (k <= 0) return false;
      out.append(buf, static_cast<std::size_t>(k));
    }
    return true;
  }
  bool call(const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply) {
    std::string b;
    (void)m.SerializeToString(&b);
    const std::uint32_t n = static_cast<std::uint32_t>(b.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    const std::string s = std::string(h, 4) + b;
    if (::send(fd, s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) return false;
    for (;;) {
      std::string hd, body;
      if (!read_exact(hd, 4)) return false;
      const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
      if (!read_exact(body, len) || !reply.ParseFromString(body)) return false;
      if (reply.request_id() == m.request_id()) return true;
    }
  }
};

// Four clients edit the default, the current map, the mode and the selected part at once while another thread renders blocks (the audio thread). The control
// thread serialises them: every reply is an Ack, each client's revisions increase, and the final state is what each client's last writes say.
void test_concurrent_clients_over_a_socket() {
  Rig rig;
  const lsp::LiveSet a = perfect(rig.lib);
  rig.start({a});
  const std::string path = "/tmp/velora-asg-" + std::to_string(::getpid()) + ".sock";
  host::ControlServer server(rig.plane, path);
  server.set_tick([&] {
    const bool x = rig.sw->tick();
    const bool y = rig.ctl->tick();
    const bool z = rig.edits->tick();
    return x || y || z;
  });
  server.set_thread_start([&] {
    rig.live->rebind_control_thread();
    rig.store->rebind_control_thread();
  });
  std::string err;
  CHECK(server.start(err));
  std::atomic<bool> stop_render{false};
  std::thread audio([&] {
    while (!stop_render.load()) {
      rig.render(1);
      std::this_thread::yield();
    }
  });
  constexpr int kClients = 4, kRounds = 12;
  const std::uint32_t ids[kRounds] = {p::kDrawbar8,  p::kDrawbar16, p::kSendReverbLevel, p::kSendDelayLevel, p::kLeakage,  p::kKeyClick,
                                      p::kRotaryOn, p::kRotarySpeed, p::kTempoBpm,        p::kDrawbar4,       p::kDrawbar2, p::kSendReverbMaster};
  std::atomic<int> failures{0};
  std::vector<std::uint64_t> revisions[kClients];
  std::vector<std::thread> threads;
  for (int ci = 0; ci < kClients; ++ci) {
    threads.emplace_back([&, ci] {
      Client cl;
      ws::v1::ServerMessage r;
      auto h = msg();
      h.mutable_hello()->set_version(1);
      if (!(cl.open(path) && cl.call(h, r) && r.has_hello_ack())) {
        ++failures;
        return;
      }
      auto send = [&](ws::v1::ClientMessage m) {
        if (!cl.call(m, r) || !r.has_ack()) {
          std::printf("client %d: %s\n", ci, r.has_error() ? r.error().message().c_str() : "no reply");
          ++failures;
          return;
        }
        revisions[ci].push_back(r.revision());
      };
      const std::uint32_t slot_a = static_cast<std::uint32_t>(ci * 2 + 1), slot_b = slot_a + 1;
      for (int k = 0; k < kRounds; ++k) {
        auto m = msg();
        m.mutable_set_assign_slot()->set_target(kDef);
        m.mutable_set_assign_slot()->set_slot(slot_a);
        m.mutable_set_assign_slot()->set_param_id(ids[k]);
        send(m);
        m = msg();
        m.mutable_set_assign_slot()->set_target(kCur);
        m.mutable_set_assign_slot()->set_slot(slot_b);
        m.mutable_set_assign_slot()->set_param_id(ids[(k + 3) % kRounds]);
        send(m);
        m = msg();
        m.mutable_set_encoder_mode()->set_mode(static_cast<ws::v1::EncoderMode>(1 + (ci + k) % 3));
        send(m);
        m = msg();
        m.mutable_select_part()->set_part(static_cast<std::uint32_t>((ci + k) % 8));
        send(m);
        if (k % 4 == 3) {
          m = msg();
          m.mutable_clear_assign_slot()->set_target(kCur);
          m.mutable_clear_assign_slot()->set_slot(slot_b);
          send(m);
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  stop_render = true;
  audio.join();
  server.stop();
  rig.live->rebind_control_thread();
  rig.store->rebind_control_thread();
  CHECK(failures.load() == 0);
  for (int ci = 0; ci < kClients; ++ci)
    for (std::size_t i = 1; i < revisions[ci].size(); ++i) CHECK(revisions[ci][i] >= revisions[ci][i - 1]);
  rig.step(3);
  const ws::v1::State st = rig.state();
  const Map def = default_of(st), cur = map_of(st);
  CHECK(def == rig.store->default_assign());
  for (int ci = 0; ci < kClients; ++ci) {
    CHECK(def[static_cast<std::size_t>(ci * 2)] == ids[kRounds - 1]);  // slot_a: the last value that client wrote
    // slot_b: set with ids[(k + 3) % 12] in round k and cleared after rounds 3, 7, 11: the last round cleared it
    CHECK(cur[static_cast<std::size_t>(ci * 2 + 1)] == 0);
    CHECK(cur[static_cast<std::size_t>(ci * 2)] == 0);
  }
  CHECK(st.encoders().mode() == rig.plane.commanded_encoder_mode() && st.encoders().rt_mode() == st.encoders().mode());
  CHECK(st.encoders().selected_part() == st.encoders().rt_selected_part());
  CHECK(rig.cur() == rig.plane.current_snapshot() && !st.edits().current_dirty());
}

}  // namespace

int main() {
  test_current_map_edit_is_dirty_and_live();
  test_validation_changes_nothing();
  test_no_live_set_and_no_data_directory();
  test_map_survives_scenes_rebuilds_and_switches();
  test_map_survives_write_and_power_cycle();
  test_global_default_edit_and_independence();
  test_routing_end_to_end_and_dirty_summary();
  test_parts_without_engine_and_stale_maps();
  test_stores_refuse_non_assignable_ids();
  test_legacy_files_still_load_and_edit();
  test_ledger_full_and_publish_refused_change_nothing();
  test_encoder_commands_with_a_full_queue();
  test_concurrent_clients_over_a_socket();
  if (g_fail) {
    std::printf("assign_map_tests: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("assign_map_tests: ok\n");
  return 0;
}

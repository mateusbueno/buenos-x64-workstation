// Story 9.8: the read-only Live Set view (ws.v1 GetLiveSetView -> LiveSetViewReply) and the State fields Velora Studio follows from pushes.
// The rig is the engine without its threads (as edit_session_test): renderer behind the real render driver, plane, scene controller, loader, switcher (no
// worker thread), both stores on a temporary data root, setlist controller, edit session and the view service. Libraries are synthetic bundles on disk.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/category.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/edit_session.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/live_set_view.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/setlist_controller.hpp"
#include "velora/host/tier_profile.hpp"

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
using fixtures::uuid_n;

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-view-XXXXXX").string();
    if (!mkdtemp(path.data())) std::exit(1);
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Installs a one-instrument library; `category` (a stable key such as "category.piano", "" = no category file) is written beside the bundle like the librarian does.
std::string install_library(const std::string& root, std::uint32_t index, const char* key, const char* instrument_name, const std::string& category) {
  const std::string fp = "velora-view-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(instrument_name, key);
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(4800, format::float_to_bits(0.1f));
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
  if (!category.empty()) {
    std::ofstream c(dir / "category", std::ios::binary | std::ios::trunc);
    c << category << "\n";
  }
  std::error_code ec;
  fs::remove(fs::path(root) / "lib" / lib_id / "current", ec);  // a re-install flips the link
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

std::string write_set(const std::string& dir, const lsp::LiveSet& s) {
  std::string bytes;
  CHECK(ls::serialize(s, bytes) == ls::Status::kOk);
  const std::string path = dir + "/" + s.id() + ".liveset";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

ws::v1::ClientMessage msg() {
  static std::uint64_t rid = 5000;
  ws::v1::ClientMessage m;
  m.set_request_id(++rid);
  return m;
}

// A head provider that counts every call: a view must never ask for a head (nothing is made resident for a read).
struct CountingHeads : host::RamHeadProvider {
  int calls = 0;
  host::LoadStatus load(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out) override { ++calls; return RamHeadProvider::load(r, out); }
  host::LoadStatus load_controlled(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out, const host::LoadHooks& h) override {
    ++calls;
    return RamHeadProvider::load_controlled(r, out, h);
  }
  std::uint64_t estimate_bytes(const host::ResolveResult& r) const override { return RamHeadProvider::estimate_bytes(r); }
};

struct Rig {
  TmpDir tmp;
  std::string root, live_dir, set_dir;
  std::string lib_piano, lib_pad, lib_nocat, lib_gone;
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
  CountingHeads heads;
  host::LiveSetLoader loader;
  std::unique_ptr<host::LiveSetSwitcher> sw;
  ls::SeededRandom rng{7};
  std::unique_ptr<ls::LiveSetStore> live;
  std::unique_ptr<sl::SetlistStore> store;
  std::unique_ptr<host::LiveSetSwitcherPort> port;
  std::unique_ptr<host::SetlistController> ctl;
  std::unique_ptr<host::EditSession> edits;
  std::unique_ptr<host::LiveSetViewService> view;
  float l[core::kBlockFrames], r[core::kBlockFrames];

  static host::LiveSetLoaderOptions lopts(host::SceneController* s) {
    host::LiveSetLoaderOptions o;
    o.scenes = s;
    return o;
  }
  Rig() : resolver(host::make_posix_fs(), tmp.path), loader(*br, plane, resolver, heads, &sample, lopts(&scenes)) {
    root = tmp.path;
    live_dir = tmp.path + "/data/live_sets";
    set_dir = tmp.path + "/data/setlists";
    fs::create_directories(live_dir);
    fs::create_directories(set_dir);
    lib_piano = install_library(root, 1, "grand.sfz", "Concert Grand", "category.piano");
    lib_pad = install_library(root, 2, "pad.sfz", "Warm Pad", "category.synth_pad");
    lib_nocat = install_library(root, 3, "nocat.sfz", "Plain Thing", "");
    lib_gone = uuid_n(900);  // never installed
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }

  // "Perfect": part 1 piano (VOL 0.8, zone 36..96), part 2 pad, part 3 organ (drawbars 88 8600 000-ish) OFF, part 4 sample with a missing library, part 5 a
  // timbre whose instrument key is not in an existing library (also missing), part 6 a library without a category, parts 7, 8 empty. Three scenes.
  lsp::LiveSet perfect(std::uint64_t seed = 10, const char* name = "Perfect") const {
    lsp::LiveSet s = ls::make_default_live_set(uuid_n(seed), name, 8);
    fixtures::timbre(s.mutable_parts(0), lib_piano, "grand.sfz");
    s.mutable_parts(0)->set_volume(0.8f);
    s.mutable_parts(0)->set_low_key(36);
    s.mutable_parts(0)->set_high_key(96);
    fixtures::timbre(s.mutable_parts(1), lib_pad, "pad.sfz");
    s.mutable_parts(1)->set_volume(0.4f);
    lsp::Part* o = s.mutable_parts(2);
    o->mutable_engine()->set_engine_type_id(2);
    o->set_keyboard_on(false);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar16, 8);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar513, 8);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar4, 6);
    fixtures::timbre(s.mutable_parts(3), lib_gone, "gone.sfz");
    fixtures::timbre(s.mutable_parts(4), lib_piano, "not-in-the-library.sfz");
    fixtures::timbre(s.mutable_parts(5), lib_nocat, "nocat.sfz");
    s.add_scenes()->set_name("Intro");
    s.add_scenes()->set_name("Verse");
    s.add_scenes()->set_name("Chorus");
    return s;
  }

  void start(const std::vector<lsp::LiveSet>& sets, std::size_t first = 0) {
    for (const lsp::LiveSet& s : sets) write_set(live_dir, s);
    host::SwitcherOptions so;
    so.background_thread = false;
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
    eo.fence_idle = [this] { render(1); };
    eo.rng = &rng;
    edits = std::make_unique<host::EditSession>(plane, scenes, *live, store.get(), ctl.get(), port.get(), eo);
    edits->attach(loader);
    edits->attach(*sw);
    view = std::make_unique<host::LiveSetViewService>(plane, *live, resolver, edits.get(), port.get());
    CHECK(loader.load(sets[first]).ok());
    render(2);
  }
  void render(int n) {
    for (int i = 0; i < n; ++i) {
      now_ns += 1'333'333;
      driver.render_block(l, r, now_ns);
    }
  }
  void step(int n = 1) {
    for (int i = 0; i < n; ++i) {
      sw->tick();
      ctl->tick();
      edits->tick();
      render(1);
    }
  }
  ws::v1::ServerMessage send(const ws::v1::ClientMessage& m, bool* changed = nullptr) {
    bool c = false;
    ws::v1::ServerMessage rep = plane.handle(m, c);
    if (changed) *changed = c;
    CHECK(rep.request_id() == m.request_id());
    return rep;
  }
  ws::v1::ServerMessage get_view(const std::string& id, bool* changed = nullptr) {
    auto m = msg();
    m.mutable_get_live_set_view()->set_live_set_id(id);
    return send(m, changed);
  }
  ws::v1::State state() {
    auto m = msg();
    m.mutable_get_state();
    const auto rep = send(m);
    CHECK(rep.has_state());
    return rep.state();
  }
};

bool is_err(const ws::v1::ServerMessage& rep, EC c) { return rep.has_error() && rep.error().code() == c; }

// ---- organ registration text -----------------------------------------------------------------------------------------------------------------------
void test_registration_text() {
  const float a[9] = {8, 8, 8, 6, 0, 0, 0, 0, 0};
  CHECK(host::organ_registration_text(a) == "88 8600 000");
  const float b[9] = {0, 0, 8, 0, 0, 0, 0, 0, 0};
  CHECK(host::organ_registration_text(b) == "00 8000 000");
  const float c[9] = {-3, 99, NAN, 7.6f, 2.4f, INFINITY, 1, 1, 1};  // clamped, non-finite = 0
  CHECK(host::organ_registration_text(c) == "08 0820 111");
}

// ---- the stored Live Set that sounds: every field ----------------------------------------------------------------------------------------------------
void test_view_of_sounding_set() {
  Rig rig;
  const lsp::LiveSet perfect = rig.perfect();
  rig.start({perfect});
  bool changed = true;
  const ws::v1::ServerMessage rep = rig.get_view(perfect.id(), &changed);
  CHECK(!changed);
  CHECK(rep.has_live_set_view_reply());
  const ws::v1::LiveSetViewReply& v = rep.live_set_view_reply();
  CHECK(v.live_set_id() == perfect.id() && v.name() == "Perfect" && !v.dirty() && v.sounding() && !v.factory());
  CHECK(v.dirty() == rig.state().edits().current_dirty());  // the diamond the view shows is the one State.edits publishes
  CHECK(v.scenes_size() == 8 && v.scene_count() == 3);
  CHECK(v.scenes(0).name() == "Intro" && !v.scenes(0).empty() && v.scenes(2).name() == "Chorus" && v.scenes(2).index() == 2);
  for (int i = 3; i < 8; ++i) CHECK(v.scenes(i).empty() && v.scenes(i).name().empty() && v.scenes(i).index() == static_cast<std::uint32_t>(i));
  CHECK(v.parts_size() == 8 && v.part_count() == 8);
  // Part 1: piano with its category from the bundle's category file, volume and zone from the Live Set.
  const auto& p0 = v.parts(0);
  CHECK(p0.index() == 0 && !p0.empty() && p0.on() && !p0.missing_library() && p0.timbre_name() == "Concert Grand");
  CHECK(p0.category() == static_cast<std::uint32_t>(format::Category::kPiano) && p0.engine_type_id() == 1);
  CHECK_NEAR(p0.volume(), 0.8, 1e-6);
  CHECK(p0.low_key() == 36 && p0.high_key() == 96 && p0.organ_registration().empty());
  CHECK(v.parts(1).timbre_name() == "Warm Pad" && v.parts(1).category() == static_cast<std::uint32_t>(format::Category::kSynthPad));
  // Part 3: an organ, off: registration from its drawbars (the registry default 8 for the 8' drawbar is overridden by the stored values), category Organ, no timbre name.
  const auto& p2 = v.parts(2);
  CHECK(p2.engine_type_id() == 2 && !p2.on() && p2.organ_registration() == "88 8600 000" && p2.timbre_name().empty());
  CHECK(p2.category() == static_cast<std::uint32_t>(format::Category::kOrgan) && !p2.empty() && !p2.missing_library());
  // Parts 4 and 5: missing library / missing instrument: flagged, and NEVER another timbre's name.
  CHECK(v.parts(3).missing_library() && v.parts(3).timbre_name().empty() && !v.parts(3).empty() && v.parts(3).category() == 0);
  CHECK(v.parts(4).missing_library() && v.parts(4).timbre_name().empty());
  // Part 6: a library with no category file: the name shows, the category is 0.
  CHECK(!v.parts(5).missing_library() && v.parts(5).timbre_name() == "Plain Thing" && v.parts(5).category() == 0);
  // Parts 7, 8: empty.
  CHECK(v.parts(6).empty() && v.parts(7).empty() && v.parts(6).timbre_name().empty() && !v.parts(6).missing_library() && v.parts(6).on());
}

// ---- READ-ONLY: nothing changes, nothing is written, nothing is published ---------------------------------------------------------------------------
void test_view_is_read_only() {
  Rig rig;
  const lsp::LiveSet a = rig.perfect(10, "Perfect"), b = rig.perfect(11, "Other");
  rig.start({a, b});
  CHECK(rig.scenes.recall_scene(1).ok());
  rig.step(3);
  const ws::v1::State before = rig.state();
  const std::uint64_t rev = rig.plane.revision(), mut = rig.live->mutations(), rejected = rig.plane.commands_rejected();
  const std::size_t gens = rig.plane.live_generations();
  const std::string sounding = rig.sw->sounding_id();
  const std::int32_t scene = rig.scenes.current_scene();
  const core::Snapshot* snap = rig.plane.current_snapshot();
  const std::uint64_t edit_seq = rig.edits->edit_seq();
  const int head_calls = rig.heads.calls;
  const auto res = rig.br->arbiter().reservation();
  const bool reserved = rig.br->arbiter().reserved();
  // The sounding set, another set (never loaded), an unknown id: none of them touches anything.
  for (const std::string& id : {a.id(), b.id(), uuid_n(77), std::string("not-an-id")}) {
    bool changed = true;
    (void)rig.get_view(id, &changed);
    CHECK(!changed);
  }
  rig.step(3);
  CHECK(rig.plane.revision() == rev);
  CHECK(rig.plane.current_snapshot() == snap);
  CHECK(rig.live->mutations() == mut);
  CHECK(rig.plane.live_generations() == gens);
  CHECK(rig.sw->sounding_id() == sounding && rig.sw->sounding_id() == a.id());
  CHECK(!rig.sw->loading());
  CHECK(rig.scenes.current_scene() == scene && scene == 1);
  CHECK(rig.edits->edit_seq() == edit_seq);
  CHECK(rig.heads.calls == head_calls);  // no head was loaded or estimated for a read
  CHECK(rig.br->arbiter().reserved() == reserved);
  CHECK(rig.br->arbiter().reservation().budget == res.budget && rig.br->arbiter().reservation().fx == res.fx && rig.br->arbiter().reservation().organ_rotary == res.organ_rotary &&
        rig.br->arbiter().reservation().floors == res.floors);
  CHECK(rig.plane.commands_rejected() == rejected + 2);  // the two refused ids only
  const ws::v1::State after = rig.state();
  CHECK(after.revision() == before.revision() && after.current_scene() == before.current_scene() && after.live_set_id() == before.live_set_id());
  CHECK(after.loading().state() == before.loading().state());
  CHECK(!after.edits().current_dirty() && after.edits().dirty_count() == 0);
  // The view of another set is static and says it does not sound.
  const auto other = rig.get_view(b.id());
  CHECK(other.has_live_set_view_reply() && !other.live_set_view_reply().sounding() && other.live_set_view_reply().name() == "Other" && !other.live_set_view_reply().dirty());
}

// ---- errors ---------------------------------------------------------------------------------------------------------------------------------------
void test_errors() {
  Rig rig;
  rig.start({rig.perfect()});
  CHECK(is_err(rig.get_view(""), EC::ERROR_CODE_INVALID_COMMAND));
  const ws::v1::ServerMessage unknown = rig.get_view(uuid_n(4242));
  CHECK(is_err(unknown, EC::ERROR_CODE_NO_SUCH_LIVE_SET) && unknown.error().details().at("id") == uuid_n(4242));
  CHECK(is_err(rig.get_view("../../etc/passwd"), EC::ERROR_CODE_NO_SUCH_LIVE_SET));
  // A corrupt file of an existing id is a typed load failure, not a crash.
  const std::string bad_id = uuid_n(31);
  {
    std::ofstream f(rig.live_dir + "/" + bad_id + ".liveset", std::ios::binary);
    f << "this is not a protobuf message \xff\xff";
  }
  const ws::v1::ServerMessage bad = rig.get_view(bad_id);
  CHECK(is_err(bad, EC::ERROR_CODE_LIVE_SET_LOAD_FAILED));
  // No view handler (an engine without a Live Set directory): a typed refusal.
  {
    TmpDir t;
    std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>();
    host::HostEventQueue q;
    host::ControlPlane plane(br->snapshots(), br->last_values(), q, [] { return std::uint64_t{0}; });
    std::string err;
    CHECK(plane.load_initial(ws::v1::State{}, err));
    auto m = msg();
    m.mutable_get_live_set_view()->set_live_set_id(uuid_n(1));
    bool changed = true;
    const auto rep = plane.handle(m, changed);
    CHECK(!changed && is_err(rep, EC::ERROR_CODE_INVALID_COMMAND));
  }
}

// ---- the edit overlay and the ◆ ----------------------------------------------------------------------------------------------------------------
void test_overlay_and_dirty() {
  Rig rig;
  const lsp::LiveSet a = rig.perfect(10, "Perfect"), b = rig.perfect(11, "Other");
  rig.start({a, b});
  CHECK(!rig.get_view(a.id()).live_set_view_reply().dirty());
  // The player (or Studio) moves part 1's fader: an edit in memory, never written.
  auto m = msg();
  m.mutable_set_part_volume()->set_part(0);
  m.mutable_set_part_volume()->set_volume(0.25f);
  CHECK(rig.send(m).has_ack());
  rig.step(6);
  const std::uint64_t mut = rig.live->mutations();
  const auto rep = rig.get_view(a.id());
  CHECK(rep.has_live_set_view_reply());
  CHECK(rep.live_set_view_reply().dirty() && rep.live_set_view_reply().sounding());
  CHECK_NEAR(rep.live_set_view_reply().parts(0).volume(), 0.25, 1e-5);  // the overlay, not the stored 0.8
  CHECK(rig.live->mutations() == mut);
  // The stored file is untouched, and another Live Set stays clean and unedited.
  ls::LoadResult lr;
  CHECK(rig.live->read(a.id(), lr) == ls::Status::kOk);
  CHECK_NEAR(lr.live_set.parts(0).volume(), 0.8, 1e-6);
  const auto other = rig.get_view(b.id());
  CHECK(!other.live_set_view_reply().dirty() && other.live_set_view_reply().parts_size() == 8);
  CHECK_NEAR(other.live_set_view_reply().parts(0).volume(), 0.8, 1e-6);
  // The edited Live Set keeps its overlay (and the ◆) after the player jumps to another one.
  auto j = msg();
  j.mutable_jump_to_live_set()->set_id(b.id());
  CHECK(rig.send(j).has_jump_reply());
  for (int i = 0; i < 300 && (rig.sw->loading()); ++i) rig.step();
  rig.step(5);
  const auto after = rig.get_view(a.id());
  CHECK(after.live_set_view_reply().dirty() && !after.live_set_view_reply().sounding());
  CHECK_NEAR(after.live_set_view_reply().parts(0).volume(), 0.25, 1e-5);
  CHECK(rig.get_view(b.id()).live_set_view_reply().sounding());
}

// ---- State carries what the view follows -----------------------------------------------------------------------------------------------------------
void test_state_part_fields() {
  Rig rig;
  const lsp::LiveSet a = rig.perfect();
  rig.start({a});
  const ws::v1::State st = rig.state();
  CHECK(st.parts_size() == 8);
  CHECK_NEAR(st.parts(0).volume(), 0.8, 1e-6);
  CHECK(st.parts(0).keyboard_on() && st.parts(0).low_key() == 36 && st.parts(0).high_key() == 96 && st.parts(0).engine_type_id() == 1);
  CHECK(st.parts(0).part_state() == ws::v1::PART_RUN_STATE_READY);
  for (const auto& ps : st.parts()) CHECK(ps.view_fields());
  CHECK(!st.parts(2).keyboard_on() && st.parts(2).engine_type_id() == 2);
  CHECK(st.parts(3).part_state() == ws::v1::PART_RUN_STATE_MISSING_LIBRARY);
  CHECK(st.parts(6).part_state() == ws::v1::PART_RUN_STATE_EMPTY_TIMBRE);
  // A fader move is a new generation: the State push carries the new volume.
  auto m = msg();
  m.mutable_set_part_volume()->set_part(1);
  m.mutable_set_part_volume()->set_volume(0.5f);
  CHECK(rig.send(m).has_ack());
  CHECK_NEAR(rig.state().parts(1).volume(), 0.5, 1e-6);
}

// ---- Pro: 16 parts -------------------------------------------------------------------------------------------------------------------------------------
void test_pro_sixteen_parts() {
  TmpDir t;
  const std::string dir = t.path + "/live";
  fs::create_directories(dir);
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(16);
  host::HostEventQueue q;
  host::ControlPlane plane(br->snapshots(), br->last_values(), q, [] { return std::uint64_t{0}; });
  plane.set_tier_profile(host::pro_profile());
  std::string err;
  CHECK(plane.load_initial(ws::v1::State{}, err));
  ls::StoreOptions lo;
  lo.dir = dir;
  lo.part_count = 16;
  std::unique_ptr<ls::LiveSetStore> live;
  CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
  lsp::LiveSet pro = ls::make_default_live_set(uuid_n(60), "Pro show", 16);
  pro.mutable_parts(12)->set_volume(0.33f);
  write_set(dir, pro);
  host::LibraryResolver resolver(host::make_posix_fs(), t.path);
  host::LiveSetViewService view(plane, *live, resolver);
  auto m = msg();
  m.mutable_get_live_set_view()->set_live_set_id(pro.id());
  bool changed = true;
  const auto rep = plane.handle(m, changed);
  CHECK(!changed && rep.has_live_set_view_reply());
  const auto& v = rep.live_set_view_reply();
  CHECK(v.parts_size() == 16 && v.part_count() == 16 && v.scenes_size() == 8 && v.name() == "Pro show" && !v.sounding() && !v.dirty());
  CHECK(v.parts(15).index() == 15 && v.parts(15).empty());
  CHECK_NEAR(v.parts(12).volume(), 0.33, 1e-6);
  // A v1 plane never reports more than its cap, even if a (hand-copied) file holds 16 parts: the cap of the plane wins.
}

// ---- ids: long, hostile, not echoed ---------------------------------------------------------------------------------------------------------------
void test_ids_and_v1_plane_with_sixteen_part_file() {
  Rig rig;
  rig.start({rig.perfect()});
  const std::string huge(100000, 'x');
  for (const std::string& id : {huge, std::string("a\0b", 3), std::string("<script>")}) {
    const auto rep = rig.get_view(id);
    CHECK(is_err(rep, EC::ERROR_CODE_NO_SUCH_LIVE_SET));
    CHECK(rep.error().details().count("id") == 0);  // a non-UUID string is never echoed
    for (const auto& kv : rep.error().details()) CHECK(kv.second.size() <= 64);
  }
  // A 16-part file on a v1 plane (a Pro file copied to a v1 unit): the store refuses it, so the view answers LIVE_SET_LOAD_FAILED (no truncation).
  lsp::LiveSet pro = ls::make_default_live_set(uuid_n(77), "Pro on v1", 16);
  write_set(rig.live_dir, pro);
  const auto rep = rig.get_view(pro.id());
  CHECK(is_err(rep, EC::ERROR_CODE_LIVE_SET_LOAD_FAILED));
}

// ---- the resolved-timbre cache ---------------------------------------------------------------------------------------------------------------------
void test_timbre_cache() {
  Rig rig;
  const lsp::LiveSet a = rig.perfect();
  rig.start({a});
  CHECK(rig.view->cache_hits() == 0);
  const auto first = rig.get_view(a.id());
  CHECK(rig.view->cache_size() == 3);  // the piano, the pad and the library without a category (failures are not cached)
  const auto second = rig.get_view(a.id());
  CHECK(rig.view->cache_hits() == 3);  // hits for the three good ones
  CHECK(second.live_set_view_reply().SerializeAsString() == first.live_set_view_reply().SerializeAsString());
  CHECK(first.live_set_view_reply().parts(3).unavailable_reason() == "missing_library");
  CHECK(first.live_set_view_reply().parts(4).unavailable_reason() == "missing_instrument");
  // The library is replaced (the link now names another bundle): the next view misses and shows the new name.
  install_library(rig.root, 1, "grand.sfz", "Concert Grand II", "category.piano");
  const std::uint64_t hits = rig.view->cache_hits();
  const auto third = rig.get_view(a.id());
  CHECK(third.live_set_view_reply().parts(0).timbre_name() == "Concert Grand II");
  CHECK(rig.view->cache_hits() == hits + 2);  // the pad and the plain one still hit
}

// ---- a freshly created Live Set that sounds is not dirty (the diamond Studio shows) -------------------------------------------------------------------
void test_fresh_live_set_is_clean() {
  Rig rig;
  rig.start({rig.perfect()});
  auto c = msg();
  c.mutable_create_live_set()->set_name("Fresh");
  const auto made = rig.send(c);
  CHECK(made.has_created_reply());
  const std::string id = made.created_reply().id();
  auto j = msg();
  j.mutable_jump_to_live_set()->set_id(id);
  CHECK(rig.send(j).has_jump_reply());
  for (int i = 0; i < 300 && rig.sw->loading(); ++i) rig.step();
  rig.step(20);
  const ws::v1::State st = rig.state();
  CHECK(st.live_set_id() == id);
  CHECK(!st.edits().current_dirty());
  CHECK(!rig.get_view(id).live_set_view_reply().dirty());
}

}  // namespace

int main() {
  test_registration_text();
  test_view_of_sounding_set();
  test_view_is_read_only();
  test_errors();
  test_overlay_and_dirty();
  test_state_part_fields();
  test_pro_sixteen_parts();
  test_ids_and_v1_plane_with_sixteen_part_file();
  test_timbre_cache();
  test_fresh_live_set_is_clean();
  if (g_fail) {
    std::printf("live_set_view_tests: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("live_set_view_tests: ok\n");
  return 0;
}

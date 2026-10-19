// Story 4.5: seamless Live Set switching with background load. The library content is synthetic (a looping constant per instrument with a
// 0.4 s release, laid out as the resolver expects), so a Live Set is told apart in the output by its level. The block renderer is driven directly,
// one block at a time; the background load runs on the switcher's real worker thread unless a test uses the deterministic mode. Nothing here
// asserts a wall-clock duration: waits are bounded only so that a hang fails instead of blocking, and the progress cadence is checked against a
// fake clock.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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
#include "velora/host/live_set_switcher.hpp"
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

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-switch-XXXXXX").string();
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

// One library with one instrument: a looping constant (`frames` frames, mono float) with a release of `release_s`.
std::string install_library(const std::string& root, std::uint32_t index, const char* key, float level, std::uint32_t frames = 4800,
                            float release_s = 0.4f) {
  const std::string fp = "velora-switch-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(key, key);
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(frames, format::float_to_bits(level));
  CHECK(w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, frames, pcm}) == format::Error::kNone);
  format::Region r;
  r.sample = 0;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = format::LoopMode::kLoopContinuous;
  r.ampeg.release = release_s;
  CHECK(w.add_region(ins.value(), r) == format::Error::kNone);
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

std::string write_set(const std::string& dir, const lsp::LiveSet& s) {
  std::string bytes;
  CHECK(ls::serialize(s, bytes) == ls::Status::kOk);
  const std::string path = dir + "/" + s.id() + ".liveset";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

template <class F>
bool wait_until(F&& pred, double seconds = 30.0) {  // a hang fails the test; it is not a timing assertion
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// Holds a load until opened; wakes when the load's cancel flag is set.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool is_open = true;
  void close() {
    std::lock_guard<std::mutex> lk(m);
    is_open = false;
  }
  void open() {
    {
      std::lock_guard<std::mutex> lk(m);
      is_open = true;
    }
    cv.notify_all();
  }
  bool wait(const std::atomic<bool>* cancel) {
    std::unique_lock<std::mutex> lk(m);
    while (!is_open) {
      if (cancel && cancel->load(std::memory_order_acquire)) return false;
      cv.wait_for(lk, std::chrono::milliseconds(1));
    }
    return true;
  }
};

// The RAM provider that counts, can block, can fail, can simulate a slow disk step by step, and tracks what is still alive.
class Heads final : public host::HeadProvider {
 public:
  host::LoadStatus load(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out) override {
    return load_controlled(r, out, host::LoadHooks{});
  }
  host::LoadStatus load_controlled(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out,
                                   const host::LoadHooks& hooks) override {
    const int n = ++loads;
    {
      std::lock_guard<std::mutex> lk(m);
      leases.push_back(r.bundle);
    }
    if (before_load) before_load(n, hooks);
    if (hooks.cancel && hooks.cancel->load(std::memory_order_acquire)) return host::LoadStatus::kCancelled;
    if (fail_on.load() == n) return host::LoadStatus::kBundleInconsistent;
    std::shared_ptr<const host::BundleInstrument> inner;
    host::LoadStatus st;
    if (throttle_steps > 0) {
      st = throttled(r, inner, hooks);
    } else {
      st = ram.load_controlled(r, inner, hooks);
    }
    if (st != host::LoadStatus::kOk) return st;
    ++alive;
    Heads* self = this;
    out = std::shared_ptr<const host::BundleInstrument>(inner.get(), [inner, self](const host::BundleInstrument*) { --self->alive; });
    {
      std::lock_guard<std::mutex> lk(m);
      loaded.emplace_back(r.instrument_info().key, out);
    }
    return host::LoadStatus::kOk;
  }
  std::uint64_t estimate_bytes(const host::ResolveResult& r) const override {
    ++estimates;
    if (on_estimate) on_estimate();
    const std::uint64_t o = estimate_override.load();
    return o != ~std::uint64_t{0} ? o : host::HeadProvider::estimate_bytes(r);
  }
  // True when every instrument this provider made resident under `key` has been freed.
  bool freed(const std::string& key) {
    std::lock_guard<std::mutex> lk(m);
    for (auto& kv : loaded)
      if (kv.first == key && !kv.second.expired()) return false;
    return true;
  }
  bool leases_alive() {
    std::lock_guard<std::mutex> lk(m);
    for (auto& w : leases)
      if (!w.expired()) return true;
    return false;
  }

  host::RamHeadProvider ram;
  std::atomic<int> loads{0}, alive{0};
  mutable std::atomic<int> estimates{0};
  std::atomic<int> fail_on{0};  // the 1-based load call that fails
  std::atomic<std::uint64_t> estimate_override{~std::uint64_t{0}};
  std::function<void(int, const host::LoadHooks&)> before_load;
  std::function<void()> on_estimate;  // a step of the stage that cannot be cancelled
  int throttle_steps = 0;  // > 0: every load reports its bytes in that many steps, one per permit
  std::atomic<int> permits{0}, steps_done{0};

 private:
  host::LoadStatus throttled(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out, const host::LoadHooks& hooks) {
    const std::uint64_t est = host::HeadProvider::estimate_bytes(r);
    for (int i = 0; i < throttle_steps; ++i) {
      while (permits.load() <= 0) {
        if (hooks.cancel && hooks.cancel->load(std::memory_order_acquire)) return host::LoadStatus::kCancelled;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      permits.fetch_sub(1);
      std::uint64_t bytes = est / static_cast<std::uint64_t>(throttle_steps);
      if (i == throttle_steps - 1) bytes = est - (est / static_cast<std::uint64_t>(throttle_steps)) * static_cast<std::uint64_t>(throttle_steps - 1);
      if (hooks.progress && !hooks.progress(bytes)) return host::LoadStatus::kCancelled;
      ++steps_done;
    }
    return ram.load(r, out);
  }
  std::mutex m;
  std::vector<std::weak_ptr<const format::BundleReader>> leases;
  std::vector<std::pair<std::string, std::weak_ptr<const host::BundleInstrument>>> loaded;
};

struct Rig {
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::SceneController scenes;
  host::LibraryResolver resolver;
  Heads heads;
  host::LiveSetLoader loader;
  std::unique_ptr<host::LiveSetSwitcher> sw;
  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::vector<std::vector<float>> blocks;

  static core::SampleEngine::Config cfg() {
    core::SampleEngine::Config c;
    c.max_voices = 340;
    return c;
  }
  static host::LiveSetLoaderOptions opts(host::SceneController* s) {
    host::LiveSetLoaderOptions o;
    o.scenes = s;
    return o;
  }
  explicit Rig(const std::string& root)
      : sample(cfg()),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        scenes(plane),
        resolver(host::make_posix_fs(), root),
        loader(*br, plane, resolver, heads, &sample, opts(&scenes)) {
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }
  // The initial (quiesced) load; the provider's call counters start from zero afterwards.
  bool load_initial(const lsp::LiveSet& set) {
    const bool ok = loader.load(set).ok();
    heads.loads = 0;
    heads.estimates = 0;
    return ok;
  }
  void make_switcher(host::SwitcherOptions o, const std::string& dir) {
    sw = std::make_unique<host::LiveSetSwitcher>(loader, plane, std::move(o));
    sw->set_catalog(host::scan_live_set_dir(dir, 8));
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
  static double unity() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }
  double level() const {
    double s = 0;
    for (std::uint32_t i = core::kBlockFrames - 32; i < core::kBlockFrames; ++i) s += blocks.back()[i];
    return s / 32.0 / unity();
  }
  const core::Snapshot* cur() const { return plane.current_snapshot(); }
  // Control tick + one block, the way the engine runs them.
  void step() {
    sw->tick();
    run(1);
  }
};

struct Libs {
  std::string a, b, c, x1, x2, y, big;
};

Libs install_all(const std::string& root) {
  Libs L;
  L.a = install_library(root, 1, "a.sfz", 0.1f);
  L.b = install_library(root, 2, "b.sfz", 0.01f);
  L.c = install_library(root, 3, "c.sfz", 0.05f);
  L.x1 = install_library(root, 4, "x1.sfz", 0.02f);
  L.x2 = install_library(root, 5, "x2.sfz", 0.03f);
  L.y = install_library(root, 6, "y.sfz", 0.04f);
  L.big = install_library(root, 7, "big.sfz", 0.2f, 480000);  // 1.92 MB of decoded PCM
  return L;
}

lsp::LiveSet simple_set(std::uint64_t seed, const char* name, const std::string& lib, const char* key) {
  lsp::LiveSet s = ls::make_default_live_set(uuid_n(seed), name, 8);
  fixtures::timbre(s.mutable_parts(0), lib, key);
  s.mutable_parts(0)->set_volume(1.0f);
  return s;
}

lsp::LiveSet with_organ(lsp::LiveSet s, int part, bool keyboard_on = true) {
  lsp::Part* o = s.mutable_parts(part);
  o->clear_timbre();
  o->mutable_engine()->set_engine_type_id(2);
  o->set_keyboard_on(keyboard_on);
  entry(o->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
  return s;
}

struct World {
  TmpDir tmp;
  std::string dir;
  Libs lib;
  lsp::LiveSet A, B, C, X, Y, Big;
  World() {
    dir = tmp.path + "/sets";
    fs::create_directories(dir);
    lib = install_all(tmp.path);
    A = with_organ(simple_set(71, "Song A", lib.a, "a.sfz"), 3, false);  // an organ part (keyboard off: silent) so that A reserves for the organ
    B = simple_set(72, "Song B", lib.b, "b.sfz");
    C = simple_set(73, "Song C", lib.c, "c.sfz");
    X = ls::make_default_live_set(uuid_n(74), "Song X", 8);
    fixtures::timbre(X.mutable_parts(0), lib.x1, "x1.sfz");
    fixtures::timbre(X.mutable_parts(1), lib.x2, "x2.sfz");
    Y = simple_set(75, "Song Y", lib.y, "y.sfz");
    Big = simple_set(76, "Song Big", lib.big, "big.sfz");
    for (const lsp::LiveSet* s : {&A, &B, &C, &X, &Y, &Big}) write_set(dir, *s);
  }
};

host::SwitcherOptions thread_opts() { return host::SwitcherOptions{}; }
host::SwitcherOptions inline_opts(std::uint32_t hold) {
  host::SwitcherOptions o;
  o.background_thread = false;
  o.hold_ticks = hold;
  return o;
}

// Runs tick + render until the switcher is no longer loading; returns the number of blocks it took.
int pump(Rig& rig, int max_blocks = 200000) {
  int n = 0;
  while (rig.sw->loading() && n < max_blocks) {
    rig.step();
    ++n;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return n;
}

// --- the Given/When/Then of the story --------------------------------------------------------------------------------

// A Live Set playing; a jump to one that is not loaded: the current generation keeps playing and answering the keys while the worker loads; the
// engine publishes `loading` with the target name; the swap only when every head is resident.
void test_background_load_current_keeps_playing() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  rig.run(2);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  gate.close();
  rig.heads.before_load = [gate_p](int, const host::LoadHooks& h) { gate_p->wait(h.cancel); };
  const std::uint64_t rev = rig.plane.revision();

  const host::SwitchOutcome o = rig.sw->jump_to(w.B.id());
  CHECK(o.result == host::SwitchOutcome::Result::kAccepted && o.target_name == "Song B" && !o.superseded_previous);  // returned at once
  CHECK(rig.sw->status().state == host::SwitchState::kLoading && rig.sw->status().target_name == "Song B" && rig.sw->status().loading());
  ws::v1::State st;
  rig.plane.fill_state(&st);
  CHECK(st.loading().loading() && st.loading().state() == ws::v1::LOADING_STATE_LOADING && st.loading().target_name() == "Song B" &&
        st.loading().target_id() == w.B.id() && st.live_set_id() == w.A.id() && st.live_set_name() == "Song A");
  CHECK(st.loading().estimated_remaining_seconds() >= 0);

  // The worker is blocked inside the load of B's instrument. The old generation answers the keys, repeatedly.
  CHECK(wait_until([&] { return rig.heads.loads.load() == 1; }));
  for (int k = 0; k < 5; ++k) {
    rig.note_on(60);
    rig.run(6);
    CHECK_NEAR(rig.level(), 0.1, 2e-3);  // Song A
    rig.note_off(60);
    rig.run(400);  // the whole 0.4 s release
    rig.sw->tick();
    CHECK(rig.plane.revision() == rev && rig.sw->loading() && rig.loader.loaded_id() == w.A.id());  // nothing published yet
  }
  gate.open();
  CHECK(pump(rig) > 0);
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.sw->status().last_error == host::SwitchError::kNone);
  CHECK(rig.plane.revision() == rev + 1 && rig.loader.loaded_id() == w.B.id() && rig.sw->switches_published() == 1);
  rig.run(2);
  rig.note_on(60);
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.01, 1e-3);  // Song B
  rig.note_off(60);
  rig.run(400);
  rig.plane.reclaim();
  CHECK(rig.plane.live_generations() == 1);
  rig.plane.fill_state(&st);
  CHECK(st.loading().state() == ws::v1::LOADING_STATE_READY && !st.loading().loading() && st.live_set_id() == w.B.id() && st.live_set_name() == "Song B");
}

// The heads of the target are all resident before anything is published (all or nothing).
void test_swap_only_when_every_head_is_resident() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  rig.run(2);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  gate.close();
  rig.heads.before_load = [gate_p](int n, const host::LoadHooks& h) {
    if (n == 2) gate_p->wait(h.cancel);  // X has two instruments: the second one is held
  };
  const std::uint64_t rev = rig.plane.revision();
  CHECK(rig.sw->jump_to(w.X.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(wait_until([&] { return rig.heads.loads.load() == 2; }));
  CHECK(rig.heads.alive.load() == 2);  // A's instrument plus X's first one: the second is not resident
  for (int k = 0; k < 50; ++k) {
    rig.step();
    CHECK(rig.plane.revision() == rev);
  }
  gate.open();
  pump(rig);
  CHECK(rig.plane.revision() == rev + 1 && rig.sw->status().state == host::SwitchState::kReady);
  const core::Snapshot* s = rig.cur();
  CHECK(s->parts[0].sample_bank && s->parts[1].sample_bank && s->parts[0].state == core::PartState::kReady && s->parts[1].state == core::PartState::kReady);
}

// Held, sustained and releasing notes finish on the old generation; new notes play on the new one; the old generation is freed only after RT
// reports no voice references it. The audio equals the old Live Set's performance of the old notes plus the new Live Set's performance of the new.
struct Ev {
  int blk;
  char kind;  // 'o' note on, 'f' note off, 'd' pedal down, 'u' pedal up
  int key;
};
void feed(Rig& rig, const std::vector<Ev>& evs, int blk) {
  for (const Ev& e : evs) {
    if (e.blk != blk) continue;
    if (e.kind == 'o') rig.note_on(static_cast<std::uint32_t>(e.key));
    else if (e.kind == 'f') rig.note_off(static_cast<std::uint32_t>(e.key));
    else rig.pedal(e.kind == 'd');
  }
}

void test_notes_across_the_switch() {
  World w;
  const int total = 900, jump_blk = 20, hold = 10;
  // Old notes: 60 held across the swap (off at 120), 64 sustained by the pedal (off at 8, pedal up at 100), 67 releasing (off at 4: its 0.4 s tail
  // runs across the swap). New notes after the swap: 62 (off at 60, held by the pedal until it goes up), 66 (on at 130).
  const std::vector<Ev> old_ev = {{0, 'o', 60}, {0, 'o', 64}, {0, 'o', 67}, {4, 'f', 67}, {6, 'd', 0}, {8, 'f', 64}, {100, 'u', 0}, {120, 'f', 60}};
  const std::vector<Ev> new_ev = {{6, 'd', 0}, {40, 'o', 62}, {60, 'f', 62}, {100, 'u', 0}, {130, 'o', 66}, {140, 'f', 66}};
  std::vector<Ev> all = old_ev;
  for (const Ev& e : new_ev)
    if (e.kind != 'd' && e.kind != 'u') all.push_back(e);

  Rig sw_rig(w.tmp.path);
  CHECK(sw_rig.load_initial(w.A));
  sw_rig.make_switcher(inline_opts(hold), w.dir);
  sw_rig.run(2);
  Rig ref_a(w.tmp.path), ref_b(w.tmp.path);
  CHECK(ref_a.loader.load(w.A).ok());
  CHECK(ref_b.loader.load(w.B).ok());
  ref_a.run(2);
  ref_b.run(2);
  const core::Snapshot* gen_a = sw_rig.cur();
  const auto organ_reserved_a = sw_rig.br->arbiter().reservation().organ_rotary;
  CHECK(organ_reserved_a > 0);
  std::uint64_t used_before_swap = 0;
  int swap_blk = -1;
  bool checked_epoch = false;
  for (int b = 0; b < total; ++b) {
    if (b == jump_blk) CHECK(sw_rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
    if (b >= jump_blk) {
      const std::uint64_t pub = sw_rig.sw->switches_published();
      if (pub == 0 && sw_rig.sw->loading()) used_before_swap = sw_rig.br->arbiter().usage().used();
      sw_rig.sw->tick();
      if (pub == 0 && sw_rig.sw->switches_published() == 1) {
        swap_blk = b;
        // The generation is published but RT has not adopted it: the arbiter still holds the OLD reservation (the reservation moves with the
        // swap, on the RT plane, in the serial phase).
        CHECK(sw_rig.br->arbiter().reservation().organ_rotary == organ_reserved_a);
        checked_epoch = true;
      }
    }
    feed(sw_rig, all, b);
    feed(ref_a, old_ev, b);
    feed(ref_b, new_ev, b);
    sw_rig.run(1);
    ref_a.run(1);
    ref_b.run(1);
    if (b == swap_blk) {
      // RT adopted the generation in this block: the new reservation (no organ part: nothing reserved for it) is in force; the sounding voices
      // are counted exactly as before, none lost, none counted twice.
      CHECK(sw_rig.br->arbiter().reservation().organ_rotary == 0);
      CHECK(sw_rig.br->arbiter().usage().used() == used_before_swap);
      CHECK(sw_rig.br->arbiter().steals() == 0);
    }
    if (swap_blk >= 0 && b == swap_blk + 5) {
      sw_rig.plane.reclaim();
      CHECK(sw_rig.plane.live_generations() == 2);  // the old generation still has sounding voices
      CHECK(sw_rig.sample.active_voices() >= 3);
    }
  }
  CHECK(checked_epoch && swap_blk > jump_blk);
  // The sum of the two reference performances is the switched performance.
  double peak = 0, worst = 0;
  for (std::size_t i = 0; i < sw_rig.blocks.size(); ++i)
    for (std::uint32_t k = 0; k < core::kBlockFrames; ++k) {
      const double expect = static_cast<double>(ref_a.blocks[i][k]) + static_cast<double>(ref_b.blocks[i][k]);
      worst = std::max(worst, std::fabs(expect - static_cast<double>(sw_rig.blocks[i][k])));
      peak = std::max(peak, std::fabs(expect));
    }
  CHECK(peak > 0.05);
  CHECK(worst < 2e-6);
  // Nothing is left: the old generation, its banks and its bundle lease are gone once RT has no voice on it.
  sw_rig.plane.reclaim();
  CHECK(sw_rig.sample.active_voices() == 0 && sw_rig.plane.live_generations() == 1);
  CHECK(sw_rig.br->router().held_keys() == 0);
  (void)gen_a;
  CHECK(sw_rig.heads.alive.load() == 1);  // only B's instrument
}

// The old generation's instruments and bundle leases stay alive while a note sounds on it, and go away with the last voice.
void test_old_generation_freed_only_when_no_voice_references_it() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  std::weak_ptr<core::SampleBank> old_bank = rig.cur()->parts[0].sample_bank;
  const core::Snapshot* old_gen = rig.cur();
  (void)old_gen;
  rig.note_on(60);
  rig.run(4);
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();  // commit + the adopting block
  CHECK(rig.loader.loaded_id() == w.B.id());
  for (int k = 0; k < 100; ++k) {
    rig.run(1);
    rig.plane.reclaim();
    CHECK(rig.plane.live_generations() == 2 && !old_bank.expired() && rig.heads.alive.load() == 2 && rig.heads.leases_alive());
  }
  rig.note_off(60);
  rig.run(10);
  rig.plane.reclaim();
  CHECK(rig.plane.live_generations() == 2 && !old_bank.expired());  // releasing tail
  rig.run(400);
  rig.plane.reclaim();
  CHECK(rig.sample.active_voices() == 0 && rig.plane.live_generations() == 1 && old_bank.expired() && rig.heads.alive.load() == 1);
  // The lease of A's bundle is released with its instrument; B's is still held by B's generation.
  CHECK(rig.heads.leases_alive());
}

// A note struck on the old generation whose key is released after the swap, and layouts that differ (zones, octave, transpose, engine).
void test_cross_layout_note_off() {
  World w;
  lsp::LiveSet A = with_organ(simple_set(81, "Layout A", w.lib.a, "a.sfz"), 2);
  A.mutable_parts(0)->set_high_key(59);
  A.mutable_parts(0)->set_octave(1);
  A.mutable_parts(2)->set_low_key(60);
  lsp::LiveSet B = simple_set(82, "Layout B", w.lib.b, "b.sfz");
  B.mutable_parts(0)->set_low_key(60);
  B.mutable_parts(0)->set_transpose(-5);
  write_set(w.dir, A);
  write_set(w.dir, B);
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  rig.note_on(50);  // part 0, pitch 62 (zone 0..59, +1 octave)
  rig.note_on(70);  // the organ (zone 60..127)
  rig.run(8);
  CHECK(rig.sample.active_voices() == 1 && rig.organ.active_voices() >= 1);
  CHECK(rig.sw->jump_to(B.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();
  CHECK(rig.loader.loaded_id() == B.id());
  rig.run(8);
  CHECK(rig.sample.active_voices() == 1 && rig.organ.active_voices() >= 1);  // still sounding on the old layout
  rig.note_on(70);                                                            // B: part 0, pitch 65 (+ no organ part any more)
  rig.note_on(50);                                                            // outside B's zone
  rig.run(8);
  CHECK(rig.sample.active_voices() == 2 && rig.br->router().unzoned_keys() == 1);
  rig.note_off(50);  // releases A's part-0 note at the pitch it sounded (62), not B's mapping
  rig.note_off(70);  // A's organ note and B's new note share the key: the router tracks them per (part, pitch, engine)
  rig.run(600);
  CHECK(rig.sample.active_voices() == 0 && rig.organ.active_voices() == 0 && rig.br->router().held_keys() == 0);
  CHECK(rig.br->router().orphan_note_offs() == 0);
  rig.plane.reclaim();
  CHECK(rig.plane.live_generations() == 1);
}


// Voices of the sample engine in the releasing state (a note off, or a pedal-up, reached them).
int releasing_voices(const core::SampleEngine& e) {
  int n = 0;
  core::VoiceInfo vi;
  for (std::uint32_t i = 0; i < e.voice_capacity(); ++i)
    if (e.voice_info(i, vi) && vi.state == core::VoiceState::kReleasing) ++n;
  return n;
}

// A part that changes engine at the switch: the old note and the pedal-held old note are released on the engine they started on.
void test_engine_change_across_the_switch() {
  World w;
  lsp::LiveSet C = with_organ(simple_set(83, "Engine C", w.lib.c, "c.sfz"), 0);  // part 0 becomes the organ
  C.mutable_parts(1)->mutable_timbre()->set_library_id(w.lib.b);
  C.mutable_parts(1)->mutable_timbre()->set_instrument_key("b.sfz");
  write_set(w.dir, C);
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  rig.note_on(60);  // held across the switch
  rig.note_on(62);
  rig.run(4);
  rig.note_off(62);
  rig.pedal(true);
  rig.run(2);
  rig.note_off(60);  // deferred by the pedal: sustained under the sample engine
  rig.run(8);
  CHECK(rig.sample.active_voices() == 2 && releasing_voices(rig.sample) == 1);  // 62 is releasing (it ended before the pedal), 60 is sustained
  CHECK(rig.sw->jump_to(C.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();
  CHECK(rig.loader.loaded_id() == C.id());
  rig.run(8);
  CHECK(rig.sample.active_voices() == 2 && rig.organ.active_voices() == 0);
  rig.note_on(64);  // the organ plays the new notes (part 0), the sample part 1 too
  rig.run(8);
  CHECK(rig.organ.active_voices() >= 1);
  rig.pedal(false);  // releases the deferred sample note on the sample engine although part 0 is an organ now
  rig.note_off(64);
  rig.run(900);
  CHECK(rig.sample.active_voices() == 0 && rig.organ.active_voices() == 0 && rig.br->router().held_keys() == 0);
  rig.plane.reclaim();
  CHECK(rig.plane.live_generations() == 1);
}

// A pedal that is down at the switch also holds the new Live Set's notes, on the parts whose pedal flag the old layout never set.
void test_pedal_down_across_the_switch_holds_new_notes() {
  World w;
  lsp::LiveSet A = simple_set(84, "Pedal A", w.lib.a, "a.sfz");
  lsp::LiveSet B = simple_set(85, "Pedal B", w.lib.b, "b.sfz");
  fixtures::timbre(B.mutable_parts(1), w.lib.c, "c.sfz");  // a second sounding part, new in B
  B.mutable_parts(1)->set_volume(1.0f);
  write_set(w.dir, A);
  write_set(w.dir, B);
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  rig.pedal(true);
  rig.run(2);
  CHECK(rig.sw->jump_to(B.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();
  CHECK(rig.loader.loaded_id() == B.id());
  rig.run(4);
  rig.note_on(60);  // part 0 (a/b) and part 1 (c), the pedal still down
  rig.run(6);
  rig.note_off(60);
  rig.run(20);
  CHECK(rig.sample.active_voices() == 2 && releasing_voices(rig.sample) == 0);  // both held by the pedal
  rig.pedal(false);
  rig.run(20);
  CHECK(releasing_voices(rig.sample) == 2);
  rig.run(600);
  CHECK(rig.sample.active_voices() == 0);
}

// --- second jump while a load is in progress -----------------------------------------------------------------------------

void test_second_jump_cancels_the_first() {
  World w;
  Rig rig(w.tmp.path), ref(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  CHECK(ref.loader.load(w.A).ok());
  rig.make_switcher(thread_opts(), w.dir);
  rig.run(2);
  ref.run(2);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  gate.close();
  rig.heads.before_load = [gate_p](int n, const host::LoadHooks& h) {
    if (n == 2) gate_p->wait(h.cancel);  // X's second instrument blocks until cancelled
  };
  const std::uint64_t rev = rig.plane.revision();
  rig.note_on(60);
  ref.note_on(60);
  rig.run(1);
  ref.run(1);
  CHECK(rig.sw->jump_to(w.X.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(wait_until([&] { return rig.heads.loads.load() == 2; }));
  CHECK(rig.heads.alive.load() == 2);  // A's instrument plus X's first one: a partial load
  for (int k = 0; k < 40; ++k) {       // the sound continues, bit for bit, while the first load is in flight
    rig.step();
    ref.run(1);
  }
  const host::SwitchOutcome o = rig.sw->jump_to(w.Y.id());
  CHECK(o.result == host::SwitchOutcome::Result::kAccepted && o.superseded_previous);
  CHECK(rig.sw->status().cancelled_loads == 1 && rig.sw->status().target_name == "Song Y");
  // X's partial heads are released by the cancelled stage; Y loads.
  CHECK(wait_until([&] { return rig.heads.loads.load() >= 3; }));  // Y's load started: X's stage has returned and freed its partial heads
  CHECK(rig.heads.freed("x1.sfz"));  // the cancelled load's partial heads are gone
  std::size_t committed_at = 0;
  while (rig.sw->loading() && committed_at < 100000) {
    rig.step();
    ref.run(1);
    ++committed_at;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.loader.loaded_id() == w.Y.id() && rig.plane.revision() == rev + 1);
  // Up to the commit nothing differs from a rig that never saw a jump: the audio is identical to the old Live Set continuing.
  const std::size_t n = rig.blocks.size() - 2;  // the last two blocks may include the adopted generation
  bool same = true;
  for (std::size_t i = 0; i < n; ++i) same = same && rig.blocks[i] == ref.blocks[i];
  CHECK(same);
  CHECK(rig.sw->status().cancelled_loads == 1);
  rig.note_off(60);
  rig.run(500);
  rig.plane.reclaim();
  // X was never published; A's instrument died with A's generation; only Y's is alive.
  CHECK(rig.heads.alive.load() == 1 && rig.plane.live_generations() == 1);
  gate.open();
}

void test_jump_back_and_same_target() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  gate.close();
  rig.heads.before_load = [gate_p](int, const host::LoadHooks& h) { gate_p->wait(h.cancel); };
  // The Live Set that already sounds: a no-op, nothing is loaded.
  const std::uint64_t seq = rig.sw->status().seq;
  host::SwitchOutcome o = rig.sw->jump_to(w.A.id());
  CHECK(o.result == host::SwitchOutcome::Result::kAlreadyLoaded && rig.heads.loads.load() == 0 && rig.sw->status().seq == seq && !rig.sw->loading());
  // The same target twice: the load is not restarted.
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(wait_until([&] { return rig.heads.loads.load() == 1; }));
  o = rig.sw->jump_to(w.B.id());
  CHECK(o.result == host::SwitchOutcome::Result::kAlreadyLoading && rig.sw->status().cancelled_loads == 0 && rig.sw->loading());
  // Back to the Live Set that sounds while B loads: the load is cancelled (informational), the sound never changed.
  o = rig.sw->jump_to(w.A.id());
  CHECK(o.result == host::SwitchOutcome::Result::kAlreadyLoaded && o.superseded_previous && !rig.sw->loading());
  CHECK(rig.sw->status().state == host::SwitchState::kIdle && rig.sw->status().last_error == host::SwitchError::kLoadCancelled &&
        rig.sw->status().informational() && rig.sw->status().cancelled_loads == 1);
  CHECK(wait_until([&] { return rig.heads.alive.load() == 1; }));  // only the instrument of the Live Set that sounds
  rig.sw->tick();
  CHECK(rig.loader.loaded_id() == w.A.id() && rig.plane.revision() == 1);
  // CancelLoad with nothing loading answers "nothing cancelled"; with a load it cancels.
  CHECK(!rig.sw->cancel_load().cancelled);
  CHECK(rig.sw->jump_to(w.C.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(rig.sw->cancel_load().cancelled && rig.sw->status().cancelled_loads == 2 && rig.sw->status().informational());
  gate.open();
}

// --- head budget and typed refusals ----------------------------------------------------------------------------------

void test_head_budget_refused_early_and_while_loading() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  host::SwitcherOptions o = thread_opts();
  o.head_budget_bytes = 1'000'000;  // the big instrument needs 1.92 MB, the small ones 19.2 kB
  rig.make_switcher(o, w.dir);
  rig.run(2);
  rig.note_on(60);
  rig.run(8);
  const std::uint64_t rev = rig.plane.revision();
  CHECK(rig.sw->jump_to(w.Big.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kHeadBudgetExceeded);
  CHECK(rig.sw->status().last_error_detail.find("head_budget=1000000") != std::string::npos);
  CHECK(rig.heads.loads.load() == 0 && rig.heads.estimates.load() >= 1);  // refused from the index sizes: no sample was loaded
  CHECK(rig.heads.alive.load() == 1 && rig.plane.revision() == rev && rig.loader.loaded_id() == w.A.id());
  rig.run(8);
  CHECK_NEAR(rig.level(), 0.1, 2e-3);  // the current Live Set keeps playing
  ws::v1::State st;
  rig.plane.fill_state(&st);
  CHECK(st.loading().state() == ws::v1::LOADING_STATE_FAILED && st.loading().last_error() == ws::v1::ERROR_CODE_HEAD_BUDGET_EXCEEDED &&
        !st.loading().last_error_informational() && !st.loading().loading());
  // A provider whose index sizes undercount: the budget is guarded while loading, and the partial heads are freed.
  rig.heads.estimate_override = 100;
  CHECK(rig.sw->jump_to(w.Big.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.heads.loads.load() == 1);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kHeadBudgetExceeded);
  CHECK(rig.heads.alive.load() == 1 && rig.plane.revision() == rev);
  // A target within the budget still loads afterwards.
  rig.heads.estimate_override = ~std::uint64_t{0};
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.plane.revision() == rev + 1 && rig.sw->status().last_error == host::SwitchError::kNone);
}

void test_typed_failures_keep_playing() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  rig.run(2);
  rig.note_on(60);
  rig.run(8);
  const std::uint64_t rev = rig.plane.revision();
  // An unknown id.
  host::SwitchOutcome o = rig.sw->jump_to("00000000-0000-4000-8000-000000000000");
  CHECK(o.result == host::SwitchOutcome::Result::kUnknownLiveSet);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kNoSuchLiveSet);
  // An instrument that cannot be made resident.
  rig.heads.fail_on = 1;
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kLoadFailed &&
        rig.sw->status().last_error_detail == "instrument_load_failed");
  // A file that fails validation (a Live Set with a wrong part count) and one that is not a Live Set at all.
  lsp::LiveSet bad = simple_set(90, "Bad", w.lib.b, "b.sfz");
  bad.mutable_parts(0)->set_low_key(100);
  bad.mutable_parts(0)->set_high_key(10);  // low > high: invalid
  std::string bytes;
  ls::serialize(bad, bytes);  // serialize does not validate
  const std::string bad_path = w.dir + "/" + bad.id() + ".liveset";
  {
    std::ofstream f(bad_path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const std::string junk_path = w.dir + "/junk.liveset";
  {
    std::ofstream f(junk_path, std::ios::binary | std::ios::trunc);
    f << "this is not a protobuf";
  }
  rig.sw->add_to_catalog({bad.id(), "Bad", bad_path});
  rig.sw->add_to_catalog({"11111111-1111-4111-8111-111111111111", "Junk", junk_path});
  rig.sw->add_to_catalog({"22222222-2222-4222-8222-222222222222", "Gone", w.dir + "/missing.liveset"});
  CHECK(rig.sw->jump_to(bad.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kLoadFailed &&
        rig.sw->status().last_error_detail == "invalid_live_set");
  CHECK(rig.sw->jump_to("11111111-1111-4111-8111-111111111111").result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error_detail == "read_failed");
  CHECK(rig.sw->jump_to("22222222-2222-4222-8222-222222222222").result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error_detail == "read_failed");
  // Through all of it the old Live Set sounded and nothing was published.
  CHECK(rig.plane.revision() == rev && rig.loader.loaded_id() == w.A.id() && rig.heads.alive.load() == 1);
  rig.run(8);
  CHECK_NEAR(rig.level(), 0.1, 2e-3);
  // A later good jump works.
  rig.heads.fail_on = 0;
  CHECK(rig.sw->jump_to(w.C.id()).result == host::SwitchOutcome::Result::kAccepted);
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.loader.loaded_id() == w.C.id() && rig.plane.revision() == rev + 1);
}

// A voice-pool refusal at the commit leaves everything as it was.
void test_arbiter_refusal_keeps_playing() {
  World w;
  Rig rig(w.tmp.path);
  host::LiveSetLoaderOptions lo;
  lo.scenes = &rig.scenes;
  lo.arbiter_budget = 1u << 20;
  CHECK(rig.load_initial(w.A));
  // A second loader with a budget too small for the sample floor refuses the reservation of any sample Live Set.
  host::LiveSetLoaderOptions small;
  small.scenes = &rig.scenes;
  small.arbiter_budget = 10;
  host::LiveSetLoader tiny(*rig.br, rig.plane, rig.resolver, rig.heads, &rig.sample, small);
  host::LiveSetSwitcher sw(tiny, rig.plane, thread_opts());
  sw.set_catalog(host::scan_live_set_dir(w.dir, 8));
  const std::uint64_t rev = rig.plane.revision();
  const auto before = rig.br->arbiter().reservation();
  CHECK(sw.jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  while (sw.loading()) {
    sw.tick();
    rig.run(1);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  CHECK(sw.status().state == host::SwitchState::kFailed && sw.status().last_error_detail == "arbiter_rejected");
  CHECK(rig.plane.revision() == rev && rig.br->arbiter().reservation().budget == before.budget && rig.br->arbiter().reservation().floors == before.floors);
}

// --- loading status: at least once per second, with a fake clock and a throttled provider -------------------------------

void test_loading_status_cadence() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  double clock = 100.0;
  host::SwitcherOptions o = thread_opts();
  o.clock = [&clock] { return clock; };
  rig.make_switcher(o, w.dir);
  rig.heads.throttle_steps = 24;  // the load takes 24 simulated steps of 0.35 s: ~8.4 s
  const double t0 = clock;
  CHECK(rig.sw->jump_to(w.Y.id()).result == host::SwitchOutcome::Result::kAccepted);
  std::vector<double> times;       // when the status was (re)published
  std::vector<host::SwitchStatus> seen;
  std::uint64_t last_seq = 0;
  auto observe = [&] {
    if (rig.sw->status().seq != last_seq) {
      last_seq = rig.sw->status().seq;
      times.push_back(clock - t0);
      seen.push_back(rig.sw->status());
    }
  };
  observe();
  CHECK(seen.size() == 1 && seen[0].state == host::SwitchState::kLoading && seen[0].target_name == "Song Y");
  for (int step = 0; step < 24; ++step) {
    rig.heads.permits.fetch_add(1);
    CHECK(wait_until([&] { return rig.heads.steps_done.load() == step + 1; }));
    // Several control ticks per simulated step (the server polls every <= 20 ms).
    for (int k = 0; k < 7; ++k) {
      clock += 0.05;
      rig.sw->tick();
      observe();
    }
  }
  // The finished load is committed on the next tick.
  for (int k = 0; k < 2000 && rig.sw->loading(); ++k) {
    clock += 0.05;
    rig.sw->tick();
    observe();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.loader.loaded_id() == w.Y.id());
  CHECK(times.size() >= 8);
  // Updates at least once per (simulated) second while loading.
  int loading_updates = 0;
  double prev_t = 0, prev_progress = 0;
  for (std::size_t i = 0; i < seen.size(); ++i) {
    if (seen[i].state != host::SwitchState::kLoading) continue;
    ++loading_updates;
    if (i > 0) CHECK(times[i] - prev_t <= 1.0 + 1e-9);
    prev_t = times[i];
    CHECK(std::isfinite(seen[i].estimated_remaining_s) && seen[i].estimated_remaining_s >= 0);
    CHECK(seen[i].progress >= prev_progress - 1e-6 && seen[i].progress <= 1.0f);
    prev_progress = seen[i].progress;
    CHECK(seen[i].target_name == "Song Y");
  }
  CHECK(loading_updates >= 8);
  CHECK(prev_t >= 7.0);  // the updates covered the load
  // The estimate follows the rate: mid-load it is positive, near the end it is small.
  const host::SwitchStatus& mid = seen[seen.size() / 2];
  CHECK(mid.estimated_remaining_s > 0.5 && mid.estimated_remaining_s < 10.0);
  double last_eta = 0;
  for (const auto& s : seen)
    if (s.state == host::SwitchState::kLoading) last_eta = s.estimated_remaining_s;
  CHECK(last_eta < 2.0);
  CHECK(rig.sw->status().estimated_remaining_s == 0 && rig.sw->status().progress == 1.0f);
}

// --- a full snapshot ledger: the commit is retried, then reported as busy ----------------------------------------------------

// Pins every ledger slot with a held note per generation (as the scene tests do).
bool fill_ledger(Rig& rig) {
  for (std::uint32_t i = 0; i < 40; ++i) {
    rig.note_on(30 + i);
    rig.run(1);
    ws::v1::ClientMessage m;
    m.set_request_id(100 + i);
    m.mutable_set_part_output()->set_part(4);
    m.mutable_set_part_output()->set_output(i % 2 ? ws::v1::PART_OUTPUT_AUX : ws::v1::PART_OUTPUT_MAIN);
    bool changed = false;
    const ws::v1::ServerMessage rep = rig.plane.handle(m, changed);
    if (rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_BUSY) return true;
  }
  return false;
}

void test_full_ledger_is_retried_then_busy() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  double clock = 10.0;
  host::SwitcherOptions o = inline_opts(0);
  o.clock = [&clock] { return clock; };
  o.busy_retry_s = 5.0;
  rig.make_switcher(o, w.dir);
  rig.run(2);
  CHECK(fill_ledger(rig));
  const std::uint64_t rev = rig.plane.revision();
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  // The staged Live Set is complete but cannot be published: it waits, the sound is untouched, nothing is published.
  for (int k = 0; k < 5; ++k) {
    clock += 0.5;
    rig.sw->tick();
    rig.run(1);
    CHECK(rig.sw->status().state == host::SwitchState::kLoading && rig.plane.revision() == rev);
  }
  // Notes end, generations are freed: the retry succeeds.
  for (std::uint32_t i = 0; i < 40; ++i) rig.note_off(30 + i);
  rig.run(600);  // the 0.4 s releases end
  clock += 0.5;
  CHECK(rig.sw->tick());
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.plane.revision() == rev + 1 && rig.loader.loaded_id() == w.B.id());

  // A ledger that stays full is reported after busy_retry_s, as a typed failure; the old Live Set keeps playing.
  rig.run(600);
  rig.plane.reclaim();
  CHECK(rig.load_initial(w.A));  // back to A with the quiesced API
  rig.run(2);
  CHECK(fill_ledger(rig));
  const std::uint64_t rev2 = rig.plane.revision();
  CHECK(rig.sw->jump_to(w.C.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.sw->tick();
  CHECK(rig.sw->status().state == host::SwitchState::kLoading);
  clock += 6.0;
  rig.sw->tick();
  CHECK(rig.sw->status().state == host::SwitchState::kFailed && rig.sw->status().last_error == host::SwitchError::kLoadFailed &&
        rig.sw->status().last_error_detail == "busy" && rig.plane.revision() == rev2);
}

// --- reservation handover -----------------------------------------------------------------------------------------------

void test_reservation_applied_on_rt_at_the_swap() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  const auto ra = rig.br->arbiter().reservation();
  CHECK(ra.organ_rotary > 0 && ra.floors > 0);
  // B: no organ part, a sample part. Prepare happens on the control thread, the arbiter only changes when RT adopts the generation.
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(rig.sw->tick() && rig.sw->switches_published() == 1);
  CHECK(rig.br->arbiter().reservation().organ_rotary == ra.organ_rotary);  // not yet: no block has run
  rig.run(1);
  CHECK(rig.br->arbiter().reservation().organ_rotary == 0 && rig.br->arbiter().reservation().floors == ra.floors && rig.br->arbiter().reserved());
  // Later generations copied from the switch generation (a scene recall, a structural command) do not re-commit the reservation.
  const std::uint64_t epoch = rig.cur()->reservation_epoch;
  CHECK(epoch != 0);
  ws::v1::ClientMessage m;
  m.set_request_id(1);
  m.mutable_set_part_output()->set_part(4);
  m.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  bool changed = false;
  CHECK(rig.plane.handle(m, changed).has_ack() && changed && rig.cur()->reservation_epoch == epoch);
  rig.br->arbiter().clear_reservation();  // if RT re-applied the epoch it would be reserved again
  rig.run(2);
  CHECK(!rig.br->arbiter().reserved());
  // The quiesced API still works as before.
  CHECK(rig.loader.load(w.C).ok() && rig.br->arbiter().reserved());
}

// The pedal state is kept in step on every path: a pedal pressed and released before the switch must not hold the new Live Set's notes.
void test_pedal_released_before_the_switch() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  rig.pedal(true);
  rig.run(2);
  rig.pedal(false);
  rig.run(2);
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();
  CHECK(rig.loader.loaded_id() == w.B.id());
  rig.run(4);
  rig.note_on(60);
  rig.run(6);
  rig.note_off(60);
  rig.run(10);
  CHECK(rig.sample.active_voices() >= 1 && releasing_voices(rig.sample) == static_cast<int>(rig.sample.active_voices()));
}

// A Live Set switch is a program change: a knob moved live under the old Live Set does not leak into a new one whose initial value is the same.
void test_live_knob_does_not_leak_across_the_switch() {
  World w;
  lsp::LiveSet A = with_organ(simple_set(86, "Knob A", w.lib.a, "a.sfz"), 2, false);
  lsp::LiveSet B = with_organ(simple_set(87, "Knob B", w.lib.b, "b.sfz"), 2, false);  // the same organ registration (drawbar_8 = 8)
  write_set(w.dir, A);
  write_set(w.dir, B);
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(A));
  rig.make_switcher(inline_opts(0), w.dir);
  rig.run(2);
  const auto slot = p::perf_slot(*p::find(p::kDrawbar8), 2);
  CHECK(rig.br->submit({core::make_param_event(*p::find(p::kDrawbar8), 2, 2.0), 0}));
  rig.run(2);
  CHECK_NEAR(rig.br->last_values().load(slot), 2.0, 1e-6);
  CHECK(rig.sw->jump_to(B.id()).result == host::SwitchOutcome::Result::kAccepted);
  rig.step();
  rig.run(2);
  CHECK_NEAR(rig.br->last_values().load(slot), 8.0, 1e-6);  // the new Live Set's value, not the leftover knob
}

// The loading status keeps its cadence while the stage is held for the simulated load time and while a busy commit is retried.
void test_status_cadence_while_held_and_busy() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  double clock = 50.0;
  host::SwitcherOptions o = inline_opts(40);
  o.clock = [&clock] { return clock; };
  o.busy_retry_s = 5.0;
  rig.make_switcher(o, w.dir);
  rig.run(2);
  auto gaps_ok = [&](int ticks, double step, double limit) {
    std::uint64_t seq = rig.sw->status().seq;
    double last = clock;
    for (int k = 0; k < ticks && rig.sw->loading(); ++k) {
      clock += step;
      rig.sw->tick();
      if (rig.sw->status().seq != seq) {
        seq = rig.sw->status().seq;
        last = clock;
      }
      if (clock - last > limit + 1e-9) return false;
    }
    return true;
  };
  CHECK(rig.sw->jump_to(w.B.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(gaps_ok(30, 0.3, 1.0));  // held (hold_ticks 40): republished all along
  CHECK(rig.sw->loading());
  // A busy commit.
  while (rig.sw->loading()) rig.step();
  rig.run(600);
  rig.plane.reclaim();
  CHECK(rig.load_initial(w.A));
  rig.run(2);
  CHECK(fill_ledger(rig));
  host::SwitcherOptions o2 = inline_opts(0);
  o2.clock = [&clock] { return clock; };
  o2.busy_retry_s = 5.0;
  rig.sw.reset();
  rig.make_switcher(o2, w.dir);
  CHECK(rig.sw->jump_to(w.C.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(gaps_ok(12, 0.4, 1.0));  // ~4.8 s of retrying, still within the 5 s limit
  CHECK(rig.sw->loading() && rig.sw->status().seq > 4);
}

// The first job is inside a step that cannot be cancelled (the provider's size estimate) when a second jump arrives: the cancel is seen at the
// next check, the first target is never loaded, and the new target loads.
void test_second_jump_during_a_non_cancellable_step() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  auto gate_p = std::make_shared<Gate>();
  gate_p->close();
  std::atomic<int> entered{0};
  rig.heads.on_estimate = [gate_p, &entered] {
    ++entered;
    gate_p->wait(nullptr);  // ignores the cancel flag on purpose
  };
  CHECK(rig.sw->jump_to(w.X.id()).result == host::SwitchOutcome::Result::kAccepted);
  CHECK(wait_until([&] { return entered.load() == 1; }));
  CHECK(rig.sw->jump_to(w.Y.id()).result == host::SwitchOutcome::Result::kAccepted);
  gate_p->open();
  pump(rig);
  rig.heads.on_estimate = nullptr;
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.loader.loaded_id() == w.Y.id() && rig.sw->status().cancelled_loads == 1);
  CHECK(rig.heads.loads.load() == 1 && rig.heads.freed("x1.sfz"));  // X was never loaded
}

// stage() on its own: a failure reports the parts as the old load() did.
void test_stage_failure_reports_parts() {
  World w;
  Rig rig(w.tmp.path);
  lsp::LiveSet S = w.X;
  fixtures::timbre(S.mutable_parts(5), uuid_n(9100), "ghost.sfz");  // a library that is not installed
  rig.heads.fail_on = 2;
  std::unique_ptr<host::StagedLiveSet> out;
  host::LiveSetLoadResult r = rig.loader.stage(S, host::StageControl{}, out);
  CHECK(r.status == host::LiveSetLoadStatus::kInstrumentLoadFailed && r.failed_part == 1 && !out);
  CHECK(r.parts.size() == 8 && r.parts[0].state == core::PartState::kReady && r.parts[1].state == core::PartState::kReady &&
        r.parts[5].state == core::PartState::kMissingLibrary && r.parts[3].state == core::PartState::kEmptyTimbre);
  CHECK(rig.heads.alive.load() == 0);  // the first instrument, already resident, was released
  rig.heads.fail_on = 0;
  host::StageControl ctl;
  ctl.head_budget_bytes = 100;
  r = rig.loader.stage(S, ctl, out);
  CHECK(r.status == host::LiveSetLoadStatus::kHeadBudgetExceeded && r.head_budget == 100 && r.head_bytes == 2 * 19200 && !out);
  CHECK(r.parts.size() == 8 && r.parts[0].state == core::PartState::kReady && r.parts[5].state == core::PartState::kMissingLibrary);
  // An estimate of zero does not skip the budget: the in-flight bytes are guarded, and the progress total follows the loaded bytes.
  rig.heads.estimate_override = 0;
  std::uint64_t last_total = 0, last_done = 0;
  ctl.progress = [&](const host::StageProgress& sp) {
    last_total = sp.bytes_total;
    last_done = sp.bytes_done;
  };
  r = rig.loader.stage(S, ctl, out);
  CHECK(r.status == host::LiveSetLoadStatus::kHeadBudgetExceeded && r.head_bytes > 100 && !out && rig.heads.alive.load() == 0);
  ctl.head_budget_bytes = 0;
  r = rig.loader.stage(S, ctl, out);
  CHECK(r.ok() && out && last_done > 0 && last_total >= last_done);
}

// --- ws.v1 over a real Unix domain socket --------------------------------------------------------------------------------

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
    (void)m.SerializeToString(&b);
    const std::uint32_t n = static_cast<std::uint32_t>(b.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    const std::string s = std::string(h, 4) + b;
    return ::send(fd, s.data(), s.size(), 0) == static_cast<ssize_t>(s.size());
  }
  bool recv(ws::v1::ServerMessage& m, int timeout_ms = 10000) {
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
  m.mutable_hello()->set_client_name("switch-test");
  return m;
}

void test_ws_v1_switch_commands() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  host::SwitcherOptions o = thread_opts();
  o.head_budget_bytes = 1'000'000;
  rig.make_switcher(o, w.dir);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  rig.heads.before_load = [gate_p](int, const host::LoadHooks& h) { gate_p->wait(h.cancel); };
  std::atomic<bool> stop{false};
  std::thread rt([&] {  // a real "RT" thread keeps rendering while clients jump
    float l[64], r[64];
    while (!stop.load()) {
      rig.br->process_block(l, r);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  const std::string path = w.tmp.path + "/e.sock";
  {
    host::ControlServer server(rig.plane, path);
    host::LiveSetSwitcher* swp = rig.sw.get();
    server.set_tick([swp] { return swp->tick(); });
    std::string err;
    CHECK(server.start(err));
    Client sub, c;
    ws::v1::ServerMessage rep, pushed;
    CHECK(sub.open(path) && sub.call(hello_msg(), rep));
    ws::v1::ClientMessage m;
    m.set_request_id(2);
    m.mutable_subscribe()->set_enable(true);
    CHECK(sub.call(m, rep));
    CHECK(sub.recv(pushed) && pushed.has_state() && pushed.state().live_set_id() == w.A.id() && !pushed.state().loading().loading());
    CHECK(c.open(path) && c.call(hello_msg(), rep));

    // Unknown id: a typed error, nothing changes.
    m.Clear();
    m.set_request_id(10);
    m.mutable_jump_to_live_set()->set_id("00000000-0000-4000-8000-000000000000");
    CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET && rep.request_id() == 10 &&
          rep.error().details().at("id") == "00000000-0000-4000-8000-000000000000");
    CHECK(sub.recv(pushed) && pushed.state().loading().last_error() == ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET);
    // The Live Set that sounds: a no-op with a typed result.
    m.set_request_id(11);
    m.mutable_jump_to_live_set()->set_id(w.A.id());
    CHECK(c.call(m, rep) && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ALREADY_LOADED && rep.request_id() == 11);

    // A jump while the provider is held: accepted at once, State says loading with the name; CancelLoad is informational.
    gate.close();
    m.set_request_id(12);
    m.mutable_jump_to_live_set()->set_id(w.X.id());
    CHECK(c.call(m, rep) && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ACCEPTED && rep.jump_reply().target_name() == "Song X");
    m.set_request_id(120);  // the same target again: not restarted, a typed no-op
    CHECK(c.call(m, rep) && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ALREADY_LOADING && rep.request_id() == 120);
    bool saw_loading = false;
    for (int k = 0; k < 20 && !saw_loading; ++k) {
      CHECK(sub.recv(pushed));
      saw_loading = pushed.has_state() && pushed.state().loading().loading() && pushed.state().loading().target_name() == "Song X";
    }
    CHECK(saw_loading);
    m.set_request_id(13);
    m.mutable_cancel_load();
    CHECK(c.call(m, rep) && rep.has_cancel_load_reply() && rep.cancel_load_reply().cancelled());
    bool saw_cancel = false;
    for (int k = 0; k < 20 && !saw_cancel; ++k) {
      CHECK(sub.recv(pushed));
      saw_cancel = pushed.has_state() && !pushed.state().loading().loading() && pushed.state().loading().last_error() == ws::v1::ERROR_CODE_LOAD_CANCELLED;
    }
    CHECK(saw_cancel && pushed.state().loading().last_error_informational() && pushed.state().loading().state() == ws::v1::LOADING_STATE_IDLE);
    m.set_request_id(14);
    m.mutable_cancel_load();
    CHECK(c.call(m, rep) && rep.has_cancel_load_reply() && !rep.cancel_load_reply().cancelled());

    // Over the head budget: the engine publishes the typed error and keeps playing.
    gate.open();
    m.set_request_id(15);
    m.mutable_jump_to_live_set()->set_id(w.Big.id());
    CHECK(c.call(m, rep) && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ACCEPTED);
    bool saw_budget = false;
    for (int k = 0; k < 50 && !saw_budget; ++k) {
      CHECK(sub.recv(pushed));
      saw_budget = pushed.has_state() && pushed.state().loading().last_error() == ws::v1::ERROR_CODE_HEAD_BUDGET_EXCEEDED;
    }
    CHECK(saw_budget && pushed.state().loading().state() == ws::v1::LOADING_STATE_FAILED && pushed.state().live_set_id() == w.A.id());

    // A good jump: loading, then ready with a new revision and the new name.
    m.set_request_id(16);
    m.mutable_jump_to_live_set()->set_id(w.B.id());
    CHECK(c.call(m, rep) && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ACCEPTED && rep.revision() == 1);
    bool ready = false;
    for (int k = 0; k < 100 && !ready; ++k) {
      CHECK(sub.recv(pushed));
      ready = pushed.has_state() && pushed.state().loading().state() == ws::v1::LOADING_STATE_READY && pushed.state().live_set_id() == w.B.id();
    }
    CHECK(ready && pushed.state().revision() == 2 && pushed.state().live_set_name() == "Song B" && pushed.state().loading().progress() == 1.0f);
    m.set_request_id(17);
    m.mutable_get_state();
    CHECK(c.call(m, rep) && rep.has_state() && rep.state().loading().state() == ws::v1::LOADING_STATE_READY && rep.state().loading().cancelled_loads() == 1);
    server.stop();
  }
  stop.store(true);
  rt.join();
}

// ws.v1 without a switcher registered: the typed NO_LIVE_SET error for both commands.
void test_ws_v1_without_switcher() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  const std::string path = w.tmp.path + "/n.sock";
  host::ControlServer server(rig.plane, path);
  std::string err;
  CHECK(server.start(err));
  Client c;
  ws::v1::ServerMessage rep;
  CHECK(c.open(path) && c.call(hello_msg(), rep));
  ws::v1::ClientMessage m;
  m.set_request_id(2);
  m.mutable_jump_to_live_set()->set_id(w.B.id());
  CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_LIVE_SET && rep.request_id() == 2);
  m.Clear();
  m.set_request_id(3);
  m.mutable_cancel_load();
  CHECK(c.call(m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_LIVE_SET && rep.request_id() == 3);
  server.stop();
}

// --- hostile: the loader seam, the provider and the worker ---------------------------------------------------------------

void test_provider_cancel_and_sizes() {
  TmpDir tmp;
  const std::string lib = install_library(tmp.path, 9, "long.sfz", 0.3f, 300000);  // several chunks
  host::LibraryResolver resolver(host::make_posix_fs(), tmp.path);
  const host::ResolveResult rr = resolver.resolve(lib, "long.sfz");
  CHECK(rr.ok());
  // The size from the index equals what loading makes resident.
  const std::uint64_t est = host::instrument_head_bytes(*rr.bundle, rr.instrument);
  host::LoadResult full = host::load_instrument_by_index(*rr.bundle, rr.instrument);
  CHECK(full.ok() && est == full.instrument->resident_bytes() && est == 300000u * 4u);
  // Progress adds up to the total; a cancel in the middle returns kCancelled and keeps nothing.
  std::uint64_t sum = 0;
  int calls = 0;
  host::LoadHooks hooks;
  hooks.progress = [&](std::uint64_t b) {
    sum += b;
    ++calls;
    return true;
  };
  host::LoadResult all = host::load_instrument_by_index(*rr.bundle, rr.instrument, &hooks);
  CHECK(all.ok() && sum == est && calls >= 4);
  sum = 0;
  calls = 0;
  hooks.progress = [&](std::uint64_t b) {
    sum += b;
    return ++calls < 2;  // stop after the second chunk
  };
  host::LoadResult stopped = host::load_instrument_by_index(*rr.bundle, rr.instrument, &hooks);
  CHECK(stopped.status == host::LoadStatus::kCancelled && !stopped.instrument && sum < est && calls == 2);
  std::atomic<bool> flag{true};
  host::LoadHooks cancelled;
  cancelled.cancel = &flag;
  host::LoadResult none = host::load_instrument_by_index(*rr.bundle, rr.instrument, &cancelled);
  CHECK(none.status == host::LoadStatus::kCancelled && !none.instrument);
  // The default provider path (load_controlled over load) honours the hooks too.
  struct Plain final : host::HeadProvider {
    host::LoadStatus load(const host::ResolveResult& r, std::shared_ptr<const host::BundleInstrument>& out) override { return ram.load(r, out); }
    host::RamHeadProvider ram;
  } plain;
  std::shared_ptr<const host::BundleInstrument> inst;
  CHECK(plain.estimate_bytes(rr) == est);
  CHECK(plain.load_controlled(rr, inst, cancelled) == host::LoadStatus::kCancelled && !inst);
  CHECK(plain.load_controlled(rr, inst, host::LoadHooks{}) == host::LoadStatus::kOk && inst);
}

// Many jumps in a row (the worker is always busy with a stale target), then the last one wins and nothing leaks.
void test_rapid_jumps_last_wins() {
  World w;
  Rig rig(w.tmp.path);
  CHECK(rig.load_initial(w.A));
  rig.make_switcher(thread_opts(), w.dir);
  rig.run(2);
  auto gate_p = std::make_shared<Gate>();  // shared with the load hook: the worker may still be inside it when the test ends
  Gate& gate = *gate_p;
  gate.close();
  rig.heads.before_load = [gate_p](int, const host::LoadHooks& h) { gate_p->wait(h.cancel); };
  const std::string order[] = {w.B.id(), w.C.id(), w.X.id(), w.Y.id(), w.B.id(), w.C.id(), w.Y.id()};
  for (const std::string& id : order) {
    CHECK(rig.sw->jump_to(id).result == host::SwitchOutcome::Result::kAccepted);
    rig.step();
  }
  CHECK(rig.sw->status().cancelled_loads == 6 && rig.sw->status().target_name == "Song Y");
  gate.open();
  pump(rig);
  CHECK(rig.sw->status().state == host::SwitchState::kReady && rig.loader.loaded_id() == w.Y.id() && rig.plane.revision() == 2);
  rig.run(4);
  rig.plane.reclaim();
  CHECK(rig.heads.alive.load() == 1 && rig.plane.live_generations() == 1);
  rig.note_on(60);
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.04, 1e-3);
}

}  // namespace

int main(int argc, char** argv) {
  const char* only = argc > 1 ? argv[1] : nullptr;  // run the tests whose name contains this text
#define RUN(t) \
  if (!only || std::strstr(#t, only)) t()
  RUN(test_background_load_current_keeps_playing);
  RUN(test_swap_only_when_every_head_is_resident);
  RUN(test_notes_across_the_switch);
  RUN(test_old_generation_freed_only_when_no_voice_references_it);
  RUN(test_cross_layout_note_off);
  RUN(test_engine_change_across_the_switch);
  RUN(test_pedal_down_across_the_switch_holds_new_notes);
  RUN(test_second_jump_cancels_the_first);
  RUN(test_jump_back_and_same_target);
  RUN(test_head_budget_refused_early_and_while_loading);
  RUN(test_typed_failures_keep_playing);
  RUN(test_arbiter_refusal_keeps_playing);
  RUN(test_loading_status_cadence);
  RUN(test_full_ledger_is_retried_then_busy);
  RUN(test_reservation_applied_on_rt_at_the_swap);
  RUN(test_ws_v1_switch_commands);
  RUN(test_ws_v1_without_switcher);
  RUN(test_pedal_released_before_the_switch);
  RUN(test_live_knob_does_not_leak_across_the_switch);
  RUN(test_status_cadence_while_held_and_busy);
  RUN(test_second_jump_during_a_non_cancellable_step);
  RUN(test_stage_failure_reports_parts);
  RUN(test_provider_cancel_and_sizes);
  RUN(test_rapid_jumps_last_wins);
  if (g_fail) {
    std::printf("live_set_switcher_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("live_set_switcher_tests: ok\n");
  return 0;
}

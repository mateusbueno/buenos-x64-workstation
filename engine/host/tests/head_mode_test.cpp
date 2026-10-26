// Instrument heads decoded into locked RAM at load (Story 1.16): head sizing, the loader's head mode (mlock + pre-fault, typed lock failure, release without
// leaks), a note onset that reads head memory only (the bundle is made unreadable after the load and every read is counted), the stream-underrun placeholder past
// the head, and the head budget through the Live Set loader (real head sizes, required vs available, loaded instruments keep playing).
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/head_memory.hpp"
#include "velora/host/head_sizing.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/sample_bundle_loader.hpp"
#include "velora/host/tier_profile.hpp"

namespace fs = std::filesystem;
using namespace velora;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-heads-XXXXXX").string();
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

constexpr std::uint64_t kLongFrames = 48000;
// The arena of the default bundle's heads: 292 + 200 frames of 4 bytes, each 64 byte aligned, page rounded.
const std::size_t kArenaA = host::head_arena_bytes(host::head_align(292 * sizeof(float)) + host::head_align(200 * sizeof(float)));

// "Long.sfz": sample 0 is long (1 s, mono 16 bit 48 kHz) on keys 60..72 (centre 60: the highest pitch ratio is 2), sample 1 is short (200 frames) on keys 73..80.
// Heads live in ONE arena per instrument: each head 64 byte aligned, the arena page rounded (kArenaA below).
// With the default sizing (2 ms + 1 ms) the head of sample 0 is 3 ms * 48000 * 2 + 4 guard = 292 frames, and sample 1 (head 220 >= 200 frames) is fully resident.
std::vector<std::uint8_t> make_bundle(const format::Uuid* lib = nullptr, std::uint64_t long_frames = kLongFrames, bool loop = false, std::uint64_t loop_end = 0) {
  using namespace velora::format;
  BundleWriter w;
  if (lib) w.set_library_id(*lib);
  const std::size_t i = w.add_instrument("Long", "Long.sfz").value();
  std::vector<std::int32_t> a(long_frames);
  for (std::size_t f = 0; f < a.size(); ++f)
    a[f] = static_cast<std::int32_t>(12000.0 * std::sin(0.031 * static_cast<double>(f)) + 6000.0 * std::sin(0.0071 * static_cast<double>(f * f % 9973)));
  (void)w.add_sample(i, {"long", {1, 16, false, 48000}, long_frames, a});
  std::vector<std::int32_t> b(200);
  for (std::size_t f = 0; f < b.size(); ++f) b[f] = static_cast<std::int32_t>(8000.0 * std::sin(0.2 * static_cast<double>(f)));
  (void)w.add_sample(i, {"short", {1, 16, false, 48000}, 200, b});
  Region r0;
  r0.sample = 0; r0.key_lo = 60; r0.key_hi = 72; r0.key_center = 60;
  if (loop) {
    r0.loop_mode = LoopMode::kLoopContinuous; r0.has_loop_points = true; r0.loop_start = 0; r0.loop_end = loop_end != 0 ? loop_end : long_frames - 1;
  }
  Region r1;
  r1.sample = 1; r1.key_lo = 73; r1.key_hi = 80; r1.key_center = 73;
  (void)w.add_region(i, r0);
  (void)w.add_region(i, r1);
  return w.finalize().take();
}

// Counts every read of sample frames; once armed, every read fails: the stand-in for a bundle that became unreadable after the load.
class CountingSource final : public host::InstrumentSource {
 public:
  explicit CountingSource(const format::BundleReader& r) : r_(r) {}
  std::span<const format::InstrumentInfo> instruments() const override { return r_.instruments(); }
  std::span<const format::SampleInfo> samples() const override { return r_.samples(); }
  format::Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const override {
    reads.fetch_add(1);
    if (armed.load()) return format::Error::kIo;
    return r_.read_frames(sample, first, count, out);
  }
  mutable std::atomic<int> reads{0};
  std::atomic<bool> armed{false};

 private:
  const format::BundleReader& r_;
};

host::SampleLoadOptions head_options(bool allow_unlocked = true) {
  host::SampleLoadOptions o;
  o.head.enabled = true;
  o.head.memory.allow_unlocked = allow_unlocked;
  o.head.memory.lock_fn = [](void*, std::size_t) { return true; };  // do not depend on this machine's RLIMIT_MEMLOCK (a real mlock is tried separately)
  o.head.memory.unlock_fn = [](void*, std::size_t) {};
  return o;
}

void render_note(core::SampleEngine& eng, std::uint32_t note, std::uint32_t frames, std::vector<float>& l, std::vector<float>& r) {
  eng.start_voice(0, note, 65535);
  l.assign(frames, 0.0f);
  r.assign(frames, 0.0f);
  for (std::uint32_t at = 0; at < frames; at += 64) eng.render(l.data() + at, r.data() + at, std::min<std::uint32_t>(64, frames - at));
}

// --- head sizing ---------------------------------------------------------------------------------------------------

void test_head_sizing() {
  host::HeadSizing s;
  s.p9999_ns = 2'000'000;
  s.margin_ns = 1'000'000;
  // p99.99 2 ms + margin 1 ms, 48 kHz, pitch ratio 2: 3 ms * 48000 * 2 = 288 frames + the 4 frame guard.
  host::HeadLength h = host::head_length(s, 48000, 2.0, 48000);
  CHECK(h.frames == 292 && h.has_tail);
  // The sample rate counts: 96 kHz doubles it.
  CHECK(host::head_length(s, 96000, 2.0, 96000).frames == 580);
  // A higher pitch needs a longer head, a lower one a shorter one (down to the one-block minimum plus the guard).
  CHECK(host::head_length(s, 48000, 4.0, 48000).frames == 580);
  CHECK(host::head_length(s, 48000, 1.0, 48000).frames == 148);
  host::HeadSizing fast = s;
  fast.p9999_ns = 0;
  fast.margin_ns = 1000;  // 1 us: the latency part is below one block, so the minimum applies
  CHECK(host::head_length(fast, 48000, 1.0, 48000).frames == 64 + 4);
  CHECK(host::head_length(fast, 48000, 2.0, 48000).frames == 128 + 4);
  // A start offset is covered too.
  CHECK(host::head_length(s, 48000, 2.0, 48000, 1000).frames == 1292);
  // Capped at the sample length: a sample shorter than its head is fully resident, no tail.
  h = host::head_length(s, 48000, 2.0, 200);
  CHECK(h.frames == 200 && !h.has_tail);
  h = host::head_length(s, 48000, 2.0, 292);
  CHECK(h.frames == 292 && !h.has_tail);
  h = host::head_length(s, 48000, 2.0, 293);
  CHECK(h.frames == 292 && h.has_tail);
  CHECK(host::head_length(s, 48000, 2.0, 0).frames == 0);
  // The pitch ratio comes from the region: top key, transpose and tune.
  CHECK(std::fabs(host::region_max_pitch_ratio(72, 60, 0, 0.0) - 2.0) < 1e-12);
  CHECK(std::fabs(host::region_max_pitch_ratio(60, 60, 12, 0.0) - 2.0) < 1e-12);
  CHECK(std::fabs(host::region_max_pitch_ratio(60, 60, 0, 1200.0) - 2.0) < 1e-12);
  CHECK(std::fabs(host::region_max_pitch_ratio(60, 72, 0, 0.0) - 0.5) < 1e-12);
  CHECK(host::region_max_pitch_ratio(127, 0, 48, 0.0) == 1024.0);  // clamped to the engine's bound
}

// --- loader head mode ------------------------------------------------------------------------------------------------

void test_head_load_plan_and_memory() {
  const auto bytes = make_bundle();
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  const host::HeadMemoryStats before = host::head_memory_stats();
  {
    host::SampleLoadOptions opt = head_options();
    const auto loaded = host::load_instrument_by_index(rd.value(), 0, nullptr, opt);
    CHECK(loaded.ok() && loaded.instrument);
    const host::BundleInstrument& ins = *loaded.instrument;
    const auto v = ins.instrument();
    CHECK(ins.head_mode() && ins.heads_locked() && ins.failed_samples() == 0 && ins.tail_samples() == 1);
    CHECK(v.samples[0].frames == 292 && v.samples[0].total_frames == kLongFrames && v.samples[0].has_tail);
    CHECK(v.samples[1].frames == 200 && !v.samples[1].has_tail);
    CHECK(ins.resident_bytes() == kArenaA && kArenaA % host::head_page_bytes() == 0);
    // The budget estimate is the real head size, and it equals what the load made resident.
    CHECK(host::instrument_head_bytes(rd.value().instruments(), rd.value().samples(), 0, opt.head) == ins.resident_bytes());
    CHECK(host::instrument_head_bytes(rd.value().instruments(), rd.value().samples(), 0) == (kLongFrames + 200) * sizeof(float));
    // The head is bit-identical to the first frames of the fully resident decode.
    const auto full = host::load_instrument_by_index(rd.value(), 0);
    CHECK(full.ok());
    CHECK(std::memcmp(v.samples[0].left, full.instrument->instrument().samples[0].left, 292 * sizeof(float)) == 0);
    CHECK(std::memcmp(v.samples[1].left, full.instrument->instrument().samples[1].left, 200 * sizeof(float)) == 0);
    // Memory: ONE arena per instrument, mapped, locked once and pre-faulted (page rounded).
    const host::HeadMemoryStats during = host::head_memory_stats();
    CHECK(during.live_buffers == before.live_buffers + 1 && during.locked_buffers == before.locked_buffers + 1 && during.live_bytes == before.live_bytes + kArenaA);
    // Alignment: every head starts on a 64 byte boundary.
    CHECK(reinterpret_cast<std::uintptr_t>(v.samples[0].left) % 64 == 0 && reinterpret_cast<std::uintptr_t>(v.samples[1].left) % 64 == 0);
    // Head mode off (the default): the Story 1.15 load, whole samples, no tail, no head buffers.
    CHECK(!full.instrument->head_mode() && full.instrument->instrument().samples[0].frames == kLongFrames && !full.instrument->instrument().samples[0].has_tail &&
          full.instrument->instrument().samples[0].total_frames == 0);
    CHECK(host::head_memory_stats().live_buffers == during.live_buffers);
  }
  // Released: unlocked and freed, nothing leaks.
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers && after.live_bytes == before.live_bytes && after.locked_buffers == before.locked_buffers);
  CHECK(after.allocations - before.allocations == after.frees - before.frees);
}

void test_head_lock_failure() {
  const auto bytes = make_bundle();
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  const host::HeadMemoryStats before = host::head_memory_stats();
  host::SampleLoadOptions strict = head_options(false);
  int locks = 0;
  strict.head.memory.lock_fn = [&](void*, std::size_t) { ++locks; return false; };  // the arena cannot be locked
  const auto refused = host::load_instrument_by_index(rd.value(), 0, nullptr, strict);
  CHECK(refused.status == host::LoadStatus::kHeadLockFailed && !refused.instrument && locks == 1);  // one mlock per instrument
  CHECK(std::string(host::to_string(refused.status)) == "head_lock_failed");
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers && after.live_bytes == before.live_bytes && after.locked_buffers == before.locked_buffers);  // nothing stays resident

  // The dev option: an unlocked (but pre-faulted) head is kept and recorded.
  host::SampleLoadOptions dev = head_options(true);
  dev.head.memory.lock_fn = [](void*, std::size_t) { return false; };
  const auto unlocked = host::load_instrument_by_index(rd.value(), 0, nullptr, dev);
  CHECK(unlocked.ok() && unlocked.instrument->head_mode() && !unlocked.instrument->heads_locked());

  // The real mlock: either it works (locked) or it fails typed -- never a silent unlocked head.
  host::SampleLoadOptions real;
  real.head.enabled = true;
  const auto r = host::load_instrument_by_index(rd.value(), 0, nullptr, real);
  CHECK((r.ok() && r.instrument->heads_locked()) || r.status == host::LoadStatus::kHeadLockFailed);
  std::printf("  real mlock of the heads: %s\n", r.ok() ? "locked" : "kHeadLockFailed (RLIMIT_MEMLOCK)");
}

void test_head_cancel_leaves_nothing() {
  const auto bytes = make_bundle();
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  const host::HeadMemoryStats before = host::head_memory_stats();
  std::atomic<bool> cancel{false};
  host::LoadHooks hooks;
  hooks.cancel = &cancel;
  int n = 0;
  hooks.progress = [&](std::uint64_t) {
    if (++n == 1) cancel.store(true);  // the first head is resident, then the load is cancelled
    return true;
  };
  CountingSource src(rd.value());
  const auto res = host::load_instrument_by_index(src, 0, &hooks, head_options());
  CHECK(res.status == host::LoadStatus::kCancelled && !res.instrument);
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers && after.live_bytes == before.live_bytes && after.locked_buffers == before.locked_buffers);
}

// --- onset reads head memory only --------------------------------------------------------------------------------------

void test_onset_reads_only_heads() {
  const auto bytes = make_bundle();
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  CountingSource src(rd.value());
  const auto heads = host::load_instrument_by_index(src, 0, nullptr, head_options());
  CHECK(heads.ok());
  const auto full = host::load_instrument_by_index(rd.value(), 0);
  CHECK(full.ok());
  const int reads_at_load = src.reads.load();
  CHECK(reads_at_load > 0);
  // The counting source is NOT connected to the engine: the proof that an onset reads only head memory is that the engine path (SampleBank / SamplePcm) holds no reference
  // to the source or the bundle at all, only pointers into the arena; the counter and the truncated file below demonstrate that nothing breaks when the bundle is gone.
  src.armed.store(true);  // any further read of the bundle fails and is counted

  core::SampleEngine::Config cfg;
  cfg.max_voices = 4;
  core::SampleEngine he(cfg), fe(cfg);
  CHECK(he.set_instrument(*heads.instrument) && fe.set_instrument(*full.instrument));
  std::uint64_t base = 0;
  for (std::uint32_t note : {60u, 66u, 72u}) {  // pitch ratios 1, 1.41, 2: the onset (head end index 290) is rendered for 100 / 128 frames, a clear margin at every note
    const std::uint32_t frames = note == 72 ? 128 : 100;
    std::vector<float> hl, hr, fl, fr;
    render_note(he, note, frames, hl, hr);
    render_note(fe, note, frames, fl, fr);
    float peak = 0.0f;
    for (float x : hl) peak = std::max(peak, std::fabs(x));
    CHECK(peak > 1e-4f);
    CHECK(std::memcmp(hl.data(), fl.data(), frames * sizeof(float)) == 0 && std::memcmp(hr.data(), fr.data(), frames * sizeof(float)) == 0);  // bit identical
    CHECK(he.stream_underrun_events() == base);  // the onset never left the head (the release tail after the note off may: that is the placeholder's business)
    he.stop_voice(0, note);
    fe.stop_voice(0, note);
    std::vector<float> d1(64), d2(64);
    for (int i = 0; i < 400; ++i) {  // let the release finish
      he.render(d1.data(), d2.data(), 64);
      fe.render(d1.data(), d2.data(), 64);
    }
    base = he.stream_underrun_events();  // a release tail may run past the head: not the onset
  }
  CHECK(src.reads.load() == reads_at_load);  // no read of the bundle during any onset

  // The same with a real file made unreadable after the load.
  TmpDir tmp;
  const std::string path = tmp.path + "/bundle.vlb";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  auto frd = format::BundleReader::open_file(path);
  CHECK(frd.ok());
  if (!frd.ok()) return;
  const auto fheads = host::load_instrument_by_index(frd.value(), 0, nullptr, head_options());
  CHECK(fheads.ok());
  // The reader is closed and the file unlinked before the render (BundleReader reads with pread, not mmap, so nothing can fault in later).
  { auto closed = std::move(frd); }
  std::error_code ec;
  fs::remove(path, ec);
  CHECK(!ec && !fs::exists(path));
  core::SampleEngine fe2(cfg), ref(cfg);
  CHECK(fe2.set_instrument(*fheads.instrument) && ref.set_instrument(*full.instrument));
  std::vector<float> a, b, c, d;
  render_note(fe2, 72, 128, a, b);
  render_note(ref, 72, 128, c, d);
  CHECK(std::memcmp(a.data(), c.data(), 128 * sizeof(float)) == 0 && std::memcmp(b.data(), d.data(), 128 * sizeof(float)) == 0);
}

// --- past the head: the stream underrun placeholder --------------------------------------------------------------------

void test_past_the_head_underruns() {
  for (const bool loop : {false, true}) {
    const auto bytes = make_bundle(nullptr, kLongFrames, loop);
    auto rd = format::BundleReader::open_memory(bytes);
    CHECK(rd.ok());
    const auto heads = host::load_instrument_by_index(rd.value(), 0, nullptr, head_options());
    const auto full = host::load_instrument_by_index(rd.value(), 0);
    CHECK(heads.ok() && full.ok());
    core::SampleEngine::Config cfg;
    cfg.max_voices = 4;
    core::SampleEngine he(cfg), fe(cfg);
    CHECK(he.set_instrument(*heads.instrument) && fe.set_instrument(*full.instrument));
    std::vector<float> hl, hr, fl, fr;
    render_note(he, 72, 8000, hl, hr);
    render_note(fe, 72, 8000, fl, fr);
    CHECK(std::memcmp(hl.data(), fl.data(), 128 * sizeof(float)) == 0);  // identical inside the head
    CHECK(he.stream_underrun_events() == 1);                              // counted once for the voice
    CHECK(he.active_voices() == 0);                                       // it faded out and ended
    CHECK(fe.active_voices() == 1 && fe.stream_underrun_events() == 0);   // the fully resident engine keeps playing
    float tail = 0.0f;
    for (std::size_t i = 7000; i < 8000; ++i) tail = std::max(tail, std::fabs(hl[i]));
    CHECK(tail == 0.0f);
    float peak = 0.0f;
    for (std::size_t i = 300; i < 600; ++i) peak = std::max(peak, std::fabs(fl[i]));
    CHECK(peak > 1e-3f);
    render_note(he, 72, 8000, hl, hr);  // every voice that runs out of its head is counted
    CHECK(he.stream_underrun_events() == 2);
    // The short (whole) sample never underruns.
    he.stop_voice(0, 72);
    std::vector<float> sl, sr;
    render_note(he, 77, 2000, sl, sr);
    CHECK(he.stream_underrun_events() == 2);
  }
}

// --- the Live Set loader: readiness, budget, release, diagnostics -----------------------------------------------------

namespace ls = host::liveset;
namespace lsp = ls::lsp;

std::string install_library(const std::string& root, std::uint32_t index) {
  const std::string fp = "velora-heads-test/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  // A different long length per library, so two libraries are two different bundles.
  const auto bytes = make_bundle(&lib, kLongFrames + index);
  auto probe = format::BundleReader::open_memory(bytes);
  CHECK(probe.ok());
  const std::string lib_id = format::uuid_to_string(lib);
  const std::string bundle_id = format::to_hex(probe.value().bundle_id());
  const fs::path dir = fs::path(root) / "lib" / lib_id / bundle_id;
  fs::create_directories(dir);
  std::ofstream f(dir / "bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.close();
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

struct Rig {
  TmpDir tmp;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::LibraryResolver resolver;
  host::RamHeadProvider heads;
  host::LiveSetLoader loader;
  float l[core::kBlockFrames], r[core::kBlockFrames];

  static core::SampleEngine::Config cfg() {
    core::SampleEngine::Config c;
    c.max_voices = host::v1_profile().sample_voice_capacity;
    return c;
  }
  static host::LiveSetLoaderOptions opts(std::uint64_t budget) {
    host::LiveSetLoaderOptions o;
    o.head_budget_bytes = budget;
    return o;
  }
  Rig(host::SampleLoadOptions load, std::uint64_t budget)
      : sample(cfg()),
        organ(host::v1_profile().part_count, host::v1_profile().organ_voice_capacity),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        resolver(host::make_posix_fs(), tmp.path),
        heads(std::move(load)),
        loader(*br, plane, resolver, heads, &sample, opts(budget)) {
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
  }
  float run(std::uint32_t blocks) {
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < blocks; ++i) {
      br->process_block(l, r);
      for (float x : l) peak = std::max(peak, std::fabs(x));
    }
    return peak;
  }
};

lsp::LiveSet live_set(std::uint64_t seed, const std::vector<std::string>& libs) {
  lsp::LiveSet s = ls::make_default_live_set(fixtures::uuid_n(seed), "Heads", 8);
  for (std::size_t p = 0; p < libs.size(); ++p) {
    lsp::Part* part = s.mutable_parts(static_cast<int>(p));
    fixtures::timbre(part, libs[p], "Long.sfz");
    part->set_low_key(0);
    part->set_high_key(127);
    part->set_volume(1.0f);
  }
  return s;
}

void test_live_set_budget_and_readiness() {
  const host::HeadMemoryStats before = host::head_memory_stats();
  const std::uint64_t kHeadsA = kArenaA;  // one instrument's heads: its arena
  {
    Rig rig(head_options(), kHeadsA + kHeadsA / 2);  // room for one arena, far below one fully resident instrument (~193 kB)
    const std::string a = install_library(rig.tmp.path, 1), b = install_library(rig.tmp.path, 2);
    // One instrument: its heads fit although the whole samples would never.
    const host::LiveSetLoadResult one = rig.loader.load(live_set(1, {a}));
    CHECK(one.ok());
    CHECK(one.resident_bytes == kHeadsA);
    CHECK(rig.plane.revision() == 1);
    CHECK(host::head_memory_stats().live_buffers == before.live_buffers + 1);
    rig.run(2);
    // A note onset plays; past the head the placeholder counts the underrun (diagnostics reply) and the voice fades out.
    CHECK(rig.br->submit({core::make_note_on(0, 0, 72, 65535), 0}));
    const float peak = rig.run(4);
    CHECK(peak > 1e-3f);
    rig.run(60);
    CHECK(rig.br->last_values().diagnostics().stream_underruns() == 1);
    CHECK(rig.sample.active_voices() == 0);

    // A second Live Set that needs two instruments: refused with the typed error, required vs available, nothing published.
    const host::LiveSetLoadResult two = rig.loader.load(live_set(2, {a, b}));
    CHECK(two.status == host::LiveSetLoadStatus::kHeadBudgetExceeded);
    CHECK(two.head_bytes == 2 * kHeadsA && two.head_budget == kHeadsA + kHeadsA / 2);  // required vs available
    CHECK(rig.plane.revision() == 1);
    CHECK(host::head_memory_stats().live_buffers == before.live_buffers + 1);  // the refused load kept nothing
    // The loaded instrument keeps playing.
    CHECK(rig.br->submit({core::make_note_on(0, 0, 66, 65535), 0}));
    CHECK(rig.run(3) > 1e-3f);
  }
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers && after.locked_buffers == before.locked_buffers && after.live_bytes == before.live_bytes);  // released: no leak
}

void test_live_set_not_ready_until_all_heads() {
  const host::HeadMemoryStats before = host::head_memory_stats();
  host::SampleLoadOptions o = head_options(false);
  int locks = 0;
  o.head.memory.lock_fn = [&](void*, std::size_t) { return ++locks <= 1; };  // two instruments, one arena each: the second cannot be locked
  {
    Rig rig(o, 0);
    const std::string a = install_library(rig.tmp.path, 1), b = install_library(rig.tmp.path, 2);
    const host::LiveSetLoadResult res = rig.loader.load(live_set(3, {a, b}));
    CHECK(res.status == host::LiveSetLoadStatus::kInstrumentLoadFailed && res.load_status == host::LoadStatus::kHeadLockFailed && res.failed_part == 1);
    CHECK(rig.plane.revision() == 0);  // nothing was published: the Live Set is playable only when every head is resident
    CHECK(host::head_memory_stats().live_buffers == before.live_buffers);  // the heads that did lock were released
    locks = -1000;                      // locking works again
    CHECK(rig.loader.load(live_set(3, {a, b})).ok() && rig.plane.revision() == 1);
  }
  CHECK(host::head_memory_stats().live_buffers == before.live_buffers);
}

void test_head_mode_refuses_protected_and_default_is_off() {
  CHECK(!host::SampleLoadOptions{}.head.enabled);  // the default is the Story 1.15 behaviour
  CHECK(std::string(host::to_string(host::LoadStatus::kHeadProtectedUnsupported)) == "head_protected_unsupported");
  // A protected source with one instrument and one sample: head mode refuses it, typed, before anything is read or allocated; head mode off gets past the check.
  class ProtectedStub final : public host::InstrumentSource {
   public:
    ProtectedStub() {
      format::InstrumentInfo ii;
      ii.first_sample = 0;
      ii.sample_count = 1;
      inst.push_back(ii);
      format::SampleInfo si;
      si.format = {1, 16, false, 48000};
      si.frame_count = 100;
      smp.push_back(si);
    }
    std::span<const format::InstrumentInfo> instruments() const override { return inst; }
    std::span<const format::SampleInfo> samples() const override { return smp; }
    format::Error read_frames(std::size_t, std::uint64_t, std::uint64_t, std::span<std::int32_t>) const override { reads.fetch_add(1); return format::Error::kIo; }
    bool protected_source() const override { return true; }
    std::vector<format::InstrumentInfo> inst;
    std::vector<format::SampleInfo> smp;
    mutable std::atomic<int> reads{0};
  } stub;
  const host::HeadMemoryStats before = host::head_memory_stats();
  const auto refused = host::load_instrument_by_index(stub, 0, nullptr, head_options());
  CHECK(refused.status == host::LoadStatus::kHeadProtectedUnsupported && !refused.instrument && stub.reads.load() == 0);
  CHECK(host::head_memory_stats().allocations == before.allocations);
  // The provider seam: ProtectedHeadProvider hands its options to the RamHeadProvider base, so its own head-mode checks are reachable.
  host::RamHeadProvider ram(head_options());
  CHECK(ram.load_options().head.enabled);
}

// Item: a loop with explicit points that ends near the natural head end extends the head over the loop end plus the guard, so a loop inside the sample never underruns.
void test_loop_near_head_end_is_covered() {
  const auto bytes = make_bundle(nullptr, kLongFrames, true, 290);  // loop 0..290; the natural head is 292 frames (end-of-head check at 290)
  auto rd = format::BundleReader::open_memory(bytes);
  CHECK(rd.ok());
  const auto plan = host::plan_heads(rd.value().instruments(), rd.value().samples(), 0, host::HeadSizing{});
  CHECK(plan.size() == 2 && plan[0].frames == 290 + 1 + 4 && plan[0].has_tail);
  const auto heads = host::load_instrument_by_index(rd.value(), 0, nullptr, head_options());
  CHECK(heads.ok() && heads.instrument->instrument().samples[0].frames == 295);
  core::SampleEngine::Config cfg;
  cfg.max_voices = 4;
  core::SampleEngine he(cfg);
  CHECK(he.set_instrument(*heads.instrument));
  std::vector<float> l, r;
  render_note(he, 72, 8000, l, r);  // a continuous loop: it plays on, wrapping inside the head
  CHECK(he.stream_underrun_events() == 0 && he.active_voices() == 1);
}


}  // namespace

int main() {
  test_head_sizing();
  test_head_load_plan_and_memory();
  test_head_lock_failure();
  test_head_cancel_leaves_nothing();
  test_onset_reads_only_heads();
  test_past_the_head_underruns();
  test_live_set_budget_and_readiness();
  test_live_set_not_ready_until_all_heads();
  test_head_mode_refuses_protected_and_default_is_off();
  test_loop_near_head_end_is_covered();
  if (g_fail) {
    std::printf("head_mode_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("head_mode_test: ok\n");
  return 0;
}

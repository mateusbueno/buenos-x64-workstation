// Streaming from NVMe ahead of the playhead (Story 1.17, Mac portable reader). The stream manager (ring pool, I/O edge threads, epoch / busy handshake), the bundle block source
// (the same decoder as the resident loader) and the sample engine's stream voices:
//   * bit for bit equality of notes rendered through streaming (rings filled by the manager, inline for determinism and with real I/O threads and delays) with the fully resident
//     render, over velocity layers, round robin, loops that reach beyond the head (restream on every wrap), pitch ratios up to the sized maximum, other sample rates, 24 bit and float
//   * a reader that stalls or fails: the audio thread never waits, silence with a short fade for the missing frames, one counted underrun per episode, playback continues and recovers
//   * a ring is never recycled under an in-flight fill (epoch / busy), pool exhaustion, no ring leaks, cancel, fence
//   * 256 voices streaming at once, the parallel voice render, protocol and sizing unit tests.
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "velora/core/sample_engine.hpp"
#include "velora/core/worker_pool.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/head_memory.hpp"
#include "velora/host/sample_bundle_loader.hpp"
#include "velora/host/stream_manager.hpp"
#include "velora/host/thread_priority.hpp"

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

#if defined(__SANITIZE_THREAD__)
#define VELORA_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VELORA_TEST_TSAN 1
#endif
#endif
#ifdef VELORA_TEST_TSAN
constexpr int kSlow = 6;  // the sanitizer slows the I/O threads: the paced tests give them proportionally more time
#else
constexpr int kSlow = 1;
#endif

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-stream-XXXXXX").string();
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

constexpr std::uint32_t kBlock = 64;
// Working head sizing of these tests: 20 ms + 20 ms of latency. At 48 kHz and the highest pitch ratio 2 the head is 3844 frames, the ring 2 chunks + that.
constexpr std::uint64_t kP9999Ns = 20'000'000, kMarginNs = 20'000'000;

// The test bundle "Mix": the samples and regions every scenario draws from.
//   0 mono 16 bit 48 kHz, 30000 frames        keys 60..72 (centre 60) velocity 0..63          (the velocity layer "soft")
//   1 stereo 24 bit 48 kHz, 26000 frames      keys 60..72 (centre 60) velocity 64..127        (the layer "hard")
//   2 mono 16 bit 44.1 kHz, 28000 frames      keys 80..92 (centre 80)                         (a sample rate ratio)
//   3 mono 16 bit 48 kHz, 40000 frames        keys 40..52 (centre 40), loop_continuous 3000..30000   (a loop that reaches past the head, loop start inside the first segment)
//   4 mono 16 bit 48 kHz, 40000 frames        keys 10..22 (centre 10), loop_sustain 20000..30000, release 50 ms  (a loop whose start is far past the head: a resident segment)
//   5 mono 16 bit 48 kHz, 300 frames          keys 96..99                                      (shorter than any head: fully resident, no tail)
//   6 stereo float32 48 kHz, 18000 frames     keys 100..110 (centre 100)
//   round robin: keys 30..35 (centre 30): region A = sample 0, seq 1 of 2; region B = sample 2, seq 2 of 2
std::vector<std::uint8_t> make_bundle() {
  using namespace velora::format;
  BundleWriter w;
  const std::size_t i = w.add_instrument("Mix", "Mix.sfz").value();
  auto wave = [](std::size_t f, double a, double ph) {
    return a * std::sin(0.031 * static_cast<double>(f) + ph) + 0.5 * a * std::sin(0.0071 * static_cast<double>(f * f % 9973) + 2.0 * ph) + 0.25 * a * std::sin(0.4 * static_cast<double>(f));
  };
  auto mono16 = [&](const char* name, std::uint64_t frames, std::uint32_t rate, double ph) {
    std::vector<std::int32_t> d(frames);
    for (std::size_t f = 0; f < d.size(); ++f) d[f] = static_cast<std::int32_t>(wave(f, 9000.0, ph));
    (void)w.add_sample(i, {name, {1, 16, false, rate}, frames, d});
  };
  mono16("soft", 30000, 48000, 0.1);
  {
    std::vector<std::int32_t> d(26000 * 2);
    for (std::size_t f = 0; f < 26000; ++f) {
      d[f * 2] = static_cast<std::int32_t>(wave(f, 2.0e6, 0.4));
      d[f * 2 + 1] = static_cast<std::int32_t>(wave(f + 17, 1.6e6, 0.9));
    }
    (void)w.add_sample(i, {"hard", {2, 24, false, 48000}, 26000, d});
  }
  mono16("cd", 28000, 44100, 0.7);
  mono16("loop_a", 40000, 48000, 1.3);
  mono16("loop_b", 40000, 48000, 2.1);
  mono16("short", 300, 48000, 0.2);
  {
    std::vector<std::int32_t> d(18000 * 2);
    for (std::size_t f = 0; f < 18000; ++f) {
      d[f * 2] = float_to_bits(static_cast<float>(wave(f, 0.4, 0.3)));
      d[f * 2 + 1] = float_to_bits(static_cast<float>(wave(f + 5, 0.3, 1.1)));
    }
    (void)w.add_sample(i, {"float", {2, 32, true, 48000}, 18000, d});
  }
  auto region = [&](std::uint32_t sample, int lo, int hi, int centre) {
    Region r;
    r.sample = sample;
    r.key_lo = static_cast<std::uint8_t>(lo);
    r.key_hi = static_cast<std::uint8_t>(hi);
    r.key_center = static_cast<std::uint8_t>(centre);
    return r;
  };
  {
    Region r = region(0, 60, 72, 60);
    r.vel_lo = 0;
    r.vel_hi = 63;
    (void)w.add_region(i, r);
    r = region(1, 60, 72, 60);
    r.vel_lo = 64;
    r.vel_hi = 127;
    (void)w.add_region(i, r);
  }
  (void)w.add_region(i, region(2, 80, 92, 80));
  {
    Region r = region(3, 40, 52, 40);
    r.loop_mode = LoopMode::kLoopContinuous;
    r.has_loop_points = true;
    r.loop_start = 3000;
    r.loop_end = 30000;
    (void)w.add_region(i, r);
    r = region(4, 10, 22, 10);
    r.loop_mode = LoopMode::kLoopSustain;
    r.has_loop_points = true;
    r.loop_start = 20000;
    r.loop_end = 30000;
    r.ampeg.release = 0.05f;
    (void)w.add_region(i, r);
  }
  (void)w.add_region(i, region(5, 96, 99, 96));
  (void)w.add_region(i, region(6, 100, 110, 100));
  {
    Region r = region(0, 30, 35, 30);
    r.seq_length = 2;
    r.seq_position = 1;
    (void)w.add_region(i, r);
    r = region(2, 30, 35, 30);
    r.seq_length = 2;
    r.seq_position = 2;
    (void)w.add_region(i, r);
  }
  return w.finalize().take();
}

struct Bundle {
  TmpDir tmp;
  std::string path;
  format::Result<format::BundleReader> rd;
  static format::Result<format::BundleReader> open(const std::vector<std::uint8_t>& bytes, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return format::BundleReader::open_file(path);
  }
  explicit Bundle(const std::vector<std::uint8_t>& bytes) : path(tmp.path + "/bundle.vlb"), rd(open(bytes, path)) {}
  const format::BundleReader& reader() const { return rd.value(); }
};

// Forwards to the real reader. `stall`: read_frames blocks until released; `fail`: read_frames fails (EIO); `delay_us`: every read takes this long. Counts the reads.
class GateReader final : public host::InstrumentSource {
 public:
  explicit GateReader(const format::BundleReader& r) : r_(r) {}
  std::span<const format::InstrumentInfo> instruments() const override { return r_.instruments(); }
  std::span<const format::SampleInfo> samples() const override { return r_.samples(); }
  format::Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const override {
    reads.fetch_add(1);
    if (stall.load()) {
      stalled_in.fetch_add(1);
      while (stall.load() && !quit.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));
      stalled_in.fetch_sub(1);
    }
    if (const int d = delay_us.load(); d > 0) std::this_thread::sleep_for(std::chrono::microseconds(d));
    if (fail.load()) return format::Error::kIo;
    return r_.read_frames(sample, first, count, out);
  }
  bool set_no_cache() const override { return r_.set_no_cache(); }
  mutable std::atomic<int> reads{0}, stalled_in{0};
  std::atomic<bool> stall{false}, fail{false}, quit{false};
  std::atomic<int> delay_us{0};

 private:
  const format::BundleReader& r_;
};

host::StreamConfig stream_cfg(std::uint32_t rings, std::uint32_t threads) {
  host::StreamConfig c;
  c.ring_count = rings;
  c.io_threads = threads;
  c.p9999_ns = kP9999Ns;
  c.margin_ns = kMarginNs;
  c.max_pitch_ratio = 2.0;
  c.sample_rate = 48000;
  c.poll_us = 100;
  c.memory.allow_unlocked = true;
  c.memory.lock_fn = [](void*, std::size_t) { return true; };  // do not depend on this machine's RLIMIT_MEMLOCK
  c.memory.unlock_fn = [](void*, std::size_t) {};
  return c;
}

std::unique_ptr<host::StreamManager> make_manager(std::uint32_t rings, std::uint32_t threads) {
  host::StreamManager::Status st;
  auto m = host::StreamManager::create(stream_cfg(rings, threads), &st);
  CHECK(m != nullptr && st == host::StreamManager::Status::kOk);
  return m;
}

host::SampleLoadOptions stream_options(host::StreamManager* mgr, std::shared_ptr<const host::InstrumentSource> tail_reader = nullptr) {
  host::SampleLoadOptions o;
  o.head.enabled = true;
  o.head.sizing.p9999_ns = kP9999Ns;
  o.head.sizing.margin_ns = kMarginNs;
  o.head.memory.allow_unlocked = true;
  o.head.memory.lock_fn = [](void*, std::size_t) { return true; };
  o.head.memory.unlock_fn = [](void*, std::size_t) {};
  o.stream.enabled = true;
  o.stream.fence = mgr;
  o.stream.reader = std::move(tail_reader);
  return o;
}

struct Event {
  std::uint32_t block;
  bool on;
  std::uint32_t channel, note, vel16;
};
struct Pcm {
  std::vector<float> l, r;
};

// Renders `blocks` blocks of kBlock frames. In inline mode (a manager with no threads) the manager is pumped before every block. `pace_us` sleeps after every block (the paced,
// threaded tests); `max_block_ns` receives the longest single render call.
Pcm run(core::SampleEngine& eng, host::StreamManager* mgr, const std::vector<Event>& ev, std::uint32_t blocks, int pace_us = 0, std::uint64_t* max_block_ns = nullptr, long* first_underrun_block = nullptr) {
  const std::uint64_t u0 = eng.stream_underrun_events();
  Pcm p;
  p.l.assign(static_cast<std::size_t>(blocks) * kBlock, 0.0f);
  p.r.assign(p.l.size(), 0.0f);
  const bool inline_mode = mgr != nullptr && mgr->io_thread_count() == 0;
  std::uint64_t worst = 0;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    if (inline_mode) mgr->pump();
    for (const Event& e : ev) {
      if (e.block != b) continue;
      if (e.on) eng.start_voice(e.channel, e.note, e.vel16);
      else eng.stop_voice(e.channel, e.note);
    }
    const auto t0 = std::chrono::steady_clock::now();
    eng.render(p.l.data() + static_cast<std::size_t>(b) * kBlock, p.r.data() + static_cast<std::size_t>(b) * kBlock, kBlock);
    worst = std::max<std::uint64_t>(worst, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    if (first_underrun_block != nullptr && *first_underrun_block < 0 && eng.stream_underrun_events() > u0) *first_underrun_block = static_cast<long>(b);
    if (pace_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
  }
  if (max_block_ns) *max_block_ns = worst;
  return p;
}

// First differing frame, or -1.
long first_diff(const Pcm& a, const Pcm& b) {
  if (a.l.size() != b.l.size()) return 0;
  for (std::size_t i = 0; i < a.l.size(); ++i)
    if (std::memcmp(&a.l[i], &b.l[i], sizeof(float)) != 0 || std::memcmp(&a.r[i], &b.r[i], sizeof(float)) != 0) return static_cast<long>(i);
  return -1;
}
// A starved attempt proves nothing about timing but must still be right up to the block in which the first underrun happened.
void check_prefix(const Pcm& a, const Pcm& b, long first_underrun_block) {
  const long d = first_diff(a, b);
  CHECK(d < 0 || (first_underrun_block >= 0 && d >= first_underrun_block * static_cast<long>(kBlock)));
}
// Retry accounting of the timing-dependent tests: they repeat an attempt that starved (a machine too loaded for the I/O threads), but they must not live on retries: outside the
// sanitizer builds (which slow the I/O threads several times over) a clean run is required within the first two attempts.
void report_attempts(const char* what, int attempts, bool clean) {
  if (attempts > 1) std::printf("  %s: clean after %d attempt(s)\n", what, attempts);
  CHECK(clean);
#ifndef VELORA_TEST_TSAN
  CHECK(attempts <= 2);
#endif
}
double energy(const Pcm& p, std::size_t from, std::size_t to) {
  double e = 0.0;
  for (std::size_t i = from; i < std::min(to, p.l.size()); ++i) e += static_cast<double>(p.l[i]) * p.l[i] + static_cast<double>(p.r[i]) * p.r[i];
  return e;
}

struct Scenario {
  const char* name;
  std::vector<Event> events;
  std::uint32_t blocks;
};
Event on(std::uint32_t b, std::uint32_t note, std::uint32_t vel16, std::uint32_t ch = 0) { return {b, true, ch, note, vel16}; }
Event off(std::uint32_t b, std::uint32_t note, std::uint32_t ch = 0) { return {b, false, ch, note, 0}; }

std::vector<Scenario> scenarios() {
  std::vector<Scenario> s;
  s.push_back({"mono 16 bit, ratio 1", {on(0, 60, 12000), off(300, 60)}, 560});
  s.push_back({"mono 16 bit, ratio 2 (the sized maximum)", {on(0, 72, 12000), off(150, 72)}, 330});
  s.push_back({"velocity layer: stereo 24 bit, ratio 1.26", {on(0, 64, 60000), off(250, 64)}, 440});
  s.push_back({"44.1 kHz sample rate ratio", {on(0, 85, 40000), off(200, 85)}, 440});
  s.push_back({"float32 stereo", {on(0, 105, 50000), off(150, 105)}, 330});
  s.push_back({"loop_continuous past the head, ratio 1 (restream on every wrap)", {on(0, 40, 30000), off(1500, 40)}, 1700});
  s.push_back({"loop_continuous past the head, ratio 2", {on(0, 52, 30000), off(900, 52)}, 1000});
  s.push_back({"loop_sustain with a far loop start, released into the tail", {on(0, 10, 30000), off(1100, 10)}, 1400});
  s.push_back({"round robin over two samples", {on(0, 30, 20000), off(120, 30), on(130, 30, 20000), off(260, 30), on(270, 31, 40000), off(380, 31), on(390, 30, 30000), off(500, 30)}, 700});
  s.push_back({"fully resident short sample (no stream)", {on(0, 97, 30000), off(40, 97)}, 120});
  s.push_back({"polyphony: layers, loops, rates", {on(0, 60, 12000), on(0, 64, 60000, 1), on(20, 72, 40000), on(40, 85, 40000), on(60, 40, 30000), on(60, 10, 30000, 2), on(80, 105, 50000), off(400, 60), off(500, 64, 1), off(600, 40), off(700, 10, 2), off(300, 85), off(200, 72), off(250, 105)}, 1400});
  return s;
}

core::SampleEngine::Config engine_cfg(std::uint32_t voices = 64, core::SampleInterpolation q = core::SampleInterpolation::kHermite3) {
  core::SampleEngine::Config c;
  c.max_voices = voices;
  c.interpolation = q;
  return c;
}

// --- unit tests: sizing, creation, the ring protocol ---------------------------------------------------------------------------

void test_sizing_and_creation() {
  host::StreamConfig c;
  c.ring_count = 4;
  // lookahead = ceil(3 ms * 48000 * 2) + 4 = 292; 2 * 4096 + 292 = 8484 is below the three chunk minimum: ring = 12288.
  CHECK(host::stream_ring_frames(c) == 12288);
  CHECK(host::stream_pool_bytes(c) == host::head_arena_bytes(4u * 12288u * 2u * sizeof(float)));
  c.p9999_ns = 50'000'000;
  c.margin_ns = 50'000'000;  // lookahead = ceil(100 ms * 48000 * 2) + ceil(64 * 2) + 4 = 9732; ring = round_up(2 * 4096 + 9732, 64) = 17984
  CHECK(host::stream_ring_frames(c) == 17984);
  c.p9999_ns = 2'000'000;
  c.margin_ns = 1'000'000;
  c.chunk_frames = 1024;
  CHECK(host::stream_ring_frames(c) == 3072);  // 2 * 1024 + 292 = 2340 -> the three chunk minimum
  c.ring_frames = 500;  // an explicit size is honoured but never below three chunks
  CHECK(host::stream_ring_frames(c) == 3072);
  c.ring_frames = 5000;
  CHECK(host::stream_ring_frames(c) == 5056);

  host::StreamManager::Status st = host::StreamManager::Status::kOk;
  host::StreamConfig bad = stream_cfg(0, 0);
  CHECK(host::StreamManager::create(bad, &st) == nullptr && st == host::StreamManager::Status::kBadConfig);
  host::StreamConfig tight = stream_cfg(8, 0);
  tight.budget_bytes = 1000;  // the stream buffer budget
  CHECK(host::StreamManager::create(tight, &st) == nullptr && st == host::StreamManager::Status::kBudgetExceeded);
  host::StreamConfig locked = stream_cfg(2, 0);
  locked.memory.allow_unlocked = false;
  locked.memory.lock_fn = [](void*, std::size_t) { return false; };
  CHECK(host::StreamManager::create(locked, &st) == nullptr && st == host::StreamManager::Status::kLockFailed);
  auto m = host::StreamManager::create(stream_cfg(8, 0), &st);
  CHECK(m && st == host::StreamManager::Status::kOk);
  if (m) {
    CHECK(m->ring_count() == 8 && m->rings_free() == 8 && m->rings_active() == 0 && m->rings_draining() == 0);
    CHECK(m->buffer_bytes() == host::stream_pool_bytes(stream_cfg(8, 0)));
  }
}

// A reader whose frame f of sample s is `base + s * 100000 + f` (exactly representable below 2^24): the ring contents can be checked frame by frame.
class PatternSource final : public ports::BlockSource {
 public:
  explicit PatternSource(float base) : base_(base) {}
  ports::BlockReadStatus read(std::uint32_t sample, std::uint64_t start, std::uint32_t frames, float* l, float* r) const noexcept override {
    reads.fetch_add(1);
    if (order_counter != nullptr) {
      int expect = -1;
      const int o = order_counter->fetch_add(1);
      first_order.compare_exchange_strong(expect, o);  // the global order of this source's FIRST read
    }
    if (const int d = delay_us.load(); d > 0) std::this_thread::sleep_for(std::chrono::microseconds(d));
    if (block.load()) {
      entered.store(true);
      while (block.load() && !quit.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
      l[i] = base_ + static_cast<float>(sample) * 100000.0f + static_cast<float>(start + i);
      if (r) r[i] = -l[i];
    }
    return ports::BlockReadStatus::kOk;
  }
  float value(std::uint32_t sample, std::uint64_t f) const { return base_ + static_cast<float>(sample) * 100000.0f + static_cast<float>(f); }
  mutable std::atomic<int> reads{0};
  mutable std::atomic<bool> block{false}, entered{false}, quit{false};
  mutable std::atomic<int> first_order{-1};
  mutable std::atomic<int> delay_us{0};
  std::atomic<int>* order_counter = nullptr;

 private:
  float base_;
};

bool ring_holds(const ports::StreamRing& g, const PatternSource& src, std::uint32_t sample, std::uint64_t from, std::uint64_t to, bool stereo) {
  for (std::uint64_t f = from; f < to; ++f) {
    const std::size_t slot = static_cast<std::size_t>(f % g.capacity);
    if (g.left[slot] != src.value(sample, f)) return false;
    if (stereo && g.right[slot] != -src.value(sample, f)) return false;
  }
  return true;
}

void test_ring_protocol_inline() {
  auto m = make_manager(2, 0);
  if (!m) return;
  PatternSource src(0.0f);
  ports::StreamOpen o;
  o.source = &src;
  o.sample = 3;
  o.total_frames = 40000;
  o.channels = 2;
  o.start = 1000;
  ports::StreamRing* g = m->open(o);
  CHECK(g != nullptr && g->state.load() == ports::StreamRing::kActive && g->seek_seq.load() == 1 && g->capacity == m->ring_frames());
  if (!g) return;
  CHECK((g->avail.load() >> 48) == 0);  // nothing available before the producer acknowledges segment 1
  m->pump();
  std::uint64_t a = g->avail.load();
  CHECK((a >> 48) == 1);
  const std::uint64_t hi = a & ports::kStreamFrameMask;
  CHECK(hi > 1000 && hi <= 1000 + g->capacity);  // never more than one ring ahead of consumed_lo (= the start)
  CHECK(ring_holds(*g, src, 3, 1000, hi, true));
  // The consumer advances: more is read, never past consumed_lo + capacity, and everything in [lo, hi) is the pattern.
  for (int i = 0; i < 20; ++i) {
    const std::uint64_t cur = g->avail.load() & ports::kStreamFrameMask;
    g->consumed_lo.store(cur > 300 ? cur - 300 : 0, std::memory_order_release);
    m->pump();
    a = g->avail.load();
    const std::uint64_t h2 = a & ports::kStreamFrameMask;
    CHECK(h2 >= cur && h2 <= g->consumed_lo.load() + g->capacity && h2 <= 40000);
    CHECK(ring_holds(*g, src, 3, g->consumed_lo.load(), h2, true));
  }
  // A seek (loop wrap): the producer restarts at the new start, in the new segment.
  g->consumed_lo.store(5000, std::memory_order_relaxed);
  g->seek_start.store(5000, std::memory_order_relaxed);
  g->seek_seq.store(2, std::memory_order_release);
  m->pump();
  a = g->avail.load();
  CHECK((a >> 48) == 2 && (a & ports::kStreamFrameMask) > 5000);
  CHECK(ring_holds(*g, src, 3, 5000, a & ports::kStreamFrameMask, true));
  // A consumer that jumped far ahead (it was starved): the producer skips what is no longer needed.
  g->consumed_lo.store(30000, std::memory_order_release);
  m->pump();
  a = g->avail.load();
  CHECK((a & ports::kStreamFrameMask) > 30000 && ring_holds(*g, src, 3, 30000, a & ports::kStreamFrameMask, true));
  // The end of the sample: filled_end stops at total_frames.
  g->consumed_lo.store(39000, std::memory_order_release);
  m->pump();
  CHECK((g->avail.load() & ports::kStreamFrameMask) == 40000);
  m->close(g);
  CHECK(g->state.load() == ports::StreamRing::kDraining && m->rings_active() == 0 && m->rings_draining() == 1);
  // Invalid requests are refused, counted, and take no ring.
  ports::StreamOpen bad = o;
  bad.start = 40000;
  CHECK(m->open(bad) == nullptr);
  bad = o;
  bad.source = nullptr;
  CHECK(m->open(bad) == nullptr);
  CHECK(m->stats().open_failures == 2);
}

// ABA, inline: a request for a ring that was recycled before the I/O thread got to it is stale: its source is never read, the new life gets its own data.
void test_recycled_ring_is_not_filled_for_the_old_life() {
  auto m = make_manager(1, 0);
  if (!m) return;
  PatternSource a(0.0f), b(7.0f);
  ports::StreamOpen oa;
  oa.source = &a;
  oa.sample = 0;
  oa.total_frames = 20000;
  oa.channels = 1;
  oa.start = 100;
  ports::StreamOpen ob = oa;
  ob.source = &b;
  ob.start = 500;
  ports::StreamRing* g1 = m->open(oa);
  CHECK(g1 != nullptr);
  m->close(g1);                    // the voice ended before any I/O happened
  ports::StreamRing* g2 = m->open(ob);  // the same ring (the pool has one), a new life
  CHECK(g2 == g1 && g2->epoch.load() == 2);
  m->pump();
  CHECK(a.reads.load() == 0);  // the stale request was dropped by the epoch check
  CHECK(b.reads.load() > 0);
  const std::uint64_t hi = g2->avail.load() & ports::kStreamFrameMask;
  CHECK((g2->avail.load() >> 48) == 1 && hi > 500 && ring_holds(*g2, b, 0, 500, hi, false));
  CHECK(m->stats().discarded >= 1);
  m->close(g2);
}

// ABA, threaded: a fill is in flight (the reader is blocked inside it) when the voice ends. The ring is NOT given to a new stream until the read has finished, and the late
// data of the old life never shows in the new one.
void test_late_fill_never_lands_in_a_recycled_ring() {
  auto m = make_manager(1, 1);
  if (!m) return;
  PatternSource slow(0.0f), fresh(5.0f);
  slow.block.store(true);
  ports::StreamOpen o;
  o.source = &slow;
  o.sample = 1;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 200;
  ports::StreamRing* g = m->open(o);
  CHECK(g != nullptr);
  for (int i = 0; i < 4000 && !slow.entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(slow.entered.load());  // the I/O thread is inside the read of the old life
  m->close(g);                 // the voice ended mid-fill
  ports::StreamOpen n = o;
  n.source = &fresh;
  n.start = 700;
  CHECK(m->open(n) == nullptr);  // draining and busy: not claimable, and the audio thread did not wait for it
  CHECK(m->stats().pool_exhausted == 1);
  slow.block.store(false);  // the old read finishes now (its data is discarded by whoever owns the ring next)
  ports::StreamRing* g2 = nullptr;
  for (int i = 0; i < 4000 && g2 == nullptr; ++i) {
    g2 = m->open(n);
    if (!g2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(g2 == g);  // claimable once the I/O thread is out of it
  if (g2) {
    for (int i = 0; i < 4000 && (g2->avail.load() >> 48) != 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::uint64_t a = g2->avail.load();
    CHECK((a >> 48) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    a = g2->avail.load();
    const std::uint64_t hi = a & ports::kStreamFrameMask;
    CHECK(hi > 700 && ring_holds(*g2, fresh, 1, 700, hi, false));  // only the new life's data
    m->close(g2);
  }
}

// fence(): returns only once no I/O thread is inside a read of a ring that is no longer active.
void test_fence_waits_for_in_flight_reads() {
  auto m = make_manager(1, 1);
  if (!m) return;
  PatternSource slow(0.0f);
  slow.block.store(true);
  ports::StreamOpen o;
  o.source = &slow;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 100;
  ports::StreamRing* g = m->open(o);
  CHECK(g != nullptr);
  for (int i = 0; i < 4000 && !slow.entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  m->fence();  // the ring is still active (a voice holds it): not waited for
  m->close(g);
  std::atomic<bool> fenced{false};
  std::thread t([&] {
    m->fence();
    fenced.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(!fenced.load());  // blocked behind the in-flight read
  slow.block.store(false);
  t.join();
  CHECK(fenced.load());
}

// --- bit-identity with the resident render -------------------------------------------------------------------------------------

void test_bit_identity_inline() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  CHECK(resident.ok());
  for (const core::SampleInterpolation q : {core::SampleInterpolation::kHermite3, core::SampleInterpolation::kLinear}) {
    auto mgr = make_manager(64, 0);
    if (!mgr) return;
    const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
    CHECK(streamed.ok());
    if (!streamed.ok()) return;
    CHECK(streamed.instrument->streaming() && streamed.instrument->head_mode() && streamed.instrument->loop_segments() >= 1 && streamed.instrument->tail_samples() == 6);
    const auto pcm = streamed.instrument->instrument();
    CHECK(pcm.samples[0].frames == 3844 && pcm.samples[0].has_tail && pcm.samples[0].tail_source != nullptr);
    CHECK(pcm.samples[5].frames == 300 && !pcm.samples[5].has_tail && pcm.samples[5].tail_source == nullptr);
    for (const Scenario& sc : scenarios()) {
      if (q == core::SampleInterpolation::kLinear && std::string(sc.name).find("loop") == std::string::npos && std::string(sc.name).find("polyphony") == std::string::npos) continue;
      core::SampleEngine ref(engine_cfg(64, q)), eng(engine_cfg(64, q));
      CHECK(ref.set_instrument(*resident.instrument) && eng.set_instrument(*streamed.instrument));
      eng.set_stream_service(mgr.get());
      const host::StreamStats before = mgr->stats();
      const Pcm a = run(ref, nullptr, sc.events, sc.blocks);
      const Pcm b = run(eng, mgr.get(), sc.events, sc.blocks);
      const long d = first_diff(a, b);
      if (d >= 0) std::printf("  scenario '%s' (%s): first difference at frame %ld\n", sc.name, q == core::SampleInterpolation::kLinear ? "linear" : "hermite", d);
      CHECK(d < 0);
      CHECK(energy(a, 0, a.l.size()) > 1.0);
      CHECK(eng.stream_underrun_events() == 0 && eng.stream_open_failures() == 0);
      CHECK(ref.active_voices() == eng.active_voices());
      const host::StreamStats after = mgr->stats();
      const bool streams = std::string(sc.name).find("fully resident") == std::string::npos;
      CHECK(streams ? after.reads > before.reads : after.opens == before.opens);  // the comparison is not vacuous: tails were really read through the manager
      CHECK(after.read_errors == 0);
      eng.clear_instrument();
    }
    CHECK(mgr->rings_active() == 0);  // no leak: every ring is back (or draining) once the voices ended
    const host::StreamStats st = mgr->stats();
    CHECK(st.opens == st.closes);
  }
}

// Real I/O threads and real delays: the same notes, paced, with every tail read taking time. Bit-identical and no underrun.
void test_bit_identity_threaded_with_delays() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  auto mgr = make_manager(64, 2);
  auto gate = std::make_shared<GateReader>(bundle.reader());
  gate->delay_us.store(300);
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get(), gate));
  CHECK(resident.ok() && streamed.ok());
  if (!resident.ok() || !streamed.ok() || !mgr) return;
  const std::vector<std::string> pick = {"mono 16 bit, ratio 2 (the sized maximum)", "loop_continuous past the head, ratio 2", "loop_sustain with a far loop start, released into the tail", "polyphony: layers, loops, rates"};
  for (const Scenario& sc : scenarios()) {
    if (std::find(pick.begin(), pick.end(), std::string(sc.name)) == pick.end()) continue;
    core::SampleEngine ref(engine_cfg());
    CHECK(ref.set_instrument(*resident.instrument));
    const Pcm a = run(ref, nullptr, sc.events, sc.blocks);
    // Timing is real here: on a machine so loaded that an I/O thread was not scheduled for longer than the head lasts, a voice legitimately starves (an underrun, which the
    // other tests check). Such an attempt proves nothing about the data path and is repeated slower; EVERY attempt without an underrun must be bit-identical.
    bool clean = false;
    int attempts = 0;
    for (int attempt = 0; attempt < 4 && !clean; ++attempt) {
      ++attempts;
      core::SampleEngine eng(engine_cfg());
      CHECK(eng.set_instrument(*streamed.instrument));
      eng.set_stream_service(mgr.get());
      long fu = -1;
      const Pcm b = run(eng, mgr.get(), sc.events, sc.blocks, (100 + 300 * attempt) * kSlow, nullptr, &fu);
      if (eng.stream_underrun_events() == 0) {
        clean = true;
        const long d = first_diff(a, b);
        if (d >= 0) std::printf("  threaded scenario '%s': first difference at frame %ld\n", sc.name, d);
        CHECK(d < 0);
      } else {
        check_prefix(a, b, fu);
      }
      eng.clear_instrument();
    }
    report_attempts(sc.name, attempts, clean);
  }
  CHECK(gate->reads.load() > 20);
  CHECK(mgr->stats().read_errors == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(mgr->rings_active() == 0);
}

// --- a reader that stalls: the audio thread never waits ---------------------------------------------------------------------------

void test_stalled_reader_never_blocks_the_audio_thread() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  auto mgr = make_manager(8, 1);
  auto gate = std::make_shared<GateReader>(bundle.reader());
  gate->stall.store(true);  // every tail read blocks from the start
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get(), gate));
  CHECK(resident.ok() && streamed.ok());
  if (!resident.ok() || !streamed.ok() || !mgr) {
    gate->quit.store(true);
    return;
  }
  const std::uint64_t head = streamed.instrument->instrument().samples[0].frames;  // 3844
  core::SampleEngine ref(engine_cfg()), eng(engine_cfg());
  CHECK(ref.set_instrument(*resident.instrument) && eng.set_instrument(*streamed.instrument));
  eng.set_stream_service(mgr.get());
  const std::vector<Event> ev = {on(0, 60, 12000)};
  const std::uint32_t blocks = 200;  // 12800 frames: well past the head end at ratio 1
  std::uint64_t worst_ns = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const Pcm a = run(ref, nullptr, ev, blocks);
  const Pcm b = run(eng, mgr.get(), ev, blocks, 0, &worst_ns);
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  // The reader was blocked the whole time (a read is in flight, nothing completed), and the render still finished, quickly.
  for (int i = 0; i < 2000 && gate->stalled_in.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // the I/O thread may only now have picked the request up
  CHECK(gate->stalled_in.load() == 1 && mgr->stats().reads == 0);
  CHECK(total_ms < 2000);
  CHECK(worst_ns < 50'000'000ull * kSlow);  // no single block waited for the reader (a stalled read would never return)
  // Up to the head end the output is the resident one bit for bit: the onset never needed the SSD.
  const std::size_t exact = head - 4;
  bool same = true;
  for (std::size_t i = 0; i < exact; ++i) same = same && std::memcmp(&a.l[i], &b.l[i], sizeof(float)) == 0 && std::memcmp(&a.r[i], &b.r[i], sizeof(float)) == 0;
  CHECK(same);
  // Then silence with a short fade: after kStreamFadeFrames the output is exactly 0, and the note is still sounding (playback continues).
  const std::size_t silent_from = head - 3 + core::kStreamFadeFrames + 2;
  bool silent = true;
  for (std::size_t i = silent_from; i < b.l.size(); ++i) silent = silent && b.l[i] == 0.0f && b.r[i] == 0.0f;
  CHECK(silent);
  CHECK(energy(b, head - 200, head - 4) > 1e-3);  // it did sound before
  CHECK(eng.active_voices() == 1);
  // One underrun for the episode, not one per frame.
  CHECK(eng.stream_underrun_events() == 1);
  // The reader recovers: the voice resumes (the position went on), still one episode.
  gate->stall.store(false);
  const Pcm c = run(eng, mgr.get(), {}, 100, 100 * kSlow);
  CHECK(energy(c, 0, c.l.size()) > 1e-3);
  CHECK(eng.stream_underrun_events() == 1 && eng.active_voices() == 1);
  eng.stop_voice(0, 60);
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 200; ++i) eng.render(l.data(), r.data(), 64);
  CHECK(eng.active_voices() == 0);
  gate->quit.store(true);
  for (int i = 0; i < 2000 && mgr->rings_active() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(mgr->rings_active() == 0);
}

// A reader that fails (EIO): a counted underrun, a typed error in the host log, no crash, and the stream recovers when the reader does.
void test_reader_errors_are_counted_not_fatal() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  auto mgr = make_manager(8, 1);
  auto gate = std::make_shared<GateReader>(bundle.reader());
  gate->fail.store(true);
  std::atomic<int> handled{0};
  mgr->set_error_handler([&](const host::StreamError& e) {
    if (e.status == ports::BlockReadStatus::kIoError) handled.fetch_add(1);
  });
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get(), gate));
  CHECK(streamed.ok());
  if (!streamed.ok() || !mgr) return;
  core::SampleEngine eng(engine_cfg());
  CHECK(eng.set_instrument(*streamed.instrument));
  eng.set_stream_service(mgr.get());
  const Pcm a = run(eng, mgr.get(), {on(0, 60, 12000)}, 160, 200 * kSlow);  // past the head end (3844 frames = 60 blocks)
  CHECK(eng.stream_underrun_events() == 1 && eng.active_voices() == 1);
  CHECK(mgr->stats().read_errors >= 1 && handled.load() >= 1);
  const auto errs = mgr->recent_errors();
  CHECK(!errs.empty() && errs[0].status == ports::BlockReadStatus::kIoError && errs[0].sample == 0);
  CHECK(energy(a, 0, 3000) > 1e-3);
  // Recovery: the reader works again, the stream retries (with a back-off) and the voice resumes.
  gate->fail.store(false);
  double e = 0.0;
  for (int round = 0; round < 60 && e < 1e-3; ++round) {
    const Pcm c = run(eng, mgr.get(), {}, 20, 500 * kSlow);
    e = energy(c, 0, c.l.size());
  }
  CHECK(e > 1e-3);
  CHECK(eng.stream_underrun_events() == 1);
  // A file that is gone behaves like any other read error: simulated by a reader that is closed (kClosed / kIoError), never a crash.
  eng.clear_instrument();
}

// A bundle file that is cut short after the load (the SSD lost it, a truncated copy): the tail reads hit the end of the file -- a typed short read, a counted underrun, no crash.
void test_truncated_file_is_a_short_read() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  auto mgr = make_manager(8, 1);
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
  CHECK(streamed.ok());
  if (!streamed.ok() || !mgr) return;
  std::error_code ec;
  fs::resize_file(bundle.path, 64, ec);  // everything after the header is gone
  CHECK(!ec);
  core::SampleEngine eng(engine_cfg());
  CHECK(eng.set_instrument(*streamed.instrument));
  eng.set_stream_service(mgr.get());
  const Pcm a = run(eng, mgr.get(), {on(0, 60, 12000)}, 160, 200 * kSlow);
  CHECK(eng.stream_underrun_events() == 1 && eng.active_voices() == 1);  // the head played, then the missing tail starved the voice (it did not crash or wait)
  const auto errs = mgr->recent_errors();
  CHECK(!errs.empty() && errs[0].status == ports::BlockReadStatus::kShortRead);
  CHECK(mgr->stats().read_errors >= 1 && mgr->stats().reads == 0);
  CHECK(energy(a, 0, 3000) > 1e-3);
  eng.clear_instrument();
}

// --- pool exhaustion, lifecycle ----------------------------------------------------------------------------------------------------

void test_pool_exhausted_and_recycled_ring_is_clean() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  auto mgr = make_manager(1, 1);  // ONE ring
  auto gate = std::make_shared<GateReader>(bundle.reader());
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get(), gate));
  CHECK(resident.ok() && streamed.ok());
  if (!resident.ok() || !streamed.ok() || !mgr) return;
  core::SampleEngine eng(engine_cfg()), ref(engine_cfg());
  CHECK(eng.set_instrument(*streamed.instrument) && ref.set_instrument(*resident.instrument));
  eng.set_stream_service(mgr.get());
  // Note 1 (sample 0) takes the only ring; its first read stalls. The note ends before any data arrived.
  gate->stall.store(true);
  std::vector<float> l(64), r(64);
  eng.start_voice(0, 60, 12000);
  for (int i = 0; i < 4000 && gate->stalled_in.load() == 0; ++i) {
    eng.render(l.data(), r.data(), 64);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  CHECK(gate->stalled_in.load() == 1);
  eng.stop_voice(0, 60);
  for (int i = 0; i < 200; ++i) eng.render(l.data(), r.data(), 64);
  CHECK(eng.active_voices() == 0 && mgr->rings_active() == 0 && mgr->rings_draining() == 1);  // given back, but the in-flight read still holds it
  // Note 2 finds no free ring: counted, and the voice plays like the Story 1.16 placeholder (head, then a fade and one counted underrun). The audio thread did not wait.
  const std::vector<Event> ev2 = {on(0, 64, 60000)};  // sample 1: a different sample (stereo 24 bit)
  const auto t0 = std::chrono::steady_clock::now();
  const Pcm p2 = run(eng, nullptr, ev2, 120);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  CHECK(eng.stream_open_failures() == 1 && mgr->stats().pool_exhausted >= 1);
  CHECK(eng.stream_underrun_events() == 1);
  eng.stop_voice(0, 64);
  for (int i = 0; i < 200; ++i) eng.render(l.data(), r.data(), 64);
  CHECK(eng.active_voices() == 0);
  // The stalled read finishes: its data goes nowhere. The ring is claimable again, and a new note on ANOTHER sample is bit-identical to the resident render
  // (the late fill of note 1 left nothing in the ring).
  gate->stall.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const std::vector<Event> ev3 = {on(0, 64, 60000), off(250, 64)};
  const Pcm a = run(ref, nullptr, ev3, 440);
  bool clean = false;
  int attempts = 0;
  for (int attempt = 0; attempt < 4 && !clean; ++attempt) {  // see test_bit_identity_threaded_with_delays: an attempt that starved on a loaded machine is repeated
    ++attempts;
    const std::uint64_t u0 = eng.stream_underrun_events();
    long fu = -1;
    const Pcm b = run(eng, mgr.get(), ev3, 440, (100 + 300 * attempt) * kSlow, nullptr, &fu);
    if (eng.stream_underrun_events() == u0) {
      clean = true;
      CHECK(first_diff(a, b) < 0);
    } else {
      check_prefix(a, b, fu);
    }
    for (int i = 0; i < 100; ++i) eng.render(l.data(), r.data(), 64);
  }
  report_attempts("pool exhausted: note 3", attempts, clean);
  CHECK(eng.stream_open_failures() == 1);  // note 3 got its ring (every attempt)
  (void)p2;
  gate->quit.store(true);
}

void test_lifecycle_no_leaks() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const host::HeadMemoryStats before = host::head_memory_stats();
  auto mgr = make_manager(16, 0);
  if (!mgr) return;
  {
    const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
    CHECK(streamed.ok());
    if (!streamed.ok()) return;
    core::SampleEngine eng(engine_cfg(32));
    CHECK(eng.set_instrument(*streamed.instrument));
    eng.set_stream_service(mgr.get());
    std::vector<float> l(64), r(64);
    // Many notes on and off. Odd rounds: served and ended by a note off. Even rounds: cancelled before the I/O side ever served them (no pump until the voices are gone): the
    // queued requests are stale and must be discarded, and the rings recycled.
    for (int round = 0; round < 6; ++round) {
      for (std::uint32_t n : {60u, 64u, 72u, 85u, 40u, 10u, 105u}) eng.start_voice(round & 3, n, 30000 + n * 100);
      const bool cancelled = (round & 1) == 0;
      for (int i = 0; i < (cancelled ? 2 : 20); ++i) {
        if (!cancelled) mgr->pump();
        eng.render(l.data(), r.data(), 64);
      }
      for (std::uint32_t n : {60u, 64u, 72u, 85u, 40u, 10u, 105u}) eng.stop_voice(round & 3, n);
      for (int i = 0; i < 300; ++i) {  // long enough for the 50 ms release tails of the loop regions
        if (!cancelled) mgr->pump();
        eng.render(l.data(), r.data(), 64);
      }
      for (int i = 0; i < 100; ++i) {
        mgr->pump();
        eng.render(l.data(), r.data(), 64);
      }
    }
    CHECK(mgr->stats().discarded >= 21);  // 3 cancelled rounds x 7 stale requests
    for (int i = 0; i < 10; ++i) eng.render(l.data(), r.data(), 64);
    CHECK(eng.active_voices() == 0);
    CHECK(mgr->rings_active() == 0);
    // A voice still sounding when the instrument is cleared (the generation goes): its ring is given back.
    eng.start_voice(0, 60, 12000);
    eng.start_voice(0, 64, 60000);
    mgr->pump();
    CHECK(mgr->rings_active() == 2);
    eng.clear_instrument();
    CHECK(mgr->rings_active() == 0);
    // And a steal fade: the arbiter-driven fade ends the voice, which gives the ring back.
    CHECK(eng.set_instrument(*streamed.instrument));
    eng.start_voice(0, 60, 12000);
    mgr->pump();
    core::VoiceInfo vi;
    std::uint32_t slot = 0;
    for (; slot < eng.voice_capacity(); ++slot)
      if (eng.voice_info(slot, vi)) break;
    CHECK(eng.begin_steal_fade(slot, 64));
    for (int i = 0; i < 4; ++i) eng.render(l.data(), r.data(), 64);
    CHECK(eng.active_voices() == 0 && mgr->rings_active() == 0);
    const host::StreamStats st = mgr->stats();
    CHECK(st.opens == st.closes && st.opens > 40);
  }  // the instrument is destroyed here: its block source fences the manager
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers + 1);  // the manager's own ring arena only
  mgr.reset();
  CHECK(host::head_memory_stats().live_buffers == before.live_buffers);
}

void test_cancel_during_stream_mode_load_leaves_nothing() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  auto mgr = make_manager(4, 0);
  const host::HeadMemoryStats before = host::head_memory_stats();
  std::atomic<bool> cancel{false};
  host::LoadHooks hooks;
  hooks.cancel = &cancel;
  int n = 0;
  hooks.progress = [&](std::uint64_t) {
    if (++n == 5) cancel.store(true);  // after four heads and a loop segment
    return true;
  };
  const auto res = host::load_instrument_by_index(bundle.reader(), 0, &hooks, stream_options(mgr.get()));
  CHECK(res.status == host::LoadStatus::kCancelled && !res.instrument);
  const host::HeadMemoryStats after = host::head_memory_stats();
  CHECK(after.live_buffers == before.live_buffers && after.live_bytes == before.live_bytes);
}

// --- loader: options, typed refusals, accounting ------------------------------------------------------------------------------

void test_loader_modes_and_accounting() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  auto mgr = make_manager(4, 0);
  // Stream mode without head mode: refused, typed (no heads-off streaming).
  host::SampleLoadOptions nohead = stream_options(mgr.get());
  nohead.head.enabled = false;
  const auto r1 = host::load_instrument_by_index(bundle.reader(), 0, nullptr, nohead);
  CHECK(r1.status == host::LoadStatus::kStreamRequiresHeads && !r1.instrument && std::string(host::to_string(r1.status)) == "stream_requires_heads");
  // A protected source: refused, typed.
  class ProtectedView final : public host::InstrumentSource {
   public:
    explicit ProtectedView(const format::BundleReader& r) : r_(r) {}
    std::span<const format::InstrumentInfo> instruments() const override { return r_.instruments(); }
    std::span<const format::SampleInfo> samples() const override { return r_.samples(); }
    format::Error read_frames(std::size_t s, std::uint64_t f, std::uint64_t c, std::span<std::int32_t> o) const override { return r_.read_frames(s, f, c, o); }
    bool protected_source() const override { return true; }

   private:
    const format::BundleReader& r_;
  } prot(bundle.reader());
  const auto r2 = host::load_instrument_by_index(prot, 0, nullptr, stream_options(mgr.get()));
  CHECK(r2.status == host::LoadStatus::kStreamProtectedUnsupported && std::string(host::to_string(r2.status)) == "stream_protected_unsupported");
  // Default off: a head-mode load without stream options has no block source, no tail reader, and the engine keeps the Story 1.16 placeholder even with a manager bound.
  host::SampleLoadOptions heads_only = stream_options(mgr.get());
  heads_only.stream.enabled = false;
  const auto r3 = host::load_instrument_by_index(bundle.reader(), 0, nullptr, heads_only);
  CHECK(r3.ok() && !r3.instrument->streaming() && r3.instrument->block_source() == nullptr && r3.instrument->instrument().samples[0].tail_source == nullptr);
  if (r3.ok()) {
    core::SampleEngine eng(engine_cfg());
    CHECK(eng.set_instrument(*r3.instrument));
    eng.set_stream_service(mgr.get());
    std::vector<float> l(64), r(64);
    eng.start_voice(0, 60, 12000);
    for (int i = 0; i < 100; ++i) eng.render(l.data(), r.data(), 64);
    CHECK(mgr->stats().opens == 0 && eng.stream_underrun_events() == 1 && eng.stream_open_failures() == 0);  // the 1.16 behaviour, unchanged
  }
  // Accounting: the head budget estimate (with the loop segments) equals what the stream-mode load made resident.
  const auto r4 = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
  CHECK(r4.ok());
  if (r4.ok()) {
    host::HeadOptions h = stream_options(mgr.get()).head;
    h.loop_segments = true;
    CHECK(host::instrument_head_bytes(bundle.reader().instruments(), bundle.reader().samples(), 0, h) == r4.instrument->resident_bytes());
    h.loop_segments = false;
    CHECK(host::instrument_head_bytes(bundle.reader().instruments(), bundle.reader().samples(), 0, h) < r4.instrument->resident_bytes());
    // Segments: the continuous loop 3000..30000 (start inside the first 7k frames) and the sustain loop 20000..30000 each need one.
    const auto plan = host::plan_heads(bundle.reader().instruments(), bundle.reader().samples(), 0, h.sizing);
    const auto segs = host::plan_loop_segments(bundle.reader().instruments(), bundle.reader().samples(), 0, h.sizing, plan);
    CHECK(segs.size() == 7 && segs[3].size() == 1 && segs[4].size() == 1 && segs[0].empty() && segs[5].empty());
    if (segs[3].size() == 1 && segs[4].size() == 1) {
      CHECK(segs[3][0].start == 3844 && segs[3][0].start + segs[3][0].frames == 2999 + 3844);  // 2999 + latency(2 * 40 ms) + guard, from the head end
      CHECK(segs[4][0].start == 19999 && segs[4][0].start + segs[4][0].frames == 19999 + 3844);
    }
    // The page cache: macOS bypasses it for the tail reads on a real file; a memory reader cannot.
#if defined(__APPLE__)
    CHECK(r4.instrument->stream_no_cache());
#else
    CHECK(!r4.instrument->stream_no_cache());
#endif
    auto mem = format::BundleReader::open_memory(bytes);
    const auto r5 = host::load_instrument_by_index(mem.value(), 0, nullptr, stream_options(mgr.get()));
    CHECK(r5.ok() && !r5.instrument->stream_no_cache());
  }
}

// The block source decodes exactly as the resident loader does, for every sample format of the bundle, at odd offsets and lengths.
void test_block_source_equals_resident_decode() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  auto mgr = make_manager(2, 0);
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
  CHECK(resident.ok() && streamed.ok());
  if (!resident.ok() || !streamed.ok()) return;
  const ports::BlockSource* bs = streamed.instrument->block_source();
  CHECK(bs != nullptr);
  const auto rs = resident.instrument->instrument().samples;
  std::vector<float> l(9000), r(9000);
  for (std::uint32_t s = 0; s < rs.size(); ++s) {
    const std::uint64_t total = rs[s].frames;
    for (const std::pair<std::uint64_t, std::uint32_t> q : {std::pair<std::uint64_t, std::uint32_t>{0, 100}, {4090, 20}, {4096, 4096}, {1234, 7000}}) {
      if (q.first + q.second > total) continue;
      CHECK(bs->read(s, q.first, q.second, l.data(), rs[s].channels == 2 ? r.data() : nullptr) == ports::BlockReadStatus::kOk);
      CHECK(std::memcmp(l.data(), rs[s].left + q.first, q.second * sizeof(float)) == 0);
      if (rs[s].channels == 2) CHECK(std::memcmp(r.data(), rs[s].right + q.first, q.second * sizeof(float)) == 0);
    }
    CHECK(bs->read(s, total - 3, 10, l.data(), r.data()) == ports::BlockReadStatus::kOutOfRange);
  }
  CHECK(bs->read(99, 0, 10, l.data(), r.data()) == ports::BlockReadStatus::kOutOfRange);
  // Errors are typed: a reader that fails with EIO / truncation.
  auto gate = std::make_shared<GateReader>(bundle.reader());
  const auto g = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get(), gate));
  CHECK(g.ok());
  if (g.ok()) {
    gate->fail.store(true);
    CHECK(g.instrument->block_source()->read(0, 0, 100, l.data(), nullptr) == ports::BlockReadStatus::kIoError);
  }
}

// --- many voices, parallel render ------------------------------------------------------------------------------------------------

void test_256_voices_streaming() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  auto mgr = make_manager(300, 2);
  const auto streamed = host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(mgr.get()));
  CHECK(resident.ok() && streamed.ok());
  if (!resident.ok() || !streamed.ok() || !mgr) return;
  // 256 voices: 16 channels x 16 notes on the 26000 / 30000 frame samples (keys 60..72 and 80..92): every one streams its tail.
  std::vector<Event> ev;
  std::uint32_t n = 0;
  for (std::uint32_t ch = 0; ch < 16; ++ch)
    for (std::uint32_t k = 0; k < 16; ++k, ++n) {
      const std::uint32_t note = k < 8 ? 60 + k : 80 + (k - 8);
      ev.push_back(on(0, note, (n & 1) ? 60000 : 12000, ch));
    }
  const std::uint32_t blocks = 520;  // 33280 frames: past every sample's end
  core::SampleEngine ref(engine_cfg(300));
  CHECK(ref.set_instrument(*resident.instrument));
  const Pcm a = run(ref, nullptr, ev, blocks);
  CHECK(ref.stream_open_failures() == 0);
  // 256 voices at once is the load that most needs the I/O threads to keep ahead; on a machine loaded enough to starve them an attempt is repeated slower (see
  // test_bit_identity_threaded_with_delays). A clean attempt must be bit-identical, have no underrun, and have opened exactly 256 streams.
  bool clean = false;
  int attempts = 0;
  for (int attempt = 0; attempt < 3 && !clean; ++attempt) {
    ++attempts;
    long first_u = -1;
    core::SampleEngine eng(engine_cfg(300));
    CHECK(eng.set_instrument(*streamed.instrument));
    eng.set_stream_service(mgr.get());
    const host::StreamStats before = mgr->stats();
    std::uint32_t peak = 0;
    Pcm b;
    b.l.assign(static_cast<std::size_t>(blocks) * kBlock, 0.0f);
    b.r.assign(b.l.size(), 0.0f);
    for (std::uint32_t blk = 0; blk < blocks; ++blk) {
      if (blk == 0)
        for (const Event& e : ev) eng.start_voice(e.channel, e.note, e.vel16);
      eng.render(b.l.data() + static_cast<std::size_t>(blk) * kBlock, b.r.data() + static_cast<std::size_t>(blk) * kBlock, kBlock);
      peak = std::max(peak, eng.active_voices());
      if (first_u < 0 && eng.stream_underrun_events() > 0) first_u = static_cast<long>(blk);
      std::this_thread::sleep_for(std::chrono::microseconds((4000 + 4000 * attempt) * kSlow));  // 256 voices consume ~2.7 M frames/s at this pace: well inside what two I/O threads read (the build may be unoptimised)
    }
    CHECK(peak == 256 && eng.stream_open_failures() == 0);
    if (eng.stream_underrun_events() == 0) {
      clean = true;
      CHECK(first_diff(a, b) < 0);  // a fast reader: zero underruns, and the 256 voices sum bit for bit like the resident render
      const host::StreamStats st = mgr->stats();
      CHECK(st.opens - before.opens == 256 && st.read_errors == 0 && st.pool_exhausted == 0);
      std::printf("  256 voices: %llu tail reads, %llu frames, buffers %llu bytes\n", static_cast<unsigned long long>(st.reads - before.reads),
                  static_cast<unsigned long long>(st.frames_read - before.frames_read), static_cast<unsigned long long>(mgr->buffer_bytes()));
    } else {
      check_prefix(a, b, first_u);
    }
    eng.clear_instrument();
  }
  report_attempts("256 voices", attempts, clean);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(mgr->rings_active() == 0);
}

void test_parallel_render_identical() {
  const auto bytes = make_bundle();
  Bundle bundle(bytes);
  CHECK(bundle.rd.ok());
  if (!bundle.rd.ok()) return;
  const auto resident = host::load_instrument_by_index(bundle.reader(), 0);
  const auto streamed_for = [&](host::StreamManager* m) { return host::load_instrument_by_index(bundle.reader(), 0, nullptr, stream_options(m)); };
  CHECK(resident.ok());
  const std::vector<Event> ev = {on(0, 60, 12000), on(0, 64, 60000, 1), on(0, 72, 40000, 2), on(2, 85, 40000, 3), on(2, 40, 30000, 4), on(4, 10, 30000, 5), on(4, 105, 50000, 6), on(6, 61, 20000, 7),
                                 off(500, 60), off(700, 40, 4), off(900, 10, 5)};
  const std::uint32_t blocks = 1300;
  for (const std::uint32_t threads : {1u, 3u}) {
    auto mgr = make_manager(32, 0);  // inline: deterministic; the voices render on 1 or 3 pool threads and push their requests from the serial phase / render workers
    const auto streamed = streamed_for(mgr.get());
    CHECK(streamed.ok());
    if (!streamed.ok() || !mgr) return;
    core::WorkerPool pool_ref, pool_eng;
    pool_ref.start(threads);
    pool_eng.start(threads);
    core::SampleEngine ref(engine_cfg()), eng(engine_cfg());
    CHECK(ref.set_instrument(*resident.instrument) && eng.set_instrument(*streamed.instrument));
    ref.set_parallel_voice_pool(&pool_ref, kBlock);
    eng.set_parallel_voice_pool(&pool_eng, kBlock);
    eng.set_stream_service(mgr.get());
    const Pcm a = run(ref, nullptr, ev, blocks);
    const Pcm b = run(eng, mgr.get(), ev, blocks);
    const long d = first_diff(a, b);
    if (d >= 0) std::printf("  parallel render with %u thread(s): first difference at frame %ld\n", threads, d);
    CHECK(d < 0);
    CHECK(eng.stream_underrun_events() == 0 && eng.parallel_voice_render() && mgr->stats().reads > 10);
    CHECK(mgr->rings_active() == 0);
    eng.clear_instrument();
  }
  // Threaded I/O under the parallel render (the TSan job's target): voices on the pool threads release rings and seek while I/O threads fill.
  auto mgr = make_manager(32, 2);
  const auto streamed = streamed_for(mgr.get());
  CHECK(streamed.ok());
  if (!streamed.ok() || !mgr) return;
  core::WorkerPool pool_ref, pool_eng;
  pool_ref.start(3);
  pool_eng.start(3);
  core::SampleEngine ref(engine_cfg());
  CHECK(ref.set_instrument(*resident.instrument));
  ref.set_parallel_voice_pool(&pool_ref, kBlock);
  const Pcm a = run(ref, nullptr, ev, blocks);
  bool clean = false;
  int attempts = 0;
  for (int attempt = 0; attempt < 4 && !clean; ++attempt) {  // an attempt that starved on a loaded machine is repeated slower (see test_bit_identity_threaded_with_delays)
    ++attempts;
    core::SampleEngine eng(engine_cfg());
    CHECK(eng.set_instrument(*streamed.instrument));
    eng.set_parallel_voice_pool(&pool_eng, kBlock);
    eng.set_stream_service(mgr.get());
    long fu = -1;
    const Pcm b = run(eng, mgr.get(), ev, blocks, (100 + 300 * attempt) * kSlow, nullptr, &fu);
    if (eng.stream_underrun_events() == 0) {
      clean = true;
      CHECK(first_diff(a, b) < 0);
    } else {
      check_prefix(a, b, fu);
    }
    eng.clear_instrument();
  }
  report_attempts("parallel render with threaded I/O", attempts, clean);
}

// --- scheduling of the I/O threads ----------------------------------------------------------------------------------------------

bool wait_avail(const ports::StreamRing& g, std::uint64_t beyond, int ms = 4000) {
  for (int i = 0; i < ms; ++i) {
    const std::uint64_t a = g.avail.load();
    if ((a >> 48) == 1 && (a & ports::kStreamFrameMask) > beyond) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// Announcements for streams that ended before they were served pile up behind a stalled read (more than any per-thread job list could hold): when the reader recovers every LIVE
// stream is served, none was dropped with the stale ones.
void test_stale_announcements_do_not_starve_live_streams() {
  auto m = make_manager(4, 1);
  if (!m) return;
  PatternSource stuck(0.0f), live1(1.0f), live2(2.0f), churn(3.0f);
  stuck.block.store(true);
  ports::StreamOpen o;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 100;
  o.source = &stuck;
  ports::StreamRing* a = m->open(o);
  for (int i = 0; i < 4000 && !stuck.entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(a && stuck.entered.load());
  o.source = &churn;
  for (int i = 0; i < 20; ++i) {  // notes retriggered while the reader is stalled: opened and ended, never served
    ports::StreamRing* g = m->open(o);
    CHECK(g != nullptr);
    if (g) m->close(g);
  }
  o.source = &live1;
  ports::StreamRing* l1 = m->open(o);
  o.source = &live2;
  ports::StreamRing* l2 = m->open(o);
  CHECK(l1 && l2);
  stuck.block.store(false);
  if (l1 && l2) {
    CHECK(wait_avail(*l1, 100));
    CHECK(wait_avail(*l2, 100));
    const std::uint64_t h1 = l1->avail.load() & ports::kStreamFrameMask;
    CHECK(ring_holds(*l1, live1, 0, 100, h1, false));
  }
  CHECK(m->stats().discarded >= 20);
  if (a) m->close(a);
  if (l1) m->close(l1);
  if (l2) m->close(l2);
}

// A chord / layer storm while a slow read is in progress: the new streams wait for at most the read that is running, then get their first chunk before the stream that was already
// served gets its next one.
void test_new_streams_are_adopted_promptly() {
  auto m = make_manager(8, 1);
  if (!m) return;
  std::atomic<int> order{0};
  PatternSource a(0.0f), b(1.0f), c(2.0f), d(3.0f), e(4.0f);
  for (PatternSource* p : {&a, &b, &c, &d, &e}) {
    p->order_counter = &order;
    p->delay_us.store(3000);  // a slow SSD: 3 ms per read
  }
  ports::StreamOpen o;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 100;
  o.source = &a;
  ports::StreamRing* ga = m->open(o);
  for (int i = 0; i < 4000 && a.reads.load() == 0 && a.first_order.load() < 0; ++i) std::this_thread::sleep_for(std::chrono::microseconds(200));
  CHECK(ga != nullptr && a.first_order.load() == 0);
  ports::StreamRing* gs[4] = {};
  PatternSource* src[4] = {&b, &c, &d, &e};
  for (int i = 0; i < 4; ++i) {
    o.source = src[i];
    gs[i] = m->open(o);
    CHECK(gs[i] != nullptr);
  }
  for (int i = 0; i < 4; ++i)
    if (gs[i]) CHECK(wait_avail(*gs[i], 100));
  for (int i = 0; i < 4; ++i) {
    // Before the already served stream got a second read: its first chunk is among the next few reads (the one in progress, then the new streams, most urgent first).
    CHECK(src[i]->first_order.load() >= 1 && src[i]->first_order.load() <= 5);
  }
  if (ga) m->close(ga);
  for (auto* g : gs)
    if (g) m->close(g);
}

// One read that never returns freezes one ring, not the pool: with two I/O threads the other streams are served while a read is stuck.
void test_a_stalled_read_does_not_freeze_other_rings() {
  auto m = make_manager(8, 2);
  if (!m) return;
  PatternSource stuck(0.0f);
  stuck.block.store(true);
  ports::StreamOpen o;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 100;
  o.source = &stuck;
  ports::StreamRing* a = m->open(o);
  for (int i = 0; i < 4000 && !stuck.entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(a && stuck.entered.load());
  PatternSource fresh[5] = {PatternSource(1.0f), PatternSource(2.0f), PatternSource(3.0f), PatternSource(4.0f), PatternSource(5.0f)};
  ports::StreamRing* gs[5] = {};
  for (int i = 0; i < 5; ++i) {
    o.source = &fresh[i];
    gs[i] = m->open(o);
    CHECK(gs[i] != nullptr);  // (ring indices 1..5: with a ring-to-thread mapping, half of them would be behind the stuck thread)
  }
  for (int i = 0; i < 5; ++i)
    if (gs[i]) CHECK(wait_avail(*gs[i], 100, 2000));
  stuck.block.store(false);
  if (a) m->close(a);
  for (auto* g : gs)
    if (g) m->close(g);
}

// fence() gives up on a read that never returns: false, counted, and the stuck ring is not reused.
void test_fence_times_out() {
  host::StreamConfig c = stream_cfg(2, 1);
  c.fence_timeout_ms = 100;
  auto m = host::StreamManager::create(c);
  CHECK(m != nullptr);
  if (!m) return;
  PatternSource stuck(0.0f);
  stuck.block.store(true);
  ports::StreamOpen o;
  o.source = &stuck;
  o.total_frames = 30000;
  o.channels = 1;
  o.start = 100;
  ports::StreamRing* g = m->open(o);
  for (int i = 0; i < 4000 && !stuck.entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  m->close(g);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!m->fence());
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
  CHECK(m->stats().fence_timeouts == 1);
  ports::StreamOpen o2 = o;
  PatternSource other(1.0f);
  o2.source = &other;
  ports::StreamRing* g2 = m->open(o2);
  CHECK(g2 != nullptr && g2 != g);  // the stuck ring is never handed to another stream
  if (g2) m->close(g2);
  stuck.block.store(false);
  CHECK(m->fence());
}

// The first read of a segment is never a sliver: a stream starting 252 frames before a chunk boundary gets more than half a chunk, so one read's latency is all the head must cover.
void test_first_read_is_not_a_sliver() {
  auto m = make_manager(2, 0);
  if (!m) return;
  PatternSource src(0.0f);
  ports::StreamOpen o;
  o.source = &src;
  o.total_frames = 40000;
  o.channels = 1;
  o.start = 3844;  // 252 frames before the 4096 boundary
  ports::StreamRing* g = m->open(o);
  m->pump(1);  // ONE read
  const std::uint64_t hi = g->avail.load() & ports::kStreamFrameMask;
  CHECK(src.reads.load() == 1 && hi - 3844 >= 4096 / 2 && hi % 4096 == 0);
  // The same after a seek.
  g->consumed_lo.store(10000, std::memory_order_relaxed);
  g->seek_start.store(10000, std::memory_order_relaxed);
  g->seek_seq.store(2, std::memory_order_release);
  const int before = src.reads.load();
  m->pump(1);
  const std::uint64_t hi2 = g->avail.load() & ports::kStreamFrameMask;
  CHECK(src.reads.load() == before + 1 && hi2 - 10000 >= 2048 && (g->avail.load() >> 48) == 2);
  m->close(g);
}

// --- the consumer's margin on a small ring; a short starve ends without a step ---------------------------------------------------------

// A hand-made instrument: one mono sample of smooth data (resident, or a head plus a tail read from the same array).
class PatternInstrument final : public ports::SampleSource, public ports::BlockSource {
 public:
  PatternInstrument(std::uint64_t total, std::uint64_t head, bool streamed) {
    data_.resize(total);
    for (std::size_t f = 0; f < total; ++f) data_[f] = static_cast<float>(0.5 * std::sin(0.02 * static_cast<double>(f)) + 0.2 * std::sin(0.013 * static_cast<double>(f) + 1.0));
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.left = data_.data();
    if (streamed) {
      p.frames = head;
      p.total_frames = total;
      p.has_tail = true;
      p.tail_source = this;
      p.tail_sample = 0;
    } else {
      p.frames = total;
    }
    pcm_.push_back(p);
    ports::SampleRegionDesc d;
    d.sample_index = 0;
    d.key_lo = 40;
    d.key_hi = 80;
    d.key_center = 60;
    regions_.push_back(d);
  }
  ports::InstrumentView instrument() const noexcept override { return {regions_, pcm_}; }
  ports::BlockReadStatus read(std::uint32_t, std::uint64_t start, std::uint32_t frames, float* l, float*) const noexcept override {
    if (start + frames > data_.size()) return ports::BlockReadStatus::kOutOfRange;
    std::memcpy(l, data_.data() + start, frames * sizeof(float));
    return ports::BlockReadStatus::kOk;
  }

 private:
  std::vector<float> data_;
  std::vector<ports::SamplePcm> pcm_;
  std::vector<ports::SampleRegionDesc> regions_;
};

host::StreamConfig tiny_ring_cfg() {
  host::StreamConfig c = stream_cfg(2, 0);
  c.chunk_frames = 128;
  c.ring_frames = 384;  // the three chunk minimum: the ring is as small as it can be, so every frame the producer may write is written
  return c;
}

// The consumer publishes the lowest frame it still reads (once per block); the producer fills up to the last slot that allows. A producer that runs while the block renders (a
// helper thread pumps the inline manager as fast as it can, on the smallest ring there is) overwrites the first tap of a block as soon as the published margin
// `i0 - 2 - ceil(ratio)` is far too small (a margin that is wrong by tens of frames makes this fail at once), and TSan flags the race. It is a STRESS check: a margin wrong by one
// frame only loses a race window of nanoseconds, which is what test_published_margin_covers_the_taps (next) pins deterministically. An attempt in which the producer fell behind
// (an underrun) proves nothing and is repeated.
void test_consumer_margin_on_a_minimal_ring() {
  PatternInstrument resident(9000, 0, false), streamed(9000, 300, true);
  for (const std::uint32_t note : {60u, 67u, 72u}) {  // ratio 1, ~1.5, 2
    bool clean = false;
    for (int attempt = 0; attempt < 8 && !clean; ++attempt) {
      host::StreamConfig cfg = tiny_ring_cfg();
      cfg.ring_count = 2;
      auto m = host::StreamManager::create(cfg);
      CHECK(m != nullptr);
      if (!m) return;
      core::SampleEngine ref(engine_cfg(4)), eng(engine_cfg(4));
      CHECK(ref.set_instrument(resident) && eng.set_instrument(streamed));
      eng.set_stream_service(m.get());
      const std::uint32_t frames = 8000 / (note == 60 ? 1 : note == 67 ? 2 : 2);
      std::vector<float> ra(frames), rb(frames), ea(frames), eb(frames);
      ref.start_voice(0, note, 65535);
      eng.start_voice(0, note, 65535);
      for (std::uint32_t f = 0; f < frames; f += 8) ref.render(ra.data() + f, rb.data() + f, 8);
      std::atomic<bool> stop{false};
      std::thread pumper([&] {
        while (!stop.load()) m->pump();
      });
      for (std::uint32_t f = 0; f < frames; f += 8) eng.render(ea.data() + f, eb.data() + f, 8);
      stop.store(true);
      pumper.join();
      if (eng.stream_underrun_events() == 0) {
        clean = true;
        CHECK(std::memcmp(ra.data(), ea.data(), frames * sizeof(float)) == 0 && std::memcmp(rb.data(), eb.data(), frames * sizeof(float)) == 0);
        CHECK(m->stats().reads > 10);
      }
      eng.clear_instrument();
    }
    CHECK(clean);
  }
}

// Deterministic form of the margin check: after every block the ring's published consumed_lo must not exceed the lowest frame that block read (its left interpolation tap, the
// block's first position minus one) -- otherwise a producer running during the block may overwrite a frame the voice still reads. Positions are exact here (ratio 1, 2).
// (Mutation check, done when this test was written: publishing `i0` instead of `i0 - 2 - ceil(ratio)` fails it at the first block past the head.)
class RecordingService final : public ports::StreamService {
 public:
  explicit RecordingService(ports::StreamService& inner) : inner_(inner) {}
  ports::StreamRing* open(const ports::StreamOpen& r) noexcept override {
    ring = inner_.open(r);
    return ring;
  }
  void close(ports::StreamRing* g) noexcept override { inner_.close(g); }
  ports::StreamRing* ring = nullptr;

 private:
  ports::StreamService& inner_;
};

void test_published_margin_covers_the_taps() {
  PatternInstrument streamed(9000, 300, true);
  for (const std::uint32_t note : {60u, 72u}) {  // ratio 1 and 2
    auto m = host::StreamManager::create(tiny_ring_cfg());
    CHECK(m != nullptr);
    if (!m) return;
    RecordingService rec(*m);
    core::SampleEngine eng(engine_cfg(4));
    CHECK(eng.set_instrument(streamed));
    eng.set_stream_service(&rec);
    eng.start_voice(0, note, 65535);
    const std::uint32_t ratio = note == 60 ? 1 : 2;
    std::vector<float> l(8), r(8);
    int checked = 0;
    for (std::uint32_t blk = 0; blk < 500 && eng.active_voices() == 1; ++blk) {
      m->pump();
      const std::uint64_t pos0 = static_cast<std::uint64_t>(blk) * 8u * ratio;  // the position at the start of this block
      eng.render(l.data(), r.data(), 8);
      if (pos0 >= 301) {
        CHECK(rec.ring != nullptr && rec.ring->consumed_lo.load() <= pos0 - 1);
        ++checked;
      }
    }
    CHECK(checked > 100);
    eng.clear_instrument();
  }
}

// A short starve (a few frames) ends without a step: the output crossfades from the held sample to the real one instead of jumping.
void test_short_starve_has_no_step() {
  PatternInstrument resident(9000, 0, false), streamed(9000, 300, true);
  auto m = host::StreamManager::create(tiny_ring_cfg());
  CHECK(m != nullptr);
  if (!m) return;
  core::SampleEngine ref(engine_cfg(4)), eng(engine_cfg(4));
  CHECK(ref.set_instrument(resident) && eng.set_instrument(streamed));
  eng.set_stream_service(m.get());
  ref.start_voice(0, 60, 65535);
  eng.start_voice(0, 60, 65535);
  std::vector<float> ra(2000), rb(2000), ea(2000), eb(2000);
  // 16 frame blocks; the stream is not served until frame 304: the head (300 frames) ends at 298, so the voice is starved for ~6 frames, well under the 64 frame fade.
  for (int blk = 0; blk < 125; ++blk) {
    if (blk >= 19) m->pump();
    ref.render(ra.data() + blk * 16, rb.data() + blk * 16, 16);
    eng.render(ea.data() + blk * 16, eb.data() + blk * 16, 16);
  }
  CHECK(eng.stream_underrun_events() == 1);
  float step_ref = 0.0f, step_eng = 0.0f;
  for (int i = 260; i < 400; ++i) {
    step_ref = std::max(step_ref, std::fabs(ra[i] - ra[i - 1]));
    step_eng = std::max(step_eng, std::fabs(ea[i] - ea[i - 1]));
  }
  CHECK(step_ref > 1e-4f && step_eng < 2.5f * step_ref);  // no step from the fade's end to the data (without the crossfade the step was ~5x)
  eng.clear_instrument();
}

void test_thread_priority_port() {
  host::PosixThreadPriority prio;
  ports::NoopThreadPriority noop;
  CHECK(!noop.apply_current_thread(ports::ThreadRole::kIoEdge));
  host::StreamConfig c = stream_cfg(2, 2);
  c.priority = &prio;
  auto m = host::StreamManager::create(c);
  CHECK(m != nullptr);
  if (!m) return;
  for (int i = 0; i < 2000 && m->stats().priority_applied < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#if defined(__APPLE__)
  CHECK(m->stats().priority_applied == 2);  // a lower QoS class on the I/O edge threads, below the real-time audio thread
#else
  std::printf("  I/O thread priority applied on %u of 2 threads (Linux needs CAP_SYS_NICE for SCHED_FIFO)\n", m->stats().priority_applied);
#endif
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"sizing and creation", test_sizing_and_creation},
      {"ring protocol (inline)", test_ring_protocol_inline},
      {"recycled ring is not filled for the old life", test_recycled_ring_is_not_filled_for_the_old_life},
      {"late fill never lands in a recycled ring", test_late_fill_never_lands_in_a_recycled_ring},
      {"fence waits for in-flight reads", test_fence_waits_for_in_flight_reads},
      {"block source equals the resident decode", test_block_source_equals_resident_decode},
      {"loader modes and accounting", test_loader_modes_and_accounting},
      {"bit identity with the resident render (inline)", test_bit_identity_inline},
      {"bit identity with real I/O threads and delays", test_bit_identity_threaded_with_delays},
      {"stalled reader never blocks the audio thread", test_stalled_reader_never_blocks_the_audio_thread},
      {"reader errors are counted, not fatal", test_reader_errors_are_counted_not_fatal},
      {"truncated file is a short read", test_truncated_file_is_a_short_read},
      {"pool exhausted; a recycled ring is clean", test_pool_exhausted_and_recycled_ring_is_clean},
      {"lifecycle: no ring leaks", test_lifecycle_no_leaks},
      {"cancelled stream-mode load leaves nothing", test_cancel_during_stream_mode_load_leaves_nothing},
      {"256 voices streaming", test_256_voices_streaming},
      {"parallel render identical", test_parallel_render_identical},
      {"stale announcements do not starve live streams", test_stale_announcements_do_not_starve_live_streams},
      {"new streams are adopted promptly", test_new_streams_are_adopted_promptly},
      {"a stalled read does not freeze other rings", test_a_stalled_read_does_not_freeze_other_rings},
      {"fence times out", test_fence_times_out},
      {"first read is not a sliver", test_first_read_is_not_a_sliver},
      {"consumer margin on a minimal ring", test_consumer_margin_on_a_minimal_ring},
      {"published margin covers the taps", test_published_margin_covers_the_taps},
      {"short starve has no step", test_short_starve_has_no_step},
      {"thread priority port", test_thread_priority_port},
  };
  for (const auto& t : tests) {
    const int before = g_fail;
    std::printf("stream_test: %s\n", t.name);
    std::fflush(stdout);
    t.fn();
    if (g_fail != before) std::printf("stream_test: %s FAILED\n", t.name);
  }
  std::printf("stream_test: %s (%d failures)\n", g_fail == 0 ? "ok" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}

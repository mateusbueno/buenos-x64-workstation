// Parallel voice render (Story 1.18), correctness only (the dev-board timing budget is deferred: no timing claim here).
//  * 1 execution thread (the 3 partitions inline) and 3 threads are bit-identical, with 256+ sample voices and churn
//    (steals, note offs, fades, releases), through render() and render_split() (per part).
//  * parallel vs the legacy serial render: equal within kSerialTolerance (only the float summation order differs).
//  * parallel off (never set, or set to 0) is bit-identical to the legacy render.
//  * 0 / 1 active voice, over-long calls and engine-level edge cases do not hang or change the result.
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/worker_pool.hpp"
#include "velora/ports/thread_affinity.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                \
  do {                                                          \
    if (!(c)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
      ++g_fail;                                                 \
    }                                                           \
  } while (0)

namespace {

// Documented tolerance: parallel and serial sum the same voice outputs in a different order. A bus carries at most a few hundred voices of
// amplitude <= ~1, so float rounding differs by a few ulp of the bus value; 1e-5 absolute is generous (measured: a few 1e-7) and still catches any real error.
constexpr double kSerialTolerance = 1e-5;

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }

  std::uint32_t add(std::size_t frames, bool stereo, std::uint32_t rate, float amp) {
    std::vector<float> buf(frames * (stereo ? 2 : 1));
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = std::sin(static_cast<float>(i) * 0.05f) * amp + static_cast<float>(i * 37 % 211) / 211.0f * 0.2f * amp - 0.1f * amp;
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.sample_rate = rate;
    p.channels = stereo ? 2 : 1;
    p.frames = frames;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + frames : nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, int lo, int hi) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = static_cast<std::uint8_t>(lo);
    d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>((lo + hi) / 2);
    regions.push_back(d);
    return regions.back();
  }
};

// Built once and shared: loops (sustained), sustain loops with releases, a one shot, a filtered region, a stereo region, release triggers.
// Region data is only read by the engines.
Instrument& instrument() {
  static Instrument ins = [] {
    Instrument i;
    const auto mono = i.add(6000, false, 48000, 0.05f);
    const auto stereo = i.add(5000, true, 44100, 0.04f);
    const auto lp = i.add(9000, false, 48000, 0.05f);
    const auto small = i.add(900, false, 48000, 0.03f);
    {
      auto& a = i.region(mono, 24, 60);
      a.ampeg_attack = 0.002f;
      a.ampeg_release = 0.03f;
      auto& b = i.region(stereo, 40, 96);
      b.pan = -30.0f;
      b.ampeg_decay = 0.05f;
      b.ampeg_sustain = 50.0f;
      auto& c = i.region(lp, 24, 96);
      c.loop_mode = ports::SampleLoopMode::kLoopContinuous;
      c.has_loop_points = true;
      c.loop_start = 1000;
      c.loop_end = 8000;
      c.ampeg_release = 0.05f;
      auto& d = i.region(lp, 50, 90);
      d.loop_mode = ports::SampleLoopMode::kLoopSustain;
      d.has_loop_points = true;
      d.loop_start = 500;
      d.loop_end = 6000;
      d.ampeg_release = 0.1f;
      d.filter_type = ports::SampleFilterType::kLpf2p;
      d.filter_cutoff_hz = 3000.0f;
      d.filter_resonance_db = 2.0f;
      auto& e = i.region(lp, 45, 70);
      e.loop_mode = ports::SampleLoopMode::kOneShot;
      auto& r = i.region(small, 24, 96);
      r.trigger = ports::SampleTrigger::kRelease;
    }
    return i;
  }();
  return ins;
}

constexpr std::uint32_t kBlock = 64;

struct Lcg {
  std::uint32_t s;
  std::uint32_t next() noexcept { return s = s * 1664525u + 1013904223u; }
};

SampleEngine::Config engine_config(bool big = true) {
  SampleEngine::Config cfg;
  cfg.max_voices = big ? 320 : 16;
  cfg.extended_voices = true;
  return cfg;
}

// --- direct engine drive: churn through start/stop/steal fade, render() and render_split() -------------------------------------------

struct Bufs {
  std::vector<float> l, r;  // 16 buses of kBlock
  Bufs() : l(16 * kBlock), r(16 * kBlock) {}
  void clear() {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
  }
};

enum class Mode { kSerial, kPool1, kPool3 };

// Runs `blocks` blocks of churn on one engine; every block's output is appended to `out` (split: 16 buses, else 1). Identical operations for
// every Mode (the operations depend only on the LCG), so the outputs can be compared across modes.
void drive_engine(Mode mode, bool split, std::uint32_t blocks, std::vector<float>& out, std::uint32_t& peak) {
  WorkerPool pool;
  SampleEngine se(engine_config());
  se.set_instrument(instrument());
  if (mode != Mode::kSerial) {
    pool.start(mode == Mode::kPool1 ? 1 : 3);
    se.set_parallel_voice_pool(&pool, kBlock);
  }
  CHECK(se.parallel_voice_render() == (mode != Mode::kSerial));
  Lcg rng{12345};
  Bufs b;
  peak = 0;
  for (std::uint32_t blk = 0; blk < blocks; ++blk) {
    const std::uint32_t starts = blk < 60 ? 6 : (rng.next() >> 28);  // ramp up to a few hundred voices, then churn
    for (std::uint32_t k = 0; k < starts; ++k) {
      const std::uint32_t ch = (rng.next() >> 8) % 16, note = 24 + (rng.next() >> 8) % 73, vel = 2000 + (rng.next() >> 8) % 60000;
      se.start_voice(ch, note, vel);
    }
    const std::uint32_t offs = (rng.next() >> 29);
    for (std::uint32_t k = 0; k < offs; ++k) se.stop_voice((rng.next() >> 8) % 16, 24 + (rng.next() >> 8) % 73);
    const std::uint32_t steals = (rng.next() >> 30);
    for (std::uint32_t k = 0; k < steals; ++k) se.begin_steal_fade((rng.next() >> 8) % se.voice_capacity(), 40 + (rng.next() >> 8) % 300);
    if (blk % 97 == 0) se.set_param(SampleEngine::kParamGain, 0.4f + 0.01f * static_cast<float>(blk % 13));
    b.clear();
    if (split) {
      float* ls[16];
      float* rs[16];
      for (std::uint32_t c = 0; c < 16; ++c) {
        ls[c] = b.l.data() + c * kBlock;
        rs[c] = b.r.data() + c * kBlock;
      }
      se.render_split(ls, rs, kBlock);
      out.insert(out.end(), b.l.begin(), b.l.end());
      out.insert(out.end(), b.r.begin(), b.r.end());
    } else {
      se.render(b.l.data(), b.r.data(), kBlock);
      out.insert(out.end(), b.l.begin(), b.l.begin() + kBlock);
      out.insert(out.end(), b.r.begin(), b.r.begin() + kBlock);
    }
    if (se.active_voices() > peak) peak = se.active_voices();
  }
}

bool bit_identical(const std::vector<float>& a, const std::vector<float>& b, std::size_t& first_diff) {
  if (a.size() != b.size()) {
    first_diff = 0;
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) {
      first_diff = i;
      return false;
    }
  return true;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0.0;
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

void test_engine_modes(bool split) {
  std::vector<float> ser, p1, p3;
  std::uint32_t pk_s = 0, pk_1 = 0, pk_3 = 0;
  drive_engine(Mode::kSerial, split, 600, ser, pk_s);
  drive_engine(Mode::kPool1, split, 600, p1, pk_1);
  drive_engine(Mode::kPool3, split, 600, p3, pk_3);
  std::size_t at = 0;
  const bool same = bit_identical(p1, p3, at);
  if (!same) std::printf("  %s: 1 vs 3 threads differ at index %zu (block %zu)\n", split ? "split" : "mono", at, at / (split ? 2 * 16 * kBlock : 2 * kBlock));
  CHECK(same);
  CHECK(pk_s == pk_1 && pk_1 == pk_3);
  CHECK(pk_3 >= 256);  // the load of the story: 256 sample voices and beyond
  const double d = max_abs_diff(ser, p3);
  std::printf("parallel_render_tests: %s peak_voices=%u serial-vs-parallel max |diff| = %.3g (tolerance %.0e)\n", split ? "split" : "mono ", pk_3, d, kSerialTolerance);
  CHECK(d <= kSerialTolerance);
  double energy = 0.0;
  for (float x : p3) energy += static_cast<double>(x) * x;
  CHECK(energy > 1.0);
}

// Voices of different parts land only in their own part's bus (render_split), each equal to the serial render of the same voices.
void test_split_isolation() {
  WorkerPool pool;
  pool.start(3);
  SampleEngine ser(engine_config()), par(engine_config());
  ser.set_instrument(instrument());
  par.set_instrument(instrument());
  par.set_parallel_voice_pool(&pool, kBlock);
  for (std::uint32_t ch : {1u, 4u, 9u}) {
    for (std::uint32_t n = 0; n < 5; ++n) {
      ser.start_voice(ch, 40 + n * 3, 40000);
      par.start_voice(ch, 40 + n * 3, 40000);
    }
  }
  Bufs bs, bp;
  float* ls[16];
  float* rs[16];
  float* lp[16];
  float* rp[16];
  for (std::uint32_t c = 0; c < 16; ++c) {
    ls[c] = bs.l.data() + c * kBlock;
    rs[c] = bs.r.data() + c * kBlock;
    lp[c] = bp.l.data() + c * kBlock;
    rp[c] = bp.r.data() + c * kBlock;
  }
  for (int i = 0; i < 20; ++i) {
    bs.clear();
    bp.clear();
    ser.render_split(ls, rs, kBlock);
    par.render_split(lp, rp, kBlock);
    for (std::uint32_t c = 0; c < 16; ++c) {
      const bool used = c == 1 || c == 4 || c == 9;
      double e = 0.0, diff = 0.0;
      for (std::uint32_t f = 0; f < kBlock; ++f) {
        e += std::fabs(bp.l[c * kBlock + f]) + std::fabs(bp.r[c * kBlock + f]);
        diff = std::max(diff, static_cast<double>(std::fabs(bp.l[c * kBlock + f] - bs.l[c * kBlock + f])));
      }
      if (!used) CHECK(e == 0.0);
      if (used && i < 3) CHECK(e > 0.0);
      CHECK(diff <= kSerialTolerance);
    }
  }
}

// Few voices: nothing to render, one voice (two of the three partitions are idle), and a voice that ends mid-run; the barrier never hangs.
void test_few_voices() {
  WorkerPool pool;
  pool.start(3);
  SampleEngine ser(engine_config(false)), par(engine_config(false));
  ser.set_instrument(instrument());
  par.set_instrument(instrument());
  par.set_parallel_voice_pool(&pool, kBlock);
  std::vector<float> a(kBlock), b(kBlock), c(kBlock), d(kBlock);
  for (int i = 0; i < 3; ++i) {  // zero voices
    std::fill(b.begin(), b.end(), 0.0f);
    std::fill(d.begin(), d.end(), 0.0f);
    par.render(b.data(), d.data(), kBlock);
    for (float x : b) CHECK(x == 0.0f);
  }
  CHECK(par.active_voices() == 0);
  ser.start_voice(0, 50, 30000);
  par.start_voice(0, 50, 30000);
  CHECK(par.active_voices() >= 1);
  for (int i = 0; i < 400; ++i) {  // long enough for one-shot and releasing voices to end on their own
    if (i == 10) {
      ser.stop_voice(0, 50);
      par.stop_voice(0, 50);
    }
    std::fill(a.begin(), a.end(), 0.0f);
    std::fill(b.begin(), b.end(), 0.0f);
    std::fill(c.begin(), c.end(), 0.0f);
    std::fill(d.begin(), d.end(), 0.0f);
    ser.render(a.data(), c.data(), kBlock);
    par.render(b.data(), d.data(), kBlock);
    CHECK(max_abs_diff(a, b) <= kSerialTolerance);
    CHECK(max_abs_diff(c, d) <= kSerialTolerance);
  }
  // a call longer than the scratch falls back to the serial render (same result as no pool)
  SampleEngine big(engine_config(false)), none(engine_config(false));
  big.set_instrument(instrument());
  none.set_instrument(instrument());
  big.set_parallel_voice_pool(&pool, kBlock);
  big.start_voice(0, 60, 30000);
  none.start_voice(0, 60, 30000);
  std::vector<float> x(256, 0.0f), y(256, 0.0f), xr(256, 0.0f), yr(256, 0.0f);
  big.render(x.data(), xr.data(), 256);
  none.render(y.data(), yr.data(), 256);
  std::size_t at = 0;
  CHECK(bit_identical(x, y, at));
  // detaching the pool returns to the serial path
  big.set_parallel_voice_pool(nullptr, 0);
  CHECK(!big.parallel_voice_render());
}

// --- block loop: serial events, parallel voices, arbiter steals --------------------------------------------------------------------------

struct LoopRun {
  std::vector<float> out;
  std::uint32_t peak = 0;
  std::uint64_t steals = 0;
};

// threads: -1 = never call set_parallel_voice_render, 0 = call it with 0, 1 / 3 = parallel.
LoopRun run_loop(int threads, std::uint32_t blocks) {
  LoopRun res;
  auto se = std::make_unique<SampleEngine>(engine_config());
  se->set_instrument(instrument());
  auto br = std::make_unique<BlockRenderer>();
  br->bind_engine(*se);
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
  br->arbiter().set_min_sample_floor_for_tests(0);
  VoiceArbiter::LiveSetConfig cfg;
  cfg.floors[cfg.floor_count++] = {static_cast<std::uint32_t>(EngineTypeId::kSample), 8};
  cfg.budget = 8 * 5 + 265 * 5;  // about 270 concurrent voices: bursts steal
  if (br->arbiter().reserve(cfg) != VoiceArbiter::ReserveStatus::kOk) {
    std::printf("FAIL reserve\n");
    ++g_fail;
    return res;
  }
  if (threads >= 0) {
    const auto r = br->set_parallel_voice_render(static_cast<std::uint32_t>(threads));
    CHECK(r == WorkerPool::StartResult::kOk);
    CHECK(br->parallel_voice_threads() == static_cast<std::uint32_t>(threads));
  }
  Lcg rng{777};
  float l[kBlockFrames], r[kBlockFrames];
  for (std::uint32_t i = 0; i < blocks; ++i) {
    const std::uint32_t n = i < 80 ? 5 : 1 + (rng.next() >> 29);
    for (std::uint32_t k = 0; k < n; ++k)
      br->submit({make_note_on(0, (rng.next() >> 8) % 16, 24 + (rng.next() >> 8) % 73, 3000 + (rng.next() >> 8) % 60000), (rng.next() >> 8) % kBlockFrames});
    const std::uint32_t o = rng.next() >> 30;
    for (std::uint32_t k = 0; k < o; ++k) br->submit({make_note_off(0, (rng.next() >> 8) % 16, 24 + (rng.next() >> 8) % 73), (rng.next() >> 8) % kBlockFrames});
    br->process_block(l, r);
    res.out.insert(res.out.end(), l, l + kBlockFrames);
    res.out.insert(res.out.end(), r, r + kBlockFrames);
    if (se->active_voices() > res.peak) res.peak = se->active_voices();
  }
  res.steals = br->arbiter().steals();
  return res;
}

void test_block_loop() {
  const std::uint32_t blocks = 500;
  const LoopRun legacy = run_loop(-1, blocks);
  const LoopRun off = run_loop(0, blocks);
  const LoopRun p1 = run_loop(1, blocks);
  const LoopRun p3 = run_loop(3, blocks);
  std::size_t at = 0;
  CHECK(bit_identical(legacy.out, off.out, at));  // default / explicitly off: the exact legacy render
  CHECK(legacy.peak == off.peak);
  const bool same = bit_identical(p1.out, p3.out, at);
  if (!same) std::printf("  block loop: 1 vs 3 threads differ at index %zu\n", at);
  CHECK(same);
  CHECK(p1.steals == p3.steals);
  CHECK(p3.steals > 0);  // steal fades happened under the parallel render
  CHECK(p3.peak >= 200);
  const double d = max_abs_diff(legacy.out, p3.out);
  std::printf("parallel_render_tests: block loop peak_voices=%u steals=%llu serial-vs-parallel max |diff| = %.3g\n", p3.peak, (unsigned long long)p3.steals, d);
  CHECK(d <= kSerialTolerance);
  // Not a no-op comparison: the rendering produced sound.
  double e = 0.0;
  for (float x : p3.out) e += static_cast<double>(x) * x;
  CHECK(e > 1.0);
  // clamping through the block loop's setting
  BlockRenderer br;
  CHECK(br.set_parallel_voice_render(2) == WorkerPool::StartResult::kClamped);
  CHECK(br.parallel_voice_threads() == 1);
  CHECK(br.set_parallel_voice_render(9) == WorkerPool::StartResult::kClamped);
  CHECK(br.parallel_voice_threads() == 3);
  CHECK(br.set_parallel_voice_render(0) == WorkerPool::StartResult::kOk);
  CHECK(br.parallel_voice_threads() == 0);
}

// An engine bound AFTER the setting is switched on is attached too.
void test_late_bind() {
  auto br = std::make_unique<BlockRenderer>();
  br->set_parallel_voice_render(3);
  auto se = std::make_unique<SampleEngine>(engine_config(false));
  se->set_instrument(instrument());
  CHECK(!se->parallel_voice_render());
  br->bind_engine(*se);
  CHECK(se->parallel_voice_render());
  br->set_parallel_voice_render(0);
  CHECK(!se->parallel_voice_render());
}

// The renderer detaches its pool from the bound engines when it is destroyed (an engine that outlives the renderer keeps no dangling pool).
void test_engine_outlives_renderer() {
  SampleEngine se(engine_config(false));
  se.set_instrument(instrument());
  {
    BlockRenderer br;
    br.bind_engine(se);
    br.set_parallel_voice_render(3);
    CHECK(se.parallel_voice_render());
  }
  CHECK(!se.parallel_voice_render());
  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  se.set_voice_allocator(nullptr);  // the dead renderer's arbiter is gone too
  se.start_voice(0, 60, 30000);
  se.render(l.data(), r.data(), kBlock);  // serial, no pool
}

struct RecordingAffinity final : ports::ThreadAffinity {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<std::uint32_t> mask{0};
  bool pin_current_thread(std::uint32_t cpu) noexcept override {
    calls.fetch_add(1);
    mask.fetch_or(1u << cpu);
    return true;
  }
};

// The affinity hook reaches the pool through the block loop: asked 3 times, with the given CPUs, once at start (not per block).
void test_renderer_affinity() {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw != 0 && hw < 2) return;
  const std::uint32_t cpus[3] = {1, 0, 1};
  RecordingAffinity aff;
  BlockRenderer br;
  CHECK(br.set_parallel_voice_render(3, &aff, cpus) == WorkerPool::StartResult::kOk);
  CHECK(aff.calls.load() == 3);
  CHECK(aff.mask.load() == 3u);
  float l[kBlockFrames], r[kBlockFrames];
  for (int i = 0; i < 50; ++i) br.process_block(l, r);
  CHECK(aff.calls.load() == 3);
}

}  // namespace

int main() {
  test_engine_modes(false);
  test_engine_modes(true);
  test_split_isolation();
  test_few_voices();
  test_block_loop();
  test_late_bind();
  test_engine_outlives_renderer();
  test_renderer_affinity();
  std::printf(g_fail ? "parallel_render_tests: FAILED (%d)\n" : "parallel_render_tests: ok\n", g_fail);
  return g_fail ? 1 : 0;
}

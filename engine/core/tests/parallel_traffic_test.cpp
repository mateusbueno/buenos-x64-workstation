// Real-time traffic through the block loop with the parallel voice render ON (3 worker threads), Story 1.18. Beside the legacy
// rtsan_sample_engine_test it checks that, with a global allocation hook armed around process_block, the parallel block path allocates nothing
// (workers included: the hook is process wide). Built twice: without the sanitizer (macOS, runs everywhere) and, when RTSan exists, with
// -fsanitize=realtime, which aborts on any lock / syscall-class call on the calling thread of the nonblocking function.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

// The global operator new hook cannot coexist with the TSan / ASan runtimes (they define the operator themselves): under those the
// allocation count is not measured (the threaded safety is what those builds are for).
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define VELORA_NO_ALLOC_HOOK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define VELORA_NO_ALLOC_HOOK 1
#endif
#endif

static std::atomic<bool> g_armed{false};
static std::atomic<std::uint64_t> g_allocs{0};

#ifndef VELORA_NO_ALLOC_HOOK
void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  std::uint32_t add(std::size_t frames, bool stereo, std::uint32_t rate, float f) {
    std::vector<float> buf(frames * (stereo ? 2 : 1));
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<float>((i * 37 % 211)) / 211.0f * f - 0.5f * f;
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

void run_block(BlockRenderer& br, SampleEngine& se, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t note = 36 + (i * 7) % 61;
  const std::uint32_t vel = 1000 + (i * 977) % 64000;
  br.submit({make_note_on(0, i % 8, note, vel), (i * 5) % 200});
  br.submit({make_note_off(0, (i + 3) % 8, 36 + ((i + 23) * 7) % 61), (i * 11) % 100});
  if (i % 3 == 0) br.submit({make_note_on(0, i % 5, note + 1, 65535), 17});
  if (i % 50 == 0)
    for (std::uint32_t k = 0; k < 40; ++k) br.submit({make_note_on(0, k % 16, 40 + k, 30000 + 500 * k), k});  // pool-full burst
  if (i % 400 == 0) for (std::uint32_t k = 36; k < 97; ++k) br.submit({make_note_off(0, k % 8, k), 3 + (k % 40)});
  if (i % 997 == 0) se.set_param(SampleEngine::kParamGain, 0.5f + 0.0001f * static_cast<float>(i % 7));
  br.process_block(l, r);
}

}  // namespace

int main() {
  Instrument ins;
  const auto mono = ins.add(9000, false, 48000, 1.0f);
  const auto stereo = ins.add(7000, true, 44100, 0.8f);
  const auto small = ins.add(1500, false, 48000, 0.6f);
  const auto lp = ins.add(12000, false, 48000, 0.7f);
  for (int p = 1; p <= 3; ++p) {
    auto& r = ins.region(mono, 36, 60);
    r.seq_length = 3;
    r.seq_position = static_cast<std::uint32_t>(p);
    r.ampeg_release = 0.03f;
  }
  ins.region(stereo, 61, 96).pan = -40.0f;
  {
    auto& c = ins.region(lp, 40, 80);
    c.loop_mode = ports::SampleLoopMode::kLoopContinuous; c.has_loop_points = true; c.loop_start = 1000; c.loop_end = 8000;
    auto& e = ins.region(lp, 45, 70);
    e.loop_mode = ports::SampleLoopMode::kOneShot;
  }
  ins.region(small, 36, 96).trigger = ports::SampleTrigger::kRelease;

  SampleEngine::Config cfg;
  cfg.max_voices = 32;
  auto se = std::make_unique<SampleEngine>(cfg);
  se->set_instrument(ins);
  auto br = std::make_unique<BlockRenderer>();
  br->bind_engine(*se);
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
  const auto started = br->set_parallel_voice_render(3);
  if (started != WorkerPool::StartResult::kOk) {
    std::printf("parallel_traffic_test: FAILED: set_parallel_voice_render(3) did not return kOk\n");
    return 1;
  }
  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  std::uint32_t peak_voices = 0;
  g_armed.store(true);
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(*br, *se, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) energy += static_cast<double>(l[k]) * l[k] + static_cast<double>(r[k]) * r[k];
    peak_voices = std::max(peak_voices, se->active_voices());
  }
  g_armed.store(false);
  const auto allocs = g_allocs.load();
  std::printf("parallel_traffic_test: peak_voices=%u energy=%.3g allocations_in_block_path=%llu\n", peak_voices, energy, (unsigned long long)allocs);
  int rc = 0;
  auto need = [&](bool cond, const char* what) {
    if (!cond) {
      std::printf("parallel_traffic_test: FAILED condition: %s\n", what);
      rc = 1;
    }
  };
  need(se->parallel_voice_render(), "parallel render attached");
  need(peak_voices == 32, "peak_voices == 32");
  need(energy > 0.0, "energy > 0");
#ifndef VELORA_NO_ALLOC_HOOK
  need(allocs == 0, "no allocation in the parallel block path");
#endif
  return rc;
}

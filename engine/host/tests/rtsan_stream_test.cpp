// Story 1.17 RT safety: the audio-thread path in stream mode. Notes start, sustain past their heads (so the rings are read), loop (so the loop wraps re-seek), end and are
// stolen inside a nonblocking function while two I/O edge threads fill the rings. The audio thread must not allocate, lock or make a blocking call: starting a stream (a ring claimed
// and a request pushed), reading the ring buffers, publishing consumed_lo, seeking and giving the ring back are atomics on pre-allocated memory.
// Built twice: without the sanitizer (macOS, runs everywhere: a per-thread allocation counter proves "no allocation on the audio thread"; the I/O threads allocate their scratch and
// are not counted) and, where RealtimeSanitizer exists (clang >= 20: docker-check, Linux), with -fsanitize=realtime, which aborts on any lock / syscall-class call on the audio thread.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/sample_bundle_loader.hpp"
#include "velora/host/stream_manager.hpp"

namespace fs = std::filesystem;
using namespace velora;

// The global operator new hook cannot coexist with the TSan / ASan runtimes: under those the allocation count is not measured.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define VELORA_NO_ALLOC_HOOK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define VELORA_NO_ALLOC_HOOK 1
#endif
#endif

static thread_local bool t_armed = false;  // only the audio thread (this test's main thread) counts
static std::atomic<std::uint64_t> g_allocs{0};

#ifndef VELORA_NO_ALLOC_HOOK
void* operator new(std::size_t n) {
  if (t_armed) g_allocs.fetch_add(1, std::memory_order_relaxed);
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

std::vector<std::uint8_t> make_bundle() {
  using namespace velora::format;
  BundleWriter w;
  const std::size_t i = w.add_instrument("Rt", "Rt.sfz").value();
  auto wave = [](std::size_t f, double a, double ph) { return a * std::sin(0.031 * static_cast<double>(f) + ph) + 0.4 * a * std::sin(0.4 * static_cast<double>(f)); };
  for (int s = 0; s < 3; ++s) {
    const std::uint64_t frames = 14000 + 6000 * static_cast<std::uint64_t>(s);
    std::vector<std::int32_t> d(frames);
    for (std::size_t f = 0; f < d.size(); ++f) d[f] = static_cast<std::int32_t>(wave(f, 9000.0, 0.3 * s));
    (void)w.add_sample(i, {"s", {1, 16, false, 48000}, frames, d});
  }
  auto region = [&](std::uint32_t sample, int lo, int hi, int centre) {
    Region r;
    r.sample = sample;
    r.key_lo = static_cast<std::uint8_t>(lo);
    r.key_hi = static_cast<std::uint8_t>(hi);
    r.key_center = static_cast<std::uint8_t>(centre);
    return r;
  };
  (void)w.add_region(i, region(0, 36, 60, 48));
  {
    Region r = region(1, 61, 84, 72);
    r.loop_mode = LoopMode::kLoopContinuous;
    r.has_loop_points = true;
    r.loop_start = 2000;
    r.loop_end = 12000;
    r.ampeg.release = 0.03f;
    (void)w.add_region(i, r);
  }
  {
    Region r = region(2, 85, 100, 92);
    r.loop_mode = LoopMode::kLoopSustain;
    r.has_loop_points = true;
    r.loop_start = 9000;
    r.loop_end = 17000;
    r.ampeg.release = 0.02f;
    (void)w.add_region(i, r);
  }
  return w.finalize().take();
}

void run_block(core::SampleEngine& se, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t note = 36 + (i * 7) % 65;
  if (i % 3 == 0) se.start_voice(i % 16, note, 1000 + (i * 977) % 64000);
  if (i % 5 == 0) se.stop_voice((i + 3) % 16, 36 + ((i + 23) * 7) % 65);
  if (i % 211 == 0)
    for (std::uint32_t k = 0; k < 60; ++k) se.start_voice(k % 16, 36 + k, 30000 + 500 * k);  // a burst: the ring pool (40) runs out, voices fall back to the placeholder
  if (i % 400 == 0)
    for (std::uint32_t k = 36; k < 101; ++k) se.stop_voice(k % 16, k);
  se.render(l, r, 64);
}

}  // namespace

int main() {
  const auto bytes = make_bundle();
  const std::string dir = (fs::temp_directory_path() / ("velora-stream-rt-" + std::to_string(::getpid()))).string();
  fs::create_directories(dir);
  const std::string path = dir + "/bundle.vlb";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  int rc = 0;
  {
    auto rd = format::BundleReader::open_file(path);
    if (!rd.ok()) {
      std::printf("stream_rt_traffic_test: FAILED: cannot open the bundle\n");
      return 1;
    }
    host::StreamConfig cfg;
    cfg.ring_count = 40;
    cfg.io_threads = 2;
    cfg.p9999_ns = 5'000'000;
    cfg.margin_ns = 5'000'000;
    cfg.poll_us = 100;
    cfg.memory.allow_unlocked = true;
    cfg.memory.lock_fn = [](void*, std::size_t) { return true; };
    cfg.memory.unlock_fn = [](void*, std::size_t) {};
    auto mgr = host::StreamManager::create(cfg);
    host::SampleLoadOptions opt;
    opt.head.enabled = true;
    opt.head.sizing.p9999_ns = cfg.p9999_ns;
    opt.head.sizing.margin_ns = cfg.margin_ns;
    opt.head.memory = cfg.memory;
    opt.stream.enabled = true;
    opt.stream.fence = mgr.get();
    const auto loaded = host::load_instrument_by_index(rd.value(), 0, nullptr, opt);
    if (!mgr || !loaded.ok()) {
      std::printf("stream_rt_traffic_test: FAILED: setup\n");
      return 1;
    }
    core::SampleEngine::Config ec;
    ec.max_voices = 48;
    core::SampleEngine se(ec);
    se.set_instrument(*loaded.instrument);
    se.set_stream_service(mgr.get());
    float l[64], r[64];
    double energy = 0.0;
    std::uint32_t peak = 0;
    t_armed = true;
    for (std::uint32_t i = 0; i < 6000; ++i) {
      run_block(se, l, r, i);
      for (int k = 0; k < 64; ++k) energy += static_cast<double>(l[k]) * l[k] + static_cast<double>(r[k]) * r[k];
      peak = std::max(peak, se.active_voices());
      if (i % 8 == 7) {
        t_armed = false;  // the pacing sleep is not the audio path
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        t_armed = true;
      }
    }
    t_armed = false;
    const auto allocs = g_allocs.load();
    const host::StreamStats st = mgr->stats();
    std::printf("stream_rt_traffic_test: peak_voices=%u energy=%.3g opens=%llu closes=%llu reads=%llu open_failures=%llu underruns=%llu allocations_on_the_audio_thread=%llu\n", peak, energy,
                static_cast<unsigned long long>(st.opens), static_cast<unsigned long long>(st.closes), static_cast<unsigned long long>(st.reads), static_cast<unsigned long long>(st.open_failures),
                static_cast<unsigned long long>(se.stream_underrun_events()), static_cast<unsigned long long>(allocs));
    auto need = [&](bool cond, const char* what) {
      if (!cond) {
        std::printf("stream_rt_traffic_test: FAILED condition: %s\n", what);
        rc = 1;
      }
    };
    need(energy > 0.0, "energy > 0");
    need(st.opens > 100 && st.reads > 200, "streams were opened and tails were read");
    need(st.pool_exhausted > 0, "the pool ran out at least once (the fallback path ran under traffic)");
#ifndef VELORA_NO_ALLOC_HOOK
    need(allocs == 0, "no allocation on the audio thread");
#endif
    // Everything ends: no ring is left held by a voice.
    se.clear_instrument();
    for (int i = 0; i < 2000 && mgr->rings_active() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    need(mgr->rings_active() == 0, "no ring leaked");
    need(mgr->stats().opens == mgr->stats().closes, "every opened stream was closed");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  return rc;
}

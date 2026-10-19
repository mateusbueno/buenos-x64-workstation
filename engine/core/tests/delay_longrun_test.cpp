// Story 2.11: the delay at maximum feedback for 10 minutes of audio (28.8 M frames, processed faster than real time in 1024-frame slices).
// Registered as a separate, slow test (label "slow", own TIMEOUT) so that delay_tests stays quick. Three runs of 10 minutes each, all with the
// feedback requested far above its cap (the cap of 0.95 is applied in code):
//   A. adversarial: impulses at 0.9 aligned with the loop period (every echo is hit again), no damping, ping-pong half: the output must never exceed
//      full scale, never produce a non-finite value, and the level must not grow from minute to minute; after the input stops the tail dies and the
//      delay goes idle.
//   B. sparse, quiet input (random 0.01 impulses): the linear regime (the limiter inactive): peak <= 0.01 / (1 - 0.95) (+ margin), level stationary.
//   C. tempo-synced with the tempo moving every second (glides all the time) under noise bursts: bounded as well.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "velora/core/delay.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr std::uint32_t kSlice = 1024;
constexpr std::uint64_t kFramesPerMinute = 48000ull * 60;
constexpr int kMinutes = 10;

struct Stats {
  double peak = 0;
  bool finite = true;
  double minute_rms[kMinutes + 1] = {};
};

struct Lcg {
  std::uint32_t s;
  float next() {  // [-1, 1)
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<double>(s >> 8) / 8388608.0 - 1.0);
  }
  std::uint32_t raw() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

// Runs `minutes` of audio, calling fill(frame0, l, r, n) for the input of each slice and tick(frame0) at each slice start.
template <class Fill, class Tick>
Stats run(DelaySend& d, int minutes, Fill fill, Tick tick) {
  Stats st;
  std::vector<float> l(kSlice), r(kSlice);
  double sum2 = 0;
  std::uint64_t cnt = 0;
  int minute = 0;
  for (std::uint64_t pos = 0; pos < kFramesPerMinute * static_cast<std::uint64_t>(minutes); pos += kSlice) {
    tick(pos);
    fill(pos, l.data(), r.data(), kSlice);
    StereoBlock b{l.data(), r.data(), kSlice};
    d.process(b);
    for (std::uint32_t i = 0; i < kSlice; ++i) {
      if (!std::isfinite(l[i]) || !std::isfinite(r[i])) st.finite = false;
      st.peak = std::max({st.peak, static_cast<double>(std::fabs(l[i])), static_cast<double>(std::fabs(r[i]))});
      sum2 += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
    }
    cnt += 2 * kSlice;
    if ((pos + kSlice) / kFramesPerMinute > static_cast<std::uint64_t>(minute) && minute < minutes) {
      st.minute_rms[minute++] = std::sqrt(sum2 / static_cast<double>(cnt));
      sum2 = 0;
      cnt = 0;
    }
  }
  return st;
}

void run_a() {
  constexpr std::uint32_t kPeriod = 4800;  // 100 ms
  DelaySend d;
  d.configure(100.0f, false, DelayNote::kEighth, /*feedback requested=*/1.7f, 1.0f, /*damping=*/0.0f, /*ping-pong=*/0.5f, true);
  CHECK(d.feedback() == 0.95f);
  const Stats st = run(
      d, kMinutes,
      [&](std::uint64_t pos, float* l, float* r, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) l[i] = r[i] = ((pos + i) % kPeriod == 0) ? 0.9f : 0.0f;
      },
      [](std::uint64_t) {});
  std::printf("  A: peak %.6f, rms by minute:", st.peak);
  for (int m = 0; m < kMinutes; ++m) std::printf(" %.4f", st.minute_rms[m]);
  std::printf("\n");
  CHECK(st.finite);
  CHECK(st.peak <= 1.0);
  CHECK(st.peak > 0.3);  // the loop really was driven
  // never runs away: the level of the last minutes is no larger than that of the second minute (the loop saturated long before)
  CHECK(st.minute_rms[kMinutes - 1] <= 1.01 * st.minute_rms[1] && st.minute_rms[kMinutes - 1] < 1.0);
  CHECK(d.engine_blocks() > 0);
  // the input stops: the tail dies away and the delay goes idle (0.95^n: 1e-6 after ~270 passes of 100 ms, plus the line length)
  std::vector<float> l(kSlice, 0.0f), r(kSlice, 0.0f);
  double last_peak = 0;
  for (std::uint64_t pos = 0; pos < 48000ull * 90; pos += kSlice) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    StereoBlock b{l.data(), r.data(), kSlice};
    d.process(b);
    if (pos >= 48000ull * 85)
      for (std::uint32_t i = 0; i < kSlice; ++i) last_peak = std::max(last_peak, static_cast<double>(std::fabs(l[i])));
  }
  CHECK(d.quiet() && last_peak == 0.0);
}

void run_b() {
  DelaySend d;
  d.configure(130.0f, false, DelayNote::kEighth, 1.0f, 1.0f, 0.0f, 0.0f, true);
  Lcg g{4242};
  const Stats st = run(
      d, kMinutes,
      [&](std::uint64_t, float* l, float* r, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
          const std::uint32_t k = g.raw();
          l[i] = r[i] = (k & 0x3FF) == 0 ? 0.01f * ((k & 0x400) ? 1.0f : -1.0f) : 0.0f;  // about one impulse in 1000 samples, random sign
        }
      },
      [](std::uint64_t) {});
  std::printf("  B: peak %.6f, rms by minute:", st.peak);
  for (int m = 0; m < kMinutes; ++m) std::printf(" %.5f", st.minute_rms[m]);
  std::printf("\n");
  CHECK(st.finite);
  CHECK(st.peak <= 0.01 / (1.0 - 0.95) * 1.05);
  // stationary: the level of minute 10 is within 20 % of minute 5
  CHECK(st.minute_rms[kMinutes - 1] < 1.2 * st.minute_rms[4] && st.minute_rms[kMinutes - 1] > 0.8 * st.minute_rms[4]);
}

void run_c() {
  DelaySend d;
  d.set_tempo(120.0f);
  d.configure(200.0f, true, DelayNote::kDottedEighth, 0.95f, 1.0f, 0.1f, 0.7f, true);
  Lcg g{99};
  const Stats st = run(
      d, kMinutes,
      [&](std::uint64_t pos, float* l, float* r, std::uint32_t n) {
        const bool burst = (pos / 24000) % 4 == 0;  // half-second noise bursts, two seconds apart
        for (std::uint32_t i = 0; i < n; ++i) {
          l[i] = burst ? 0.6f * g.next() : 0.0f;
          r[i] = burst ? 0.6f * g.next() : 0.0f;
        }
      },
      [&](std::uint64_t pos) {
        if (pos % 48000 < kSlice) {  // every second a new tempo between 40 and 200 bpm
          const std::uint64_t sec = pos / 48000;
          d.set_tempo(40.0f + static_cast<float>((sec * 37) % 161));
        }
      });
  std::printf("  C: peak %.6f, rms by minute:", st.peak);
  for (int m = 0; m < kMinutes; ++m) std::printf(" %.4f", st.minute_rms[m]);
  std::printf("\n");
  CHECK(st.finite);
  CHECK(st.peak <= 1.0);
  CHECK(st.minute_rms[kMinutes - 1] < 1.0);
}

}  // namespace

int main() {
  std::printf("delay long run: 3 x %d min of audio\n", kMinutes);
  run_a();
  run_b();
  run_c();
  if (g_fail == 0) std::printf("delay_longrun_tests: ok\n");
  else std::printf("delay_longrun_tests: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}

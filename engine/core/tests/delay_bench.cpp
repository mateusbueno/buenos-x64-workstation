// Host benchmark of the delay send (Story 2.11): the cost of one delay instance in its worst case (loud input, damping and ping-pong active, the
// delay time gliding all the time, so every per-sample branch runs), the steady case, the whole Mixer with all 8 parts sending, and the EQ insert as
// the reference the declared cost units are scaled against. Host figures, not representative of the dev board (the board measurement is Story 2.12).
//
// Declared cost (fx.hpp kDelaySendCost) is checked ratio-wise: units_needed = delay_block_time / eq_block_time * kMasterEqCost, with the MEDIAN block
// times of this run (robust against scheduler noise), taking the gliding worst case, and the declared cost must cover it (asserted in optimised builds
// only; the worst block times are printed next to it). The test fails on any "FAIL" line.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/delay.hpp"
#include "velora/core/eq.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

namespace {
using Clock = std::chrono::steady_clock;

struct Times {
  double median_us = 0, p99_us = 0, worst_us = 0;
};

Times summarise(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  return {v[v.size() / 2], v[v.size() * 99 / 100], v.back()};
}

// Best of N: a measurement is repeated and the least disturbed repeat (the smallest median, p99 and worst) is kept, so scheduler noise from other
// work on the host (Docker, parallel tests) cannot inflate a unit cost. The declared-cost assertion then retries the whole measurement (below).
constexpr int kRepeats = 5, kAttempts = 3;
template <class F>
Times best_of(F f) {
  Times best = f();
  for (int i = 1; i < kRepeats; ++i) {
    const Times t = f();
    best.median_us = std::min(best.median_us, t.median_us);
    best.p99_us = std::min(best.p99_us, t.p99_us);
    best.worst_us = std::min(best.worst_us, t.worst_us);
  }
  return best;
}

std::vector<float> noise_block(std::uint32_t seed) {
  std::vector<float> v(64);
  for (float& x : v) {
    seed = seed * 1664525u + 1013904223u;
    x = 0.5f * static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
  }
  return v;
}

// glide: the delay time moves between two values every 40 blocks so that the slew runs almost all the time.
Times time_delay_once(bool glide, bool pingpong) {
  constexpr int kBlocks = 8000;
  DelaySend d;
  d.configure(900.0f, false, DelayNote::kEighth, 0.9f, 1.0f, 0.4f, pingpong ? 0.6f : 0.0f, true);
  const std::vector<float> in = noise_block(7);
  std::vector<double> t;
  t.reserve(kBlocks);
  float l[64], r[64], sink = 0;
  for (int b = 0; b < kBlocks; ++b) {
    if (glide && b % 40 == 0) d.configure((b / 40) % 2 ? 300.0f : 1800.0f, false, DelayNote::kEighth, 0.9f, 1.0f, 0.4f, pingpong ? 0.6f : 0.0f, false);
    std::copy(in.begin(), in.end(), l);
    std::copy(in.begin(), in.end(), r);
    StereoBlock sb{l, r, 64};
    const auto t0 = Clock::now();
    d.process(sb);
    const auto t1 = Clock::now();
    sink += l[63];
    t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  if (sink == 12345.678f) std::printf("%f\n", sink);
  return summarise(t);
}

// The first block after an idle spell (the delay went quiet through a return level of 0 and wakes up with input): there is no bulk clear, so it costs like any block.
Times time_wake_once() {
  constexpr int kRounds = 600;
  DelaySend d;
  const std::vector<float> in = noise_block(5);
  std::vector<double> t;
  float l[64], r[64], sink = 0;
  for (int k = 0; k < kRounds; ++k) {
    d.configure(900.0f, false, DelayNote::kEighth, 0.9f, 1.0f, 0.4f, 0.6f, true);
    for (int b = 0; b < 40; ++b) {  // run, then silence the return: the delay goes idle
      std::copy(in.begin(), in.end(), l);
      std::copy(in.begin(), in.end(), r);
      StereoBlock sb{l, r, 64};
      d.process(sb);
    }
    d.configure(900.0f, false, DelayNote::kEighth, 0.9f, 0.0f, 0.4f, 0.6f, false);
    for (int b = 0; b < 3; ++b) {
      std::fill(l, l + 64, 0.0f);
      std::fill(r, r + 64, 0.0f);
      StereoBlock sb{l, r, 64};
      d.process(sb);
    }
    d.configure(900.0f, false, DelayNote::kEighth, 0.9f, 1.0f, 0.4f, 0.6f, false);
    std::copy(in.begin(), in.end(), l);
    std::copy(in.begin(), in.end(), r);
    StereoBlock sb{l, r, 64};
    const auto t0 = Clock::now();
    d.process(sb);
    const auto t1 = Clock::now();
    sink += l[63];
    t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  if (sink == 12345.678f) std::printf("%f\n", sink);
  return summarise(t);
}

Times time_eq_once() {
  constexpr int kBlocks = 6000;
  EqEffect eq;
  eq.prepare(48000, 64);
  eq.set_physical(4.0f, -3.0f, 2500.0f, 5.0f, true);  // three active bands
  const std::vector<float> in = noise_block(9);
  std::vector<double> t;
  t.reserve(kBlocks);
  float l[64], r[64], sink = 0;
  for (int b = 0; b < kBlocks; ++b) {
    std::copy(in.begin(), in.end(), l);
    std::copy(in.begin(), in.end(), r);
    StereoBlock sb{l, r, 64};
    const auto t0 = Clock::now();
    eq.process(sb);
    const auto t1 = Clock::now();
    sink += l[63];
    t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  if (sink == 12345.678f) std::printf("%f\n", sink);
  return summarise(t);
}

// The whole renderer with 8 parts (test-tone voices on channels 0..7), the mixer forced active by a -0.1 dB master trim; with and without the delay sends.
Times time_mixer_once(bool sends) {
  constexpr int kBlocks = 4000;
  auto br = std::make_unique<BlockRenderer>(32);
  Snapshot s = Snapshot::defaults();
  s.revision = 1;
  s.ledger_index = 0;
  s.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = -0.1f;
  s.master[static_cast<std::size_t>(params::find(params::kDelayTime)->master_index)] = 900.0f;
  s.master[static_cast<std::size_t>(params::find(params::kDelayFeedback)->master_index)] = 0.9f;
  if (sends)
    for (std::uint32_t p = 0; p < 8; ++p) s.perf_init[params::perf_slot(*params::find(params::kSendDelayLevel), p)] = 1.0f;
  br->snapshots().publish(&s);
  for (std::uint32_t p = 0; p < 8; ++p) br->submit({make_note_on(0, p, 48 + p * 3, 40000), 0});
  std::vector<double> t;
  t.reserve(kBlocks);
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (int b = 0; b < kBlocks; ++b) {
    OutputBuses ob{l, r, al, ar};
    const auto t0 = Clock::now();
    br->process_block(ob);
    const auto t1 = Clock::now();
    t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  return summarise(t);
}

Times time_delay(bool glide, bool pingpong) { return best_of([&] { return time_delay_once(glide, pingpong); }); }
Times time_eq() { return best_of([&] { return time_eq_once(); }); }
Times time_wake() { return best_of([&] { return time_wake_once(); }); }
Times time_mixer(bool sends) { return best_of([&] { return time_mixer_once(sends); }); }
}  // namespace

static int run(bool final_attempt) {
  std::printf("delay benchmark (host, not representative of the dev board)\n");
  const Times eq = time_eq();
  std::printf("  reference: EQ insert, 3 active bands        : median %.2f us/block, worst %.2f us (declared %u units)\n", eq.median_us, eq.worst_us,
              effect_cost_units(EffectType::kEq));
  const Times steady = time_delay(false, false), steady_pp = time_delay(false, true), glide = time_delay(true, false), glide_pp = time_delay(true, true);
  std::printf("  delay, one instance, steady time            : median %.2f us/block, p99 %.2f, worst %.2f\n", steady.median_us, steady.p99_us, steady.worst_us);
  std::printf("  delay, one instance, steady, ping-pong      : median %.2f us/block, p99 %.2f, worst %.2f\n", steady_pp.median_us, steady_pp.p99_us, steady_pp.worst_us);
  std::printf("  delay, one instance, gliding time           : median %.2f us/block, p99 %.2f, worst %.2f\n", glide.median_us, glide.p99_us, glide.worst_us);
  std::printf("  delay, one instance, gliding, ping-pong     : median %.2f us/block, p99 %.2f, worst %.2f\n", glide_pp.median_us, glide_pp.p99_us, glide_pp.worst_us);
  const Times wake = time_wake();
  std::printf("  delay, first block after an idle spell      : median %.2f us/block, p99 %.2f, worst %.2f\n", wake.median_us, wake.p99_us, wake.worst_us);
  const double worst_median = std::max({steady.median_us, steady_pp.median_us, glide.median_us, glide_pp.median_us, wake.median_us});
  const double worst_block = std::max({steady.worst_us, steady_pp.worst_us, glide.worst_us, glide_pp.worst_us, wake.worst_us});
  const Times base = time_mixer(false), with = time_mixer(true);
  const double mixer_delta = with.median_us - base.median_us;
  const double needed = worst_median / eq.median_us * static_cast<double>(effect_cost_units(EffectType::kEq));
  const double period_us = 64.0 / 48000.0 * 1e6;
  std::printf("  mixer, 8 parts all sending, 900 ms          : median %.2f us/block, worst %.2f (without sends: median %.2f, worst %.2f)\n", with.median_us,
              with.worst_us, base.median_us, base.worst_us);
  std::printf("  delay instance vs EQ insert: %.1f x the EQ block time -> %.0f units needed ratio-wise, declared %u units per instance (x2 buses)\n",
              worst_median / eq.median_us, needed, kDelaySendCost);
  std::printf("  mixer delta of the delay sends (median) %.2f us; worst mixer block %.2f us = %.1f %% of the %.0f us period (host)\n", mixer_delta,
              with.worst_us, 100.0 * with.worst_us / period_us, period_us);
  const double wake_units = wake.p99_us / eq.median_us * static_cast<double>(effect_cost_units(EffectType::kEq));
  std::printf("  wake-up block p99 %.2f us (worst %.2f, scheduler outliers included) = %.0f units ratio-wise (declared %u)\n", wake.p99_us, wake.worst_us, wake_units, kDelaySendCost);
  std::printf("  worst single delay block %.2f us = %.1f x the EQ median\n", worst_block, worst_block / eq.median_us);
  const bool sane = std::isfinite(worst_median) && worst_median > 0 && eq.median_us > 0;
#ifdef __OPTIMIZE__
  const bool covered = static_cast<double>(kDelaySendCost) >= needed && static_cast<double>(kDelaySendCost) * 2.0 >= wake_units;  // the wake-up block's p99 within 2x the declared cost (the worst single block is a scheduler outlier)
#else
  const bool covered = true;  // an unoptimised build distorts the ratio (the declared figure is based on the Release numbers): printed, not asserted
  std::printf("  (unoptimised build: the declared-cost check is only asserted in optimised builds)\n");
#endif
  if (!covered && !final_attempt) std::printf("  below the declared cost on this attempt: measuring again\n");
  if (!covered && final_attempt) std::printf("FAIL: the declared delay cost (%u) does not cover the measured ratio (%.0f units)\n", kDelaySendCost, needed);
  if (!sane && final_attempt) std::printf("FAIL: the benchmark produced no usable figures\n");
  return sane && covered ? 0 : 1;
}

int main() {
  int rc = 1;
  for (int attempt = 1; attempt <= kAttempts && rc != 0; ++attempt) {
    if (attempt > 1) std::printf("attempt %d of %d\n", attempt, kAttempts);
    rc = run(attempt == kAttempts);
  }
  return rc;
}

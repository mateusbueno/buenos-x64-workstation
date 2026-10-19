// Host benchmark of the reverb send (Story 2.10): the cost of one reverb instance at its longest time with a loud input, the cross-fade
// worst case (two engines), the whole Mixer with all 8 parts sending, and the EQ insert as the reference the declared cost units are
// scaled against. Host figures, not representative of the dev board (the board measurement is Story 2.12).
//
// Declared cost (fx.hpp kReverbSendCost) is checked ratio-wise: units_needed = reverb_block_time / eq_block_time * kMasterEqCost, with the
// MEDIAN block times of this run (robust against scheduler noise), taking the type cross-fade (two engines running) as the steady worst
// case, and the declared cost must cover it (asserted in optimised builds only); the declared figure carries a margin so that the worst
// block is covered too (the worst block times are printed next to it). Release numbers on the dev Mac: EQ 0.67 us, reverb 0.75 us, reverb
// during a type cross-fade 1.5 us per 64-frame block.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/eq.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/reverb.hpp"
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

std::vector<float> noise_block(std::uint32_t seed) {
  std::vector<float> v(64);
  for (float& x : v) {
    seed = seed * 1664525u + 1013904223u;
    x = 0.5f * static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
  }
  return v;
}

Times time_reverb(ReverbType type, bool crossfade) {
  constexpr int kBlocks = 6000;
  ReverbSend rv;
  rv.configure(type, 10.0f, 1.0f, true);
  const std::vector<float> in = noise_block(7);
  std::vector<double> t;
  t.reserve(kBlocks);
  float l[64], r[64], sink = 0;
  for (int b = 0; b < kBlocks; ++b) {
    if (crossfade && b % 100 == 0) rv.configure(static_cast<ReverbType>((static_cast<int>(rv.current_type()) + 1) % 3), 10.0f, 1.0f, false);
    std::copy(in.begin(), in.end(), l);
    std::copy(in.begin(), in.end(), r);
    StereoBlock sb{l, r, 64};
    const auto t0 = Clock::now();
    rv.process(sb);
    const auto t1 = Clock::now();
    sink += l[63];
    if (!crossfade || rv.fading()) t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  if (sink == 12345.678f) std::printf("%f\n", sink);
  return summarise(t);
}

Times time_eq() {
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

// The whole renderer with 8 parts (test-tone voices on channels 0..7), the mixer forced active by a -0.1 dB master trim; with and without
// the reverb sends (all 8 parts at send 1, time 10 s).
Times time_mixer(bool sends, ReverbType type) {
  constexpr int kBlocks = 4000;
  auto br = std::make_unique<BlockRenderer>(32);
  Snapshot s = Snapshot::defaults();
  s.revision = 1;
  s.ledger_index = 0;
  s.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = -0.1f;
  s.master[static_cast<std::size_t>(params::find(params::kReverbType)->master_index)] = static_cast<float>(static_cast<int>(type));
  s.master[static_cast<std::size_t>(params::find(params::kReverbTime)->master_index)] = 10.0f;
  if (sends)
    for (std::uint32_t p = 0; p < 8; ++p) s.perf_init[params::perf_slot(*params::find(params::kSendReverbLevel), p)] = 1.0f;
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
}  // namespace

int main() {
  const char* const names[3] = {"room", "hall", "plate"};
  std::printf("reverb benchmark (host, not representative of the dev board)\n");
  const Times eq = time_eq();
  std::printf("  reference: EQ insert, 3 active bands        : median %.2f us/block, worst %.2f us (declared %u units)\n", eq.median_us,
              eq.worst_us, effect_cost_units(EffectType::kEq));
  double worst_median = 0, worst_block = 0;
  for (int ty = 0; ty < 3; ++ty) {
    const Times t = time_reverb(static_cast<ReverbType>(ty), false);
    const Times x = time_reverb(static_cast<ReverbType>(ty), true);
    std::printf("  reverb %-5s, 10 s, one instance            : median %.2f us/block, p99 %.2f, worst %.2f\n", names[ty], t.median_us, t.p99_us, t.worst_us);
    std::printf("  reverb %-5s, 10 s, type cross-fade blocks  : median %.2f us/block, p99 %.2f, worst %.2f (two engines)\n", names[ty], x.median_us, x.p99_us, x.worst_us);
    worst_median = std::max({worst_median, t.median_us, x.median_us});  // the cross-fade (two engines) is the worst steady case
    worst_block = std::max({worst_block, t.worst_us, x.worst_us});
  }
  const Times base = time_mixer(false, ReverbType::kHall);
  double mixer_delta = 0, mixer_worst = 0;
  for (int ty = 0; ty < 3; ++ty) {
    const Times m = time_mixer(true, static_cast<ReverbType>(ty));
    std::printf("  mixer, 8 parts all sending, %-5s, 10 s     : median %.2f us/block, worst %.2f (without sends: median %.2f, worst %.2f)\n", names[ty],
                m.median_us, m.worst_us, base.median_us, base.worst_us);
    mixer_delta = std::max(mixer_delta, m.median_us - base.median_us);
    mixer_worst = std::max(mixer_worst, m.worst_us);
  }
  const double needed = worst_median / eq.median_us * static_cast<double>(effect_cost_units(EffectType::kEq));
  const double period_us = 64.0 / 48000.0 * 1e6;
  std::printf("  reverb instance vs EQ insert: %.1f x the EQ block time -> %.0f units needed ratio-wise, declared %u units per instance (x2 buses)\n",
              worst_median / eq.median_us, needed, kReverbSendCost);
  std::printf("  mixer delta of the reverb sends (median) %.2f us; worst mixer block %.2f us = %.1f %% of the %.0f us period (host)\n", mixer_delta,
              mixer_worst, 100.0 * mixer_worst / period_us, period_us);
  std::printf("  worst single reverb block (any type, incl. cross-fade) %.2f us = %.1f x the EQ median\n", worst_block, worst_block / eq.median_us);
  const bool sane = std::isfinite(worst_median) && worst_median > 0 && eq.median_us > 0;
#ifdef __OPTIMIZE__
  const bool covered = static_cast<double>(kReverbSendCost) >= needed;
#else
  const bool covered = true;  // an unoptimised build distorts the ratio (the declared figure is based on the Release numbers): printed, not asserted
  std::printf("  (unoptimised build: the declared-cost check is only asserted in optimised builds)\n");
#endif
  if (!covered) std::printf("FAIL: the declared reverb cost (%u) does not cover the measured ratio (%.0f units)\n", kReverbSendCost, needed);
  return sane && covered ? 0 : 1;
}

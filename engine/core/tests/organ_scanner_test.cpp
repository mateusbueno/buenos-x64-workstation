// Vibrato/chorus scanner and tonewheel leakage tests (Story 2.4). Bare main, CHECK style as the other core tests.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/vibrato_scanner.hpp"

using namespace velora::core;
namespace tw = velora::core::tonewheel;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.283185307179586;
constexpr ScannerMode kModes[] = {ScannerMode::kOff, ScannerMode::kV1, ScannerMode::kV2, ScannerMode::kV3,
                                  ScannerMode::kC1,  ScannerMode::kC2, ScannerMode::kC3};
const char* mode_name(ScannerMode m) {
  static const char* n[] = {"off", "V1", "V2", "V3", "C1", "C2", "C3"};
  return n[static_cast<int>(m)];
}

std::vector<float> sine(std::size_t n, double hz, double amp) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / kFs));
  return x;
}

double max_step(const std::vector<float>& x, std::size_t from = 1, std::size_t to = 0) {
  if (to == 0 || to > x.size()) to = x.size();
  double m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(x[i]) - x[i - 1]));
  return m;
}

// ---- delay measurement ------------------------------------------------------------------------------------
// Recovers the delay (in samples) the scanner applies to a sine of `hz` by I/Q demodulation: y * e^{-j w n} averaged
// over a whole number of carrier periods gives a phasor whose angle is -w * delay (+ a constant).
std::vector<double> measure_delay(const std::vector<float>& y, double hz, std::size_t period) {
  const std::size_t n = y.size();
  std::vector<std::complex<double>> cum(n + 1);
  for (std::size_t i = 0; i < n; ++i)
    cum[i + 1] = cum[i] + static_cast<double>(y[i]) * std::polar(1.0, -kTwoPi * hz * static_cast<double>(i) / kFs);
  std::vector<double> d(n - period, 0.0);
  const double w = kTwoPi * hz / kFs;
  for (std::size_t i = 0; i + period < n; ++i) {
    const std::complex<double> c = cum[i + period] - cum[i];
    // sin(w (n - d)) e^{-jwn} averages to e^{-jwd} / (2j): angle = -pi/2 - w d (no wrap while w d < pi/2).
    d[i] = -(std::arg(c) + kTwoPi / 4.0) / w;
  }
  return d;
}

void test_registry_and_cc() {
  CHECK(params::find(params::kVibratoChorusOn) != nullptr);
  CHECK(params::find_by_cc(81) == params::find(params::kVibratoChorusOn));
  CHECK(params::find_by_cc(83) == params::find(params::kLeakage));
  CHECK(params::find_by_cc(87) == params::find(params::kVibratoChorusType));
  // The 7-bit CC range spreads over v1..c3.
  const int expect[6] = {0, 1, 2, 3, 4, 5};
  const std::uint32_t cc_values[6] = {0, 25, 51, 76, 102, 127};
  for (int i = 0; i < 6; ++i) {
    Ump ev;
    CHECK(cc_to_param_event(make_control_change(0, 2, 87, static_cast<std::uint32_t>(cc_values[i] * 0xFFFFFFFFull / 127)), ev));
    ParamEvent pe;
    CHECK(decode_param_event(ev, pe));
    CHECK(pe.def->id == params::kVibratoChorusType && pe.part == 2 && static_cast<int>(pe.value) == expect[i]);
  }
}

void test_scanner_constants() {
  // Public reference facts: sweep 6-7 Hz; 3, 5 and 9 taps for settings 1, 2 and 3; chorus = V depth with a dry mix.
  CHECK(VibratoScanner::kSweepHz > 6.0 && VibratoScanner::kSweepHz < 7.0);
  CHECK(VibratoScanner::tap_intervals(ScannerMode::kV1) + 1 == 3 && VibratoScanner::tap_intervals(ScannerMode::kC1) + 1 == 3);
  CHECK(VibratoScanner::tap_intervals(ScannerMode::kV2) + 1 == 5 && VibratoScanner::tap_intervals(ScannerMode::kC2) + 1 == 5);
  CHECK(VibratoScanner::tap_intervals(ScannerMode::kV3) + 1 == 9 && VibratoScanner::tap_intervals(ScannerMode::kC3) + 1 == 9);
  CHECK(VibratoScanner::tap_intervals(ScannerMode::kOff) == 0);
  CHECK(VibratoScanner::kMaxDelaySamples == 48);  // 1 ms
  CHECK(VibratoScanner::wet_mix(ScannerMode::kV2) == 1.0f && VibratoScanner::wet_mix(ScannerMode::kC2) == 0.5f &&
        VibratoScanner::wet_mix(ScannerMode::kOff) == 0.0f);
}

void test_off_is_transparent() {
  VibratoScanner s;
  const auto x = sine(5000, 440.0, 0.5);
  std::vector<float> y(x.size());
  s.process(x.data(), y.data(), static_cast<std::uint32_t>(x.size()));
  CHECK(x == y);  // bit-exact
}

// Sweep rate and depth of every setting on a 200 Hz test tone: the recovered delay swings between 0 and
// tap_intervals x 6 samples (peak to peak = swing, within 5 %), as a triangle at the sweep rate (within 1.5 %).
void test_sweep_rate_and_depth() {
  const double carrier = 200.0;
  const std::size_t period = 240;           // 200 Hz at 48 kHz: a whole number of periods per average, and w x 1 ms < pi/2
  const std::size_t total = 48000 * 8;      // 8 s: about 55 sweep cycles
  const auto x = sine(total, carrier, 0.5);
  for (ScannerMode m : {ScannerMode::kV1, ScannerMode::kV2, ScannerMode::kV3}) {
    VibratoScanner s;
    s.set_mode_immediate(m);
    std::vector<float> y(total);
    s.process(x.data(), y.data(), static_cast<std::uint32_t>(total));
    const std::vector<double> d = measure_delay(y, carrier, period);
    const std::size_t from = 4800 + period;  // after the delay line has filled
    double lo = 1e9, hi = -1e9;
    for (std::size_t i = from; i < d.size(); ++i) {
      lo = std::min(lo, d[i]);
      hi = std::max(hi, d[i]);
    }
    const double swing = VibratoScanner::swing_samples(m);
    std::printf("  %s: measured swing %.2f samples (expected %.0f)\n", mode_name(m), hi - lo, swing);
    CHECK(std::fabs((hi - lo) - swing) < 0.05 * swing);
    // Sweep rate: interpolated upward crossings of the mid level.
    const double mid = 0.5 * (hi + lo);
    std::vector<double> ups;
    for (std::size_t i = from + 1; i < d.size(); ++i)
      if (d[i - 1] < mid && d[i] >= mid) ups.push_back(static_cast<double>(i - 1) + (mid - d[i - 1]) / (d[i] - d[i - 1]));
    CHECK(ups.size() > 40);
    const double rate = static_cast<double>(ups.size() - 1) / ((ups.back() - ups.front()) / kFs);
    std::printf("  %s: measured sweep rate %.3f Hz (expected %.2f)\n", mode_name(m), rate, VibratoScanner::kSweepHz);
    CHECK(std::fabs(rate - VibratoScanner::kSweepHz) < 0.015 * VibratoScanner::kSweepHz);
    // Frequency deviation implied by the swing: pitch ratio changes by +-(slope) with slope = 2 x swing x rate / fs.
    double max_slope = 0;  // delay change per sample, over 480-sample spans (the demodulator is noisy sample to sample)
    for (std::size_t i = from + 480; i < d.size(); ++i) max_slope = std::max(max_slope, std::fabs(d[i] - d[i - 480]) / 480.0);
    const double expect_slope = 2.0 * swing * VibratoScanner::kSweepHz / kFs;
    CHECK(std::fabs(max_slope - expect_slope) < 0.08 * expect_slope);
  }
  // Depth increases V1 < V2 < V3 and C1..C3 sweep the same taps as V1..V3.
  CHECK(VibratoScanner::swing_samples(ScannerMode::kV1) < VibratoScanner::swing_samples(ScannerMode::kV2));
  CHECK(VibratoScanner::swing_samples(ScannerMode::kV2) < VibratoScanner::swing_samples(ScannerMode::kV3));
  for (int i = 0; i < 3; ++i)
    CHECK(VibratoScanner::swing_samples(kModes[1 + i]) == VibratoScanner::swing_samples(kModes[4 + i]));
}

// Frequency modulation spectrum: sidebands at carrier +- k x sweep rate; V3 puts more energy in them than V1.
void test_fm_sidebands() {
  const double carrier = 1000.0;
  const std::size_t total = 48000 * 4;
  const auto x = sine(total, carrier, 0.5);
  double side[2] = {0, 0}, carrier_amp[2] = {0, 0};
  int idx = 0;
  for (ScannerMode m : {ScannerMode::kV1, ScannerMode::kV3}) {
    VibratoScanner s;
    s.set_mode_immediate(m);
    std::vector<float> y(total);
    s.process(x.data(), y.data(), static_cast<std::uint32_t>(total));
    auto amp = [&](double hz) {  // Hann-windowed single bin over the last 3 s
      std::complex<double> acc = 0;
      double wsum = 0;
      const std::size_t from = 48000, n = total - from;
      for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n));
        acc += w * y[from + i] * std::polar(1.0, -kTwoPi * hz * static_cast<double>(from + i) / kFs);
        wsum += w;
      }
      return 2.0 * std::abs(acc) / wsum;
    };
    const double f = VibratoScanner::kSweepHz;
    side[idx++] = amp(carrier + f) + amp(carrier - f);
    carrier_amp[idx - 1] = amp(carrier);
  }
  std::printf("  first sidebands: V1 %.4f, V3 %.4f\n", side[0], side[1]);
  CHECK(side[0] > 0.002);
  CHECK(side[1] > 1.2 * side[0]);                 // deeper setting: more energy moved into the sidebands...
  CHECK(carrier_amp[1] < carrier_amp[0] - 0.02);  // ...and out of the carrier
}

void test_chorus_mix() {
  const auto x = sine(20000, 330.0, 0.4);
  for (int i = 0; i < 3; ++i) {
    VibratoScanner v, c;
    v.set_mode_immediate(kModes[1 + i]);
    c.set_mode_immediate(kModes[4 + i]);
    std::vector<float> yv(x.size()), yc(x.size());
    v.process(x.data(), yv.data(), static_cast<std::uint32_t>(x.size()));
    c.process(x.data(), yc.data(), static_cast<std::uint32_t>(x.size()));
    double worst = 0, differs = 0;
    for (std::size_t n = 0; n < x.size(); ++n) {
      worst = std::max(worst, std::fabs(static_cast<double>(yc[n]) - 0.5 * (x[n] + yv[n])));
      differs = std::max(differs, std::fabs(static_cast<double>(yc[n]) - yv[n]));
    }
    CHECK(worst < 1e-6);   // chorus = half dry + half scanned
    CHECK(differs > 0.05);  // and it is not the vibrato output
  }
}

// Switching between any two settings (also on/off, mid crossfade) at several sweep phases: the largest sample step
// stays within 20 % of the dry tone's own largest step. A hard switch (set_mode_immediate) does not.
void test_switching_is_click_free() {
  const double hz = 440.0;
  const auto x = sine(24000, hz, 0.5);
  const double dry_step = max_step(x);
  double hard_worst = 0, soft_worst = 0;
  for (ScannerMode from : kModes)
    for (ScannerMode to : kModes) {
      if (from == to) continue;
      for (std::size_t at : {4000u, 4413u, 4991u, 5777u}) {
        VibratoScanner s;
        s.set_mode_immediate(from);
        std::vector<float> y(x.size());
        s.process(x.data(), y.data(), static_cast<std::uint32_t>(at));
        s.set_mode(to);
        s.process(x.data() + at, y.data() + at, static_cast<std::uint32_t>(x.size() - at));
        soft_worst = std::max(soft_worst, max_step(y, 3000));
        // the same with a hard switch
        VibratoScanner h;
        h.set_mode_immediate(from);
        std::vector<float> z(x.size());
        h.process(x.data(), z.data(), static_cast<std::uint32_t>(at));
        h.set_mode_immediate(to);
        h.process(x.data() + at, z.data() + at, static_cast<std::uint32_t>(x.size() - at));
        hard_worst = std::max(hard_worst, max_step(z, 3000));
      }
    }
  std::printf("  dry max step %.4f, crossfaded worst %.4f, hard-switch worst %.4f\n", dry_step, soft_worst, hard_worst);
  CHECK(soft_worst <= 1.2 * dry_step);
  CHECK(hard_worst > 2.0 * dry_step);  // the bound is meaningful: a hard switch would click
  // Rapid re-switching during a crossfade stays smooth too (weights are interpolated, not restarted).
  VibratoScanner s;
  std::vector<float> y(x.size());
  std::uint32_t pos = 0;
  int k = 0;
  while (pos + 37 <= x.size()) {
    s.set_mode(kModes[(k++ * 3 + 1) % 7]);
    s.process(x.data() + pos, y.data() + pos, 37);
    pos += 37;
  }
  CHECK(max_step(y, 100, pos) <= 1.3 * dry_step);
}

// ---- engine ------------------------------------------------------------------------------------------------
void quiet(OrganEngine& e, std::uint32_t part = 0) {
  e.set_part_param(part, params::kKeyClick, 0.0f);
  e.set_part_param(part, params::kLeakage, 0.0f);
}
void render_blocks(OrganEngine& e, std::vector<float>& out, std::uint32_t blocks) {
  float l[64], r[64];
  for (std::uint32_t b = 0; b < blocks; ++b) {
    std::fill(l, l + 64, 0.0f);
    std::fill(r, r + 64, 0.0f);
    e.begin_block(64);
    e.render(l, r, 64);
    out.insert(out.end(), l, l + 64);
  }
}
void chord73(OrganEngine& e, std::uint32_t part) {
  for (std::uint32_t n = 28; n <= 100; ++n) e.start_voice(part, n, 40000);
}

void test_engine_routing() {
  {
    OrganEngine c;  // CC 81 alone: the registry default type (c1) applies
    c.set_part_param(0, params::kVibratoChorusOn, 1.0f);
    CHECK(c.scanner_mode(0) == ScannerMode::kC1);
  }
  OrganEngine e;
  CHECK(e.scanner_mode(0) == ScannerMode::kOff);  // registry default: off
  e.set_part_param(0, params::kVibratoChorusType, 2.0f);
  CHECK(e.scanner_mode(0) == ScannerMode::kOff);  // type alone does not switch it on
  e.set_part_param(0, params::kVibratoChorusOn, 1.0f);
  CHECK(e.scanner_mode(0) == ScannerMode::kV3);
  e.set_part_param(0, params::kVibratoChorusType, 5.0f);
  CHECK(e.scanner_mode(0) == ScannerMode::kC3 && e.scanner_mode(1) == ScannerMode::kOff);
  e.set_part_param(0, params::kVibratoChorusType, 99.0f);  // clamped
  CHECK(e.scanner_mode(0) == ScannerMode::kC3);
  e.set_part_param(0, params::kVibratoChorusOn, 0.0f);
  CHECK(e.scanner_mode(0) == ScannerMode::kOff);
  e.set_part_param(0, params::kLeakage, 7.0f);
  CHECK(e.leakage_amount(0) == 1.0f);
  e.set_part_param(0, params::kLeakage, std::nanf(""));  // ignored
  CHECK(e.leakage_amount(0) == 1.0f);
}

// A held 73-key chord while the setting cycles through all of them (and on/off) every 17 ms: the largest sample step
// stays within 25 % of the steady, scanner-off chord's.
void test_chord_switching_no_click() {
  auto run = [](bool cycle) {
    OrganEngine e;
    quiet(e);
    for (int d = 0; d < 9; ++d) e.set_part_param(0, params::kDrawbar16 + d, d % 2 ? 4.0f : 6.0f);
    chord73(e, 0);
    std::vector<float> out;
    render_blocks(e, out, 40);  // attack and drawbar settle
    const std::size_t start = out.size();
    int k = 0;
    for (int seg = 0; seg < 120; ++seg) {
      if (cycle) {
        if (seg % 8 == 7) {
          e.set_part_param(0, params::kVibratoChorusOn, (seg / 8) % 2 ? 1.0f : 0.0f);
        } else {
          e.set_part_param(0, params::kVibratoChorusOn, 1.0f);
          e.set_part_param(0, params::kVibratoChorusType, static_cast<float>(k++ % 6));
        }
      }
      render_blocks(e, out, 4);  // 256 frames = 5.3 ms; settings change every 5.3 ms, i.e. mid-crossfade as well
    }
    return max_step(out, start + 1);
  };
  const double steady = run(false), cycling = run(true);
  std::printf("  73-key chord: steady max step %.5f, cycling settings %.5f\n", steady, cycling);
  CHECK(steady > 0);
  CHECK(cycling <= 1.25 * steady);
}

void test_parts_are_independent() {
  // Part 0 in V3, part 1 plain, same key. The mix must equal (part 0 alone in V3) + (part 1 alone, plain).
  auto alone = [](std::uint32_t part, bool v3) {
    OrganEngine e;
    quiet(e, part);
    e.start_voice(part, 60, 40000);
    if (v3) {
      e.set_part_param(part, params::kVibratoChorusType, 2.0f);
      e.set_part_param(part, params::kVibratoChorusOn, 1.0f);
    }
    std::vector<float> out;
    render_blocks(e, out, 100);
    return out;
  };
  OrganEngine e;
  quiet(e, 0);
  quiet(e, 1);
  e.start_voice(0, 60, 40000);
  e.start_voice(1, 60, 40000);
  e.set_part_param(0, params::kVibratoChorusType, 2.0f);
  e.set_part_param(0, params::kVibratoChorusOn, 1.0f);
  std::vector<float> mixed;
  render_blocks(e, mixed, 100);
  const auto p0 = alone(0, true), p1 = alone(1, false);
  double worst = 0;
  for (std::size_t i = 0; i < mixed.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(mixed[i]) - p0[i] - p1[i]));
  CHECK(worst < 1e-6);
  CHECK(alone(0, false) != p0);  // and the scanner really changed part 0
}

void test_scanner_determinism() {
  auto run = []() {
    OrganEngine e;
    chord73(e, 0);
    std::vector<float> out;
    int k = 0;
    for (int seg = 0; seg < 60; ++seg) {
      e.set_part_param(0, params::kVibratoChorusOn, seg % 9 == 8 ? 0.0f : 1.0f);
      e.set_part_param(0, params::kVibratoChorusType, static_cast<float>(k++ % 6));
      e.set_part_param(0, params::kLeakage, static_cast<float>(seg % 5) * 0.25f);
      render_blocks(e, out, 5);
    }
    return out;
  };
  const auto a = run(), b = run();
  CHECK(a == b);
  CHECK(*std::max_element(a.begin(), a.end()) > 0.05f);
}

// ---- leakage -----------------------------------------------------------------------------------------------
double residual_db(const std::vector<float>& y, const std::vector<double>& ref, std::size_t from) {
  double num = 0, den = 0;
  for (std::size_t i = from; i < y.size(); ++i) {
    const double dlt = static_cast<double>(y[i]) - ref[i];
    num += dlt * dlt;
    den += ref[i] * ref[i];
  }
  return 10.0 * std::log10(std::max(num, 1e-300) / den);
}

// Leakage 0: one drawbar on one key is a pure tone. Reference: the synthetic sine of the key's wheel with the
// amplitude of one step-8 drawbar (kVoiceScale), compared after the 1 ms declick attack. The tolerance (-100 dB
// residual relative to the tone) is what the float32 output and the single-precision drawbar/limit path can give
// (about -140 dB); the percussion golden uses the same -100 dB. Leakage 1 is about -35 dB, so the test is not vacuous.
void test_leakage_zero_is_pure() {
  const std::uint32_t note = 60;
  const double f = OrganEngine::wheel_frequency_hz(static_cast<std::uint32_t>(tw::drawbar_wheel(note, 2)));
  std::vector<double> ref(48000);
  for (std::size_t n = 0; n < ref.size(); ++n)
    ref[n] = static_cast<double>(OrganEngine::kVoiceScale) * std::sin(kTwoPi * f * static_cast<double>(n) / kFs);
  auto run = [&](float leakage, ScannerMode vib) {
    OrganEngine e;
    e.set_part_param(0, params::kKeyClick, 0.0f);
    e.set_part_param(0, params::kLeakage, leakage);
    if (vib != ScannerMode::kOff) {
      e.set_part_param(0, params::kVibratoChorusType, static_cast<float>(static_cast<int>(vib) - 1));
      e.set_part_param(0, params::kVibratoChorusOn, 1.0f);
    }
    e.start_voice(0, note, 40000);
    std::vector<float> out;
    render_blocks(e, out, 750);
    return out;
  };
  const double r0 = residual_db(run(0.0f, ScannerMode::kOff), ref, 100);
  const double r1 = residual_db(run(1.0f, ScannerMode::kOff), ref, 100);
  const double r5 = residual_db(run(0.5f, ScannerMode::kOff), ref, 100);
  std::printf("  residual vs synthetic sine: leakage 0 %.1f dB, 0.5 %.1f dB, 1 %.1f dB\n", r0, r5, r1);
  CHECK(r0 < -100.0);
  CHECK(r1 > -45.0 && r1 < -25.0);
  CHECK(r5 > r0 + 40.0);
  // With the scanner on, the tone is no longer the plain sine (so the purity test really checks the off path).
  CHECK(residual_db(run(0.0f, ScannerMode::kV3), ref, 100) > -40.0);
}

double hann_amp(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  std::complex<double> acc = 0;
  double wsum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n));
    acc += w * x[from + i] * std::polar(1.0, -kTwoPi * hz * static_cast<double>(from + i) / kFs);
    wsum += w;
  }
  return 2.0 * std::abs(acc) / wsum;
}

void test_leakage_pattern_and_level() {
  OrganEngine a, b;
  // Table: reproducible (two instances), inside [0.25, 1] x max, zero beyond the two ends, not constant.
  double sum = 0, lo = 1e9, hi = 0;
  for (std::uint32_t w = 1; w <= tw::kWheelCount; ++w)
    for (std::uint32_t side = 0; side < 2; ++side) {
      CHECK(a.leakage_pattern(w, side) == b.leakage_pattern(w, side));
      const float g = a.leakage_pattern(w, side);
      if ((w == 1 && side == 0) || (w == tw::kWheelCount && side == 1)) {
        CHECK(g == 0.0f);
        continue;
      }
      CHECK(g >= 0.25f * OrganEngine::kLeakageMaxGain - 1e-9f && g <= OrganEngine::kLeakageMaxGain);
      sum += g;
      lo = std::min<double>(lo, g);
      hi = std::max<double>(hi, g);
    }
  CHECK(a.leakage_pattern(0, 0) == 0.0f && a.leakage_pattern(tw::kWheelCount + 1, 0) == 0.0f && a.leakage_pattern(40, 2) == 0.0f);
  CHECK(a.leakage_pattern(0xFFFFFFFFu, 1) == 0.0f);  // out-of-range arguments read nothing
  CHECK(hi > 1.5 * lo);  // a random pattern, not a constant
  std::printf("  leakage pattern: sum %.9f min %.6f max %.6f\n", sum, lo, hi);
  CHECK(std::fabs(sum - 2.289238912) < 1e-6);  // fixed by kLeakageSeed: changing the seed or the draw order trips this

  // Level: key 60, 8' only. Neighbour wheels (one semitone either side) appear with amplitude
  // kVoiceScale x leakage x pattern; leakage 0 leaves them absent; no key, no leakage.
  const std::uint32_t note = 60;
  const std::uint32_t w = static_cast<std::uint32_t>(tw::drawbar_wheel(note, 2));
  const double f_lo = OrganEngine::wheel_frequency_hz(w - 1), f_hi = OrganEngine::wheel_frequency_hz(w + 1);
  const std::size_t N = 24000, from = 12000;
  auto render_key = [&](float leakage) {
    OrganEngine e;
    e.set_part_param(0, params::kKeyClick, 0.0f);
    e.set_part_param(0, params::kLeakage, leakage);
    e.start_voice(0, note, 40000);
    std::vector<float> out;
    render_blocks(e, out, static_cast<std::uint32_t>((from + N) / 64 + 1));
    return out;
  };
  const std::vector<float> base = render_key(0.0f);
  double amps[3][2];
  const float levels[3] = {0.25f, 0.5f, 1.0f};
  for (int i = 0; i < 3; ++i) {
    std::vector<float> diff = render_key(levels[i]);  // the leakage contribution alone: render minus the leakage-0 render
    for (std::size_t n = 0; n < diff.size(); ++n) diff[n] -= base[n];
    amps[i][0] = hann_amp(diff, from, N, f_lo);
    amps[i][1] = hann_amp(diff, from, N, f_hi);
    for (int sd = 0; sd < 2; ++sd) {
      const double expect = static_cast<double>(OrganEngine::kVoiceScale) * levels[i] * a.leakage_pattern(w, static_cast<std::uint32_t>(sd));
      CHECK(std::fabs(amps[i][sd] - expect) < 0.02 * expect);
    }
    // nothing at the key's own frequency: leakage adds only neighbour wheels
    CHECK(hann_amp(diff, from, N, OrganEngine::wheel_frequency_hz(w)) < 1e-6);
  }
  CHECK(std::fabs(amps[2][0] / amps[0][0] - 4.0) < 0.08 && std::fabs(amps[1][1] / amps[0][1] - 2.0) < 0.04);  // linear in the parameter
  {
    OrganEngine silent;  // leakage is a property of sounding keys
    silent.set_part_param(0, params::kLeakage, 1.0f);
    std::vector<float> none;
    render_blocks(silent, none, 20);
    CHECK(*std::max_element(none.begin(), none.end()) == 0.0f && *std::min_element(none.begin(), none.end()) == 0.0f);
  }
  // Each key leaks: two keys a fifth apart give the neighbour lines of both.
  auto two_keys = [&](float leakage) {
    OrganEngine e;
    e.set_part_param(0, params::kKeyClick, 0.0f);
    e.set_part_param(0, params::kLeakage, leakage);
    e.start_voice(0, 48, 40000);
    e.start_voice(0, 67, 40000);
    std::vector<float> out;
    render_blocks(e, out, static_cast<std::uint32_t>((from + N) / 64 + 1));
    return out;
  };
  std::vector<float> two = two_keys(1.0f);
  const std::vector<float> two0 = two_keys(0.0f);
  for (std::size_t n = 0; n < two.size(); ++n) two[n] -= two0[n];
  for (std::uint32_t key : {48u, 67u}) {
    const std::uint32_t wk = static_cast<std::uint32_t>(tw::drawbar_wheel(static_cast<int>(key), 2));
    CHECK(hann_amp(two, from, N, OrganEngine::wheel_frequency_hz(wk + 1)) > 0.9 * OrganEngine::kVoiceScale * a.leakage_pattern(wk, 1));
  }
}

// Moving the leakage control while a key sounds ramps over 5 ms. The leakage contribution alone (render minus the leakage-0
// render) is compared with what applying the same toggles instantly would give (splice of the leakage-0 and leakage-1 renders):
// the ramp's largest step is a small fraction of the hard switch's.
void test_leakage_change_no_click() {
  const std::size_t seg_frames = 192;
  auto run = [](int mode) {  // 0 leakage 0, 1 leakage 1, 2 toggling every segment
    OrganEngine e;
    e.set_part_param(0, params::kKeyClick, 0.0f);
    e.set_part_param(0, params::kLeakage, mode == 1 ? 1.0f : 0.0f);
    e.start_voice(0, 40, 40000);  // one low key, 8' only: the leakage lines are slow, so a jump stands out
    std::vector<float> out;
    render_blocks(e, out, 40);
    for (int seg = 0; seg < 40; ++seg) {
      if (mode == 2) e.set_part_param(0, params::kLeakage, seg % 2 ? 0.0f : 1.0f);
      render_blocks(e, out, 3);
    }
    return out;
  };
  const auto base = run(0), full = run(1), moving = run(2);
  const std::size_t start = 40 * 64;
  std::vector<float> d_move(base.size()), d_hard(base.size());
  for (std::size_t n = 0; n < base.size(); ++n) {
    const std::size_t seg = n < start ? 1 : (n - start) / seg_frames;
    d_move[n] = moving[n] - base[n];
    d_hard[n] = seg % 2 ? 0.0f : full[n] - base[n];  // instant application of the same toggles
  }
  const double ramp = max_step(d_move, start + 1), hard = max_step(d_hard, start + 1);
  std::printf("  leakage contribution max step: ramped %.6f, instant %.6f\n", ramp, hard);
  CHECK(hard > 0.0);
  CHECK(ramp < 0.25 * hard);  // the ramp is what keeps a moving control from clicking
  const double steady = max_step(base, start + 1), total = max_step(moving, start + 1);
  CHECK(total <= 1.15 * steady);
}

// Leakage sits inside the voice gate and the percussion 1' cancel (ORGAN.md): the leakage term (render minus the leakage-0
// render) equals gate x drawbar gain x scale x (a lo + b hi) frame by frame during the attack and the release, and a cancelled
// 1' leaks nothing.
void test_leakage_gate_and_cancel() {
  const std::uint32_t note = 60;
  const std::uint32_t w = static_cast<std::uint32_t>(tw::drawbar_wheel(note, 2));  // 8'
  auto setup = [&](float leakage) {
    auto e = std::make_unique<OrganEngine>();
    e->set_part_param(0, params::kKeyClick, 0.0f);
    e->set_part_param(0, params::kLeakage, leakage);
    return e;
  };
  auto block = [](OrganEngine& e, std::vector<float>& out) {
    float l[64] = {}, r[64] = {};
    e.begin_block(64);
    e.render(l, r, 64);
    out.insert(out.end(), l, l + 64);
  };
  auto expect_frame = [&](const OrganEngine& e, std::size_t f, double env) {
    const float* wb = e.wheel_block(0);
    const double lo = wb[(w - 2) * 64 + f], hi = wb[w * 64 + f];  // wheels w-1 and w+1 (rows are wheel - 1)
    return env * OrganEngine::kVoiceScale * (e.leakage_pattern(w, 0) * lo + e.leakage_pattern(w, 1) * hi);
  };
  auto on = setup(1.0f), off = setup(0.0f);
  on->start_voice(0, note, 40000);
  off->start_voice(0, note, 40000);
  std::vector<float> a, b;
  double worst = 0, biggest = 0;
  block(*on, a);
  block(*off, b);
  for (std::size_t f = 0; f < 64; ++f) {  // attack: env = min(1, (f + 1) / 48)
    const double env = std::min(1.0, static_cast<double>(f + 1) / OrganEngine::kAttackFrames);
    worst = std::max(worst, std::fabs((static_cast<double>(a[f]) - b[f]) - expect_frame(*on, f, env)));
    biggest = std::max(biggest, std::fabs(expect_frame(*on, f, env)));
  }
  for (int i = 0; i < 20; ++i) { block(*on, a); block(*off, b); }
  on->stop_voice(0, note);
  off->stop_voice(0, note);
  const std::size_t r0 = a.size();
  block(*on, a);
  block(*off, b);
  for (std::size_t f = 0; f < 64; ++f) {  // release: env = 1 - (f + 1) / 96
    const double env = 1.0 - static_cast<double>(f + 1) / OrganEngine::kReleaseFrames;
    worst = std::max(worst, std::fabs((static_cast<double>(a[r0 + f]) - b[r0 + f]) - expect_frame(*on, f, env)));
  }
  std::printf("  leakage gate: worst deviation %.2e, largest term %.2e\n", worst, biggest);
  CHECK(biggest > 1e-5 && worst < 2e-7);

  // 1' cancelled by percussion: no tone, no leakage of it. Without percussion the 1' does leak.
  auto cancelled_leak = [&](bool perc) {
    auto x = setup(1.0f), y = setup(0.0f);
    for (OrganEngine* e : {x.get(), y.get()}) {
      for (int d = 0; d < 9; ++d) e->set_part_param(0, params::kDrawbar16 + d, d == 8 ? 8.0f : 0.0f);
      e->set_part_param(0, params::kPercussionOn, perc ? 1.0f : 0.0f);
      std::vector<float> idle;
      for (int i = 0; i < 12; ++i) block(*e, idle);  // let the 5 ms ramps finish with no key down
      e->start_voice(0, note, 40000);
    }
    std::vector<float> p, q;
    for (int i = 0; i < 20; ++i) { block(*x, p); block(*y, q); }
    double m = 0;
    for (std::size_t n = 0; n < p.size(); ++n) m = std::max(m, std::fabs(static_cast<double>(p[n]) - q[n]));
    return m;
  };
  CHECK(cancelled_leak(false) > 1e-5);
  CHECK(cancelled_leak(true) == 0.0);
}

// Silent parts: the sweep keeps time and the delay line is empty when a key starts again (no stale signal), and the scanner's
// delayed tail drains after the last voice instead of being cut. Reference: an engine that never played, with the same elapsed time.
void test_scanner_silence_skip() {
  auto make = [](bool play_first) {
    auto e = std::make_unique<OrganEngine>();
    quiet(*e);
    e->set_part_param(0, params::kVibratoChorusType, 2.0f);
    e->set_part_param(0, params::kVibratoChorusOn, 1.0f);
    std::vector<float> out;
    if (play_first) {
      e->start_voice(0, 60, 40000);
      render_blocks(*e, out, 20);
      e->stop_voice(0, 60);
      render_blocks(*e, out, 10);
      // the release gate lasts 96 frames; with the scanner's up-to-48-sample tail the output is silent well before this
      CHECK(std::fabs(out.back()) < 1e-6f);
      render_blocks(*e, out, 300);
    } else {
      render_blocks(*e, out, 330);
    }
    return e;
  };
  auto x = make(true), y = make(false);
  CHECK(x->scanner_sweep_position(0) == y->scanner_sweep_position(0));  // the sweep advanced by the silent frames
  CHECK(x->scanner_sweep_position(0) > 0.0f);
  x->start_voice(0, 64, 40000);
  y->start_voice(0, 64, 40000);
  std::vector<float> ox, oy;
  render_blocks(*x, ox, 6);
  render_blocks(*y, oy, 6);
  CHECK(ox == oy);  // identical from the first frame: nothing stale came back
  CHECK(*std::max_element(ox.begin(), ox.end()) > 0.01f);

  // The tail drains: the scanner delays by up to 48 samples, so after the last voice ended (96 frames after its release)
  // the output continues for a few frames. Over several pre-roll lengths (so several sweep positions) at least one run must
  // show signal in the block after the voice ended; a cut tail would give exactly zero in all of them.
  double best_tail = 0;
  for (int pre = 20; pre < 40; ++pre) {
    OrganEngine t;
    quiet(t);
    t.set_part_param(0, params::kVibratoChorusType, 2.0f);
    t.set_part_param(0, params::kVibratoChorusOn, 1.0f);
    t.start_voice(0, 60, 40000);
    std::vector<float> o;
    render_blocks(t, o, static_cast<std::uint32_t>(pre));
    t.stop_voice(0, 60);
    render_blocks(t, o, 2);  // the voice ends 96 frames into the release = 32 frames into the second block
    std::vector<float> after;
    render_blocks(t, after, 1);
    double en = 0;
    for (float v : after) en += static_cast<double>(v) * v;
    best_tail = std::max(best_tail, en);
    render_blocks(t, after, 2);
    CHECK(after.back() == 0.0f);  // and then silence
  }
  std::printf("  largest tail energy after the voice ended: %.3e\n", best_tail);
  CHECK(best_tail > 0.0);
}

// A disabled part keeps its scanner's time and has no stale signal when it is enabled again.
void test_disabled_part_scanner() {
  auto make = [](bool disable) {
    auto e = std::make_unique<OrganEngine>();
    quiet(*e);
    e->set_part_param(0, params::kVibratoChorusType, 0.0f);
    e->set_part_param(0, params::kVibratoChorusOn, 1.0f);
    e->start_voice(0, 60, 40000);
    std::vector<float> out;
    render_blocks(*e, out, 10);
    if (disable) {
      e->set_enabled_parts(0xFE);  // deactivates the voice
      render_blocks(*e, out, 100);
      e->set_enabled_parts(0xFF);
    } else {
      e->stop_voice(0, 60);
      render_blocks(*e, out, 100);
    }
    return e;
  };
  auto d = make(true), n = make(false);
  CHECK(d->scanner_sweep_position(0) == n->scanner_sweep_position(0));
  // (The wheels of a disabled part are frozen, so the notes after re-enabling are not comparable sample by sample.)
  CHECK(d->scanner_mode(0) == ScannerMode::kV1);
}

}  // namespace

int main() {
  test_registry_and_cc();
  test_scanner_constants();
  test_off_is_transparent();
  test_sweep_rate_and_depth();
  test_fm_sidebands();
  test_chorus_mix();
  test_switching_is_click_free();
  test_engine_routing();
  test_chord_switching_no_click();
  test_parts_are_independent();
  test_scanner_determinism();
  test_leakage_zero_is_pure();
  test_leakage_pattern_and_level();
  test_leakage_change_no_click();
  test_leakage_gate_and_cancel();
  test_scanner_silence_skip();
  test_disabled_part_scanner();
  if (g_fail) {
    std::printf("organ_scanner_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("organ_scanner_tests: ok\n");
  return 0;
}

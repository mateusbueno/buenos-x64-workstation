// Rotary speaker tests (Story 2.6). Bare main, CHECK style as the other core tests. Part 1 tests the RotarySpeaker DSP unit alone
// (crossover, amplitude modulation, Doppler, stereo spread, ramp time constants, stop, click-free, determinism); part 2 tests it
// inside the organ (registry and CC, bypass is bit-exact, speed changes at the event frame, state publication, click-free
// switching under a held chord, reservation).
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/rotary_speaker.hpp"

using namespace velora::core;

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
// Per-part peak bound with the rotary stage on: the stage output is soft-limited like the dry signal, so it stays strictly below 1.0 per
// channel (measured peak on the loudest chord is printed by test_peak_bound; without the limiter it reached about 3.0).
constexpr float kRotaryPartPeakBound = 1.0f;

struct Stereo {
  std::vector<float> l, r;
};

std::vector<float> sine(std::size_t n, double hz, double amp) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / kFs));
  return x;
}

// Runs the unit over `in` in blocks of 64 frames.
Stereo run(RotarySpeaker& rot, const std::vector<float>& in) {
  Stereo o;
  o.l.assign(in.size(), 0.0f);
  o.r.assign(in.size(), 0.0f);
  for (std::size_t i = 0; i < in.size(); i += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, in.size() - i));
    rot.process(&in[i], &o.l[i], &o.r[i], n);
  }
  return o;
}

double max_step(const std::vector<float>& x, std::size_t from = 1, std::size_t to = 0) {
  if (to == 0 || to > x.size()) to = x.size();
  double m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(x[i]) - x[i - 1]));
  return m;
}

// RMS envelope: window `win` samples, hop `hop`.
std::vector<double> envelope(const std::vector<float>& x, std::size_t from, std::size_t win, std::size_t hop) {
  std::vector<double> e;
  for (std::size_t i = from; i + win <= x.size(); i += hop) {
    double s = 0;
    for (std::size_t k = 0; k < win; ++k) s += static_cast<double>(x[i + k]) * x[i + k];
    e.push_back(std::sqrt(s / static_cast<double>(win)));
  }
  return e;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// Modulation rate of an envelope that goes through one cycle per rotation: complete cycles counted with a Schmitt trigger (hysteresis of
// 20 % of the peak-to-peak range around the mean, so ripple near the mean is not counted) over the span between the first and the
// last upward trigger.
double modulation_hz(const std::vector<double>& e, double hop_seconds) {
  const double mx = *std::max_element(e.begin(), e.end()), mn = *std::min_element(e.begin(), e.end());
  const double mid = 0.5 * (mx + mn), h = 0.2 * (mx - mn);
  bool low = false;
  long first = -1, last = -1;
  int cycles = 0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    if (e[i] < mid - h) low = true;
    if (low && e[i] > mid + h) {
      low = false;
      if (first < 0) first = static_cast<long>(i);
      else ++cycles;
      last = static_cast<long>(i);
    }
  }
  if (cycles == 0) return 0.0;
  return cycles / (hop_seconds * static_cast<double>(last - first));
}

// A rotor-only configuration helper: a config whose horn and drum are the same (so a broadband test sees one rotor).
RotaryConfig no_motion_config() {
  RotaryConfig c;
  c.horn.am_depth = c.drum.am_depth = 0.0f;
  c.horn.radius_m = c.drum.radius_m = 0.0;
  return c;
}

// ----------------------------------------------------------------------------------------------------------------------------
void test_design_values() {
  const RotaryConfig c;
  CHECK(c.horn.fast_hz >= 6.0 && c.horn.fast_hz <= 7.0);        // public figure: fast about 6-7 Hz (horn)
  CHECK(c.drum.fast_hz < c.horn.fast_hz && c.drum.fast_hz > 5.0);
  CHECK(c.horn.slow_hz > c.drum.slow_hz && c.horn.slow_hz < 1.0);  // slow about 0.7-0.8 Hz
  CHECK(c.horn.accel_tau_s < c.drum.accel_tau_s);                // horn faster than drum, both ways
  CHECK(c.horn.brake_tau_s < c.drum.brake_tau_s);
  CHECK(c.crossover_hz == 800.0);
  CHECK(RotarySpeaker::kCostUnits == OrganEngine::kRotaryCostUnits);
  CHECK(static_cast<int>(RotarySpeed::kStop) == 0 && static_cast<int>(RotarySpeed::kSlow) == 1 && static_cast<int>(RotarySpeed::kFast) == 2);
}

// With the rotors standing still and no modulation depth the two bands add back to an all-pass: flat magnitude, identical channels.
void test_crossover_reconstruction() {
  RotarySpeaker rot(no_motion_config());
  rot.set_speed_immediate(RotarySpeed::kStop);
  double worst_db = 0.0, worst_lr = 0.0;
  for (double f : {60.0, 100.0, 200.0, 400.0, 600.0, 800.0, 1000.0, 1500.0, 3000.0, 6000.0, 10000.0, 16000.0}) {
    rot.clear_signal();
    const std::vector<float> x = sine(24000, f, 0.5);
    const Stereo y = run(rot, x);
    const double gain = rms(y.l, 12000, 24000) / rms(x, 12000, 24000);
    worst_db = std::max(worst_db, std::fabs(20.0 * std::log10(gain)));
    for (std::size_t i = 12000; i < 24000; ++i) worst_lr = std::max(worst_lr, static_cast<double>(std::fabs(y.l[i] - y.r[i])));
  }
  CHECK(worst_db < 0.1);   // flat within +-0.1 dB from 60 Hz to 16 kHz, crossover included
  CHECK(worst_lr == 0.0);  // no stereo difference without depth and distance
  // Transparent to an impulse's energy too: the all-pass keeps the energy.
  RotarySpeaker r2(no_motion_config());
  r2.set_speed_immediate(RotarySpeed::kStop);
  std::vector<float> imp(4096, 0.0f);
  imp[0] = 1.0f;
  const Stereo yi = run(r2, imp);
  double e = 0;
  for (float v : yi.l) e += static_cast<double>(v) * v;
  CHECK(std::fabs(e - 1.0) < 0.02);
}

// Standing still at angle 0 the output has the documented per-band directivity gain (1 - depth * (1 - cos 60 deg) / 2), identical
// in both channels, at a frequency well inside each band.
void test_rest_gain() {
  const RotaryConfig c;
  RotarySpeaker rot;
  rot.set_speed_immediate(RotarySpeed::kStop);
  const std::vector<float> lo = sine(24000, 150.0, 0.5), hi = sine(24000, 5000.0, 0.5);
  const Stereo yl = run(rot, lo);
  rot.clear_signal();
  const Stereo yh = run(rot, hi);
  const double gd = 1.0 - c.drum.am_depth * (1.0 - std::cos(c.mic_half_angle_rad)) * 0.5;
  const double gh = 1.0 - c.horn.am_depth * (1.0 - std::cos(c.mic_half_angle_rad)) * 0.5;
  CHECK(std::fabs(rms(yl.l, 12000, 24000) / rms(lo, 12000, 24000) - gd) < 0.01);
  CHECK(std::fabs(rms(yh.l, 12000, 24000) / rms(hi, 12000, 24000) - gh) < 0.01);
  CHECK(std::fabs(rms(yl.l, 12000, 24000) - rms(yl.r, 12000, 24000)) < 1e-4);
  CHECK(std::fabs(rms(yh.l, 12000, 24000) - rms(yh.r, 12000, 24000)) < 1e-4);
  // Standing still, the amplitude is constant over time (nothing modulates).
  const std::vector<double> e = envelope(yh.l, 12000, 480, 480);
  CHECK(*std::max_element(e.begin(), e.end()) - *std::min_element(e.begin(), e.end()) < 1e-4);
}

// Amplitude modulation depth and rate at slow and fast, measured on a test tone in each band.
void test_am_depth_and_rate() {
  const RotaryConfig c;
  struct Case {
    double tone_hz;
    const RotorSpec* spec;
    RotarySpeed speed;
  } cases[] = {{4000.0, &c.horn, RotarySpeed::kSlow}, {4000.0, &c.horn, RotarySpeed::kFast},
               {200.0, &c.drum, RotarySpeed::kSlow}, {200.0, &c.drum, RotarySpeed::kFast}};
  for (const Case& k : cases) {
    RotarySpeaker rot;
    rot.set_speed_immediate(k.speed);
    const double hz = RotarySpeaker::target_hz(*k.spec, k.speed);
    const std::size_t n = static_cast<std::size_t>(kFs * (k.speed == RotarySpeed::kSlow ? 8.0 : 2.0));
    const Stereo y = run(rot, sine(n, k.tone_hz, 0.5));
    const std::vector<double> e = envelope(y.l, 4800, 240, 48);  // 5 ms windows: whole cycles of both test tones
    const double mx = *std::max_element(e.begin(), e.end()), mn = *std::min_element(e.begin(), e.end());
    CHECK(std::fabs(mn / mx - (1.0 - k.spec->am_depth)) < 0.03);  // gain goes from 1 (facing the microphone) to 1 - depth
    CHECK(std::fabs(mx / 0.3535534 - 1.0) < 0.03);                // peak is the unmodulated level (0.5 / sqrt 2)
    const double rate = modulation_hz(e, 48.0 / kFs);
    if (!(std::fabs(rate / hz - 1.0) < 0.05)) std::printf("rate %.3f expected %.3f (tone %.0f)\n", rate, hz, k.tone_hz);
    CHECK(std::fabs(rate / hz - 1.0) < 0.05);                     // one amplitude cycle per rotation
  }
  // Slower rotation gives a slower modulation (fast is about 8.5 x slow for the horn).
  CHECK(c.horn.fast_hz / c.horn.slow_hz > 8.0);
}

// Doppler: a fixed tone in the horn band deviates in pitch by at most the radiator's peak speed over the speed of sound.
void test_doppler() {
  const RotaryConfig c;
  const double f0 = 4000.0;
  auto deviation = [&](RotarySpeed sp) {
    RotarySpeaker rot;
    rot.set_speed_immediate(sp);
    const std::size_t n = static_cast<std::size_t>(kFs * 1.5);
    const Stereo y = run(rot, sine(n, f0, 0.5));
    // I/Q demodulation at f0 over 240-sample windows (20 whole carrier cycles), phase advance per 48-sample hop -> frequency.
    std::vector<std::complex<double>> cum(n + 1);
    for (std::size_t i = 0; i < n; ++i) cum[i + 1] = cum[i] + static_cast<double>(y.l[i]) * std::polar(1.0, -kTwoPi * f0 * static_cast<double>(i) / kFs);
    double worst = 0, prev = 0;
    bool have = false;
    for (std::size_t i = 4800; i + 240 < n; i += 48) {
      const double ph = std::arg(cum[i + 240] - cum[i]);
      if (have) {
        double d = ph - prev;
        while (d > kTwoPi / 2) d -= kTwoPi;
        while (d < -kTwoPi / 2) d += kTwoPi;
        worst = std::max(worst, std::fabs(d / (kTwoPi * 48.0 / kFs)) / f0);
      }
      prev = ph;
      have = true;
    }
    return worst;
  };
  const double bound = kTwoPi * c.horn.fast_hz * c.horn.radius_m / RotarySpeaker::kSpeedOfSoundMps;  // peak v / c
  const double fast = deviation(RotarySpeed::kFast), slow = deviation(RotarySpeed::kSlow), stop = deviation(RotarySpeed::kStop);
  CHECK(fast <= bound * 1.05);                       // bounded by the geometry (about 2 percent, 36 cents)
  CHECK(fast >= bound * 0.7);                        // and the effect is really there
  CHECK(slow < bound * (c.horn.slow_hz / c.horn.fast_hz) * 1.2);  // proportional to the rotation speed
  CHECK(stop < 1e-4);                                // a rotor at rest shifts nothing
  std::printf("rotary_tests: horn Doppler fast %.3f %% (bound %.3f %%), slow %.3f %%\n", fast * 100, bound * 100, slow * 100);
}

// Stereo spread: at fast the two channels are modulated out of phase and differ in level.
void test_stereo_spread() {
  RotarySpeaker rot;
  rot.set_speed_immediate(RotarySpeed::kFast);
  const Stereo y = run(rot, sine(static_cast<std::size_t>(kFs * 2.0), 4000.0, 0.5));
  const std::vector<double> el = envelope(y.l, 4800, 240, 48), er = envelope(y.r, 4800, 240, 48);
  double ml = 0, mr = 0;
  for (std::size_t i = 0; i < el.size(); ++i) { ml += el[i]; mr += er[i]; }
  ml /= static_cast<double>(el.size());
  mr /= static_cast<double>(er.size());
  double num = 0, dl = 0, dr = 0, worst_db = 0;
  for (std::size_t i = 0; i < el.size(); ++i) {
    num += (el[i] - ml) * (er[i] - mr);
    dl += (el[i] - ml) * (el[i] - ml);
    dr += (er[i] - mr) * (er[i] - mr);
    worst_db = std::max(worst_db, std::fabs(20.0 * std::log10(el[i] / er[i])));
  }
  const double corr = num / std::sqrt(dl * dr);
  CHECK(corr < -0.3);        // microphones 120 degrees apart: the envelopes are about cos(120 deg) = -0.5 correlated
  CHECK(worst_db > 3.0);     // and the channels differ in level by several dB at times
  std::printf("rotary_tests: stereo envelope correlation %.2f, max level difference %.1f dB\n", corr, worst_db);
}

// First sample at which a speed comes within `thresh` of its start-to-target path, found by skipping frame by frame.
template <typename Get>
double time_to_fraction(RotarySpeaker& rot, Get get, double level, bool rising, double max_seconds) {
  for (std::uint32_t f = 1; f <= static_cast<std::uint32_t>(max_seconds * kFs); ++f) {
    rot.skip(1);
    if (rising ? get() >= level : get() <= level) return f / kFs;
  }
  return -1.0;
}

// Independent acceleration and braking time constants for the horn and the drum, measured on the speed.
void test_time_constants() {
  const RotaryConfig c;
  const double e1 = 1.0 - std::exp(-1.0);  // 63.2 %
  struct R {
    const RotorSpec* spec;
    bool horn;
  } rotors[] = {{&c.horn, true}, {&c.drum, false}};
  double tau_acc[2], tau_brk[2], tau_stop[2];
  for (int k = 0; k < 2; ++k) {
    const RotorSpec& s = *rotors[k].spec;
    auto get = [&](RotarySpeaker& r) { return rotors[k].horn ? r.horn_hz() : r.drum_hz(); };
    {  // slow -> fast: rises to 63.2 % of the way after one acceleration time constant
      RotarySpeaker rot;
      rot.set_speed_immediate(RotarySpeed::kSlow);
      rot.set_target(RotarySpeed::kFast);
      tau_acc[k] = time_to_fraction(rot, [&] { return get(rot); }, s.slow_hz + e1 * (s.fast_hz - s.slow_hz), true, 60.0);
      CHECK(std::fabs(tau_acc[k] / s.accel_tau_s - 1.0) < 0.01);
      // and it settles on the fast speed
      rot.skip(static_cast<std::uint32_t>(12 * s.accel_tau_s * kFs));
      CHECK(std::fabs(get(rot) - s.fast_hz) < 0.01 * s.fast_hz);
    }
    {  // fast -> slow: falls to 36.8 % of the distance after one braking time constant
      RotarySpeaker rot;
      rot.set_speed_immediate(RotarySpeed::kFast);
      rot.set_target(RotarySpeed::kSlow);
      tau_brk[k] = time_to_fraction(rot, [&] { return get(rot); }, s.slow_hz + (1.0 - e1) * (s.fast_hz - s.slow_hz), false, 60.0);
      CHECK(std::fabs(tau_brk[k] / s.brake_tau_s - 1.0) < 0.01);
    }
    {  // fast -> stop aims below zero so that it arrives: (speed + overshoot) falls by 1/e after one braking time constant
      RotarySpeaker rot;
      rot.set_speed_immediate(RotarySpeed::kFast);
      rot.set_target(RotarySpeed::kStop);
      const double ov = RotarySpeaker::kStopOvershootHz;
      tau_stop[k] = time_to_fraction(rot, [&] { return get(rot); }, (s.fast_hz + ov) * (1.0 - e1) - ov, false, 60.0);
      CHECK(std::fabs(tau_stop[k] / s.brake_tau_s - 1.0) < 0.01);
    }
  }
  CHECK(tau_acc[0] < tau_acc[1] && tau_brk[0] < tau_brk[1] && tau_stop[0] < tau_stop[1]);  // horn is the faster rotor
  CHECK(tau_brk[0] > tau_acc[0] && tau_brk[1] > tau_acc[1]);                                 // braking is slower than speeding up
  // Both rotors are independent: after the same time the horn is at its target and the drum is not.
  RotarySpeaker rot;
  rot.set_speed_immediate(RotarySpeed::kSlow);
  rot.set_target(RotarySpeed::kFast);
  rot.skip(static_cast<std::uint32_t>(3.0 * kFs));
  CHECK(rot.horn_hz() > 0.98 * c.horn.fast_hz);
  CHECK(rot.drum_hz() < 0.85 * c.drum.fast_hz && rot.drum_hz() > c.drum.slow_hz);
}

// STOP: brakes both rotors to a standstill in finite time (monotone), leaves them where they stopped, and stays there.
void test_stop_standstill() {
  RotarySpeaker rot;
  rot.set_speed_immediate(RotarySpeed::kFast);
  rot.set_target(RotarySpeed::kStop);
  double prev_h = rot.horn_hz(), prev_d = rot.drum_hz();
  double horn_stopped = -1, drum_stopped = -1;
  bool monotone = true;
  for (std::uint32_t f = 1; f <= static_cast<std::uint32_t>(30 * kFs); ++f) {
    rot.skip(1);
    monotone = monotone && rot.horn_hz() <= prev_h && rot.drum_hz() <= prev_d;
    prev_h = rot.horn_hz();
    prev_d = rot.drum_hz();
    if (horn_stopped < 0 && rot.horn_hz() == 0.0) horn_stopped = f / kFs;
    if (drum_stopped < 0 && rot.drum_hz() == 0.0) drum_stopped = f / kFs;
  }
  CHECK(monotone);
  CHECK(horn_stopped > 0 && drum_stopped > 0);        // both reach exactly 0
  CHECK(drum_stopped < 16.0);                         // drum: 10 s of braking plus at most 4 s of creep to the rest angle
  CHECK(horn_stopped < 9.0);
  const double ah = rot.horn_angle_cycles(), ad = rot.drum_angle_cycles();
  CHECK(ah == 0.0 && ad == 0.0);                      // both rotors rest at the same angle, 0
  rot.skip(48000);
  CHECK(rot.horn_angle_cycles() == ah && rot.drum_angle_cycles() == ad);  // and stay there
  // From standstill, slow starts again.
  rot.set_target(RotarySpeed::kSlow);
  rot.skip(48000);
  CHECK(rot.horn_hz() > 0.5 && rot.horn_angle_cycles() != ah);
  // Whatever the angles when the stop starts, both rotors end at the rest angle and the bands add up as at a fresh rest: the response
  // around the crossover equals that of a unit that never moved (no comb, no level dip), and the final approach is click-free.
  RotarySpeaker fresh;
  fresh.set_speed_immediate(RotarySpeed::kStop);
  const double starts[][2] = {{0.0, 0.0}, {0.3, 0.9}, {0.5, 0.1}, {0.77, 0.42}, {0.99, 0.5}};
  const std::vector<float> two_tone = [] {
    std::vector<float> x(static_cast<std::size_t>(kFs * 24.0));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.4 * std::sin(kTwoPi * 220.0 * i / kFs) + 0.4 * std::sin(kTwoPi * 1760.0 * i / kFs));
    return x;
  }();
  double steady_fast;
  {
    RotarySpeaker f;
    f.set_speed_immediate(RotarySpeed::kFast);
    const Stereo y = run(f, two_tone);
    steady_fast = std::max(max_step(y.l, 4800), max_step(y.r, 4800));
  }
  for (const auto& st : starts) {
    RotarySpeaker r;
    r.set_speed_immediate(RotarySpeed::kFast);
    r.set_angles(st[0], st[1]);
    r.set_target(RotarySpeed::kStop);
    const Stereo y = run(r, two_tone);  // 24 s: well past the standstill
    CHECK(r.horn_hz() == 0.0 && r.drum_hz() == 0.0 && r.horn_angle_cycles() == 0.0 && r.drum_angle_cycles() == 0.0);
    CHECK(std::max(max_step(y.l, 0), max_step(y.r, 0)) <= steady_fast * 1.02);  // click-free including the final approach
    for (double f : {500.0, 700.0, 800.0, 900.0, 1200.0}) {
      const std::vector<float> x = sine(24000, f, 0.5);
      r.clear_signal();
      fresh.clear_signal();
      const Stereo a = run(r, x), b = run(fresh, x);
      CHECK(std::fabs(rms(a.l, 12000, 24000) - rms(b.l, 12000, 24000)) < 1e-4);
      CHECK(std::fabs(rms(a.r, 12000, 24000) - rms(b.r, 12000, 24000)) < 1e-4);
    }
  }
  // set_angles wraps with floor: negative, large and non-finite values.
  rot.set_angles(-0.25, 2.75);
  CHECK(rot.horn_angle_cycles() == 0.75 && rot.drum_angle_cycles() == 0.75);
  rot.set_angles(std::nan(""), HUGE_VAL);
  CHECK(rot.horn_angle_cycles() == 0.0 && rot.drum_angle_cycles() == 0.0);
  rot.set_angles(-1e-20, 1.0);
  CHECK(rot.horn_angle_cycles() >= 0.0 && rot.horn_angle_cycles() < 1.0 && rot.drum_angle_cycles() == 0.0);
  std::printf("rotary_tests: stop from fast: horn %.2f s, drum %.2f s\n", horn_stopped, drum_stopped);
}

// Click-free: the largest sample-to-sample step during slow -> fast -> stop -> slow stays within what the fastest steady rotation
// produces on its own; an abrupt jump of the rotor angle is the contrast (a real discontinuity).
void test_click_free() {
  std::vector<float> in(static_cast<std::size_t>(kFs * 24.0));
  for (std::size_t i = 0; i < in.size(); ++i) {
    const double t = static_cast<double>(i) / kFs;
    in[i] = static_cast<float>(0.4 * std::sin(kTwoPi * 220.0 * t) + 0.4 * std::sin(kTwoPi * 1760.0 * t));
  }
  // Steady fast reference.
  double steady_fast;
  {
    RotarySpeaker rot;
    rot.set_speed_immediate(RotarySpeed::kFast);
    const Stereo y = run(rot, in);
    steady_fast = std::max(max_step(y.l, 4800), max_step(y.r, 4800));
  }
  RotarySpeaker rot;
  Stereo y;
  y.l.assign(in.size(), 0.0f);
  y.r.assign(in.size(), 0.0f);
  const struct { double t; RotarySpeed s; } plan[] = {{0.0, RotarySpeed::kSlow}, {2.0, RotarySpeed::kFast}, {6.0, RotarySpeed::kStop},
                                                       {16.0, RotarySpeed::kSlow}, {18.0, RotarySpeed::kFast}, {19.0, RotarySpeed::kStop},
                                                       {19.5, RotarySpeed::kSlow}};
  rot.set_speed_immediate(RotarySpeed::kSlow);
  std::size_t k = 0;
  for (std::size_t i = 0; i < in.size(); i += 64) {
    // A speed change lands on the exact frame: split the block where the plan says.
    std::size_t at = i;
    while (at < i + 64) {
      std::size_t next = i + 64;
      if (k < std::size(plan) && static_cast<std::size_t>(plan[k].t * kFs) < next) next = std::max(at, static_cast<std::size_t>(plan[k].t * kFs));
      if (next > at) rot.process(&in[at], &y.l[at], &y.r[at], static_cast<std::uint32_t>(next - at));
      at = next;
      if (k < std::size(plan) && at == static_cast<std::size_t>(plan[k].t * kFs)) rot.set_target(plan[k++].s);
      if (next == i + 64) break;
    }
  }
  const double transitions = std::max(max_step(y.l, 4800), max_step(y.r, 4800));
  CHECK(transitions <= steady_fast * 1.02);
  // The contrast: jump the horn to the opposite angle in the middle of a steady stream.
  RotarySpeaker bad;
  bad.set_speed_immediate(RotarySpeed::kStop);
  Stereo z;
  z.l.assign(in.size() / 4, 0.0f);
  z.r.assign(in.size() / 4, 0.0f);
  for (std::size_t i = 0; i < z.l.size(); i += 64) {
    if (i == 64 * 100) bad.set_angles(0.5, 0.5);
    bad.process(&in[i], &z.l[i], &z.r[i], 64);
  }
  CHECK(max_step(z.l, 4800) > steady_fast * 1.3);
  std::printf("rotary_tests: max step steady fast %.4f, through the speed changes %.4f, abrupt angle jump %.4f\n", steady_fast,
              transitions, max_step(z.l, 4800));
}

void test_determinism_and_blocks() {
  std::vector<float> in(static_cast<std::size_t>(kFs * 2.0));
  for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(0.5 * std::sin(0.01 * static_cast<double>(i * i % 7919)));
  auto render = [&](std::size_t chunk) {
    RotarySpeaker rot;
    rot.set_speed_immediate(RotarySpeed::kSlow);
    Stereo y;
    y.l.assign(in.size(), 0.0f);
    y.r.assign(in.size(), 0.0f);
    for (std::size_t i = 0; i < in.size(); i += chunk) {
      if (i / chunk == 40) rot.set_target(RotarySpeed::kFast);
      const std::uint32_t n = static_cast<std::uint32_t>(std::min(chunk, in.size() - i));
      rot.process(&in[i], &y.l[i], &y.r[i], n);
    }
    return y;
  };
  const Stereo a = render(64), b = render(64);
  CHECK(std::memcmp(a.l.data(), b.l.data(), a.l.size() * 4) == 0 && std::memcmp(a.r.data(), b.r.data(), a.r.size() * 4) == 0);
  // Chunking does not change the signal path: with the same frame for the speed change the output is bit-identical.
  const Stereo c64 = render(64), c32 = [&] {
    RotarySpeaker rot;
    rot.set_speed_immediate(RotarySpeed::kSlow);
    Stereo y;
    y.l.assign(in.size(), 0.0f);
    y.r.assign(in.size(), 0.0f);
    for (std::size_t i = 0; i < in.size(); i += 32) {
      if (i == 40 * 64) rot.set_target(RotarySpeed::kFast);
      rot.process(&in[i], &y.l[i], &y.r[i], static_cast<std::uint32_t>(std::min<std::size_t>(32, in.size() - i)));
    }
    return y;
  }();
  CHECK(std::memcmp(c64.l.data(), c32.l.data(), c64.l.size() * 4) == 0);
  // skip() and process() advance the rotors identically.
  RotarySpeaker p, s;
  p.set_speed_immediate(RotarySpeed::kSlow);
  s.set_speed_immediate(RotarySpeed::kSlow);
  p.set_target(RotarySpeed::kFast);
  s.set_target(RotarySpeed::kFast);
  std::vector<float> zero(64, 0.0f), ol(64), orr(64);
  for (int i = 0; i < 500; ++i) {
    p.process(zero.data(), ol.data(), orr.data(), 64);
    s.skip(64);
  }
  CHECK(p.horn_hz() == s.horn_hz() && p.drum_hz() == s.drum_hz() && p.horn_angle_cycles() == s.horn_angle_cycles() &&
        p.drum_angle_cycles() == s.drum_angle_cycles());
}

// The unit is finite and drains: an impulse leaves nothing behind after kTailFrames.
void test_tail_and_bounds() {
  RotarySpeaker rot;
  rot.set_speed_immediate(RotarySpeed::kFast);
  std::vector<float> in(8192, 0.0f);
  in[0] = 1.0f;
  const Stereo y = run(rot, in);
  double after = 0, peak = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    CHECK(std::isfinite(y.l[i]) && std::isfinite(y.r[i]));
    peak = std::max({peak, static_cast<double>(std::fabs(y.l[i])), static_cast<double>(std::fabs(y.r[i]))});
    if (i >= RotarySpeaker::kTailFrames) after = std::max({after, static_cast<double>(std::fabs(y.l[i])), static_cast<double>(std::fabs(y.r[i]))});
  }
  CHECK(after < 1e-6);
  CHECK(peak < 1.5);
  // Extreme configuration values are clamped into what the fixed storage can take.
  RotaryConfig c;
  c.horn.radius_m = 100.0;
  c.drum.radius_m = -1.0;
  c.crossover_hz = 1.0;
  RotarySpeaker wild(c);
  CHECK(wild.max_delay_samples() < RotarySpeaker::kDelayLine);  // the largest delay the rotors can ask for (plus the interpolator margin) fits the line
  CHECK(RotarySpeaker().max_delay_samples() < RotarySpeaker::kDelayLine);
  for (double mic : {0.0, 0.5, 1.5707963, 3.0, -2.0, 1e9}) {  // any microphone angle, with the biggest radii, stays inside
    RotaryConfig w;
    w.horn.radius_m = w.drum.radius_m = 1e9;
    w.mic_half_angle_rad = mic;
    CHECK(RotarySpeaker(w).max_delay_samples() < RotarySpeaker::kDelayLine);
    w.horn.radius_m = 1e9;
    w.drum.radius_m = 0.0;
    CHECK(RotarySpeaker(w).max_delay_samples() < RotarySpeaker::kDelayLine);
    w.horn.radius_m = 0.0;
    w.drum.radius_m = 1e9;
    CHECK(RotarySpeaker(w).max_delay_samples() < RotarySpeaker::kDelayLine);
  }
  RotaryConfig nan_cfg;
  nan_cfg.horn.radius_m = std::nan("");
  nan_cfg.mic_half_angle_rad = std::nan("");
  nan_cfg.crossover_hz = std::nan("");
  CHECK(RotarySpeaker(nan_cfg).max_delay_samples() < RotarySpeaker::kDelayLine);
  wild.set_speed_immediate(RotarySpeed::kFast);
  const Stereo yw = run(wild, sine(48000, 300.0, 0.5));
  bool finite = true;
  for (std::size_t i = 0; i < yw.l.size(); ++i) finite = finite && std::isfinite(yw.l[i]) && std::isfinite(yw.r[i]);
  CHECK(finite);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Inside the organ
struct Rig {
  std::unique_ptr<OrganEngine> organ = std::make_unique<OrganEngine>();
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(8);
  std::vector<float> l, r;
  Rig() {
    br->bind_engine(*organ);
    for (std::uint32_t ch = 0; ch < 8; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
    param(params::kKeyClick, 0, 0.0);
    param(params::kLeakage, 0, 0.0);
    param(params::kLeakage, 1, 0.0);
    param(params::kKeyClick, 1, 0.0);
  }
  void param(std::uint32_t id, std::uint32_t part, double v, std::uint32_t at = 0) {
    br->submit({make_param_event(*params::find(id), part, v), at});
  }
  void block() {
    float a[kBlockFrames], b[kBlockFrames];
    br->process_block(a, b);
    l.insert(l.end(), a, a + kBlockFrames);
    r.insert(r.end(), b, b + kBlockFrames);
  }
  void blocks(int n) {
    for (int i = 0; i < n; ++i) block();
  }
  void chord(std::uint32_t part, std::initializer_list<std::uint32_t> notes) {
    for (std::uint32_t n : notes) br->submit({make_note_on(0, part, n, 40000), 0});
  }
};

void test_registry_and_cc() {
  const params::ParamDef* speed = params::find(params::kRotarySpeed);
  const params::ParamDef* on = params::find(params::kRotaryOn);
  CHECK(speed && on);
  CHECK(speed->cls == params::Class::kPerformance && on->cls == params::Class::kPerformance);
  CHECK(speed->def == params::kRotarySpeedSlow && on->def == 0.0f);  // default: slow, stage off (existing renders unchanged)
  CHECK(params::kRotarySpeedCount == 3 && params::kRotarySpeedStop == 0 && params::kRotarySpeedFast == 2);
  CHECK(params::find_by_cc(79) == speed && params::find_by_cc(89) == on);
  const std::uint32_t cc_values[] = {0, 31, 32, 64, 95, 96, 127};
  const int expect[] = {0, 0, 1, 1, 1, 2, 2};
  for (int i = 0; i < 7; ++i) {
    Ump ev;
    CHECK(cc_to_param_event(make_control_change(0, 3, 79, static_cast<std::uint32_t>(cc_values[i] * 0xFFFFFFFFull / 127)), ev));
    ParamEvent pe;
    CHECK(decode_param_event(ev, pe) && pe.def == speed && pe.part == 3 && pe.value == static_cast<float>(expect[i]));
  }
  Ump ev;
  ParamEvent pe;
  CHECK(cc_to_param_event(make_control_change(0, 0, 89, 0xFFFFFFFFu), ev) && decode_param_event(ev, pe) && pe.def == on && pe.value == 1.0f);
}

// Rotary off is the plain mono organ, bit for bit; on, the channels differ.
void test_off_is_bit_exact() {
  auto render = [](int variant) {
    Rig rig;
    if (variant == 1) {  // explicitly off, speed commanded
      rig.param(params::kRotaryOn, 0, 0.0);
      rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast, 7);
    }
    if (variant == 2) rig.param(params::kRotaryOn, 0, 1.0);
    rig.chord(0, {48, 55, 60, 64});
    rig.blocks(300);
    return std::make_pair(rig.l, rig.r);
  };
  const auto base = render(0), off = render(1), on = render(2);
  CHECK(base.first.size() == off.first.size());
  CHECK(std::memcmp(base.first.data(), off.first.data(), base.first.size() * 4) == 0);
  CHECK(std::memcmp(base.second.data(), off.second.data(), base.second.size() * 4) == 0);
  CHECK(std::memcmp(base.first.data(), base.second.data(), base.first.size() * 4) == 0);  // mono: L == R exactly
  double diff = 0;
  for (std::size_t i = 0; i < on.first.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(on.first[i] - on.second[i])));
  CHECK(diff > 0.01);  // on: a stereo image
  CHECK(on.first != base.first);
}

// A speed command lands on the exact frame of its event, and the published state follows it.
void test_event_frame_and_state() {
  auto render = [](std::uint32_t at, bool send) {
    Rig rig;
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.chord(0, {48, 55, 60, 64, 72});
    rig.blocks(40);
    if (send) rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast, at);
    rig.blocks(300);
    return rig;
  };
  Rig a = render(20, true), none = render(0, false);
  const std::size_t ev = 40 * kBlockFrames + 20;
  CHECK(std::memcmp(&a.l[0], &none.l[0], ev * 4) == 0 && std::memcmp(&a.r[0], &none.r[0], ev * 4) == 0);  // identical before it
  double diff = 0;
  for (std::size_t i = ev; i < a.l.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a.l[i] - none.l[i])));
  CHECK(diff > 0.001);  // different after it
  // Event one frame later: identical up to that frame.
  Rig b = render(21, true);
  CHECK(std::memcmp(&a.l[0], &b.l[0], ev * 4) == 0);

  // State publication: reports the target at once, the actual speeds follow the ramps.
  Rig rig;
  OrganEngine::RotaryStatus s = rig.organ->rotary_status(0);
  CHECK(s.target == RotarySpeed::kSlow && !s.on && s.horn_hz == static_cast<float>(RotaryConfig{}.horn.slow_hz));
  rig.param(params::kRotaryOn, 0, 1.0);
  rig.chord(0, {60});
  rig.blocks(2);
  s = rig.organ->rotary_status(0);
  CHECK(s.on && s.target == RotarySpeed::kSlow);
  rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast, 10);
  rig.block();
  s = rig.organ->rotary_status(0);
  CHECK(s.target == RotarySpeed::kFast);                                                  // the commanded state, immediately
  CHECK(s.horn_hz > RotaryConfig{}.horn.slow_hz && s.horn_hz < 0.9f * RotaryConfig{}.horn.fast_hz);  // the rotors are still ramping
  CHECK(rig.br->last_values().load(params::perf_slot(*params::find(params::kRotarySpeed), 0)) == 2.0f);  // and the ws.v1 read path
  float prev = s.horn_hz;
  bool rising = true;
  for (int i = 0; i < 1000; ++i) {
    rig.block();
    const float h = rig.organ->rotary_status(0).horn_hz;
    rising = rising && h >= prev;
    prev = h;
  }
  CHECK(rising);
  s = rig.organ->rotary_status(0);
  {  // 1.35 s after the command: the horn (tau 0.6 s) is about 89 % of the way, the drum (tau 2 s) about 49 %
    const RotaryConfig c;
    const double hf = (s.horn_hz - c.horn.slow_hz) / (c.horn.fast_hz - c.horn.slow_hz);
    const double df = (s.drum_hz - c.drum.slow_hz) / (c.drum.fast_hz - c.drum.slow_hz);
    CHECK(hf > 0.8 && hf < 0.95 && df > 0.35 && df < 0.65);
  }
  rig.param(params::kRotarySpeed, 0, params::kRotarySpeedStop, 0);
  rig.block();
  CHECK(rig.organ->rotary_status(0).target == RotarySpeed::kStop);
  CHECK(rig.br->last_values().load(params::perf_slot(*params::find(params::kRotarySpeed), 0)) == 0.0f);
  rig.blocks(static_cast<int>(20 * kFs / kBlockFrames));
  s = rig.organ->rotary_status(0);
  CHECK(s.horn_hz == 0.0f && s.drum_hz == 0.0f && s.target == RotarySpeed::kStop);          // standstill
  CHECK(rig.organ->rotary_status(1).target == RotarySpeed::kSlow && !rig.organ->rotary_status(1).on);  // other parts untouched
  CHECK(rig.organ->rotary_status(99).on == false);
}

// Click-free under a held chord: cycling the speeds (and switching the stage on and off) stays within what the steady fast rotation
// does on its own.
void test_chord_click_free() {
  auto chord_rig = [](Rig& rig) {
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
    for (std::uint32_t n = 28; n <= 100; ++n) rig.br->submit({make_note_on(0, 0, n, 30000), n % 64});
    rig.blocks(200);  // settled: the declick gates are done, the rotors are at fast
  };
  Rig steady;
  chord_rig(steady);
  const std::size_t from = steady.l.size();
  steady.blocks(1500);
  const double ref = std::max(max_step(steady.l, from), max_step(steady.r, from));

  Rig rig;
  chord_rig(rig);
  const std::size_t start = rig.l.size();
  const std::uint32_t speeds[] = {params::kRotarySpeedSlow, params::kRotarySpeedFast, params::kRotarySpeedStop, params::kRotarySpeedSlow,
                                  params::kRotarySpeedFast, params::kRotarySpeedStop, params::kRotarySpeedFast};
  for (int i = 0; i < 6000; ++i) {
    if (i % 83 == 0) rig.param(params::kRotarySpeed, 0, speeds[(i / 83) % 7], static_cast<std::uint32_t>((i * 5) % 64));
    if (i % 301 == 150) rig.param(params::kRotaryOn, 0, (i / 301) % 2 == 0 ? 0.0 : 1.0, static_cast<std::uint32_t>(i % 64));
    rig.block();
  }
  const double cycled = std::max(max_step(rig.l, start), max_step(rig.r, start));
  // The stage switching includes a crossfade to the mono signal (a different but equally smooth signal): allow a modest margin.
  CHECK(cycled <= ref * 1.35);
  std::printf("rotary_tests: 73-key chord max step steady fast %.4f, cycling speeds and switching %.4f\n", ref, cycled);

  // Switching on from off with a chord held crossfades over 5 ms: the step at the switch stays below the signal's own scale.
  Rig sw;
  sw.chord(0, {48, 55, 60, 64, 67, 72});
  sw.blocks(200);
  const std::size_t at = sw.l.size();
  sw.param(params::kRotaryOn, 0, 1.0, 5);
  sw.blocks(100);
  double pre = max_step(sw.l, 100, at), post = max_step(sw.l, at);
  CHECK(post <= pre * 2.5);
}

// Parts are independent: the rotary on part 0 does not change part 1.
void test_parts_independent() {
  auto render = [](bool rotary) {
    Rig rig;
    if (rotary) rig.param(params::kRotaryOn, 0, 1.0);
    rig.chord(1, {50, 57, 62});
    rig.blocks(200);
    return rig.l;
  };
  CHECK(render(false) == render(true));  // part 0 has no notes here, part 1 never gets the rotary
  auto render2 = [](bool rotary_on_part1) {
    Rig rig;
    if (rotary_on_part1) rig.param(params::kRotaryOn, 1, 1.0);
    rig.chord(0, {50, 57, 62});
    rig.blocks(200);
    return rig.l;
  };
  CHECK(render2(false) == render2(true));
}

// A tail after the last key: with the rotary on the output decays to exact silence and nothing is cut.
void test_tail_in_organ() {
  Rig rig;
  rig.param(params::kRotaryOn, 0, 1.0);
  rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
  rig.chord(0, {60, 64, 67});
  rig.blocks(300);
  for (std::uint32_t n : {60u, 64u, 67u}) rig.br->submit({make_note_off(0, 0, n), 0});
  rig.blocks(40);
  const std::size_t from = rig.l.size() - 8 * kBlockFrames;
  double tail = 0;
  for (std::size_t i = from; i < rig.l.size(); ++i) tail = std::max(tail, static_cast<double>(std::fabs(rig.l[i]) + std::fabs(rig.r[i])));
  CHECK(tail == 0.0);
  // the release itself is smooth (no cut at the end of the rotary tail)
  const std::size_t rel = 300 * kBlockFrames;
  CHECK(max_step(rig.l, rel) < 0.2);
}

void test_reservation_includes_rotary() {
  CHECK(OrganEngine::kRotaryCostUnits == RotarySpeaker::kCostUnits && RotarySpeaker::kCostUnits > 0);
  for (std::uint32_t parts : {1u, 3u, 8u})
    CHECK(OrganEngine::reservation_cost_units(parts) ==
          static_cast<std::uint64_t>(parts) * (OrganEngine::kGeneratorCostUnits + RotarySpeaker::kCostUnits +
                                               73u * OrganEngine::kVoiceCostUnits));
  CHECK(OrganEngine::reservation_cost_units(2) - OrganEngine::reservation_cost_units(1) ==
        OrganEngine::kGeneratorCostUnits + RotarySpeaker::kCostUnits + 73u * OrganEngine::kVoiceCostUnits);
  Rig rig;
  VoiceArbiter::LiveSetConfig cfg;
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(2);
  cfg.budget = cfg.organ_rotary_cost - 1;  // one unit short of generator + rotary + keys for two parts
  CHECK(rig.br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOrganExceedsBudget);
  cfg.budget = cfg.organ_rotary_cost;
  CHECK(rig.br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
  CHECK(rig.br->arbiter().reservation().organ_rotary == OrganEngine::reservation_cost_units(2));
}


// Rotary on, then off at a block boundary under a held chord: after the 5 ms crossfade the output is the plain mono organ, bit for bit.
void test_on_then_off_is_bit_exact() {
  auto render = [](bool rotary) {
    Rig rig;
    if (rotary) rig.param(params::kRotaryOn, 0, 1.0);
    rig.chord(0, {48, 55, 60, 64, 72});
    rig.blocks(100);
    if (rotary) {
      CHECK(rig.organ->rotary_status(0).on);
      rig.param(params::kRotaryOn, 0, 0.0, 0);  // first frame of the next block
    }
    rig.blocks(40);
    if (rotary) CHECK(!rig.organ->rotary_status(0).on);
    return std::make_pair(rig.l, rig.r);
  };
  const auto with = render(true), none = render(false);
  const std::size_t from = (100 + 10) * kBlockFrames, n = with.first.size() - from;
  CHECK(std::memcmp(&with.first[from], &with.second[from], n * 4) == 0);   // L == R
  CHECK(std::memcmp(&with.first[from], &none.first[from], n * 4) == 0);    // and equal to the never-rotary render
  CHECK(std::memcmp(&with.second[from], &none.second[from], n * 4) == 0);
}

// A disabled part keeps its rotors in step, publishes them, and comes back with no stale filter or delay-line tail.
void test_disabled_part() {
  auto run_rig = [](bool disable) {
    Rig rig;
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
    if (disable) rig.chord(0, {48, 55, 60, 64});  // sounding when the part is disabled
    rig.blocks(100);
    if (disable) rig.organ->set_enabled_parts(0xFE);
    else rig.organ->set_enabled_parts(0xFF);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedStop, 0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedSlow, 3000 % 64);
    rig.blocks(60);
    if (disable) {
      double e = 0;
      for (std::size_t i = 101 * kBlockFrames; i < rig.l.size(); ++i) e += std::fabs(rig.l[i]) + std::fabs(rig.r[i]);
      CHECK(e == 0.0);                                    // a disabled part is silent
    }
    rig.organ->set_enabled_parts(0xFF);
    const auto st = rig.organ->rotary_status(0);
    const double ang = rig.organ->rotary(0).horn_angle_cycles();
    const std::size_t at = rig.l.size();
    rig.chord(0, {50, 57, 62, 66});
    rig.blocks(20);
    return std::make_tuple(st, ang, std::vector<float>(rig.l.begin() + at, rig.l.end()), std::vector<float>(rig.r.begin() + at, rig.r.end()));
  };
  // Reference: the part is enabled and silent for the same time (no keys), so the generator, rotors and ramps run the same.
  const auto a = run_rig(true), b = run_rig(false);
  CHECK(std::get<0>(a).horn_hz == std::get<0>(b).horn_hz && std::get<0>(a).drum_hz == std::get<0>(b).drum_hz);  // published while disabled
  CHECK(std::get<0>(a).target == RotarySpeed::kSlow && std::get<0>(b).target == RotarySpeed::kSlow);
  CHECK(std::get<1>(a) == std::get<1>(b));
  CHECK(std::get<0>(a).horn_hz > 0.0f);
  // The wheel generator does not advance while a part is disabled, so compare against a reference that is disabled too, without keys.
  auto run_ref = [](bool keys_before) {
    Rig rig;
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
    if (keys_before) rig.chord(0, {48, 55, 60, 64});
    rig.blocks(100);
    rig.organ->set_enabled_parts(0xFE);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedStop, 0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedSlow, 3000 % 64);
    rig.blocks(60);
    rig.organ->set_enabled_parts(0xFF);
    const std::size_t at = rig.l.size();
    rig.chord(0, {50, 57, 62, 66});
    rig.blocks(20);
    return std::make_pair(std::vector<float>(rig.l.begin() + at, rig.l.end()), std::vector<float>(rig.r.begin() + at, rig.r.end()));
  };
  const auto k = run_ref(true), n = run_ref(false);
  CHECK(k.first.size() == n.first.size());
  CHECK(std::memcmp(k.first.data(), n.first.data(), k.first.size() * 4) == 0);   // first re-enabled blocks identical: nothing stale survived
  CHECK(std::memcmp(k.second.data(), n.second.data(), k.second.size() * 4) == 0);
}

// With the stage on and the tail expired the signal state is cleared once, not only when the stage is bypassed.
void test_tail_expired_clears_state() {
  auto run_rig = [](bool keys) {
    Rig rig;
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
    if (keys) {
      rig.chord(0, {48, 55, 60, 64});
      rig.blocks(100);
      for (std::uint32_t n : {48u, 55u, 60u, 64u}) rig.br->submit({make_note_off(0, 0, n), 0});
      rig.blocks(60);  // release + 1024-frame tail expired
    } else {
      rig.blocks(160);
    }
    return rig;
  };
  Rig a = run_rig(true);
  // The stage state is zero: a silent input now gives exactly zero through the unit (a stale tail would not).
  RotarySpeaker copy = a.organ->rotary(0);
  std::vector<float> z(64, 0.0f), ol(64, 1.0f), orr(64, 1.0f);
  copy.process(z.data(), ol.data(), orr.data(), 64);
  double e = 0;
  for (int i = 0; i < 64; ++i) e += std::fabs(ol[i]) + std::fabs(orr[i]);
  CHECK(e == 0.0);
}

// Same-frame events: rotary on and rotary speed give the same result in either order, and non-finite values change nothing.
void test_same_frame_order_and_bad_values() {
  auto render = [](bool on_first) {
    Rig rig;
    if (on_first) {
      rig.param(params::kRotaryOn, 0, 1.0, 5);
      rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast, 5);
    } else {
      rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast, 5);
      rig.param(params::kRotaryOn, 0, 1.0, 5);
    }
    rig.block();
    rig.chord(0, {48, 55, 60, 64});
    rig.blocks(60);
    return std::make_tuple(rig.l, rig.r, rig.organ->rotary_status(0));
  };
  const auto a = render(true), b = render(false);
  CHECK(std::memcmp(std::get<0>(a).data(), std::get<0>(b).data(), std::get<0>(a).size() * 4) == 0);
  CHECK(std::memcmp(std::get<1>(a).data(), std::get<1>(b).data(), std::get<1>(a).size() * 4) == 0);
  CHECK(std::get<2>(a).horn_hz == std::get<2>(b).horn_hz && std::get<2>(a).horn_hz == static_cast<float>(RotaryConfig{}.horn.fast_hz));
  OrganEngine e;
  e.set_part_param(0, params::kRotarySpeed, std::nanf(""));
  e.set_part_param(0, params::kRotarySpeed, HUGE_VALF);
  e.set_part_param(0, params::kRotaryOn, std::nanf(""));
  const auto s = e.rotary_status(0);
  CHECK(s.target == RotarySpeed::kSlow && !s.on);
  CHECK(&e.rotary(200) == &e.rotary(0));  // out-of-range part is guarded
}

// Loudest case: a 73-key chord at full registration through the stage on all three speeds. The bound is what the RTSan test uses.
void test_peak_bound() {
  float peak = 0.0f;
  for (std::uint32_t sp : {params::kRotarySpeedSlow, params::kRotarySpeedFast, params::kRotarySpeedStop, params::kRotarySpeedFast}) {
    Rig rig;
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, sp);
    for (std::uint32_t d = 0; d < 9; ++d) rig.param(params::kDrawbar16 + d, 0, 8.0);
    for (std::uint32_t n = 28; n <= 100; ++n) rig.br->submit({make_note_on(0, 0, n, 65000), 0});
    rig.blocks(3000);
    for (std::size_t i = 0; i < rig.l.size(); ++i) peak = std::max({peak, std::fabs(rig.l[i]), std::fabs(rig.r[i])});
  }
  std::printf("rotary_tests: loudest 73-key full-registration chord through the stage: peak %.3f\n", static_cast<double>(peak));
  CHECK(peak <= kRotaryPartPeakBound);
}

}  // namespace

int main() {
  test_design_values();
  test_crossover_reconstruction();
  test_rest_gain();
  test_am_depth_and_rate();
  test_doppler();
  test_stereo_spread();
  test_time_constants();
  test_stop_standstill();
  test_click_free();
  test_determinism_and_blocks();
  test_tail_and_bounds();
  test_registry_and_cc();
  test_off_is_bit_exact();
  test_event_frame_and_state();
  test_chord_click_free();
  test_parts_independent();
  test_tail_in_organ();
  test_reservation_includes_rotary();
  test_on_then_off_is_bit_exact();
  test_disabled_part();
  test_tail_expired_clears_state();
  test_same_frame_order_and_bad_values();
  test_peak_bound();
  if (g_fail) {
    std::printf("rotary_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("rotary_tests: ok\n");
  return 0;
}

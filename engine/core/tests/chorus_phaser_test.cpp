// Story 2.9: chorus / phaser insert. Bare main, CHECK style as the other core tests.
//
// Chorus (time domain): the delay is recovered from a ramp input (the 4-point Lagrange interpolator reproduces a ramp exactly, so
// wet(n) = ramp(n - d(n)) gives d(n) = n - wet(n) / slope): range 5..25 ms, LFO rate, quarter-cycle offset between the channels.
// Phaser (frequency domain): the response of  mix 0.5 -> 0.5 (1 + A(z))  with A the four-stage all-pass chain is computed analytically
// from the bilinear all-pass (this file carries its own formula) and compared with sine measurements through process(); with feedback
// the closed form  A / (1 - fb z^-1 A)  is used.
// Also: mapping and the mode threshold, identity bit-exactness at mix 0, mode cross-fade step bound, parameter-change step bound,
// determinism, non-finite input, reset equals fresh, chain bypass, the declared cost and the Mixer path.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/chorus_phaser.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
using Params = std::array<float, velora::core::params::kInsertParamCount>;

struct Stereo {
  std::vector<float> l, r;
};

// Runs `fx` over the signal in 64-frame blocks (the last one shorter), in place on copies.
Stereo run(Effect& fx, const Stereo& in) {
  Stereo out = in;
  for (std::size_t i = 0; i < out.l.size(); i += 64) {
    StereoBlock b{out.l.data() + i, out.r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(64, out.l.size() - i))};
    fx.process(b);
  }
  return out;
}

Stereo sine(std::size_t n, double hz, double amp = 0.5) {
  Stereo s;
  s.l.resize(n);
  s.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) s.l[i] = s.r[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kFs));
  return s;
}

Stereo noise(std::size_t n, std::uint32_t seed, float amp = 0.3f) {
  Stereo s;
  s.l.resize(n);
  s.r.resize(n);
  std::uint32_t x = seed;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s.l[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * amp;
    x = x * 1664525u + 1013904223u;
    s.r[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * amp;
  }
  return s;
}

double max_step(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double m = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
  return m;
}

std::unique_ptr<ChorusPhaserEffect> make(const Params& p) {
  auto fx = std::make_unique<ChorusPhaserEffect>();
  fx->set_params(p);
  fx->prepare(48000, 64);
  return fx;
}

// Normalised parameters for given physical-ish choices.
float rate_norm(double hz) { return static_cast<float>(std::log(hz / 0.1) / std::log(100.0)); }

// ---- mapping -----------------------------------------------------------------------------------------------------------------------
void test_mapping() {
  using namespace cpmap;
  CHECK(mode(0.0f) == ChorusPhaserMode::kChorus && mode(0.25f) == ChorusPhaserMode::kChorus);
  CHECK(mode(0.5f) == ChorusPhaserMode::kChorus);  // the threshold itself is chorus (the registry default 0.5 is a chorus)
  CHECK(mode(std::nextafter(0.5f, 1.0f)) == ChorusPhaserMode::kPhaser);
  CHECK(mode(std::nextafter(0.5f, 0.0f)) == ChorusPhaserMode::kChorus);
  CHECK(mode(1.0f) == ChorusPhaserMode::kPhaser && mode(7.0f) == ChorusPhaserMode::kPhaser && mode(-3.0f) == ChorusPhaserMode::kChorus);
  CHECK(mode(std::nanf("")) == ChorusPhaserMode::kChorus);
  CHECK(std::fabs(rate_hz(0.0f) - 0.1f) < 1e-6f && std::fabs(rate_hz(0.5f) - 1.0f) < 1e-5f && std::fabs(rate_hz(1.0f) - 10.0f) < 1e-4f);
  CHECK(depth(0.0f) == 0.0f && depth(1.0f) == 1.0f && depth(0.3f) == 0.3f && depth(2.0f) == 1.0f);
  CHECK(feedback(0.5f) == 0.0f && std::fabs(feedback(1.0f) - 0.8f) < 1e-6f && std::fabs(feedback(0.0f) + 0.8f) < 1e-6f);
  CHECK(mix(0.0f) == 0.0f && mix(1.0f) == 1.0f && mix(0.25f) == 0.25f);
  CHECK(kDefaultInsertParams[4] == 0.0f);  // the registry default of insert_param_5: mix 0 = dry
  auto fx = make_effect(EffectType::kChorusPhaser);
  CHECK(fx && fx->type() == EffectType::kChorusPhaser && fx->cost_units() == effect_cost_units(EffectType::kChorusPhaser));
  CHECK(dynamic_cast<ChorusPhaserEffect*>(fx.get()) != nullptr);  // not a pass-through placeholder any more
}

// ---- chorus: delay range, rate, stereo ------------------------------------------------------------------------------------------------
struct DelayTrack {
  std::vector<double> dl, dr;
};
DelayTrack track_delays(double rate_hz, float depth, std::size_t n) {
  Stereo in;
  in.l.resize(n);
  in.r.resize(n);
  constexpr double slope = 1.0 / 60000.0;
  for (std::size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = static_cast<float>(slope * static_cast<double>(i));
  const Params p = {0.0f, rate_norm(rate_hz), depth, 0.5f, 1.0f};  // chorus, full wet
  auto fx = make(p);
  const Stereo out = run(*fx, in);
  DelayTrack t;
  t.dl.assign(n, 0.0);
  t.dr.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    t.dl[i] = static_cast<double>(i) - static_cast<double>(out.l[i]) / slope;
    t.dr[i] = static_cast<double>(i) - static_cast<double>(out.r[i]) / slope;
  }
  return t;
}

void test_chorus_delay() {
  // no modulation: a fixed 15 ms delay (720 samples), exactly, for an impulse
  {
    Params p = {0.0f, 0.5f, 0.0f, 0.5f, 1.0f};
    auto fx = make(p);
    Stereo in;
    in.l.assign(2000, 0.0f);
    in.r.assign(2000, 0.0f);
    in.l[100] = 1.0f;
    in.r[300] = 0.5f;
    const Stereo out = run(*fx, in);
    CHECK(out.l[100 + 720] == 1.0f && out.r[300 + 720] == 0.5f);
    double other = 0.0;
    for (std::size_t i = 0; i < 2000; ++i)
      if (i != 820) other = std::max(other, std::fabs(static_cast<double>(out.l[i])));
    CHECK(other == 0.0);
  }
  // full depth at 2 Hz: the delay swings over 5..25 ms with a 2 Hz period
  const std::size_t n = 60000;
  const DelayTrack t = track_delays(2.0, 1.0f, n);
  double dmin = 1e9, dmax = -1e9;
  for (std::size_t i = 3000; i < n; ++i) {
    dmin = std::min({dmin, t.dl[i], t.dr[i]});
    dmax = std::max({dmax, t.dl[i], t.dr[i]});
  }
  std::printf("chorus_phaser_tests: chorus delay range %.1f..%.1f samples (%.2f..%.2f ms)\n", dmin, dmax, dmin / 48.0, dmax / 48.0);
  CHECK(dmin >= 5.0 * 48.0 - 6.0 && dmin <= 5.0 * 48.0 + 6.0);
  CHECK(dmax <= 25.0 * 48.0 + 6.0 && dmax >= 25.0 * 48.0 - 6.0);
  // period: the up-crossings of the centre delay are 24000 samples apart (2 Hz)
  std::vector<std::size_t> up;
  for (std::size_t i = 3001; i < n; ++i)
    if (t.dl[i - 1] < 720.0 && t.dl[i] >= 720.0) up.push_back(i);
  CHECK(up.size() >= 2);
  if (up.size() >= 2) CHECK(std::llabs(static_cast<long long>(up[1] - up[0]) - 24000) <= 12);
  // stereo: the right LFO runs a quarter cycle (6000 samples) after the left one
  std::size_t il = 3000, ir = 3000;
  for (std::size_t i = 3000; i < 3000 + 24000; ++i) {
    if (t.dl[i] > t.dl[il]) il = i;
    if (t.dr[i] > t.dr[ir]) ir = i;
  }
  const long long lag = static_cast<long long>((ir + 24000 - il) % 24000);
  CHECK(std::llabs(lag - 6000) < 40 || std::llabs(lag - 18000) < 40);
  CHECK(std::fabs(ChorusPhaserEffect::kChannelPhaseOffset - 0.25f) < 1e-9f);
  // depth scales the swing; depth 0.5 spans half of it
  const DelayTrack h = track_delays(2.0, 0.5f, n);
  double hmin = 1e9, hmax = -1e9;
  for (std::size_t i = 3000; i < n; ++i) {
    hmin = std::min(hmin, h.dl[i]);
    hmax = std::max(hmax, h.dl[i]);
  }
  CHECK(std::fabs((hmax - hmin) - 0.5 * (dmax - dmin)) < 12.0);
  // the rate follows the mapping: 0.5 -> 1 Hz (48000 samples per cycle)
  const DelayTrack s = track_delays(1.0, 1.0f, 130000);
  std::vector<std::size_t> up1;
  for (std::size_t i = 3001; i < s.dl.size(); ++i)
    if (s.dl[i - 1] < 720.0 && s.dl[i] >= 720.0) up1.push_back(i);
  CHECK(up1.size() >= 2);
  if (up1.size() >= 2) CHECK(std::llabs(static_cast<long long>(up1[1] - up1[0]) - 48000) <= 24);
}

// ---- phaser: frequency response --------------------------------------------------------------------------------------------------
std::complex<double> allpass_chain(double f_hz, double f_break_hz) {
  const double t = std::tan(kPi * f_break_hz / kFs), a = (t - 1.0) / (t + 1.0);
  const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * f_hz / kFs);
  const std::complex<double> h = (a + z1) / (1.0 + a * z1);
  return h * h * h * h;
}

double measured_gain_db(Effect& fx, double hz, std::size_t n = 24000) {
  const Stereo in = sine(n, hz, 0.25);
  const Stereo out = run(fx, in);
  double si = 0.0, so = 0.0;
  for (std::size_t i = n / 2; i < n; ++i) {
    si += static_cast<double>(in.l[i]) * in.l[i];
    so += static_cast<double>(out.l[i]) * out.l[i];
  }
  return 10.0 * std::log10(so / si);
}

void test_phaser_response() {
  const double fc = 894.427190999915878564;
  const double freqs[] = {100, 250, 370, 600, 894, 1300, 2159, 3000, 6000};
  // fixed sweep (depth 0), no feedback, mix 0.5: response 0.5 (1 + A)
  const Params p = {1.0f, 0.5f, 0.0f, 0.5f, 0.5f};
  for (double f : freqs) {
    auto fx = make(p);
    const double meas = measured_gain_db(*fx, f);
    const double want = 20.0 * std::log10(std::max(1e-9, std::abs(0.5 * (1.0 + allpass_chain(f, fc)))));
    if (want > -25.0) CHECK(std::fabs(meas - want) < 0.25);  // deep notches are limited by the measurement, checked separately
  }
  // notches near fc * tan(pi/8) (370 Hz) and fc * tan(3 pi/8) (2.16 kHz): deep, while 894 Hz passes at 0 dB
  {
    auto fx = make(p);
    const double n1 = measured_gain_db(*fx, std::atan(std::tan(kPi * fc / kFs) * std::tan(kPi / 8.0)) * kFs / kPi);
    auto fy = make(p);
    const double n2 = measured_gain_db(*fy, std::atan(std::tan(kPi * fc / kFs) * std::tan(3.0 * kPi / 8.0)) * kFs / kPi);
    auto fz = make(p);
    const double pass = measured_gain_db(*fz, fc);
    std::printf("chorus_phaser_tests: phaser notches %.1f dB / %.1f dB, centre %.2f dB\n", n1, n2, pass);
    CHECK(n1 < -30.0 && n2 < -30.0 && std::fabs(pass) < 0.3);
  }
  // mix 1: an all-pass: flat magnitude
  {
    const Params q = {1.0f, 0.5f, 0.0f, 0.5f, 1.0f};
    for (double f : {200.0, 900.0, 3000.0}) {
      auto fx = make(q);
      CHECK(std::fabs(measured_gain_db(*fx, f)) < 0.1);
    }
  }
  // feedback +0.8: closed form A / (1 - fb z^-1 A) for the wet path, wet gain 5 at the break frequency
  for (float fbn : {1.0f, 0.0f}) {
    const double fb = (fbn - 0.5) * 2.0 * 0.8;
    const Params q = {1.0f, 0.5f, 0.0f, fbn, 0.5f};
    for (double f : {300.0, 894.0, 1500.0, 4000.0}) {
      auto fx = make(q);
      const std::complex<double> A = allpass_chain(f, fc), z1 = std::polar(1.0, -2.0 * kPi * f / kFs);
      const std::complex<double> wet = A / (1.0 - fb * z1 * A);
      const double want = 20.0 * std::log10(std::abs(0.5 * (1.0 + wet)));
      CHECK(std::fabs(measured_gain_db(*fx, f, 48000) - want) < 0.3);
    }
  }
  // the sweep moves the notch: a 370 Hz sine through depth 1 is modulated, through depth 0 it is steadily cancelled
  {
    const double f_notch = std::atan(std::tan(kPi * fc / kFs) * std::tan(kPi / 8.0)) * kFs / kPi;
    const Stereo in = sine(96000, f_notch, 0.25);
    auto fx0 = make({1.0f, rate_norm(2.0), 0.0f, 0.5f, 0.5f});
    auto fx1 = make({1.0f, rate_norm(2.0), 1.0f, 0.5f, 0.5f});
    const Stereo o0 = run(*fx0, in), o1 = run(*fx1, in);
    auto rms = [](const std::vector<float>& v, std::size_t a, std::size_t b) {
      double s = 0;
      for (std::size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
      return std::sqrt(s / static_cast<double>(b - a));
    };
    double lo = 1e9, hi = 0;
    for (std::size_t a = 4800; a + 240 <= 96000; a += 240) {
      const double r = rms(o1.l, a, a + 240);
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
    CHECK(rms(o0.l, 48000, 96000) < 0.02 * rms(in.l, 48000, 96000));
    std::printf("chorus_phaser_tests: swept notch envelope hi/lo %.1f\n", hi / lo);
    CHECK(hi > 5.0 * lo && hi > 0.3 * rms(in.l, 48000, 96000));
  }
}

// ---- identity, mix, determinism --------------------------------------------------------------------------------------------------
void test_identity_and_mix() {
  const Stereo in = noise(48000, 7);
  for (int mode_ : {0, 1}) {
    const Params p = {static_cast<float>(mode_), 0.7f, 0.9f, 0.9f, 0.0f};  // mix 0
    auto fx = make(p);
    const Stereo out = run(*fx, in);
    CHECK(out.l == in.l && out.r == in.r);  // bit for bit
  }
  // the registry defaults (mix 0): identity
  {
    auto fx = make_effect(EffectType::kChorusPhaser);
    auto* cp = dynamic_cast<ChorusPhaserEffect*>(fx.get());
    CHECK(cp && cp->is_identity());
    const Stereo out = run(*fx, in);
    CHECK(out.l == in.l && out.r == in.r);
  }
  // mix leaves 0 and comes back: it ends exactly bit-identical again, and in between the output changes smoothly
  {
    auto fx = make({0.0f, 0.5f, 0.8f, 0.5f, 0.0f});
    Stereo a = noise(6400, 3);
    Stereo b = run(*fx, a);
    CHECK(b.l == a.l);
    fx->set_params({0.0f, 0.5f, 0.8f, 0.5f, 1.0f});
    Stereo c = run(*fx, noise(6400, 4));
    CHECK(c.l != noise(6400, 4).l);
    fx->set_params({0.0f, 0.5f, 0.8f, 0.5f, 0.0f});
    Stereo d = run(*fx, noise(64000, 5));
    Stereo d_in = noise(64000, 5);
    bool tail_exact = true;
    for (std::size_t i = 64000 - 6400; i < 64000; ++i) tail_exact = tail_exact && d.l[i] == d_in.l[i] && d.r[i] == d_in.r[i];
    CHECK(tail_exact);
    CHECK(static_cast<ChorusPhaserEffect*>(fx.get())->is_identity());
  }
  // a smooth mix: dry-to-wet with a sine, no step beyond the signal's own slope
  {
    const Stereo in2 = sine(48000, 440.0, 0.5);
    auto fx = make({0.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    Stereo out;
    out.l.reserve(48000);
    out.r.reserve(48000);
    Stereo a = in2;
    for (std::size_t i = 0; i < 48000; i += 64) {
      if (i == 9600) fx->set_params({0.0f, 0.5f, 0.5f, 0.5f, 1.0f});
      StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
      fx->process(b);
    }
    const double bound = 0.5 * 2.0 * kPi * 440.0 / kFs * 1.35;  // the sine's own step, with a little room for the delayed copy
    CHECK(max_step(a.l, 0, 48000) < bound);
  }
}

void test_determinism_and_nan() {
  const Stereo in = noise(30000, 11);
  const Params p = {0.9f, 0.6f, 0.7f, 0.8f, 0.6f};
  auto a = make(p), b = make(p);
  const Stereo oa = run(*a, in), ob = run(*b, in);
  CHECK(oa.l == ob.l && oa.r == ob.r);
  for (int mode_ : {0, 1}) {
    const Params q = {static_cast<float>(mode_), 0.6f, 0.7f, 0.9f, 0.7f};
    Stereo dirty = noise(30000, 12), clean = dirty;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (std::size_t i = 1000; i < 30000; i += 997) {
      dirty.l[i] = nan;
      clean.l[i] = 0.0f;
      dirty.r[i + 1] = i % 2 ? inf : -inf;
      clean.r[i + 1] = 0.0f;
    }
    auto x = make(q), y = make(q);
    const Stereo od = run(*x, dirty), oc = run(*y, clean);
    bool finite = true;
    for (std::size_t i = 0; i < od.l.size(); ++i) finite = finite && std::isfinite(od.l[i]) && std::isfinite(od.r[i]);
    CHECK(finite);
    CHECK(od.l == oc.l && od.r == oc.r);  // a non-finite sample is exactly a zero sample
  }
  // out-of-range / NaN parameters never produce non-finite output
  auto fx = make({std::nanf(""), -5.0f, 9.0f, std::nanf(""), 3.0f});
  const Stereo o = run(*fx, in);
  bool finite = true;
  for (std::size_t i = 0; i < o.l.size(); ++i) finite = finite && std::isfinite(o.l[i]) && std::isfinite(o.r[i]);
  CHECK(finite);
}

void test_reset_and_chain_bypass() {
  const Stereo prime = noise(20000, 21), in = noise(20000, 22);
  for (float mode_ : {0.0f, 1.0f}) {
    const Params p = {mode_, 0.6f, 0.8f, 0.9f, 0.8f};
    auto used = make(p), fresh = make(p);
    run(*used, prime);
    used->reset();
    const Stereo a = run(*used, in), b = run(*fresh, in);
    CHECK(a.l == b.l && a.r == b.r);  // a reset (what a bypassed slot gets) leaves no stale state behind
  }
  // an FxChain with the slot bypassed leaves the block bit-exact and resets the instance
  FxChain chain;
  chain.set_slot(0, make({1.0f, 0.5f, 1.0f, 0.9f, 1.0f}), /*bypass=*/true);
  Stereo x = noise(640, 5);
  const Stereo ref = x;
  for (std::size_t i = 0; i < x.l.size(); i += 64) {
    StereoBlock b{x.l.data() + i, x.r.data() + i, 64};
    chain.process(b);
  }
  CHECK(x.l == ref.l && x.r == ref.r);
  CHECK(chain.cost_units() == effect_cost_units(EffectType::kChorusPhaser));  // a bypassed slot still reserves its cost
}

// ---- no click: mode cross-fade and parameter changes ------------------------------------------------------------------------------------
void test_mode_crossfade() {
  const std::size_t n = 48000, at = 24000;
  const double kHz = 1200.0;
  const Stereo in = sine(n, kHz, 0.5);
  const double steady = 0.5 * 2.0 * kPi * kHz / kFs;  // the sine's own largest step
  // steady states: chorus-only and phaser-only wet signals (mix 1)
  const Params chorus = {0.0f, 0.5f, 0.3f, 0.5f, 1.0f}, phaser = {1.0f, 0.5f, 0.3f, 0.5f, 1.0f};
  auto c = make(chorus), p = make(phaser);
  const Stereo oc = run(*c, in), op = run(*p, in);
  double jump = 0.0;  // what a hard switch from one to the other at `at` would do
  for (std::size_t k = 0; k < 40; ++k) jump = std::max(jump, std::fabs(static_cast<double>(oc.l[at + k]) - op.l[at + k]));
  std::printf("chorus_phaser_tests: hard mode switch would step by %.3f, steady step %.4f\n", jump, steady);
  CHECK(jump > 4.0 * steady);  // the check below is meaningful: a hard switch would click
  for (int dir = 0; dir < 2; ++dir) {
    auto fx = make(dir == 0 ? chorus : phaser);
    Stereo a = in;
    for (std::size_t i = 0; i < n; i += 64) {
      if (i == at) fx->set_params(dir == 0 ? phaser : chorus);
      StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
      fx->process(b);
    }
    double worst = 0.0;
    for (std::size_t i = at - 200; i < at + 3000; ++i) worst = std::max({worst, std::fabs(static_cast<double>(a.l[i]) - a.l[i - 1]), std::fabs(static_cast<double>(a.r[i]) - a.r[i - 1])});
    CHECK(worst < 1.6 * steady);
    CHECK(fx->type() == EffectType::kChorusPhaser);
    CHECK(static_cast<ChorusPhaserEffect*>(fx.get())->phaser_weight() == (dir == 0 ? 1.0f : 0.0f));
  }
  // the ramp takes kModeRampFrames: halfway through, the weight is in between
  {
    auto fx = make(chorus);
    Stereo a = noise(64 * 20, 3);
    for (std::size_t i = 0; i < 64 * 4; i += 64) {
      StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
      fx->process(b);
    }
    fx->set_params(phaser);
    StereoBlock b{a.l.data(), a.r.data(), 64};
    fx->process(b);
    const float w = static_cast<ChorusPhaserEffect*>(fx.get())->phaser_weight();
    CHECK(w > 0.1f && w < 0.2f);  // 64 / 480
    CHECK(static_cast<ChorusPhaserEffect*>(fx.get())->target_mode() == ChorusPhaserMode::kPhaser);
  }
}

void test_param_changes_click_free() {
  const std::size_t n = 96000;
  const Stereo in = sine(n, 330.0, 0.5);
  const double steady = 0.5 * 2.0 * kPi * 330.0 / kFs;
  for (int mode_ = 0; mode_ < 2; ++mode_) {
    auto fx = make({static_cast<float>(mode_), 0.4f, 0.3f, 0.6f, 0.5f});
    Stereo a = in;
    for (std::size_t i = 0; i < n; i += 64) {
      if (i == 20032) fx->set_params({static_cast<float>(mode_), 0.9f, 1.0f, 0.2f, 1.0f});  // rate, depth, feedback, mix jump
      if (i == 40000) fx->set_params({static_cast<float>(mode_), 0.1f, 0.0f, 0.9f, 0.2f});
      StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
      fx->process(b);
    }
    // the chorus delay (and with it the pitch) moves, so the bound allows the Doppler of the fastest sweep; no block-edge steps
    double worst = 0.0;
    for (std::size_t i = 1; i < n; ++i) worst = std::max(worst, std::fabs(static_cast<double>(a.l[i]) - a.l[i - 1]));
    std::printf("chorus_phaser_tests: mode %d max step through parameter jumps %.4f (steady %.4f)\n", mode_, worst, steady);
    CHECK(worst < 2.6 * steady);
  }
}

void test_declared_cost() {
  auto fx = make({1.0f, 0.5f, 1.0f, 0.9f, 1.0f});
  Stereo a = noise(64, 1);
  const int iters = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    StereoBlock b{a.l.data(), a.r.data(), 64};
    fx->process(b);
    for (std::size_t k = 0; k < 64; ++k) {  // keep the signal bounded and alive
      a.l[k] *= 0.5f;
      a.r[k] *= 0.5f;
      a.l[k] += 0.1f * static_cast<float>(k % 7) - 0.3f;
    }
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / iters;
  auto c = make({0.0f, 0.5f, 1.0f, 0.5f, 1.0f});
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    StereoBlock b{a.l.data(), a.r.data(), 64};
    c->process(b);
  }
  const double us_c = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / iters;
  std::printf("host cost per 64-frame block: chorus_phaser phaser %.2f us, chorus %.2f us (declared %u units)\n", us, us_c,
              effect_cost_units(EffectType::kChorusPhaser));
  CHECK(effect_cost_units(EffectType::kChorusPhaser) > 0 && effect_cost_units(EffectType::kChorusPhaser) <= kMaxInsertCost);
}

// The same DSP inside the Mixer: a part with a chorus/phaser insert is the effect applied to the part rendered alone.
void test_in_the_mixer() {
  Snapshot s = Snapshot::defaults();
  s.parts[0].insert_count = 1;
  s.parts[0].inserts[0].type = velora::core::params::kInsertTypeChorusPhaser;
  s.parts[0].inserts[0].params = {1.0f, 0.5f, 0.6f, 0.7f, 0.8f};
  build_chains(s);
  CHECK(fx_cost_units(s) == kFixedFxCost + effect_cost_units(EffectType::kChorusPhaser));
  CHECK(s.parts[0].chain && s.parts[0].chain->slot_count() == 1 && s.parts[0].chain->slot(0).effect->type() == EffectType::kChorusPhaser);
  CHECK(!s.parts[0].chain->is_empty());
  // the chain itself equals a standalone instance with the same parameters
  Stereo in = noise(6400, 9);
  Stereo a = in;
  for (std::size_t i = 0; i < a.l.size(); i += 64) {
    StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
    s.parts[0].chain->process(b);
  }
  auto fx = make({1.0f, 0.5f, 0.6f, 0.7f, 0.8f});
  const Stereo ref = run(*fx, in);
  CHECK(a.l == ref.l && a.r == ref.r);
}


// Block-split invariance (static parameters), prepare() refusals, identity with non-finite input, re-entry after identity.
void test_blocks_and_prepare() {
  const Stereo in = noise(1000, 31);
  for (float mode_ : {0.0f, 1.0f}) {
    const Params p = {mode_, 0.6f, 0.7f, 0.8f, 0.7f};
    auto ref = make(p), big = make(p), one = make(p), odd = make(p);
    const Stereo r64 = run(*ref, in);
    Stereo b200 = in, b1 = in, b7 = in;
    for (std::size_t i = 0; i < 1000; i += 200) {
      StereoBlock b{b200.l.data() + i, b200.r.data() + i, 200};
      big->process(b);
    }
    for (std::size_t i = 0; i < 1000; ++i) {
      StereoBlock b{b1.l.data() + i, b1.r.data() + i, 1};
      one->process(b);
    }
    for (std::size_t i = 0; i < 1000; i += 7) {
      StereoBlock b{b7.l.data() + i, b7.r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(7, 1000 - i))};
      odd->process(b);
    }
    CHECK(b200.l == r64.l && b200.r == r64.r);  // an oversized block is processed in full, in chunks
    CHECK(b1.l == r64.l && b1.r == r64.r && b7.l == r64.l && b7.r == r64.r);
  }
  // prepare() refuses a rate / block size the DSP is not designed for: pass-through, documented, prepare_ok() false
  {
    ChorusPhaserEffect fx;
    CHECK(fx.prepare_ok());
    fx.set_params({0.0f, 0.5f, 1.0f, 0.5f, 1.0f});
    fx.prepare(44100, 64);
    CHECK(!fx.prepare_ok());
    Stereo a = noise(256, 4);
    const Stereo ref = a;
    run(fx, a);
    Stereo b = a;
    CHECK(run(fx, ref).l == ref.l);
    fx.prepare(48000, 128);
    CHECK(!fx.prepare_ok());
    fx.prepare(48000, 64);
    CHECK(fx.prepare_ok());
    CHECK(run(fx, ref).l != ref.l);
    (void)b;
  }
  // identity with non-finite input: finite samples bit-exact, NaN / inf become 0
  {
    auto fx = make({0.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    Stereo a = noise(256, 6);
    Stereo want = a;
    a.l[3] = std::numeric_limits<float>::quiet_NaN();
    a.r[70] = std::numeric_limits<float>::infinity();
    want.l[3] = 0.0f;
    want.r[70] = 0.0f;
    const Stereo o = run(*fx, a);
    CHECK(o.l == want.l && o.r == want.r);
  }
  // a mode target that changes while the effect is in identity is the audible mode when the mix leaves 0; the phaser coefficient is fresh
  for (int to_phaser = 0; to_phaser < 2; ++to_phaser) {
    auto live = make({to_phaser ? 0.0f : 1.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    run(*live, noise(640, 1));  // running, in identity
    const Params after = {to_phaser ? 1.0f : 0.0f, 0.5f, 0.5f, 0.5f, 1.0f};
    live->set_params(after);
    auto fresh = make(after);
    const Stereo in2 = noise(4800, 2);
    const Stereo a = run(*live, in2), b = run(*fresh, in2);
    // the mix ramp (smoothing) differs from a fresh start, so compare the settled second half
    double worst = 0;
    for (std::size_t i = 3000; i < 4800; ++i) worst = std::max(worst, std::fabs(static_cast<double>(a.l[i]) - b.l[i]));
    CHECK(static_cast<ChorusPhaserEffect*>(live.get())->phaser_weight() == (to_phaser ? 1.0f : 0.0f));
    if (to_phaser) CHECK(worst < 0.05);  // phaser has no long memory: same audible mode as a fresh phaser (chorus lines differ in LFO phase only)
  }
}

// Running instance: set_params(B) on a warmed-up instance converges to what a fresh B does.
void test_running_params() {
  // RATE and DEPTH: the delay track of a chorus changed from (2 Hz, depth 1) to (4 Hz, depth 0.5)
  {
    constexpr double slope = 1.0 / 60000.0;
    const std::size_t n = 100000, sw = 20032;
    Stereo in;
    in.l.resize(n);
    in.r.resize(n);
    for (std::size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = static_cast<float>(slope * static_cast<double>(i));
    auto fx = make({0.0f, rate_norm(2.0), 1.0f, 0.5f, 1.0f});
    Stereo a = in;
    for (std::size_t i = 0; i < n; i += 64) {
      if (i == sw) fx->set_params({0.0f, rate_norm(4.0), 0.5f, 0.5f, 1.0f});
      StereoBlock b{a.l.data() + i, a.r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(64, n - i))};
      fx->process(b);
    }
    double dmin = 1e9, dmax = -1e9;
    std::vector<std::size_t> up;
    std::vector<double> d(n);
    for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<double>(i) - static_cast<double>(a.l[i]) / slope;
    for (std::size_t i = sw + 6000; i < n; ++i) {
      dmin = std::min(dmin, d[i]);
      dmax = std::max(dmax, d[i]);
      if (d[i - 1] < 720.0 && d[i] >= 720.0) up.push_back(i);
    }
    CHECK(std::fabs(dmin - 480.0) < 8.0 && std::fabs(dmax - 960.0) < 8.0);  // depth 0.5: 15 ms +- 5 ms
    CHECK(up.size() >= 3 && std::llabs(static_cast<long long>(up[1] - up[0]) - 12000) <= 12);  // 4 Hz
  }
  // FEEDBACK (and RATE irrelevant at depth 0): the phaser gain at the break frequency after set_params equals the closed form for the new feedback
  {
    const double fc = 894.427190999915878564;
    const Params A = {1.0f, 0.5f, 0.0f, 0.5f, 0.5f}, B = {1.0f, 0.5f, 0.0f, 1.0f, 0.5f};
    auto fx = make(A);
    run(*fx, sine(4800, fc, 0.25));
    fx->set_params(B);
    const Stereo in = sine(24000, fc, 0.25);
    const Stereo out = run(*fx, in);
    double si = 0, so = 0;
    for (std::size_t i = 12000; i < 24000; ++i) {
      si += static_cast<double>(in.l[i]) * in.l[i];
      so += static_cast<double>(out.l[i]) * out.l[i];
    }
    const std::complex<double> Ac = allpass_chain(fc, fc), z1 = std::polar(1.0, -2.0 * kPi * fc / kFs);
    const double want = 20.0 * std::log10(std::abs(0.5 * (1.0 + Ac / (1.0 - 0.8 * z1 * Ac))));
    CHECK(std::fabs(10.0 * std::log10(so / si) - want) < 0.3);
  }
}

}  // namespace

int main() {
  test_mapping();
  test_chorus_delay();
  test_phaser_response();
  test_identity_and_mix();
  test_determinism_and_nan();
  test_reset_and_chain_bypass();
  test_mode_crossfade();
  test_param_changes_click_free();
  test_declared_cost();
  test_in_the_mixer();
  test_blocks_and_prepare();
  test_running_params();
  if (g_fail) {
    std::printf("chorus_phaser_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("chorus_phaser_tests: ok\n");
  return 0;
}

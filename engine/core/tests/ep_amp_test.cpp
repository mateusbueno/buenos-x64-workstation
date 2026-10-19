// Story 2.9: EP amp / drive insert. Bare main, CHECK style as the other core tests.
//
// Frequency domain: the linear parts (tone tilt, presence peak, cabinet, level) are measured with sine sweeps at a small level and no
// drive (the shaper is then linear to better than 1e-3) and compared with analytic references this file carries itself (RBJ peak,
// Butterworth 2nd-order high-pass / low-pass, the one-pole tilt). Aliasing: a high sine through the drive, with and without the
// oversampling (the same shaper applied at the base rate is the "non-oversampled" reference): the energy that is not the fundamental
// (all of it is alias, since every harmonic of a 9.7 kHz sine is above Nyquist or folded) must drop clearly.
// Also: the half-band oversampler (flatness, image rejection, latency), the shaper's shape, drive/harmonics, loudness compensation,
// mapping, bypass bit-exact through a chain, reset equals fresh, determinism, non-finite input, no click on parameter jumps, cost.
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
#include "velora/core/ep_amp.hpp"
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

Stereo run(Effect& fx, const Stereo& in) {
  Stereo out = in;
  for (std::size_t i = 0; i < out.l.size(); i += 64) {
    StereoBlock b{out.l.data() + i, out.r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(64, out.l.size() - i))};
    fx.process(b);
  }
  return out;
}

Stereo sine(std::size_t n, double hz, double amp) {
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

std::unique_ptr<EpAmpEffect> make(const Params& p) {
  auto fx = std::make_unique<EpAmpEffect>();
  fx->set_params(p);
  fx->prepare(48000, 64);
  return fx;
}

double db(double lin) { return 20.0 * std::log10(lin); }

// Gain in dB of the left channel at `hz`, measured over the second half of the run (the filters have settled).
double gain_db(const Params& p, double hz, double amp = 0.02, std::size_t n = 48000) {
  auto fx = make(p);
  const Stereo in = sine(n, hz, amp);
  const Stereo out = run(*fx, in);
  double si = 0.0, so = 0.0;
  for (std::size_t i = n / 2; i < n; ++i) {
    si += static_cast<double>(in.l[i]) * in.l[i];
    so += static_cast<double>(out.l[i]) * out.l[i];
  }
  return 10.0 * std::log10(so / si);
}

// The no-drive, flat baseline: drive 0, tone 0.5, presence 0, level 0.5, cabinet 0 (only the fixed 40 Hz high-pass acts).
const Params kFlat = {0.0f, 0.5f, 0.0f, 0.5f, 0.0f};

// ---- analytic references -------------------------------------------------------------------------------------------------------------
std::complex<double> z1_of(double hz) { return std::polar(1.0, -2.0 * kPi * hz / kFs); }
double biquad_db(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z = z1_of(hz), z2 = z * z;
  return db(std::abs((b0 + b1 * z + b2 * z2) / (1.0 + a1 * z + a2 * z2)));
}
double hp2_db(double f0, double hz) {
  const double w0 = 2 * kPi * f0 / kFs, c = std::cos(w0), al = std::sin(w0) / (2 * std::sqrt(0.5)), a0 = 1 + al;
  return biquad_db((1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0, hz);
}
double lp2_db(double f0, double hz) {
  const double w0 = 2 * kPi * f0 / kFs, c = std::cos(w0), al = std::sin(w0) / (2 * std::sqrt(0.5)), a0 = 1 + al;
  return biquad_db((1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0, hz);
}
double peak_db(double gain, double f0, double q, double hz) {
  const double A = std::pow(10.0, gain / 40.0), w0 = 2 * kPi * f0 / kFs, c = std::cos(w0), al = std::sin(w0) / (2 * q), a0 = 1 + al / A;
  return biquad_db((1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, -2 * c / a0, (1 - al / A) / a0, hz);
}

// ---- spectrum ---------------------------------------------------------------------------------------------------------------------------
// In-place radix-2 FFT.
void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w = 1.0;
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Power in the band [0, 22 kHz] outside +-`guard` bins around the fundamental bin, relative to the fundamental's power, in dB.
double alias_db(const std::vector<float>& x, std::size_t bin0, std::size_t guard = 4) {
  const std::size_t n = x.size();
  std::vector<std::complex<double>> a(n);
  for (std::size_t i = 0; i < n; ++i) a[i] = static_cast<double>(x[i]) * (0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n)));  // Hann
  fft(a);
  double fund = 0.0, other = 0.0;
  const std::size_t top = static_cast<std::size_t>(22000.0 / kFs * static_cast<double>(n));
  for (std::size_t k = 1; k <= top; ++k) {
    const double p = std::norm(a[k]);
    if (k + guard >= bin0 && k <= bin0 + guard) fund += p;
    else other += p;
  }
  return 10.0 * std::log10(other / fund);
}

// ---- tests -------------------------------------------------------------------------------------------------------------------------------
void test_mapping() {
  using namespace epmap;
  CHECK(drive_db(0.0f) == 0.0f && drive_db(1.0f) == 30.0f && std::fabs(drive_db(0.5f) - 15.0f) < 1e-5f);
  CHECK(tone_db(0.5f) == 0.0f && tone_db(1.0f) == 6.0f && tone_db(0.0f) == -6.0f);
  CHECK(presence_db(0.0f) == 0.0f && presence_db(1.0f) == 9.0f && std::fabs(presence_db(0.5f) - 4.5f) < 1e-5f);
  CHECK(level_db(0.5f) == 0.0f && level_db(1.0f) == 12.0f && level_db(0.0f) == -12.0f);
  CHECK(cabinet(0.0f) == 0.0f && cabinet(1.0f) == 1.0f && cabinet(0.3f) == 0.3f);
  CHECK(drive_db(std::nanf("")) == 15.0f && level_db(-4.0f) == -12.0f && cabinet(9.0f) == 1.0f);
  CHECK(kDefaultInsertParams[4] == 0.0f);  // cabinet off by default
  auto fx = make_effect(EffectType::kEpAmp);
  CHECK(fx && fx->type() == EffectType::kEpAmp && fx->cost_units() == effect_cost_units(EffectType::kEpAmp));
  CHECK(dynamic_cast<EpAmpEffect*>(fx.get()) != nullptr);
}

void test_shaper() {
  using epmap::shape;
  CHECK(shape(0.0f) == 0.0f && shape(3.0f) == 1.0f && shape(-3.0f) == -1.0f && shape(100.0f) == 1.0f && shape(-100.0f) == -1.0f);
  double worst = 0.0, prev = -2.0, odd = 0.0, step = 0.0;
  for (int i = -6000; i <= 6000; ++i) {
    const float x = static_cast<float>(i) * 0.001f;
    const double y = shape(x);
    worst = std::max(worst, std::fabs(y - std::tanh(static_cast<double>(x))));
    CHECK(y >= prev - 1e-6 && std::fabs(y) <= 1.0);  // monotonic and bounded
    prev = y;
    odd = std::max(odd, std::fabs(static_cast<double>(shape(x)) + shape(-x)));
    if (i > -6000) step = std::max(step, std::fabs(y - shape(static_cast<float>(i - 1) * 0.001f)));
  }
  std::printf("ep_amp_tests: shaper vs tanh max error %.4f, max step per 0.001 %.5f\n", worst, step);
  CHECK(worst < 0.025 && odd == 0.0 && step < 0.0011);  // tanh-like, odd, slope <= 1
  CHECK(std::fabs(shape(0.001f) / 0.001f - 1.0f) < 1e-4f);  // unity small-signal slope
}

// ---- the oversampler ---------------------------------------------------------------------------------------------------------------------
// The linear round trip of the effect's oversampler: 48 -> 96 -> 192 kHz and back, without the shaper.
std::vector<float> roundtrip(const std::vector<float>& x, std::size_t chunk = 64) {
  HalfBand2x<12> a;
  HalfBand2x<4> b;
  std::vector<float> y;
  for (std::size_t i = 0; i < x.size(); i += chunk) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min(chunk, x.size() - i));
    std::vector<float> m(2 * n), q(4 * n), m2(2 * n), o(n);
    a.up(x.data() + i, n, m.data());
    b.up(m.data(), 2 * n, q.data());
    b.down(q.data(), 2 * n, m2.data());
    a.down(m2.data(), n, o.data());
    y.insert(y.end(), o.begin(), o.end());
  }
  return y;
}

void test_oversampler() {
  // DC gain 1
  {
    const std::vector<float> y = roundtrip(std::vector<float>(256, 1.0f));
    CHECK(std::fabs(y[255] - 1.0f) < 1e-5f);
  }
  // passband flatness and image rejection of the round trip (linear, no shaper)
  auto gain = [](double hz) {
    std::vector<float> x(40000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kFs));
    const std::vector<float> y = roundtrip(x);
    double si = 0, so = 0;
    for (std::size_t i = 20000; i < x.size(); ++i) {
      si += static_cast<double>(x[i]) * x[i];
      so += static_cast<double>(y[i]) * y[i];
    }
    return 10.0 * std::log10(so / si);
  };
  double worst = 0.0;
  for (double f : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 17000.0, 18000.0}) worst = std::max(worst, std::fabs(gain(f)));
  std::printf("ep_amp_tests: oversampler round trip passband ripple up to 18 kHz %.4f dB; 20 kHz %.2f dB, 22 kHz %.2f dB\n", worst, gain(20000.0), gain(22000.0));
  CHECK(worst < 0.05 && std::fabs(gain(20000.0)) < 0.5 && gain(23500.0) < -3.0);
  // latency: the phase delay of a low sine (= the group delay of a linear-phase chain) is kLatencyFrames
  {
    const double f = 300.0;
    std::vector<float> x(8000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs));
    const std::vector<float> y = roundtrip(x);
    double re = 0, im = 0;  // phase of y relative to x over whole cycles
    for (std::size_t i = 4000; i < 8000; ++i) {
      const double ph = 2.0 * kPi * f * static_cast<double>(i) / kFs;
      re += y[i] * std::cos(ph);
      im += y[i] * std::sin(ph);
    }
    const double delay = -std::atan2(re, im) / (2.0 * kPi * f) * kFs;  // y = sin(w (t - d))  =>  re ~ -sin(wd), im ~ cos(wd)
    std::printf("ep_amp_tests: oversampler delay %.3f frames\n", delay);
    CHECK(std::fabs(delay - static_cast<double>(EpAmpEffect::kLatencyFrames)) < 0.05);
  }
  // the result does not depend on how the signal is cut into blocks
  {
    std::vector<float> x(1000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(0.37 * static_cast<double>(i)) * 0.5);
    CHECK(roundtrip(x, 50) == roundtrip(x, 64) && roundtrip(x, 7) == roundtrip(x, 64));
  }
}

// ---- aliasing ----------------------------------------------------------------------------------------------------------------------------
void test_aliasing() {
  const std::size_t n = 16384, bin0 = 3300;  // 3300 / 16384 * 48000 = 9667.97 Hz: exactly on a bin, so the fundamental has no leakage
  const double f0 = static_cast<double>(bin0) * kFs / static_cast<double>(n);
  const Stereo in = sine(n, f0, 0.5);
  for (float drive : {0.6f, 0.8f, 1.0f}) {
    auto fx = make({drive, 0.5f, 0.0f, 0.5f, 0.0f});
    const Stereo out = run(*fx, in);
    // reference: the same pre-gain, compensation and shaper at the base rate
    const float g = std::pow(10.0f, epmap::drive_db(drive) * 0.05f);
    std::vector<float> naive(n);
    for (std::size_t i = 0; i < n; ++i) naive[i] = epmap::shape(in.l[i] * g) / std::sqrt(g);
    const double a_os = alias_db(std::vector<float>(out.l.begin(), out.l.end()), bin0);
    const double a_naive = alias_db(naive, bin0);
    std::printf("ep_amp_tests: drive %.1f alias energy: oversampled %.1f dB, base rate %.1f dB (relative to the fundamental)\n", static_cast<double>(drive), a_os, a_naive);
    CHECK(a_os < a_naive - 15.0);
  }
}

// ---- drive, loudness, harmonics -----------------------------------------------------------------------------------------------------------
double harmonic_db(const Stereo& s, double f0, int k, std::size_t from) {
  double re = 0, im = 0;
  for (std::size_t i = from; i < s.l.size(); ++i) {
    const double ph = 2.0 * kPi * f0 * k * static_cast<double>(i) / kFs;
    re += s.l[i] * std::cos(ph);
    im += s.l[i] * std::sin(ph);
  }
  return db(std::hypot(re, im) * 2.0 / static_cast<double>(s.l.size() - from));
}

void test_drive() {
  // 1 kHz (an integer number of samples per 48 cycles): the 3rd harmonic relative to the fundamental rises with the drive
  double prev = -200.0;
  double rms_lo = 0, rms_hi = 0;
  for (float drive : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
    auto fx = make({drive, 0.5f, 0.0f, 0.5f, 0.0f});
    const Stereo in = sine(48000, 1000.0, 0.3);
    const Stereo out = run(*fx, in);
    const double h3 = harmonic_db(out, 1000.0, 3, 24000) - harmonic_db(out, 1000.0, 1, 24000);
    std::printf("ep_amp_tests: drive %.1f: 3rd harmonic %.1f dB re fundamental\n", static_cast<double>(drive), h3);
    CHECK(h3 > prev - 0.5);
    prev = h3;
    double s = 0;
    for (std::size_t i = 24000; i < 48000; ++i) s += static_cast<double>(out.l[i]) * out.l[i];
    const double rms = std::sqrt(s / 24000.0);
    if (drive == 0.2f) rms_lo = rms;
    if (drive == 1.0f) rms_hi = rms;
  }
  CHECK(prev > -12.0);  // a strongly driven amp is audibly distorted
  // loudness compensation: from drive 0.2 to 1.0 (+24 dB of pre-gain) the output level moves by far less than 24 dB
  std::printf("ep_amp_tests: output rms drive 0.2 -> 1.0: %.1f dB\n", db(rms_hi / rms_lo));
  CHECK(std::fabs(db(rms_hi / rms_lo)) < 8.0);
  // even a driven output stays bounded (the ceiling is 1/sqrt(g), then level and the filters)
  auto fx = make({1.0f, 0.5f, 1.0f, 1.0f, 0.0f});
  const Stereo hot = run(*fx, sine(48000, 330.0, 1.0));
  float pk = 0;
  for (float v : hot.l) pk = std::max(pk, std::fabs(v));
  CHECK(pk < 4.0f);
}

// ---- tone, presence, cabinet, level -------------------------------------------------------------------------------------------------------
void test_filters() {
  // flat baseline = the 40 Hz Butterworth high-pass only
  for (double f : {30.0, 40.0, 80.0, 200.0, 1000.0, 8000.0, 16000.0}) CHECK(std::fabs(gain_db(kFlat, f) - hp2_db(40.0, f)) < 0.1);
  // level
  CHECK(std::fabs(gain_db({0.0f, 0.5f, 0.0f, 1.0f, 0.0f}, 1000.0) - (hp2_db(40.0, 1000.0) + 12.0)) < 0.1);
  CHECK(std::fabs(gain_db({0.0f, 0.5f, 0.0f, 0.0f, 0.0f}, 1000.0) - (hp2_db(40.0, 1000.0) - 12.0)) < 0.1);
  // presence: the RBJ peak at 3 kHz
  for (float p : {0.5f, 1.0f})
    for (double f : {500.0, 1500.0, 3000.0, 6000.0}) {
      const double want = hp2_db(40.0, f) + peak_db(static_cast<double>(epmap::presence_db(p)), 3000.0, 0.9, f);
      CHECK(std::fabs(gain_db({0.0f, 0.5f, p, 0.5f, 0.0f}, f) - want) < 0.15);
    }
  // cabinet: blend of the 80 Hz high-pass and the 5 kHz low-pass (both Butterworth); half amount = linear blend of the two paths, so check full
  for (double f : {40.0, 80.0, 200.0, 2000.0, 5000.0, 8000.0, 12000.0}) {
    const double want = hp2_db(40.0, f) + hp2_db(80.0, f) + lp2_db(5000.0, f);
    CHECK(std::fabs(gain_db({0.0f, 0.5f, 0.0f, 0.5f, 1.0f}, f) - want) < 0.15);
  }
  CHECK(gain_db({0.0f, 0.5f, 0.0f, 0.5f, 1.0f}, 12000.0) < gain_db(kFlat, 12000.0) - 12.0);  // the cabinet really darkens
  // tone: a tilt around 1 kHz: y = gh x + (gl - gh) lp(x) with the one-pole low-pass lp; compare with that formula, and check the direction
  for (float tone : {0.0f, 0.25f, 1.0f}) {
    const double t = static_cast<double>(epmap::tone_db(tone)), gl = std::pow(10.0, -t / 20.0), gh = std::pow(10.0, t / 20.0);
    const double a = 1.0 - std::exp(-2.0 * kPi * 1000.0 / kFs);
    for (double f : {100.0, 500.0, 1000.0, 3000.0, 8000.0}) {
      const std::complex<double> z = z1_of(f), lp = a / (1.0 - (1.0 - a) * z);
      const double want = hp2_db(40.0, f) + db(std::abs(gh + (gl - gh) * lp));
      CHECK(std::fabs(gain_db({0.0f, tone, 0.0f, 0.5f, 0.0f}, f) - want) < 0.15);
    }
  }
  const double lo_flat = gain_db(kFlat, 100.0), hi_flat = gain_db(kFlat, 8000.0);
  CHECK(gain_db({0.0f, 1.0f, 0.0f, 0.5f, 0.0f}, 8000.0) - hi_flat > 4.5 && gain_db({0.0f, 1.0f, 0.0f, 0.5f, 0.0f}, 100.0) - lo_flat < -4.5);
  CHECK(gain_db({0.0f, 0.0f, 0.0f, 0.5f, 0.0f}, 8000.0) - hi_flat < -3.0 && gain_db({0.0f, 0.0f, 0.0f, 0.5f, 0.0f}, 100.0) - lo_flat > 4.5);
}

// ---- state, determinism, robustness ---------------------------------------------------------------------------------------------------------
void test_state_and_robustness() {
  const Stereo in = noise(30000, 3);
  const Params p = {0.7f, 0.3f, 0.6f, 0.4f, 0.5f};
  auto a = make(p), b = make(p);
  const Stereo oa = run(*a, in), ob = run(*b, in);
  CHECK(oa.l == ob.l && oa.r == ob.r);
  // stereo channels are independent and identical for identical input
  {
    Stereo mono = in;
    mono.r = mono.l;
    auto fx = make(p);
    const Stereo o = run(*fx, mono);
    CHECK(o.l == o.r);
  }
  // (block-split invariance, an empty block and an oversized block: test_blocks_and_prepare)
  // non-finite input is exactly a zero sample
  {
    Stereo dirty = noise(30000, 12), clean = dirty;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (std::size_t i = 1000; i < 29000; i += 997) {
      dirty.l[i] = nan;
      clean.l[i] = 0.0f;
      dirty.r[i + 1] = i % 2 ? inf : -inf;
      clean.r[i + 1] = 0.0f;
    }
    auto x = make(p), y = make(p);
    const Stereo od = run(*x, dirty), oc = run(*y, clean);
    bool finite = true;
    for (std::size_t i = 0; i < od.l.size(); ++i) finite = finite && std::isfinite(od.l[i]) && std::isfinite(od.r[i]);
    CHECK(finite && od.l == oc.l && od.r == oc.r);
  }
  // NaN / out-of-range parameters
  {
    auto fx = make({std::nanf(""), -3.0f, 9.0f, std::nanf(""), 4.0f});
    const Stereo o = run(*fx, in);
    bool finite = true;
    for (std::size_t i = 0; i < o.l.size(); ++i) finite = finite && std::isfinite(o.l[i]);
    CHECK(finite);
  }
  // reset equals fresh
  {
    auto used = make(p), fresh = make(p);
    run(*used, noise(20000, 8));
    used->reset();
    const Stereo ra = run(*used, in), rb = run(*fresh, in);
    CHECK(ra.l == rb.l && ra.r == rb.r);
  }
  // a bypassed chain slot is bit-exact and keeps reserving its cost
  {
    FxChain chain;
    chain.set_slot(0, make(p), true);
    Stereo x = noise(640, 5);
    const Stereo ref = x;
    for (std::size_t i = 0; i < x.l.size(); i += 64) {
      StereoBlock bl{x.l.data() + i, x.r.data() + i, 64};
      chain.process(bl);
    }
    CHECK(x.l == ref.l && x.r == ref.r && chain.cost_units() == effect_cost_units(EffectType::kEpAmp));
  }
  // silence in, silence out (no DC, no noise from the filters)
  {
    auto fx = make({1.0f, 0.9f, 1.0f, 1.0f, 1.0f});
    Stereo z;
    z.l.assign(4096, 0.0f);
    z.r.assign(4096, 0.0f);
    const Stereo o = run(*fx, z);
    CHECK(o.l == z.l && o.r == z.r);
  }
}

void test_click_free() {
  // parameter jumps on a running instance: the smoothing keeps every step near the signal's own slope
  const std::size_t n = 96000;
  const Stereo in = sine(n, 220.0, 0.4);
  auto fx = make({0.1f, 0.5f, 0.2f, 0.5f, 0.0f});
  Stereo a = in;
  for (std::size_t i = 0; i < n; i += 64) {
    if (i == 20032) fx->set_params({1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    if (i == 60032) fx->set_params({0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
    fx->process(b);
  }
  double worst = 0.0, level = 0.0;
  for (std::size_t i = 1; i < n; ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(a.l[i]) - a.l[i - 1]));
    level = std::max(level, std::fabs(static_cast<double>(a.l[i])));
  }
  // the largest output (drive 1, level +12 dB) sets the scale: a step must stay a small fraction of it
  std::printf("ep_amp_tests: max step through full-range parameter jumps %.4f, peak %.3f\n", worst, level);
  CHECK(worst < 0.25 * level);
}

void test_cost() {
  CHECK(effect_cost_units(EffectType::kEpAmp) > effect_cost_units(EffectType::kEq));  // the oversampling is declared in the cost
  CHECK(effect_cost_units(EffectType::kEpAmp) <= kMaxInsertCost);
  auto fx = make({0.8f, 0.5f, 0.5f, 0.5f, 1.0f});
  Stereo a = noise(64, 1, 0.5f);
  const int iters = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    StereoBlock b{a.l.data(), a.r.data(), 64};
    fx->process(b);
    for (std::size_t k = 0; k < 64; ++k) {
      a.l[k] = 0.5f * std::sin(0.3f * static_cast<float>(k + static_cast<std::size_t>(i)));
      a.r[k] = a.l[k];
    }
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / iters;
  std::printf("host cost per 64-frame block: ep_amp %.2f us (declared %u units)\n", us, effect_cost_units(EffectType::kEpAmp));
}


std::complex<double> biquad_c(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z = z1_of(hz), z2 = z * z;
  return (b0 + b1 * z + b2 * z2) / (1.0 + a1 * z + a2 * z2);
}
std::complex<double> cab_c(double hz) {  // 80 Hz high-pass * 5 kHz low-pass (Butterworth)
  auto bq = [&](bool high, double f0) {
    const double w0 = 2 * kPi * f0 / kFs, c = std::cos(w0), al = std::sin(w0) / (2 * std::sqrt(0.5)), a0 = 1 + al;
    return high ? biquad_c((1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0, hz)
                : biquad_c((1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0, hz);
  };
  return bq(true, 80.0) * bq(false, 5000.0);
}

void test_blocks_prepare_robustness() {
  const Stereo in = noise(1000, 41);
  const Params p = {0.7f, 0.3f, 0.6f, 0.4f, 0.5f};
  auto ref = make(p), big = make(p), one = make(p), odd = make(p);
  const Stereo r64 = run(*ref, in);
  Stereo b200 = in, b1 = in, b7 = in, b0 = in;
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
  StereoBlock empty{b0.l.data(), b0.r.data(), 0};
  ref->process(empty);  // a zero-length block is a no-op
  CHECK(b0.l == in.l);
  // an oversized block is processed in full, and the oversampler's history does not depend on the split
  CHECK(b200.l == r64.l && b200.r == r64.r);
  CHECK(b1.l == r64.l && b1.r == r64.r && b7.l == r64.l && b7.r == r64.r);
  // prepare() refusals
  {
    EpAmpEffect fx;
    CHECK(fx.prepare_ok());
    fx.prepare(96000, 64);
    CHECK(!fx.prepare_ok());
    Stereo a = in;
    CHECK(run(fx, a).l == in.l);
    fx.prepare(48000, 256);
    CHECK(!fx.prepare_ok());
    fx.prepare(48000, 64);
    CHECK(fx.prepare_ok());
    CHECK(run(fx, a).l != in.l);
  }
  // huge finite input: no inf / NaN left in the state, the output recovers
  {
    auto fx = make({1.0f, 0.5f, 0.5f, 0.5f, 1.0f});
    Stereo huge;
    huge.l.assign(640, 1.0e38f);
    huge.r.assign(640, -3.0e38f);
    const Stereo oh = run(*fx, huge);
    bool finite = true;
    for (std::size_t i = 0; i < 640; ++i) finite = finite && std::isfinite(oh.l[i]) && std::isfinite(oh.r[i]);
    CHECK(finite);
    const Stereo back = run(*fx, sine(48000, 500.0, 0.2));
    auto fresh = make({1.0f, 0.5f, 0.5f, 0.5f, 1.0f});
    const Stereo ref2 = run(*fresh, sine(48000, 500.0, 0.2));
    finite = true;
    for (float v : back.l) finite = finite && std::isfinite(v);
    CHECK(finite);
    double worst = 0;
    for (std::size_t i = 24000; i < 48000; ++i) worst = std::max(worst, std::fabs(static_cast<double>(back.l[i]) - ref2.l[i]));
    CHECK(worst < 1e-3);
  }
  // raising CABINET from 0: the filters are primed, so there is no transient beyond the step of the blend itself
  {
    const Stereo sig = sine(48000, 300.0, 0.4);
    auto fx = make({0.0f, 0.5f, 0.0f, 0.5f, 0.0f});
    Stereo a = sig;
    for (std::size_t i = 0; i < 48000; i += 64) {
      if (i == 24000 - 24000 % 64) fx->set_params({0.0f, 0.5f, 0.0f, 0.5f, 1.0f});
      StereoBlock b{a.l.data() + i, a.r.data() + i, 64};
      fx->process(b);
    }
    // reference: a fresh amp with the cabinet on from the start, same signal; the blended output stays within the span of the two steady outputs
    auto on = make({0.0f, 0.5f, 0.0f, 0.5f, 1.0f}), off = make({0.0f, 0.5f, 0.0f, 0.5f, 0.0f});
    const Stereo yon = run(*on, sig), yoff = run(*off, sig);
    double worst = 0;
    for (std::size_t i = 23000; i < 26000; ++i) {
      const double lo = std::min(yon.l[i], yoff.l[i]), hi = std::max(yon.l[i], yoff.l[i]);
      worst = std::max({worst, lo - a.l[i], a.l[i] - hi});
    }
    CHECK(worst < 0.01);
  }
}

// Running instance: set_params(B) converges to what a fresh instance with B does; partial cabinet blend against the analytic response.
void test_running_params() {
  const Params A = {0.2f, 0.5f, 0.0f, 0.5f, 0.0f}, B = {0.6f, 0.85f, 0.9f, 0.3f, 0.7f};
  for (double f : {300.0, 3000.0, 8000.0}) {
    auto live = make(A), fresh = make(B);
    run(*live, sine(4800, f, 0.02));
    live->set_params(B);
    const Stereo in = sine(19200, f, 0.02);
    const Stereo a = run(*live, in), b = run(*fresh, in);
    double ra = 0, rb = 0;
    for (std::size_t i = 14400; i < 19200; ++i) {
      ra += static_cast<double>(a.l[i]) * a.l[i];
      rb += static_cast<double>(b.l[i]) * b.l[i];
    }
    CHECK(std::fabs(10.0 * std::log10(ra / rb)) < 0.1);
  }
  // each parameter alone, including the presence redesign reaching its target (gain at 3 kHz)
  for (int which = 0; which < 5; ++which) {
    Params Bp = A;
    Bp[static_cast<std::size_t>(which)] = which == 4 ? 1.0f : 0.95f;
    auto live = make(A), fresh = make(Bp);
    run(*live, sine(4800, 3000.0, 0.02));
    live->set_params(Bp);
    const Stereo in = sine(19200, 3000.0, 0.02);
    const Stereo a = run(*live, in), b = run(*fresh, in);
    double ra = 0, rb = 0;
    for (std::size_t i = 14400; i < 19200; ++i) {
      ra += static_cast<double>(a.l[i]) * a.l[i];
      rb += static_cast<double>(b.l[i]) * b.l[i];
    }
    CHECK(std::fabs(10.0 * std::log10(ra / rb)) < 0.1);
  }
  // CABINET 0.5: the blend x + 0.5 (cab(x) - x) has the response |0.5 + 0.5 Hcab| (complex sum), times the 40 Hz high-pass
  for (double f : {60.0, 100.0, 1000.0, 4000.0, 6000.0, 9000.0}) {
    const std::complex<double> h = 0.5 + 0.5 * cab_c(f);
    const double want = hp2_db(40.0, f) + db(std::abs(h));
    CHECK(std::fabs(gain_db({0.0f, 0.5f, 0.0f, 0.5f, 0.5f}, f) - want) < 0.15);
  }
}

}  // namespace

int main() {
  test_mapping();
  test_shaper();
  test_oversampler();
  test_aliasing();
  test_drive();
  test_filters();
  test_state_and_robustness();
  test_click_free();
  test_blocks_prepare_robustness();
  test_running_params();
  test_cost();
  if (g_fail) {
    std::printf("ep_amp_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("ep_amp_tests: ok\n");
  return 0;
}

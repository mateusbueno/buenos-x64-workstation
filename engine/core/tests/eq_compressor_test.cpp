// Story 2.8: EQ and compressor as inserts and in the master section.
//
// EQ: the "design curve" is the magnitude of the analog RBJ cookbook prototype (shelf slope S = 1, peak Q = 1) with s normalised to the
// band's corner / centre frequency, evaluated at the real frequency (this test carries its own copy of the prototype formulas, independent of
// the implementation). The biquads (bilinear transform, pre-warped at the corner) must be within +-0.5 dB of it at the test frequencies
// below; the response is measured twice: from the coefficients (EqEffect::magnitude_db) and with sine sweeps through process().
// The mid peak is a matched-magnitude design (eq.cpp) so the bilinear warp does not pull it down: +-0.5 dB up to 16 kHz for every mid
// frequency 200..8000 Hz at +-15 dB, 1 dB at 20 kHz (the last test point, 0.42 x fs); no restricted range per mid frequency.
//
// Compressor: attack / release are the time to 63.2 % of the gain-reduction step (see compressor.hpp), measured on a DC step from the
// output/input ratio; the static curve, stereo link, makeup, identity and the published gain reduction are checked as well. Then the same
// DSP in the Mixer: master EQ then compressor in that order, separate bypass cross-fades (step bound), identity = bit-exact, published
// gain reduction per insert slot and master bus.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/compressor.hpp"
#include "velora/core/eq.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

double db_of(double lin) { return 20.0 * std::log10(lin); }

// ---- independent analog prototypes (RBJ cookbook, s normalised to f0) ----------------------------------------------------------------
double analog_db(char kind, double gain_db, double f0, double f) {
  const double A = std::pow(10.0, gain_db / 40.0), r = std::sqrt(A) * std::sqrt(2.0);  // sqrt(A)/Q with Q = 1/sqrt(2)
  const std::complex<double> s(0.0, f / f0);
  std::complex<double> h;
  if (kind == 'p') h = (s * s + s * A / 1.0 + 1.0) / (s * s + s / (A * 1.0) + 1.0);
  else if (kind == 'l') h = A * (s * s + r * s + A) / (A * s * s + r * s + 1.0);
  else h = A * (A * s * s + r * s + 1.0) / (s * s + r * s + A);
  return db_of(std::abs(h));
}

const double kTestFreqs[] = {20, 31.5, 63, 100, 125, 250, 500, 1000, 2000, 3000, 4000, 6000, 8000, 10000, 12000, 16000, 20000};

double design_db(double low, double mid, double mid_hz, double high, double f) {
  double d = 0.0;
  if (low != 0.0) d += analog_db('l', low, eqmap::kLowShelfHz, f);
  if (mid != 0.0) d += analog_db('p', mid, mid_hz, f);
  if (high != 0.0) d += analog_db('h', high, eqmap::kHighShelfHz, f);
  return d;
}

double peak_tol(double f) { return f > 16000.0 ? 1.0 : 0.5; }

// Steady-state gain of a sine through process() in dB (I/Q correlation over an integer number of cycles).
double measure_db(EqEffect& eq, double hz) {
  constexpr std::uint32_t kN = 24000, kMeasure = 12000;  // hz is a multiple of 4: kMeasure holds whole cycles
  std::vector<float> l(kN), r(kN);
  for (std::uint32_t i = 0; i < kN; ++i) l[i] = r[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / kFs));
  for (std::uint32_t o = 0; o < kN; o += 64) {
    StereoBlock b{&l[o], &r[o], 64};
    eq.process(b);
  }
  double re = 0, im = 0, rr = 0;
  for (std::uint32_t i = kN - kMeasure; i < kN; ++i) {
    re += l[i] * std::cos(2.0 * kPi * hz * i / kFs);
    im += l[i] * std::sin(2.0 * kPi * hz * i / kFs);
    rr += r[i] * std::sin(2.0 * kPi * hz * i / kFs);
  }
  const double amp = 2.0 * std::sqrt(re * re + im * im) / kMeasure;
  CHECK(std::fabs(2.0 * rr / kMeasure - 2.0 * im / kMeasure) < 1e-6);  // both channels identical
  return db_of(amp / 0.25);
}

// ---- EQ -------------------------------------------------------------------------------------------------------------------------
void test_eq_mapping() {
  CHECK(eqmap::gain_db(0.0f) == -15.0f && eqmap::gain_db(1.0f) == 15.0f && eqmap::gain_db(0.5f) == 0.0f);
  CHECK(std::fabs(eqmap::gain_db(0.75f) - 7.5f) < 1e-5f && std::fabs(eqmap::gain_db(0.25f) + 7.5f) < 1e-5f);
  CHECK(std::fabs(eqmap::mid_hz(0.0f) - 200.0f) < 1e-3f && std::fabs(eqmap::mid_hz(1.0f) - 8000.0f) < 0.1f);
  CHECK(std::fabs(eqmap::mid_hz(0.5f) - 1264.9f) < 0.2f);  // geometric middle of 200..8000
  CHECK(eqmap::gain_db(-3.0f) == -15.0f && eqmap::gain_db(7.0f) == 15.0f && eqmap::gain_db(std::nanf("")) == 0.0f);  // clamped, NaN = centre
  // insert params 1..4 are LOW, MID, MID FREQ, HIGH: the same as the physical master values
  EqEffect a, b;
  a.set_params({0.9f, 0.2f, 0.7f, 0.65f, 0.0f});
  b.set_physical(eqmap::gain_db(0.9f), eqmap::gain_db(0.2f), eqmap::mid_hz(0.7f), eqmap::gain_db(0.65f), true);
  for (double f : kTestFreqs) CHECK(a.magnitude_db(f) == b.magnitude_db(f));
  // out-of-range physical values are clamped to the registry range
  EqEffect c, d;
  c.set_physical(99.0f, -99.0f, 10.0f, 15.0f, true);
  d.set_physical(15.0f, -15.0f, 200.0f, 15.0f, true);
  for (double f : kTestFreqs) CHECK(c.magnitude_db(f) == d.magnitude_db(f));
}

void test_eq_design_curve() {
  const double gains[] = {15.0, -15.0, 6.0, -6.0};
  const double mids[] = {200.0, 500.0, 1000.0, 2000.0, 3000.0, 4000.0, 6000.0, 8000.0};
  double worst = 0.0;
  for (double g : gains) {
    for (double f : kTestFreqs) {  // low shelf, high shelf (each alone)
      EqEffect e;
      e.set_physical(static_cast<float>(g), 0, 1000, 0, true);
      worst = std::max(worst, std::fabs(e.magnitude_db(f) - design_db(g, 0, 1000, 0, f)));
      CHECK(std::fabs(e.magnitude_db(f) - design_db(g, 0, 1000, 0, f)) <= 0.5);
      EqEffect h;
      h.set_physical(0, 0, 1000, static_cast<float>(g), true);
      CHECK(std::fabs(h.magnitude_db(f) - design_db(0, 0, 1000, g, f)) <= 0.5);
      worst = std::max(worst, std::fabs(h.magnitude_db(f) - design_db(0, 0, 1000, g, f)));
    }
    for (double m : mids) {
      for (double f : kTestFreqs) {
        EqEffect e;
        e.set_physical(0, static_cast<float>(g), static_cast<float>(m), 0, true);
        CHECK(std::fabs(e.magnitude_db(f) - design_db(0, g, m, 0, f)) <= peak_tol(f));
        EqEffect all;  // the three bands together
        all.set_physical(static_cast<float>(g), static_cast<float>(-g), static_cast<float>(m), static_cast<float>(g * 0.5), true);
        CHECK(std::fabs(all.magnitude_db(f) - design_db(g, -g, m, g * 0.5, f)) <= peak_tol(f) + 0.3);  // bands add their small errors
      }
    }
  }
  // the extremes are really +-15 dB at the corner / centre frequencies and 0 dB far away (sanity for the design curve itself)
  EqEffect e;
  e.set_physical(15, 0, 1000, 0, true);
  CHECK(std::fabs(e.magnitude_db(20) - 15.0) < 0.2 && std::fabs(e.magnitude_db(16000)) < 0.2);
  EqEffect p;
  p.set_physical(0, -15, 1000, 0, true);
  CHECK(std::fabs(p.magnitude_db(1000) + 15.0) < 1e-6);
  std::printf("eq: worst shelf deviation (low and high) from the design curve %.3f dB\n", worst);
}

void test_eq_sine_sweep() {
  struct Cfg {
    float low, mid, hz, high;
  };
  const Cfg cfgs[] = {{12, -9, 1000, 6}, {-15, 15, 300, -15}, {15, 15, 2000, -6}, {5, 0, 1000, 0}};
  const double freqs[] = {32, 100, 500, 1000, 4000, 8000, 12000};
  for (const Cfg& c : cfgs) {
    for (double f : freqs) {
      EqEffect e;
      e.set_physical(c.low, c.mid, c.hz, c.high, true);
      const double got = measure_db(e, f), want = e.magnitude_db(f);
      CHECK(std::fabs(got - want) < 0.03);
      CHECK(std::fabs(got - design_db(c.low, c.mid, c.hz, c.high, f)) <= 0.8);
    }
  }
}

void test_eq_identity_and_robustness() {
  std::vector<float> l(640), r(640);
  std::uint32_t s = 1;
  for (std::size_t i = 0; i < l.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    l[i] = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
    r[i] = -l[i] * 0.5f;
  }
  const auto l0 = l, r0 = r;
  EqEffect e;  // default = flat
  CHECK(e.is_identity());
  for (std::size_t o = 0; o < l.size(); o += 64) {
    StereoBlock b{&l[o], &r[o], 64};
    e.process(b);
  }
  CHECK(std::memcmp(l.data(), l0.data(), l.size() * 4) == 0 && std::memcmp(r.data(), r0.data(), r.size() * 4) == 0);
  // a smoothed move back to 0 dB ends at exact identity (and then is bit-exact again)
  e.set_physical(9, -6, 800, 4, true);
  CHECK(!e.is_identity());
  e.set_physical(0, 0, 800, 0, false);
  StereoBlock junk{l.data(), r.data(), 64};
  for (int i = 0; i < 400 && !e.is_identity(); ++i) e.process(junk);
  CHECK(e.is_identity());
  auto l1 = l0, r1 = r0;
  StereoBlock b1{l1.data(), r1.data(), 64};
  e.process(b1);
  CHECK(std::memcmp(l1.data(), l0.data(), 64 * 4) == 0);
  // a non-finite sample does not poison the filter
  EqEffect f;
  f.set_physical(6, 6, 1000, 6, true);
  std::vector<float> x(64, 0.1f), y(64, 0.1f);
  x[3] = std::nanf("");
  y[5] = INFINITY;
  StereoBlock nb{x.data(), y.data(), 64};
  f.process(nb);
  std::vector<float> a(64, 0.1f), b(64, 0.1f);
  for (int i = 0; i < 20; ++i) {
    StereoBlock ok{a.data(), b.data(), 64};
    std::fill(a.begin(), a.end(), 0.1f);
    std::fill(b.begin(), b.end(), 0.1f);
    f.process(ok);
  }
  CHECK(std::isfinite(a[63]) && std::isfinite(b[63]));
}

void test_eq_smoothing_no_click() {
  // 1 kHz sine, the mid band moves 0 -> +15 dB: the output stays continuous (no step above the steady-state slope of the final signal)
  EqEffect e;
  e.set_physical(0, 0, 1000, 0, true);
  const double amp = 0.3, final_gain = std::pow(10.0, 15.0 / 20.0);
  const double max_step = amp * final_gain * 2.0 * kPi * 1000.0 / kFs * 1.05;
  std::vector<float> l(64 * 80), r;
  for (std::size_t i = 0; i < l.size(); ++i) l[i] = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
  r = l;
  for (std::size_t o = 0; o < l.size(); o += 64) {
    if (o == 64 * 10) e.set_physical(0, 15, 1000, 0, false);
    StereoBlock b{&l[o], &r[o], 64};
    e.process(b);
  }
  double worst = 0, peak = 0;
  for (std::size_t i = 1; i < l.size(); ++i) {
    worst = std::max(worst, static_cast<double>(std::fabs(l[i] - l[i - 1])));
    peak = std::max(peak, static_cast<double>(std::fabs(l[i])));
  }
  CHECK(worst <= max_step);
  CHECK(peak <= amp * final_gain * 1.05);  // no overshoot beyond the final level
  CHECK(e.magnitude_db(1000) > 14.9);       // and it arrived
  // the same move applied instantly does not click either for a pure gain at the centre (sanity), but the jump of a hard bypass would:
  // |wet - dry| of the final signal is far above the bound, which is what the smoothing / cross-fade avoids
  CHECK(amp * (final_gain - 1.0) > max_step);
}

// ---- compressor -----------------------------------------------------------------------------------------------------------------
void test_comp_mapping() {
  using namespace compmap;
  CHECK(threshold_db(0.0f) == -60.0f && threshold_db(1.0f) == 0.0f && std::fabs(threshold_db(0.5f) + 30.0f) < 1e-5f);
  CHECK(ratio(0.0f) == 1.0f && std::fabs(ratio(1.0f) - 20.0f) < 1e-4f && std::fabs(ratio(0.5f) - 4.4721f) < 1e-3f);
  CHECK(std::fabs(attack_ms(0.0f) - 0.1f) < 1e-6f && std::fabs(attack_ms(1.0f) - 100.0f) < 1e-3f && std::fabs(attack_ms(0.5f) - 3.1623f) < 1e-3f);
  CHECK(std::fabs(release_ms(0.0f) - 10.0f) < 1e-5f && std::fabs(release_ms(1.0f) - 1000.0f) < 1e-2f && std::fabs(release_ms(0.5f) - 100.0f) < 1e-3f);
  CHECK(makeup_db(0.0f) == 0.0f && makeup_db(1.0f) == 24.0f && makeup_db(0.25f) == 6.0f);
  CHECK(threshold_db(std::nanf("")) == -30.0f && makeup_db(std::nanf("")) == 0.0f);
  CHECK(kDefaultInsertParams[4] == 0.0f);
  const params::ParamDef* d = params::find(params::kInsertParam5);
  CHECK(d && d->def == 0.0 && d->cls == params::Class::kStructural);
}

// Feeds `value` (DC, both channels) for `samples` and returns the per-sample gain in dB (output / input).
std::vector<double> run_dc(CompressorEffect& c, float value, std::size_t samples) {
  std::vector<double> g;
  g.reserve(samples);
  for (std::size_t o = 0; o < samples; o += 64) {
    float l[64], r[64];
    for (int i = 0; i < 64; ++i) l[i] = r[i] = value;
    StereoBlock b{l, r, 64};
    c.process(b);
    for (int i = 0; i < 64; ++i) g.push_back(db_of(static_cast<double>(l[i]) / value));
  }
  return g;
}

// Index (fractional, linear interpolation) at which the gain-reduction sequence crosses `level` (monotone sequence).
double crossing(const std::vector<double>& gr, double level) {
  for (std::size_t i = 1; i < gr.size(); ++i) {
    if ((gr[i - 1] - level) * (gr[i] - level) <= 0.0 && gr[i - 1] != gr[i])
      return static_cast<double>(i - 1) + (level - gr[i - 1]) / (gr[i] - gr[i - 1]);
  }
  return -1.0;
}

void test_comp_static_curve_and_times() {
  // static curve: DC 0.5 (-6.02 dB), threshold -20 dB, ratio 4 -> reduction (-6.02 + 20) * (1 - 1/4) = 10.48 dB
  {
    CompressorEffect c;
    c.set_physical(-20, 4, 1, 50, 0, true);
    const std::vector<double> g = run_dc(c, 0.5f, 48000);
    const double want = (db_of(0.5) + 20.0) * 0.75;
    CHECK(std::fabs(-g.back() - want) < 0.01);
    CHECK(std::fabs(c.gain_reduction_db() - want) < 0.01);
    // below the threshold nothing happens
    CompressorEffect q;
    q.set_physical(-20, 4, 1, 50, 0, true);
    const std::vector<double> h = run_dc(q, 0.05f, 4800);
    CHECK(std::fabs(h.back()) < 1e-6 && q.gain_reduction_db() == 0.0f);
  }
  // attack: 63.2 % of the final reduction after `attack` ms, within 10 % of the setting
  for (double att : {1.0, 5.0, 10.0, 50.0}) {
    CompressorEffect c;
    c.set_physical(-40, 10, static_cast<float>(att), 200, 0, true);
    run_dc(c, 0.001f, 6400);  // -60 dB: below threshold, settled
    const std::vector<double> g = run_dc(c, 0.5f, static_cast<std::size_t>(att * 48.0 * 8.0) / 64 * 64 + 64);
    std::vector<double> gr(g.size());
    for (std::size_t i = 0; i < g.size(); ++i) gr[i] = -g[i];
    const double final_gr = (db_of(0.5) + 40.0) * 0.9;
    const double t = crossing(gr, final_gr * (1.0 - std::exp(-1.0))) / 48.0;  // ms; the step happens at sample 0 (the gain applies from sample 0)
    CHECK(t > 0 && std::fabs(t - att) <= 0.1 * att);
    std::printf("compressor attack: set %.1f ms, measured %.3f ms\n", att, t);
  }
  // release: the reduction falls to 36.8 % of where it started after `release` ms
  for (double rel : {20.0, 100.0, 300.0}) {
    CompressorEffect c;
    c.set_physical(-40, 10, 1, static_cast<float>(rel), 0, true);
    run_dc(c, 0.5f, 48000);  // settled at the full reduction
    const double start = c.gain_reduction_db();
    CHECK(start > 20.0);
    const std::vector<double> g = run_dc(c, 0.001f, static_cast<std::size_t>(rel * 48.0 * 6.0) / 64 * 64 + 64);
    std::vector<double> gr(g.size());
    for (std::size_t i = 0; i < g.size(); ++i) gr[i] = -g[i];
    const double t = crossing(gr, start * std::exp(-1.0)) / 48.0;
    CHECK(t > 0 && std::fabs(t - rel) <= 0.1 * rel);
    std::printf("compressor release: set %.0f ms, measured %.3f ms\n", rel, t);
  }
}

void test_comp_stereo_makeup_identity() {
  // stereo link: loud left, quiet right -> the same gain on both
  CompressorEffect c;
  c.set_physical(-30, 8, 0.5f, 100, 0, true);
  float l[64], r[64];
  float gl = 0, gr = 0;
  for (int blk = 0; blk < 200; ++blk) {
    for (int i = 0; i < 64; ++i) {
      l[i] = 0.8f;
      r[i] = 0.01f;
    }
    StereoBlock b{l, r, 64};
    c.process(b);
    gl = l[63] / 0.8f;
    gr = r[63] / 0.01f;
  }
  CHECK(gl < 0.5f && std::fabs(gl - gr) < 1e-6f);
  // makeup only: ratio 1 -> +6 dB after the ramp (first block ramps, the second is steady)
  CompressorEffect m;
  m.set_physical(-12, 1, 10, 100, 6, true);
  CHECK(!m.is_identity());
  float a[64], b[64];
  for (int blk = 0; blk < 3; ++blk) {
    for (int i = 0; i < 64; ++i) a[i] = b[i] = 0.1f;
    StereoBlock sb{a, b, 64};
    m.process(sb);
  }
  CHECK(std::fabs(db_of(a[63] / 0.1) - 6.0) < 1e-3);
  // makeup change ramps without a step: 0 -> 12 dB over one block
  CompressorEffect k;
  k.set_physical(-12, 1, 10, 100, 0, true);
  k.set_physical(-12, 1, 10, 100, 12, false);
  for (int i = 0; i < 64; ++i) a[i] = b[i] = 0.1f;
  StereoBlock kb{a, b, 64};
  k.process(kb);
  double worst = 0;
  for (int i = 1; i < 64; ++i) worst = std::max(worst, static_cast<double>(std::fabs(a[i] - a[i - 1])));
  CHECK(worst < 0.1 * 4.0 / 64.0 * 1.5);
  // identity (ratio 1, makeup 0): bit-exact for any signal; the default insert params compress (ratio 4.5, threshold -30 dB)
  CompressorEffect id;
  id.set_physical(-12, 1, 10, 100, 0, true);
  CHECK(id.is_identity());
  std::vector<float> x(256), y(256);
  std::uint32_t s = 7;
  for (std::size_t i = 0; i < x.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    x[i] = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    y[i] = x[i] * 0.3f;
  }
  const auto x0 = x, y0 = y;
  for (std::size_t o = 0; o < x.size(); o += 64) {
    StereoBlock ib{&x[o], &y[o], 64};
    id.process(ib);
  }
  CHECK(std::memcmp(x.data(), x0.data(), x.size() * 4) == 0 && std::memcmp(y.data(), y0.data(), y.size() * 4) == 0);
  CHECK(id.gain_reduction_db() == 0.0f);
  auto fx = make_effect(EffectType::kCompressor);
  CHECK(fx && fx->cost_units() == effect_cost_units(EffectType::kCompressor) && fx->gain_reduction_db() == 0.0f);
  // ratio back to 1 after compressing: the reduction recovers to exactly 0 and the effect is the identity again
  CompressorEffect rec;
  rec.set_physical(-40, 10, 1, 20, 0, true);
  run_dc(rec, 0.5f, 4800);
  CHECK(!rec.is_identity() && rec.gain_reduction_db() > 10.0f);
  rec.set_physical(-40, 1, 1, 20, 0, true);
  run_dc(rec, 0.5f, 48000);
  CHECK(rec.is_identity() && rec.gain_reduction_db() == 0.0f);
  // non-finite input does not poison the detector
  CompressorEffect nf;
  nf.set_physical(-30, 4, 1, 50, 0, true);
  float p[64], q[64];
  for (int i = 0; i < 64; ++i) p[i] = q[i] = 0.5f;
  p[2] = std::nanf("");
  q[9] = INFINITY;
  StereoBlock nb{p, q, 64};
  nf.process(nb);
  CHECK(std::isfinite(nf.gain_reduction_db()));
  for (int i = 0; i < 64; ++i) p[i] = q[i] = 0.5f;
  nf.process(nb);
  CHECK(std::isfinite(p[63]) && p[63] < 0.5f);
}

// ---- the Mixer: master EQ then master compressor, bypass, identity, published gain reduction ---------------------------------------
class Sine final : public velora::ports::LineInSource {
 public:
  Sine(double hz, double amp) : hz_(hz), amp_(amp) {}
  void read_block(float* l, float* r, std::uint32_t frames) noexcept override {
    for (std::uint32_t i = 0; i < frames; ++i, ++n_) {
      l[i] = static_cast<float>(amp_ * std::sin(2.0 * kPi * hz_ * static_cast<double>(n_) / kFs));
      r[i] = l[i] * 0.5f;
    }
  }
  float at(std::uint64_t n) const { return static_cast<float>(amp_ * std::sin(2.0 * kPi * hz_ * static_cast<double>(n) / kFs)); }

 private:
  double hz_, amp_;
  std::uint64_t n_ = 0;
};

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(32);
  std::vector<std::unique_ptr<Snapshot>> snaps;
  std::uint64_t rev = 0;
  std::vector<float> l, r, al, ar;

  void publish(Snapshot s) {
    s.revision = ++rev;
    s.ledger_index = static_cast<std::uint32_t>(rev % SnapshotExchange::kLedgerSlots);
    snaps.push_back(std::make_unique<Snapshot>(std::move(s)));
    br->snapshots().publish(snaps.back().get());
  }
  void run(int blocks) {
    for (int b = 0; b < blocks; ++b) {
      float ol[64], or_[64], oal[64], oar[64];
      br->process_block(OutputBuses{ol, or_, oal, oar});
      l.insert(l.end(), ol, ol + 64);
      r.insert(r.end(), or_, or_ + 64);
      al.insert(al.end(), oal, oal + 64);
      ar.insert(ar.end(), oar, oar + 64);
    }
  }
};

void set_master(Snapshot& s, std::uint16_t id, float v) { s.master[static_cast<std::size_t>(params::find(id)->master_index)] = v; }

Snapshot line_snapshot() {
  Snapshot s = Snapshot::defaults();
  s.perf_init[params::perf_slot(*params::find(params::kLineInLevel), 0)] = 1.0f;
  return s;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0) {
  double m = 0;
  for (std::size_t i = from; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

void test_master_eq_comp_order() {
  constexpr int kBlocks = 60;
  Snapshot s = line_snapshot();
  set_master(s, params::kMasterEqLowGain, -6.0f);
  set_master(s, params::kMasterEqMidGain, 12.0f);
  set_master(s, params::kMasterEqMidFreq, 1000.0f);
  set_master(s, params::kMasterEqHighGain, 4.0f);
  set_master(s, params::kMasterCompThreshold, -24.0f);
  set_master(s, params::kMasterCompRatio, 8.0f);
  set_master(s, params::kMasterCompAttack, 1.0f);
  set_master(s, params::kMasterCompRelease, 50.0f);
  set_master(s, params::kMasterCompMakeup, 3.0f);
  Sine src(1000.0, 0.3);
  Rig rig;
  rig.br->set_line_in(&src);
  rig.publish(s);
  rig.run(kBlocks);
  CHECK(rig.br->mixer().routed());

  // reference: the same two DSP instances in the same order (EQ, then compressor), then the opposite order
  auto reference = [&](bool eq_first) {
    EqEffect eq;
    CompressorEffect comp;
    eq.set_physical(-6, 12, 1000, 4, true);
    comp.set_physical(-24, 8, 1, 50, 3, true);
    Sine ref(1000.0, 0.3);
    std::vector<float> out;
    for (int b = 0; b < kBlocks; ++b) {
      float x[64], y[64];
      ref.read_block(x, y, 64);
      StereoBlock blk{x, y, 64};
      if (eq_first) { eq.process(blk); comp.process(blk); }
      else { comp.process(blk); eq.process(blk); }
      out.insert(out.end(), x, x + 64);
    }
    return out;
  };
  const std::vector<float> want = reference(true), swapped = reference(false);
  CHECK(max_abs_diff(rig.l, want) < 1e-6);
  CHECK(max_abs_diff(rig.l, swapped, 64 * 10) > 1e-3);  // the order is audible, and the Mixer uses EQ first
  // the Aux bus has its own state and no signal: silent
  double aux_peak = 0;
  for (float v : rig.al) aux_peak = std::max(aux_peak, static_cast<double>(std::fabs(v)));
  CHECK(aux_peak == 0.0);
  // published gain reduction: the master compressor of Main reduces, Aux does not
  CHECK(rig.br->last_values().load_gain_reduction(LastValueTable::gr_master_slot(0)) > 3.0f);
  CHECK(rig.br->last_values().load_gain_reduction(LastValueTable::gr_master_slot(1)) == 0.0f);
  CHECK(rig.br->last_values().load_gain_reduction(LastValueTable::gr_insert_slot(0, 0)) == 0.0f);
}

void test_master_identity_exact() {
  constexpr int kBlocks = 30;
  // the line-in signal alone (routed path, master stages at their defaults = identity): the signal bit for bit
  Sine src(700.0, 0.4);
  Rig rig;
  rig.br->set_line_in(&src);
  rig.publish(line_snapshot());
  rig.run(kBlocks);
  Sine ref(700.0, 0.4);
  std::vector<float> want;
  for (int b = 0; b < kBlocks; ++b) {
    float x[64], y[64];
    ref.read_block(x, y, 64);
    want.insert(want.end(), x, x + 64);
  }
  CHECK(rig.br->mixer().routed());  // (line-in is on) but the master section did nothing
  CHECK(std::memcmp(rig.l.data(), want.data(), want.size() * 4) == 0);
  // non-flat master settings that are bypassed: the mixer stays trivial and renders exactly the default path with notes
  auto play = [&](bool bypassed_settings) {
    Rig rg;
    Snapshot s = Snapshot::defaults();
    if (bypassed_settings) {
      set_master(s, params::kMasterEqMidGain, 9.0f);
      set_master(s, params::kMasterEqBypass, 1.0f);
      set_master(s, params::kMasterCompThreshold, -50.0f);
      set_master(s, params::kMasterCompRatio, 20.0f);
      set_master(s, params::kMasterCompMakeup, 12.0f);
      set_master(s, params::kMasterCompBypass, 1.0f);
    }
    rg.publish(s);
    for (std::uint32_t i = 0; i < 4; ++i) rg.br->submit({make_note_on(0, i % 4, 48 + 7 * i, 30000), 0});
    rg.run(20);
    CHECK(!rg.br->mixer().routed());
    return rg.l;
  };
  const std::vector<float> a = play(false), b = play(true);
  CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 4) == 0);
  double pk = 0;
  for (float v : a) pk = std::max(pk, static_cast<double>(std::fabs(v)));
  CHECK(pk > 0.01);
}

void test_master_bypass_crossfade() {
  constexpr int kPre = 20, kPost = 20;
  const double amp = 0.3, wet_gain = std::pow(10.0, 12.0 / 20.0);
  const double step_bound = amp * wet_gain * 2.0 * kPi * 1000.0 / kFs * 1.15;
  auto make = [&](bool eq_bypass, bool comp_bypass) {
    Snapshot s = line_snapshot();
    set_master(s, params::kMasterEqMidGain, 12.0f);
    set_master(s, params::kMasterEqMidFreq, 1000.0f);
    set_master(s, params::kMasterEqBypass, eq_bypass ? 1.0f : 0.0f);
    set_master(s, params::kMasterCompThreshold, -30.0f);
    set_master(s, params::kMasterCompRatio, 6.0f);
    set_master(s, params::kMasterCompMakeup, 2.0f);
    set_master(s, params::kMasterCompBypass, comp_bypass ? 1.0f : 0.0f);
    return s;
  };
  for (int which = 0; which < 2; ++which) {  // 0: EQ bypass toggles, 1: compressor bypass toggles (each stage has its own flag)
    Sine src(1000.0, amp);
    Rig rig;
    rig.br->set_line_in(&src);
    rig.publish(which == 0 ? make(false, true) : make(true, false));
    rig.run(kPre);
    rig.publish(which == 0 ? make(true, true) : make(true, true));  // bypass the stage that was active
    rig.run(kPost);
    rig.publish(which == 0 ? make(false, true) : make(true, false));  // and back
    rig.run(kPost + 40);
    // continuity across both toggles
    double worst = 0;
    for (std::size_t i = 1; i < rig.l.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(rig.l[i] - rig.l[i - 1])));
    CHECK(worst <= step_bound);
    // fully bypassed: exactly the dry signal (everything is identity -> the stage is skipped)
    const std::size_t from = static_cast<std::size_t>(kPre + 6) * 64, to = static_cast<std::size_t>(kPre + kPost) * 64;
    Sine ref(1000.0, amp);
    bool exact = true;
    for (std::size_t i = from; i < to; ++i) exact = exact && rig.l[i] == ref.at(i);
    CHECK(exact);
    // while active, the stage did change the signal
    double diff = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kPre) * 64; ++i) diff = std::max(diff, static_cast<double>(std::fabs(rig.l[i] - ref.at(i))));
    CHECK(diff > 0.05);
    // a hard switch would have jumped by far more than the bound (the test has teeth)
    CHECK(amp * (wet_gain - 1.0) > step_bound || which == 1);
    // comes back: the last blocks equal an always-active render of the same settings
    Sine src2(1000.0, amp);
    Rig always;
    always.br->set_line_in(&src2);
    always.publish(which == 0 ? make(false, true) : make(true, false));
    always.run(kPre + 2 * kPost + 40);
    CHECK(max_abs_diff(rig.l, always.l, static_cast<std::size_t>(kPre + 2 * kPost + 30) * 64) < 5e-3);  // state restarted clean, settled again
  }
}

void test_master_param_change_smooth() {
  const double amp = 0.3, wet = std::pow(10.0, 15.0 / 20.0);
  Sine src(1000.0, amp);
  Rig rig;
  rig.br->set_line_in(&src);
  Snapshot s = line_snapshot();
  rig.publish(s);
  rig.run(20);
  set_master(s, params::kMasterEqMidGain, 15.0f);
  set_master(s, params::kMasterEqMidFreq, 1000.0f);
  rig.publish(s);
  rig.run(60);
  double worst = 0, pk = 0;
  for (std::size_t i = 1; i < rig.l.size(); ++i) {
    worst = std::max(worst, static_cast<double>(std::fabs(rig.l[i] - rig.l[i - 1])));
    pk = std::max(pk, static_cast<double>(std::fabs(rig.l[i])));
  }
  CHECK(worst <= amp * wet * 2.0 * kPi * 1000.0 / kFs * 1.05);
  CHECK(pk <= amp * wet * 1.05 && pk > amp * wet * 0.95);
  // back to flat: the mixer returns to the identity path once smoothing is over
  set_master(s, params::kMasterEqMidGain, 0.0f);
  rig.publish(s);
  rig.run(120);
  CHECK(rig.br->mixer().routed());  // line-in keeps it routed, but the EQ stage is the identity again:
  Sine ref(1000.0, amp);
  const std::size_t n = rig.l.size();
  bool exact = true;
  for (std::size_t i = n - 640; i < n; ++i) exact = exact && rig.l[i] == ref.at(i);
  CHECK(exact);
}

void test_insert_gain_reduction_published() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  PartCfg& p = s.parts[0];
  p.insert_count = 3;
  p.inserts[0].type = params::kInsertTypeEq;
  p.inserts[1].type = params::kInsertTypeCompressor;
  p.inserts[1].params = {0.0f, 1.0f, 0.0f, 0.5f, 0.0f};  // threshold -60 dB, ratio 20:1, attack 0.1 ms, release 100 ms
  p.inserts[2].type = params::kInsertTypeCompressor;
  p.inserts[2].bypass = 1;
  p.inserts[2].params = p.inserts[1].params;
  build_chains(s);
  rig.publish(s);
  rig.br->submit({make_note_on(0, 0, 60, 50000), 0});
  rig.run(40);
  const LastValueTable& lv = rig.br->last_values();
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 1)) > 6.0f);
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 0)) == 0.0f);  // the EQ
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 2)) == 0.0f);  // bypassed
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(1, 1)) == 0.0f);  // another part
  // the chain is removed: the value drops to 0 (the part is trivial again)
  Snapshot t = Snapshot::defaults();
  rig.publish(t);
  rig.run(40);
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 1)) == 0.0f);
}

// ---- factory defaults are pinned (kDefaultInsertParams cannot drift from the registry or the effects) -----------------------------
void test_factory_defaults() {
  for (std::uint32_t k = 0; k < params::kInsertParamCount; ++k) {
    const params::ParamDef* d = params::find(static_cast<std::uint16_t>(params::kInsertParam1 + k));
    CHECK(d && static_cast<float>(d->def) == kDefaultInsertParams[k]);
  }
  auto comp = make_effect(EffectType::kCompressor);
  const CompressorEffect* c = dynamic_cast<const CompressorEffect*>(comp.get());
  CHECK(c);
  if (c) {
    CHECK(c->threshold_db() == -30.0f && std::fabs(c->ratio() - 4.4721f) < 1e-3f);
    CHECK(std::fabs(c->attack_ms() - 3.1623f) < 1e-3f && std::fabs(c->release_ms() - 100.0f) < 1e-3f && c->makeup_db() == 0.0f);
    CHECK(!c->is_identity());  // the default compressor works
  }
  auto eq = make_effect(EffectType::kEq);
  const EqEffect* e = dynamic_cast<const EqEffect*>(eq.get());
  CHECK(e && e->is_identity());  // the default EQ is flat
  // Documented difference: the insert's MID FREQ centre value 0.5 is the geometric middle 1265 Hz of 200..8000 Hz, the master default is 1000 Hz
  CHECK(std::fabs(eqmap::mid_hz(kDefaultInsertParams[2]) - 1264.9f) < 0.2f);
  CHECK(params::find(params::kMasterEqMidFreq)->def == 1000.0);
}

// Bypassed chain slots are reset, a non-finite sample becomes silence in the compressor.
void test_bypass_reset_and_nonfinite() {
  FxChain chain;
  auto fx = std::make_unique<CompressorEffect>();
  CompressorEffect* raw = fx.get();
  raw->set_physical(-40, 10, 1, 500, 0, true);
  run_dc(*raw, 0.5f, 4800);
  CHECK(raw->gain_reduction_db() > 10.0f);  // stale reduction
  chain.set_slot(0, std::move(fx), /*bypass=*/true);
  float l[64] = {}, r[64] = {};
  StereoBlock b{l, r, 64};
  chain.process(b);
  CHECK(raw->gain_reduction_db() == 0.0f);  // reset while bypassed: nothing left to step in on un-bypass
  CompressorEffect c;
  c.set_physical(-30, 4, 1, 50, 0, true);
  float x[64], y[64];
  for (int i = 0; i < 64; ++i) x[i] = y[i] = 0.5f;
  x[4] = std::nanf("");
  y[7] = INFINITY;
  StereoBlock nb{x, y, 64};
  c.process(nb);
  CHECK(x[4] == 0.0f && y[7] == 0.0f);
}

// ---- Aux bus master section, flag thresholds, chain replacement ---------------------------------------------------------------------
std::vector<float> aux_reference(const std::vector<float>& dry, bool eq_on, bool comp_on) {
  EqEffect eq;
  CompressorEffect comp;
  eq.set_physical(-6, 12, 1000, 4, true);
  comp.set_physical(-24, 8, 1, 50, 3, true);
  std::vector<float> out = dry, r = dry;
  for (std::size_t o = 0; o < out.size(); o += 64) {
    StereoBlock b{&out[o], &r[o], 64};
    if (eq_on) eq.process(b);
    if (comp_on) comp.process(b);
  }
  return out;
}

void test_aux_master_section() {
  constexpr int kBlocks = 50;
  std::vector<float> dry;
  {
    Rig ref;  // the part alone on Main, trivial routing: the signal the Aux bus must carry
    ref.publish(Snapshot::defaults());
    ref.br->submit({make_note_on(0, 2, 64, 50000), 0});
    ref.run(kBlocks);
    dry = ref.l;
  }
  auto aux_case = [&](float eq_bypass, float comp_bypass, bool eq_on, bool comp_on) {
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    set_master(s, params::kMasterEqLowGain, -6.0f);
    set_master(s, params::kMasterEqMidGain, 12.0f);
    set_master(s, params::kMasterEqMidFreq, 1000.0f);
    set_master(s, params::kMasterEqHighGain, 4.0f);
    set_master(s, params::kMasterCompThreshold, -24.0f);
    set_master(s, params::kMasterCompRatio, 8.0f);
    set_master(s, params::kMasterCompAttack, 1.0f);
    set_master(s, params::kMasterCompRelease, 50.0f);
    set_master(s, params::kMasterCompMakeup, 3.0f);
    set_master(s, params::kMasterEqBypass, eq_bypass);
    set_master(s, params::kMasterCompBypass, comp_bypass);
    Rig rig;
    rig.publish(s);
    rig.br->submit({make_note_on(0, 2, 64, 50000), 0});
    rig.run(kBlocks);
    const std::vector<float> want = aux_reference(dry, eq_on, comp_on);
    CHECK(max_abs_diff(rig.al, want) < 1e-6 && max_abs_diff(rig.ar, want) < 1e-6);
    double main_peak = 0;
    for (float v : rig.l) main_peak = std::max(main_peak, static_cast<double>(std::fabs(v)));
    CHECK(main_peak == 0.0);  // the part is on Aux only
    return std::make_pair(rig.br->last_values().load_gain_reduction(LastValueTable::gr_master_slot(0)),
                          rig.br->last_values().load_gain_reduction(LastValueTable::gr_master_slot(1)));
  };
  {
    const auto gr = aux_case(0.0f, 0.0f, true, true);  // {Main, Aux}
    CHECK(gr.second > 0.0f && gr.first == 0.0f);
  }
  {
    const auto gr = aux_case(1.0f, 0.0f, false, true);  // EQ bypassed: the compressor alone
    CHECK(gr.second > 0.0f && gr.first == 0.0f);
  }
  {
    const auto gr = aux_case(0.0f, 1.0f, true, false);  // compressor bypassed: the EQ alone, no reduction
    CHECK(gr.second == 0.0f && gr.first == 0.0f);
  }
}

void test_bypass_flag_thresholds() {
  // the flag bypasses at >= 0.5; 0, 0.49 and NaN leave the stage active
  const float flags[] = {0.0f, 0.49f, std::nanf(""), 0.5f, 1.0f};
  const bool bypassed[] = {false, false, false, true, true};
  for (int i = 0; i < 5; ++i) {
    Snapshot s = line_snapshot();
    set_master(s, params::kMasterEqMidGain, 12.0f);
    set_master(s, params::kMasterEqBypass, flags[i]);
    Sine src(1000.0, 0.3);
    Rig rig;
    rig.br->set_line_in(&src);
    rig.publish(s);
    rig.run(10);
    Sine ref(1000.0, 0.3);
    bool exact = true;
    for (std::size_t k = 0; k < rig.l.size(); ++k) exact = exact && rig.l[k] == ref.at(k);
    CHECK(exact == bypassed[i]);
  }
}

void test_replaced_chain_clears_gain_reduction() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.parts[0].insert_count = 1;
  s.parts[0].inserts[0].type = params::kInsertTypeCompressor;
  s.parts[0].inserts[0].params = {0.0f, 1.0f, 0.0f, 0.5f, 0.0f};
  build_chains(s);
  rig.publish(s);
  rig.br->submit({make_note_on(0, 0, 60, 50000), 0});
  rig.run(30);
  const LastValueTable& lv = rig.br->last_values();
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 0)) > 3.0f);
  // replaced by an EQ while the note still sounds (the part stays routed): the old compressor is audible only while it fades out,
  // after that the slot reads 0
  Snapshot t = s;
  t.parts[0].inserts[0].type = params::kInsertTypeEq;
  t.parts[0].chain = build_chain(t.parts[0]);
  rig.publish(t);
  rig.run(1);
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 0)) > 0.0f);  // still fading out: counted (the maximum of both chains)
  rig.run(8);
  CHECK(lv.load_gain_reduction(LastValueTable::gr_insert_slot(0, 0)) == 0.0f);
  CHECK(rig.br->mixer().routed());
}

void test_declared_costs() {
  // placeholder costs, measured on this host only (Story 2.12 measures the board): printed for the record
  EqEffect eq;
  CompressorEffect cp;
  eq.set_physical(6, 6, 1000, 6, true);
  cp.set_physical(-40, 4, 1, 50, 0, true);
  std::vector<float> x(64), y(64);
  for (int i = 0; i < 64; ++i) x[i] = y[i] = 0.4f * std::sin(0.05f * static_cast<float>(i));
  auto time = [&](Effect& e) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; ++i) {
      auto a = x, b = y;
      StereoBlock blk{a.data(), b.data(), 64};
      e.process(blk);
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20000.0;
  };
  std::printf("host cost per 64-frame block: eq %.2f us (declared %u units), compressor %.2f us (declared %u units)\n", time(eq),
              effect_cost_units(EffectType::kEq), time(cp), effect_cost_units(EffectType::kCompressor));
  CHECK(eq.cost_units() == kMasterEqCost && cp.cost_units() == kMasterCompCost);
}

}  // namespace

int main() {
  test_eq_mapping();
  test_eq_design_curve();
  test_eq_sine_sweep();
  test_eq_identity_and_robustness();
  test_eq_smoothing_no_click();
  test_comp_mapping();
  test_comp_static_curve_and_times();
  test_comp_stereo_makeup_identity();
  test_master_eq_comp_order();
  test_master_identity_exact();
  test_master_bypass_crossfade();
  test_master_param_change_smooth();
  test_insert_gain_reduction_published();
  test_factory_defaults();
  test_bypass_reset_and_nonfinite();
  test_aux_master_section();
  test_bypass_flag_thresholds();
  test_replaced_chain_clears_gain_reduction();
  test_declared_costs();
  if (g_fail) std::printf("eq_compressor_tests: %d failure(s)\n", g_fail);
  else std::printf("eq_compressor_tests: ok\n");
  return g_fail ? 1 : 0;
}

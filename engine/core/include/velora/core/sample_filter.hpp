#pragma once
// Per-voice filters and CC crossfade curves of the sample engine (Story 5.2). Header-only, plain arithmetic: no allocation, no
// locks, no OS. The formulas are the ones of the sfizz 1.2.3 reference (the null tests in tools/offline-render prove them); see
// engine/core/README.md ("Keyswitches, CC crossfades and filters").
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "velora/ports/sample_source.hpp"

namespace velora::core {

// --- filters ---------------------------------------------------------------------------------------------------------------
//
// sfizz's SFZ v2 multi-mode filters: lpf_1p / hpf_1p are one-pole filters with the pole p = exp(-2 pi fc / fs); the 2-pole types
// are the public RBJ Audio-EQ-Cookbook biquads (LPF, HPF and the constant-0-dB-peak-gain BPF) with Q = 10^(resonance_dB / 20).
// The cutoff is clamped to [1 Hz, 20 kHz] (sfizz) and to 0.49 fs so that the filter stays stable for any cutoff at any engine
// rate (at 48 kHz the 20 kHz limit is the tighter one, so the null tests are unaffected). The resonance is clamped to +-60 dB
// and Q to >= 0.001 (sfizz). Arithmetic is double, the voice casts the float signal in and the result out (sfizz's Faust
// filters are double inside as well).

inline constexpr double kSampleFilterMaxCutoffHz = 20000.0;

struct SampleFilterCoeffs {
  enum class Kind : std::uint8_t { kNone, kOnePoleLow, kOnePoleHigh, kBiquad };
  Kind kind = Kind::kNone;
  double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // biquad: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
  double p = 0, g = 0;                            // one pole: w = x + p w1; low: y = (1 - p) w; high: y = g (w - w1), g = (1 + p) / 2
};

// State of one channel of a filter (the voice owns one per channel).
struct SampleFilterState {
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;  // biquad
  double w1 = 0;                           // one pole
};

inline SampleFilterCoeffs make_sample_filter(velora::ports::SampleFilterType type, double cutoff_hz, double resonance_db,
                                             double sample_rate) noexcept {
  using T = velora::ports::SampleFilterType;
  SampleFilterCoeffs k;
  if (type == T::kNone || !(sample_rate > 0.0)) return k;
  double fc = std::isfinite(cutoff_hz) ? cutoff_hz : 1.0;
  fc = std::max(1.0, std::min(kSampleFilterMaxCutoffHz, fc));
  fc = std::min(fc, 0.49 * sample_rate);
  const double two_pi = 6.2831853071795864769;
  if (type == T::kLpf1p || type == T::kHpf1p) {
    k.p = std::exp(-two_pi * fc / sample_rate);
    if (type == T::kLpf1p) {
      k.kind = SampleFilterCoeffs::Kind::kOnePoleLow;
    } else {
      k.kind = SampleFilterCoeffs::Kind::kOnePoleHigh;
      k.g = 0.5 * (1.0 + k.p);
    }
    return k;
  }
  const double res = std::isfinite(resonance_db) ? std::max(-60.0, std::min(60.0, resonance_db)) : 0.0;
  const double q = std::max(0.001, std::pow(10.0, 0.05 * res));
  const double w0 = two_pi * fc / sample_rate;
  const double cs = std::cos(w0), sn = std::sin(w0);
  const double alpha = 0.5 * (sn / q);
  const double a0 = 1.0 + alpha;
  k.kind = SampleFilterCoeffs::Kind::kBiquad;
  k.a1 = (-2.0 * cs) / a0;
  k.a2 = (1.0 - alpha) / a0;
  if (type == T::kLpf2p) {
    k.b0 = 0.5 * ((1.0 - cs) / a0);
    k.b1 = (1.0 - cs) / a0;
    k.b2 = k.b0;
  } else if (type == T::kHpf2p) {
    k.b0 = 0.5 * ((1.0 + cs) / a0);
    k.b1 = (-1.0 - cs) / a0;
    k.b2 = k.b0;
  } else {  // kBpf2p: constant 0 dB peak gain
    k.b0 = 0.5 * (sn / (q * a0));
    k.b1 = 0.0;
    k.b2 = -k.b0;
  }
  return k;
}

// Denormals in a decaying filter are slow on some CPUs: states under 1e-200 (far below any audible or float-representable signal) flush to 0.
inline double sample_filter_flush(double v) noexcept { return std::fabs(v) < 1e-200 ? 0.0 : v; }

inline double sample_filter_tick(const SampleFilterCoeffs& k, SampleFilterState& s, double x) noexcept {
  switch (k.kind) {
    case SampleFilterCoeffs::Kind::kOnePoleLow: {
      const double w = sample_filter_flush(x + k.p * s.w1);
      s.w1 = w;
      return w * (1.0 - k.p);
    }
    case SampleFilterCoeffs::Kind::kOnePoleHigh: {
      const double w = sample_filter_flush(x + k.p * s.w1);
      const double y = k.g * (w - s.w1);
      s.w1 = w;
      return y;
    }
    case SampleFilterCoeffs::Kind::kBiquad: {
      const double y = sample_filter_flush(k.b0 * x + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2);
      s.x2 = s.x1;
      s.x1 = sample_filter_flush(x);
      s.y2 = s.y1;
      s.y1 = y;
      return y;
    }
    case SampleFilterCoeffs::Kind::kNone:
    default:
      return x;
  }
}

// |H(e^{j 2 pi f / fs})| of the digital filter described by the coefficients (for tests and the documentation tables).
inline double sample_filter_magnitude(const SampleFilterCoeffs& k, double f_hz, double sample_rate) noexcept {
  const double w = 6.2831853071795864769 * f_hz / sample_rate;
  const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
  auto mag = [](double re, double im) { return std::sqrt(re * re + im * im); };
  switch (k.kind) {
    case SampleFilterCoeffs::Kind::kOnePoleLow:
      return (1.0 - k.p) / mag(1.0 - k.p * c1, k.p * s1);
    case SampleFilterCoeffs::Kind::kOnePoleHigh:
      return k.g * mag(1.0 - c1, s1) / mag(1.0 - k.p * c1, k.p * s1);
    case SampleFilterCoeffs::Kind::kBiquad: {
      const double nr = k.b0 + k.b1 * c1 + k.b2 * c2, ni = -(k.b1 * s1 + k.b2 * s2);
      const double dr = 1.0 + k.a1 * c1 + k.a2 * c2, di = -(k.a1 * s1 + k.a2 * s2);
      return mag(nr, ni) / mag(dr, di);
    }
    case SampleFilterCoeffs::Kind::kNone:
    default:
      return 1.0;
  }
}

// --- CC crossfades -----------------------------------------------------------------------------------------------------------
//
// sfizz 1.2.3 (ModifierHelpers.h crossfadeIn / crossfadeOut, Defaults.cpp xfin/xfout specs), written in CC units (the CC value
// 0..127; sfizz normalises all of it by 127). The range is [lo, hi] as written in the SFZ. sfizz "fills the gap" of the high end
// (hi <= 126 becomes (hi + 1) / 127, just below), so the end of the range is e = hi + 1 (e = 127 when hi = 127) and the usable
// length is (e - lo) - 1 CC steps:
//   in : x < lo -> 0; length <= 0 -> 1; x < e -> t = (x - lo) / length; gain: t, power: sqrt(t); else 1.
//   out: length <= 0 -> 1 (a range that narrow does not fade at all, sfizz); x > lo -> t = (x - lo) / length, t > 1 -> 0,
//        gain: 1 - t, power: sqrt(1 - t); else 1.
// For the whole-number CC values of MIDI 1.0 these are exactly sfizz's values. A fractional value (a MIDI 2.0 CC of 32 bits)
// between hi and hi + 1 would give t > 1 in sfizz's "in" curve; t is clamped to 1 here (the only difference).
inline float sample_xf_in_gain(std::uint8_t lo, std::uint8_t hi, double x, velora::ports::SampleXfCurve curve) noexcept {
  if (x < static_cast<double>(lo)) return 0.0f;
  const double e = hi <= 126 ? static_cast<double>(hi) + 1.0 : 127.0;
  const double length = (e - static_cast<double>(lo)) - 1.0;
  if (length <= 0.0) return 1.0f;
  if (x >= e) return 1.0f;
  const double t = std::min(1.0, (x - static_cast<double>(lo)) / length);
  return static_cast<float>(curve == velora::ports::SampleXfCurve::kPower ? std::sqrt(t) : t);
}

inline float sample_xf_out_gain(std::uint8_t lo, std::uint8_t hi, double x, velora::ports::SampleXfCurve curve) noexcept {
  const double e = hi <= 126 ? static_cast<double>(hi) + 1.0 : 127.0;
  const double length = (e - static_cast<double>(lo)) - 1.0;
  if (length <= 0.0) return 1.0f;
  if (x > static_cast<double>(lo)) {
    const double t = (x - static_cast<double>(lo)) / length;
    if (t > 1.0) return 0.0f;
    return static_cast<float>(curve == velora::ports::SampleXfCurve::kPower ? std::sqrt(1.0 - t) : 1.0 - t);
  }
  return 1.0f;
}

}  // namespace velora::core

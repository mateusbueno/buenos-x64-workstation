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

// --- region EQ (Story 13.9) --------------------------------------------------------------------------------------------------
//
// sfizz's sfzEqPeak / sfzEqLshelf / sfzEqHshelf (gen/filters, the RBJ Audio-EQ-Cookbook peaking and shelving biquads with a bandwidth in octaves), in double. type: 1 peak,
// 2 low shelf, 3 high shelf. The clamps are the generated code's: frequency 1 Hz .. 20 kHz (and below 0.49 fs), bandwidth 0.01 .. 12 octaves, gain -120 .. +60 dB, the
// Q-like term at least 0.001. The result is a kBiquad that sample_filter_tick runs (b1 is the coefficient the Faust code applies to the previous input).
inline SampleFilterCoeffs make_sample_eq(unsigned type, double freq_hz, double bw_oct, double gain_db, double sample_rate) noexcept {
  SampleFilterCoeffs k;
  if (type < 1 || type > 3 || !(sample_rate > 0.0)) return k;
  double fc = std::isfinite(freq_hz) ? freq_hz : 1.0;
  fc = std::min(0.49 * sample_rate, std::max(1.0, std::min(kSampleFilterMaxCutoffHz, fc)));
  const double bw = std::isfinite(bw_oct) ? std::min(12.0, std::max(0.01, bw_oct)) : 1.0;
  const double g = std::isfinite(gain_db) ? std::min(60.0, std::max(-120.0, gain_db)) : 0.0;
  const double two_pi = 6.2831853071795864769;
  const double a_lin = std::pow(10.0, 0.025 * g);
  const double w0 = two_pi * fc / sample_rate;
  const double cs = std::cos(w0), sn = std::sin(w0);
  const double q = std::max(0.001, 0.5 / std::sinh((2.1775860903036022 / sample_rate) * ((fc * bw) / sn)));
  k.kind = SampleFilterCoeffs::Kind::kBiquad;
  if (type == 1) {
    const double al = 0.5 * (sn / (a_lin * q)), ala = 0.5 * ((a_lin * sn) / q);
    const double d = al + 1.0;
    k.a1 = (-2.0 * cs) / d;
    k.b1 = k.a1;
    k.b0 = (ala + 1.0) / d;
    k.b2 = (1.0 - ala) / d;
    k.a2 = (1.0 - al) / d;
  } else if (type == 2) {
    const double s5 = (a_lin + 1.0) * cs, s6 = (a_lin - 1.0) * cs, s7 = (std::sqrt(a_lin) * sn) / q;
    const double s8 = s6 + s7, d = (a_lin + s8) + 1.0;
    k.b1 = (2.0 * ((a_lin * (a_lin + (-1.0 - s5))) / d));
    k.b0 = (a_lin * ((a_lin + s7) + (1.0 - s6))) / d;
    k.b2 = (a_lin * (a_lin + (1.0 - s8))) / d;
    k.a2 = ((a_lin + s6) + (1.0 - s7)) / d;
    k.a1 = (0.0 - (2.0 * ((a_lin + s5) + -1.0))) / d;
  } else {
    const double s5 = (a_lin + 1.0) * cs, s6 = (std::sqrt(a_lin) * sn) / q, s7 = (a_lin - 1.0) * cs;
    const double s8 = (a_lin + s6) + (1.0 - s7), s11 = s7 + s6;
    k.b1 = ((0.0 - (2.0 * a_lin)) * ((a_lin + s5) + -1.0)) / s8;
    k.b0 = (a_lin * ((a_lin + s11) + 1.0)) / s8;
    k.b2 = (a_lin * ((a_lin + s7) + (1.0 - s6))) / s8;
    k.a2 = (a_lin + (1.0 - s11)) / s8;
    k.a1 = 2.0 * ((a_lin + (-1.0 - s5)) / s8);
  }
  return k;
}

// |H| of a biquad is shared with sample_filter_magnitude below; the generic per-frame move of a coefficient set towards a target (sfizz's one-pole coefficient smoothing,
// pole exp(-1000 / fs): c += (1 - pole) * (target - c)), used by a filter whose cutoff a LFO or an EG moves (Story 13.9).
inline void sample_filter_smooth(SampleFilterCoeffs& cur, const SampleFilterCoeffs& tgt, double step) noexcept {
  cur.b0 += step * (tgt.b0 - cur.b0);
  cur.b1 += step * (tgt.b1 - cur.b1);
  cur.b2 += step * (tgt.b2 - cur.b2);
  cur.a1 += step * (tgt.a1 - cur.a1);
  cur.a2 += step * (tgt.a2 - cur.a2);
  cur.p += step * (tgt.p - cur.p);
  cur.g += step * (tgt.g - cur.g);
}

// Denormals in a decaying filter are slow on some CPUs: states under 1e-200 (far below any audible or float-representable signal) flush to 0.
inline double sample_filter_flush(double v) noexcept { return std::fabs(v) < 1e-200 ? 0.0 : v; }

// Story 13.8: a sample the filter cannot digest (NaN, infinity, or a magnitude no audio has) makes the new state non-finite or out of bound; the tick then clears the
// state and answers silence instead of storing it, so the filter never keeps a NaN, an infinity or a runaway value (a NaN input yields a NaN candidate, an infinite one an
// infinity or a NaN). One compare per tick on the finite path, which it does not change.
inline constexpr double kSampleFilterSignalLimit = 1.0e18;

inline double sample_filter_tick(const SampleFilterCoeffs& k, SampleFilterState& s, double x) noexcept {
  switch (k.kind) {
    case SampleFilterCoeffs::Kind::kOnePoleLow: {
      const double w = sample_filter_flush(x + k.p * s.w1);
      if (!(std::fabs(w) < kSampleFilterSignalLimit)) {
        s = SampleFilterState{};
        return 0.0;
      }
      s.w1 = w;
      return w * (1.0 - k.p);
    }
    case SampleFilterCoeffs::Kind::kOnePoleHigh: {
      const double w = sample_filter_flush(x + k.p * s.w1);
      if (!(std::fabs(w) < kSampleFilterSignalLimit)) {
        s = SampleFilterState{};
        return 0.0;
      }
      const double y = k.g * (w - s.w1);
      s.w1 = w;
      return y;
    }
    case SampleFilterCoeffs::Kind::kBiquad: {
      double y = sample_filter_flush(k.b0 * x + k.b1 * s.x1 + k.b2 * s.x2 - k.a1 * s.y1 - k.a2 * s.y2);
      if (!(std::fabs(y) < kSampleFilterSignalLimit)) {
        s = SampleFilterState{};
        return 0.0;
      }
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

// Story 13.9: the filter of a cutoff that moves. sfizz's Faust biquads (lpf_2p, hpf_2p, bpf_2p) read their smoothed coefficients at different frames for different terms:
// y[n] = b0[n] x[n] + b1[n-1] x[n-1] + b2[n-2] x[n-2] - a2[n-1] y[n-2] - a1[n] y[n-1] (each product is formed when its coefficient is current and delayed after that). With a
// constant coefficient set this is the direct form I of sample_filter_tick; while the coefficients move (a few percent per frame during a step of the cutoff) it is not,
// and the difference is audible against sfizz for a millisecond or two. v0 = x[n-1] b1[n-1], v1 = x[n-1] b2[n-1], v2 = x[n-2] b2[n-2] - a2[n-1] y[n-2]; one-pole types as the
// plain tick (their coefficient is read at the current frame in sfizz as well).
struct SampleFilterSmoothState {
  double v0 = 0, v1 = 0, v2 = 0, y1 = 0, w1 = 0;
};

inline double sample_filter_tick_smooth(const SampleFilterCoeffs& k, SampleFilterSmoothState& s, double x) noexcept {
  switch (k.kind) {
    case SampleFilterCoeffs::Kind::kBiquad: {
      const double y = sample_filter_flush(k.b0 * x + s.v0 + s.v2 - k.a1 * s.y1);
      if (!(std::fabs(y) < kSampleFilterSignalLimit)) {
        s = SampleFilterSmoothState{};
        return 0.0;
      }
      const double nv2 = s.v1 - k.a2 * s.y1;
      s.v0 = sample_filter_flush(x * k.b1);
      s.v1 = sample_filter_flush(x * k.b2);
      s.v2 = sample_filter_flush(nv2);
      s.y1 = y;
      return y;
    }
    case SampleFilterCoeffs::Kind::kOnePoleLow:
    case SampleFilterCoeffs::Kind::kOnePoleHigh: {
      const double w = sample_filter_flush(x + k.p * s.w1);
      if (!(std::fabs(w) < kSampleFilterSignalLimit)) {
        s = SampleFilterSmoothState{};
        return 0.0;
      }
      const double y = k.kind == SampleFilterCoeffs::Kind::kOnePoleLow ? w * (1.0 - k.p) : k.g * (w - s.w1);
      s.w1 = w;
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

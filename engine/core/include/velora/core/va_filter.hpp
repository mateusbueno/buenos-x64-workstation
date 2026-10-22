#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "velora/core/va_parameters.hpp"

namespace velora::core {

// Single 2-pole (12 dB/oct) Zero-Delay Feedback State-Variable Filter (Zavalishin 2012 TPT).
class VaZdfSvf {
 public:
  void reset() noexcept {
    s1_ = 0.0f;
    s2_ = 0.0f;
  }

  // Process a single sample
  // cutoff_hz: 20 Hz .. 0.48 * sample_rate
  // resonance: 0.0 .. 1.0
  // Returns: struct with lp, bp, hp outputs
  struct Output {
    float lp{0.0f};
    float bp{0.0f};
    float hp{0.0f};
  };

  Output process(float x, float cutoff_hz, float resonance, float sample_rate) noexcept {
    // Clamp cutoff safely below Nyquist
    float max_cutoff = 0.48f * sample_rate;
    float fc = std::clamp(cutoff_hz, 10.0f, max_cutoff);

    // Prewarped frequency tuning factor g = tan(pi * fc / Fs)
    constexpr float kPi = 3.14159265358979323846f;
    float g = std::tan(kPi * fc / sample_rate);

    // Damping factor R:
    // Butterworth alignment (Q = 1/sqrt(2) ~ 0.7071) at resonance = 0: R = 1/sqrt(2) ~ 0.707107f
    // High Q / self-oscillation as resonance approaches 1.0
    float res_clamped = std::clamp(resonance, 0.0f, 1.0f);
    constexpr float kButterworthR = 0.70710678f;
    float R = kButterworthR * (1.0f - res_clamped * 0.98f);

    // Instantaneous output resolution
    // To allow stable self-oscillation without blowing up when resonance is high,
    // soft saturation is engaged smoothly when resonance exceeds 0.7
    float s1_sat = (res_clamped > 0.7f) ? std::tanh(s1_) : s1_;
    float hp = (x - (2.0f * R + g) * s1_sat - s2_) / (1.0f + 2.0f * R * g + g * g);
    float bp = g * hp + s1_sat;
    float lp = g * bp + s2_;

    // Update state registers with trapezoidal integration
    s1_ += 2.0f * g * hp;
    s2_ += 2.0f * g * bp;

    // Anti-denormal flushing
    if (std::abs(s1_) < 1e-15f) s1_ = 0.0f;
    if (std::abs(s2_) < 1e-15f) s2_ = 0.0f;

    return Output{lp, bp, hp};
  }

 private:
  float s1_{0.0f};
  float s2_{0.0f};
};

// Multi-mode Filter: supports LP12, LP24 (cascaded), BP12, HP12 with drive.
class VaFilter {
 public:
  void reset() noexcept {
    stage1_.reset();
    stage2_.reset();
  }

  float process(float in, VaFilterType type, float cutoff_hz, float resonance, float drive, float sample_rate) noexcept {
    // Apply input drive with soft saturation when drive > 0
    float x = in;
    if (drive > 0.001f) {
      float drive_factor = 1.0f + drive * 3.0f;
      x = std::tanh(in * drive_factor);
    }

    auto out1 = stage1_.process(x, cutoff_hz, resonance, sample_rate);

    switch (type) {
      case VaFilterType::kLowPass12:
        return out1.lp;

      case VaFilterType::kLowPass24: {
        // Cascade through second stage for 24 dB/oct slope
        auto out2 = stage2_.process(out1.lp, cutoff_hz, resonance * 0.85f, sample_rate);
        return out2.lp;
      }

      case VaFilterType::kBandPass12:
        return out1.bp;

      case VaFilterType::kHighPass12:
        return out1.hp;

      default:
        return out1.lp;
    }
  }

 private:
  VaZdfSvf stage1_{};
  VaZdfSvf stage2_{};
};

} // namespace velora::core

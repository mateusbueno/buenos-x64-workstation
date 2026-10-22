#pragma once

#include <cmath>
#include <cstdint>

#include "velora/core/va_parameters.hpp"
#include "velora/core/va_polyblep.hpp"

namespace velora::core {

class VaLfo {
 public:
  static constexpr float kTwoPi = 6.28318530717958647692f;

  void init(VaLfoWaveform wave, float rate_hz, float delay_ms, bool key_sync, float sample_rate) noexcept {
    wave_ = wave;
    rate_hz_ = rate_hz > 0.01f ? rate_hz : 1.0f;
    sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
    key_sync_ = key_sync;

    phase_inc_ = rate_hz_ / sample_rate_;
    delay_samples_ = static_cast<uint32_t>((delay_ms * 0.001f) * sample_rate_);
    delay_counter_ = 0;

    if (key_sync_) {
      phase_ = 0.0f;
      delay_counter_ = 0;
    }
  }

  void reset() noexcept {
    if (key_sync_) {
      phase_ = 0.0f;
      delay_counter_ = 0;
      sh_value_ = 0.0f;
    }
  }

  float step() noexcept {
    // Check delay ramp
    float delay_factor = 1.0f;
    if (delay_counter_ < delay_samples_) {
      delay_counter_++;
      delay_factor = static_cast<float>(delay_counter_) / static_cast<float>(delay_samples_ > 0 ? delay_samples_ : 1);
    }

    float out = 0.0f;
    switch (wave_) {
      case VaLfoWaveform::kSine:
        out = std::sin(kTwoPi * phase_);
        break;

      case VaLfoWaveform::kTriangle:
        out = 2.0f * std::abs(2.0f * phase_ - 1.0f) - 1.0f;
        break;

      case VaLfoWaveform::kSawUp:
        out = 2.0f * phase_ - 1.0f;
        break;

      case VaLfoWaveform::kSawDown:
        out = 1.0f - 2.0f * phase_;
        break;

      case VaLfoWaveform::kSquare:
        out = (phase_ < 0.5f) ? 1.0f : -1.0f;
        break;

      case VaLfoWaveform::kSampleAndHold:
        // Update sample on phase wrap
        if (phase_ < phase_inc_) {
          sh_value_ = generate_white_noise(rng_state_);
        }
        out = sh_value_;
        break;

      default:
        out = std::sin(kTwoPi * phase_);
        break;
    }

    phase_ += phase_inc_;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }

    return out * delay_factor;
  }

 private:
  VaLfoWaveform wave_{VaLfoWaveform::kTriangle};
  float rate_hz_{4.0f};
  float sample_rate_{48000.0f};
  bool key_sync_{false};
  float phase_{0.0f};
  float phase_inc_{0.001f};
  uint32_t delay_samples_{0};
  uint32_t delay_counter_{0};
  float sh_value_{0.0f};
  uint32_t rng_state_{0x87654321};
};

} // namespace velora::core

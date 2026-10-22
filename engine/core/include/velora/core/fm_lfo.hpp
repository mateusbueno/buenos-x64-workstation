#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace velora::core {

class FmLfo {
 public:
  void init(uint8_t speed, uint8_t delay, uint8_t wave, bool key_sync,
            float sample_rate = 48000.0f) noexcept {
    sample_rate_ = sample_rate;
    wave_ = wave;

    // Speed 0: 0.06 Hz, Speed 99: ~50 Hz
    float freq = 0.06f * std::pow(50.0f / 0.06f, static_cast<float>(speed) / 99.0f);
    phase_inc_ = freq / sample_rate_;

    // Delay frames
    if (delay == 0) {
      delay_frames_left_ = 0;
      delay_fade_step_ = 1.0f;
      delay_gain_ = 1.0f;
    } else {
      float delay_s = (static_cast<float>(delay) / 99.0f) * 2.5f;
      delay_frames_left_ = static_cast<uint32_t>(delay_s * sample_rate_);
      float fade_s = 0.5f;
      delay_fade_step_ = 1.0f / std::max(1.0f, fade_s * sample_rate_);
      delay_gain_ = 0.0f;
    }

    if (key_sync) {
      phase_ = 0.0f;
      sh_value_ = 0.0f;
    }
  }

  inline void step(float& pitch_out, float& amp_out) noexcept {
    // Handle delay fade
    if (delay_frames_left_ > 0) {
      delay_frames_left_--;
      pitch_out = 0.0f;
      amp_out = 0.0f;
      return;
    }
    if (delay_gain_ < 1.0f) {
      delay_gain_ = std::min(1.0f, delay_gain_ + delay_fade_step_);
    }

    // Generate raw waveform [-1.0, 1.0]
    float raw = 0.0f;
    switch (wave_) {
      case 0: // Triangle
        raw = (phase_ < 0.5f) ? (4.0f * phase_ - 1.0f) : (3.0f - 4.0f * phase_);
        break;
      case 1: // Saw Down
        raw = 1.0f - 2.0f * phase_;
        break;
      case 2: // Saw Up
        raw = 2.0f * phase_ - 1.0f;
        break;
      case 3: // Square
        raw = (phase_ < 0.5f) ? 1.0f : -1.0f;
        break;
      case 4: // Sine
        raw = std::sin(phase_ * 6.28318530718f);
        break;
      case 5: // Sample & Hold
        raw = sh_value_;
        break;
      default:
        raw = std::sin(phase_ * 6.28318530718f);
        break;
    }

    pitch_out = raw * delay_gain_;
    amp_out = (raw * 0.5f + 0.5f) * delay_gain_; // [0..1]

    phase_ += phase_inc_;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
      // New S&H sample at cycle boundary
      sh_seed_ = (sh_seed_ * 1103515245 + 12345) & 0x7FFFFFFF;
      sh_value_ = (static_cast<float>(sh_seed_) / 1073741824.0f) - 1.0f;
    }
  }

 private:
  float sample_rate_{48000.0f};
  uint8_t wave_{0};
  float phase_{0.0f};
  float phase_inc_{0.001f};
  uint32_t delay_frames_left_{0};
  float delay_gain_{1.0f};
  float delay_fade_step_{0.01f};
  float sh_value_{0.0f};
  uint32_t sh_seed_{1234567};
};

} // namespace velora::core

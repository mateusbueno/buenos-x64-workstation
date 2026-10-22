#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace velora::core {

enum class VaEnvState : uint8_t {
  kIdle = 0,
  kAttack,
  kDecay,
  kSustain,
  kRelease,
};

class VaAdsrEnvelope {
 public:
  void init(float attack_ms, float decay_ms, float sustain_level, float release_ms, float sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
    sustain_ = std::clamp(sustain_level, 0.0f, 1.0f);

    float att_ms = std::max(attack_ms, 0.5f);
    float dec_ms = std::max(decay_ms, 1.0f);
    float rel_ms = std::max(release_ms, 1.0f);

    float att_samples = (att_ms * 0.001f) * sample_rate_;
    float dec_samples = (dec_ms * 0.001f) * sample_rate_;
    float rel_samples = (rel_ms * 0.001f) * sample_rate_;

    attack_step_ = 1.0f / std::max(att_samples, 1.0f);
    // Exponential decay/release: 4.6 time constants reaches ~1% of target
    decay_coeff_ = 1.0f - std::exp(-4.6f / std::max(dec_samples, 1.0f));
    release_coeff_ = 1.0f - std::exp(-4.6f / std::max(rel_samples, 1.0f));

    state_ = VaEnvState::kIdle;
    level_ = 0.0f;
  }

  void trigger() noexcept {
    state_ = VaEnvState::kAttack;
    if (level_ < 0.0f) level_ = 0.0f;
  }

  void release() noexcept {
    if (state_ != VaEnvState::kIdle) {
      state_ = VaEnvState::kRelease;
    }
  }

  void kill() noexcept {
    state_ = VaEnvState::kIdle;
    level_ = 0.0f;
  }

  float step() noexcept {
    switch (state_) {
      case VaEnvState::kAttack:
        level_ += attack_step_;
        if (level_ >= 1.0f) {
          level_ = 1.0f;
          state_ = VaEnvState::kDecay;
        }
        break;

      case VaEnvState::kDecay:
        level_ += decay_coeff_ * (sustain_ - level_);
        // If close to sustain level, enter sustain
        if (std::abs(level_ - sustain_) < 0.001f) {
          level_ = sustain_;
          state_ = VaEnvState::kSustain;
        }
        break;

      case VaEnvState::kSustain:
        level_ = sustain_;
        break;

      case VaEnvState::kRelease:
        level_ += release_coeff_ * (0.0f - level_);
        if (level_ <= 0.0005f) {
          level_ = 0.0f;
          state_ = VaEnvState::kIdle;
        }
        break;

      case VaEnvState::kIdle:
      default:
        level_ = 0.0f;
        break;
    }
    return level_;
  }

  VaEnvState state() const noexcept { return state_; }
  bool is_active() const noexcept { return state_ != VaEnvState::kIdle; }
  bool is_releasing() const noexcept { return state_ == VaEnvState::kRelease; }
  float level() const noexcept { return level_; }

 private:
  VaEnvState state_{VaEnvState::kIdle};
  float level_{0.0f};
  float sustain_{0.7f};
  float attack_step_{0.01f};
  float decay_coeff_{0.01f};
  float release_coeff_{0.01f};
  float sample_rate_{48000.0f};
};

} // namespace velora::core

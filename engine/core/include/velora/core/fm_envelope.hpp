#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "velora/core/fm_tables.hpp"

namespace velora::core {

enum class FmEnvelopeStage : uint8_t {
  Attack = 0,
  Decay1 = 1,
  Decay2 = 2,
  Sustain = 3,
  Release = 4,
  Finished = 5
};

class FmEnvelope {
 public:
  void init(const std::array<uint8_t, 4>& rates,
            const std::array<uint8_t, 4>& levels,
            uint8_t note,
            uint8_t rate_scaling,
            uint8_t breakpoint,
            uint8_t left_depth,
            uint8_t right_depth,
            uint8_t left_curve,
            uint8_t right_curve,
            float sample_rate = 48000.0f) noexcept {
    sample_rate_ = sample_rate;
    rates_ = rates;
    levels_ = levels;

    // Apply keyboard rate scaling: higher notes increase rates
    int rate_boost = 0;
    if (rate_scaling > 0 && note > 21) {
      rate_boost = ((note - 21) * rate_scaling) / 16;
    }

    for (int i = 0; i < 4; ++i) {
      effective_rates_[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(rates_[i]) + rate_boost, 0, 99));
      target_levels_[i] = fm_level_to_gain(levels_[i]);
    }

    // Apply keyboard level scaling
    float kbd_scale = compute_kbd_scaling(note, breakpoint, left_depth, right_depth, left_curve, right_curve);
    for (int i = 0; i < 3; ++i) {
      target_levels_[i] = std::clamp(target_levels_[i] * kbd_scale, 0.0f, 1.0f);
    }

    stage_ = FmEnvelopeStage::Attack;
    current_level_ = 0.0f;
    compute_stage_step();
  }

  void note_off() noexcept {
    if (stage_ != FmEnvelopeStage::Finished) {
      stage_ = FmEnvelopeStage::Release;
      compute_stage_step();
    }
  }

  inline float step() noexcept {
    if (stage_ == FmEnvelopeStage::Finished) {
      return 0.0f;
    }
    if (stage_ == FmEnvelopeStage::Sustain) {
      return current_level_;
    }

    current_level_ += stage_step_;

    // Check stage boundary
    if (stage_ascending_) {
      if (current_level_ >= stage_target_) {
        current_level_ = stage_target_;
        advance_stage();
      }
    } else {
      if (current_level_ <= stage_target_) {
        current_level_ = stage_target_;
        advance_stage();
      }
    }

    return current_level_;
  }

  bool is_finished() const noexcept { return stage_ == FmEnvelopeStage::Finished; }
  FmEnvelopeStage stage() const noexcept { return stage_; }
  float current_level() const noexcept { return current_level_; }

 private:
  void advance_stage() noexcept {
    switch (stage_) {
      case FmEnvelopeStage::Attack:
        stage_ = FmEnvelopeStage::Decay1;
        compute_stage_step();
        break;
      case FmEnvelopeStage::Decay1:
        stage_ = FmEnvelopeStage::Decay2;
        compute_stage_step();
        break;
      case FmEnvelopeStage::Decay2:
        stage_ = FmEnvelopeStage::Sustain;
        stage_step_ = 0.0f;
        break;
      case FmEnvelopeStage::Release:
        stage_ = FmEnvelopeStage::Finished;
        current_level_ = 0.0f;
        stage_step_ = 0.0f;
        break;
      default:
        break;
    }
  }

  void compute_stage_step() noexcept {
    int idx = 0;
    if (stage_ == FmEnvelopeStage::Attack) idx = 0;
    else if (stage_ == FmEnvelopeStage::Decay1) idx = 1;
    else if (stage_ == FmEnvelopeStage::Decay2) idx = 2;
    else if (stage_ == FmEnvelopeStage::Release) idx = 3;
    else {
      stage_step_ = 0.0f;
      return;
    }

    stage_target_ = target_levels_[idx];
    uint8_t r = effective_rates_[idx];

    // Compute duration in seconds based on rate 0..99
    // Rate 99: ~2 ms, Rate 50: ~1 s, Rate 1: ~40 s
    float duration_s = 0.002f;
    if (r < 99) {
      duration_s = 0.002f * std::pow(2.0f, static_cast<float>(99 - r) * 0.145f);
    }
    float frames = std::max(1.0f, duration_s * sample_rate_);

    float diff = stage_target_ - current_level_;
    stage_ascending_ = (diff >= 0.0f);
    stage_step_ = diff / frames;
  }

  static float compute_kbd_scaling(uint8_t note, uint8_t bp, uint8_t l_depth, uint8_t r_depth,
                                   uint8_t l_curve, uint8_t r_curve) noexcept {
    if (l_depth == 0 && r_depth == 0) return 1.0f;

    int delta = static_cast<int>(note) - static_cast<int>(bp);
    float depth = 0.0f;
    uint8_t curve = 0;

    if (delta < 0) {
      depth = static_cast<float>(l_depth) / 99.0f;
      curve = l_curve;
      delta = -delta;
    } else {
      depth = static_cast<float>(r_depth) / 99.0f;
      curve = r_curve;
    }

    if (depth <= 0.001f || delta == 0) return 1.0f;

    float norm_dist = std::clamp(static_cast<float>(delta) / 48.0f, 0.0f, 1.0f);
    float factor = 1.0f;

    switch (curve) {
      case 0: // -LIN
        factor = 1.0f - (norm_dist * depth);
        break;
      case 1: // -EXP
        factor = 1.0f - (std::pow(norm_dist, 2.0f) * depth);
        break;
      case 2: // +EXP
        factor = 1.0f + (std::pow(norm_dist, 2.0f) * depth * 0.5f);
        break;
      case 3: // +LIN
        factor = 1.0f + (norm_dist * depth * 0.5f);
        break;
      default:
        break;
    }
    return std::clamp(factor, 0.0f, 2.0f);
  }

  float sample_rate_{48000.0f};
  std::array<uint8_t, 4> rates_{99, 99, 99, 99};
  std::array<uint8_t, 4> levels_{99, 99, 99, 0};
  std::array<uint8_t, 4> effective_rates_{99, 99, 99, 99};
  std::array<float, 4> target_levels_{1.0f, 1.0f, 1.0f, 0.0f};

  FmEnvelopeStage stage_{FmEnvelopeStage::Finished};
  float current_level_{0.0f};
  float stage_target_{0.0f};
  float stage_step_{0.0f};
  bool stage_ascending_{true};
};

// 4-stage Pitch Envelope generator
class FmPitchEnvelope {
 public:
  void init(const std::array<uint8_t, 4>& rates,
            const std::array<uint8_t, 4>& levels,
            float sample_rate = 48000.0f) noexcept {
    sample_rate_ = sample_rate;
    rates_ = rates;
    levels_ = levels;
    for (int i = 0; i < 4; ++i) {
      target_ratios_[i] = fm_pitch_env_to_ratio(levels_[i]);
    }
    stage_ = 0;
    current_ratio_ = target_ratios_[3]; // starts from L4
    compute_step();
  }

  void note_off() noexcept {
    if (stage_ < 3) {
      stage_ = 3; // Release to L4
      compute_step();
    }
  }

  inline float step() noexcept {
    if (stage_ >= 4) return current_ratio_;

    current_ratio_ += step_;
    if (ascending_) {
      if (current_ratio_ >= target_) {
        current_ratio_ = target_;
        advance();
      }
    } else {
      if (current_ratio_ <= target_) {
        current_ratio_ = target_;
        advance();
      }
    }
    return current_ratio_;
  }

 private:
  void advance() noexcept {
    if (stage_ < 3) {
      stage_++;
      if (stage_ < 3) {
        compute_step();
      } else {
        step_ = 0.0f; // Sustain at L3
      }
    } else {
      stage_ = 4; // Finished
      step_ = 0.0f;
    }
  }

  void compute_step() noexcept {
    if (stage_ >= 4) return;
    target_ = target_ratios_[stage_];
    uint8_t r = rates_[stage_];
    float duration_s = 0.002f;
    if (r < 99) {
      duration_s = 0.002f * std::pow(2.0f, static_cast<float>(99 - r) * 0.145f);
    }
    float frames = std::max(1.0f, duration_s * sample_rate_);
    float diff = target_ - current_ratio_;
    ascending_ = (diff >= 0.0f);
    step_ = diff / frames;
  }

  float sample_rate_{48000.0f};
  std::array<uint8_t, 4> rates_{99, 99, 99, 99};
  std::array<uint8_t, 4> levels_{50, 50, 50, 50};
  std::array<float, 4> target_ratios_{1.0f, 1.0f, 1.0f, 1.0f};
  uint8_t stage_{4};
  float current_ratio_{1.0f};
  float target_{1.0f};
  float step_{0.0f};
  bool ascending_{true};
};

} // namespace velora::core

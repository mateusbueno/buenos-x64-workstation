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

inline int scaleoutlevel(int outlevel) noexcept {
  static const int levellut[20] = {
    0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46
  };
  return outlevel >= 20 ? 28 + outlevel : levellut[outlevel];
}

inline int scale_velocity(int velocity, int sensitivity) noexcept {
  static const uint8_t velocity_data[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254
  };
  int clamped_vel = std::clamp(velocity, 0, 127);
  int vel_value = static_cast<int>(velocity_data[clamped_vel >> 1]) - 239;
  int scaled_vel = ((sensitivity * vel_value + 7) >> 3) << 4;
  return scaled_vel;
}

inline int scale_rate(int midinote, int sensitivity) noexcept {
  int x = std::clamp(midinote / 3 - 7, 0, 31);
  return (sensitivity * x) >> 3;
}

inline int scale_curve(int group, int depth, int curve) noexcept {
  static const uint8_t exp_scale_data[33] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
    80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
  };
  int scale = 0;
  if (curve == 0 || curve == 3) {
    scale = (group * depth * 329) >> 12;
  } else {
    int raw_exp = exp_scale_data[std::min(group, 32)];
    scale = (raw_exp * depth * 329) >> 15;
  }
  if (curve < 2) {
    scale = -scale;
  }
  return scale;
}

inline int scale_level(int midinote, int break_pt, int left_depth, int right_depth,
                       int left_curve, int right_curve) noexcept {
  int offset = midinote - break_pt - 17;
  if (offset >= 0) {
    return scale_curve(offset / 3, right_depth, right_curve);
  } else {
    return scale_curve((-offset) / 3, left_depth, left_curve);
  }
}

class FmEnvelope {
 public:
  void init(const std::array<uint8_t, 4>& rates,
            const std::array<uint8_t, 4>& levels,
            uint8_t total_level,
            uint8_t note,
            uint8_t velocity,
            uint8_t velocity_sens,
            uint8_t rate_scaling,
            uint8_t breakpoint,
            uint8_t left_depth,
            uint8_t right_depth,
            uint8_t left_curve,
            uint8_t right_curve,
            float sample_rate = 48000.0f) noexcept {
    (void)sample_rate;
    for (int i = 0; i < 4; ++i) {
      rates_[i] = rates[i];
      levels_[i] = levels[i];
    }
    int outlevel = scaleoutlevel(total_level);
    outlevel += scale_level(note, breakpoint, left_depth, right_depth, left_curve, right_curve);
    outlevel = std::min(127, outlevel);
    outlevel = outlevel << 5;
    outlevel += scale_velocity(velocity, velocity_sens);
    outlevel = std::max(0, outlevel);
    outlevel_ = outlevel;
    rate_scaling_ = scale_rate(note, rate_scaling);

    level_ = 0;
    down_ = true;
    current_gain_ = 0.0f;
    target_gain_ = 0.0f;
    dgain_ = 0.0f;
    advance(0);
  }

  void begin_block() noexcept {
    current_gain_ = target_gain_;
    int32_t lvl = getsample();
    float octaves = (static_cast<float>(lvl) / 16777216.0f) - 15.0f;
    target_gain_ = std::pow(2.0f, octaves);
    dgain_ = (target_gain_ - current_gain_) / 64.0f;
  }

  inline float step() noexcept {
    float g = current_gain_;
    current_gain_ += dgain_;
    return g;
  }

  void note_off() noexcept {
    if (down_) {
      down_ = false;
      advance(3);
    }
  }

  bool is_finished() const noexcept {
    return (!down_ && ix_ >= 4 && current_gain_ <= 5e-5f);
  }

  FmEnvelopeStage stage() const noexcept {
    if (is_finished()) return FmEnvelopeStage::Finished;
    if (!down_) return FmEnvelopeStage::Release;
    if (ix_ == 0) return FmEnvelopeStage::Attack;
    if (ix_ == 1) return FmEnvelopeStage::Decay1;
    if (ix_ == 2) return FmEnvelopeStage::Decay2;
    return FmEnvelopeStage::Sustain;
  }

  float current_level() const noexcept { return current_gain_; }

 private:
  void advance(int newix) noexcept {
    ix_ = newix;
    if (ix_ < 4) {
      int newlevel = levels_[ix_];
      int actuallevel = scaleoutlevel(newlevel) >> 1;
      actuallevel = (actuallevel << 6) + outlevel_ - 4256;
      actuallevel = actuallevel < 16 ? 16 : actuallevel;
      targetlevel_ = actuallevel << 16;
      rising_ = (targetlevel_ > level_);

      int qrate = (rates_[ix_] * 41) >> 6;
      qrate += rate_scaling_;
      qrate = std::min(qrate, 63);
      inc_ = (4 + (qrate & 3)) << (8 + (qrate >> 2));
    }
  }

  int32_t getsample() noexcept {
    if (ix_ < 3 || (ix_ < 4 && !down_)) {
      if (rising_) {
        const int jumptarget = 1716;
        if (level_ < (jumptarget << 16)) {
          level_ = jumptarget << 16;
        }
        level_ += (((17 << 24) - level_) >> 24) * inc_;
        if (level_ >= targetlevel_) {
          level_ = targetlevel_;
          advance(ix_ + 1);
        }
      } else {
        level_ -= inc_;
        if (level_ <= targetlevel_) {
          level_ = targetlevel_;
          advance(ix_ + 1);
        }
      }
    }
    if (level_ < 0) level_ = 0;
    return level_;
  }

  int rates_[4]{99, 99, 99, 99};
  int levels_[4]{99, 99, 99, 0};
  int outlevel_{0};
  int rate_scaling_{0};
  int32_t level_{0};
  int32_t targetlevel_{0};
  bool rising_{false};
  int ix_{0};
  int inc_{0};
  bool down_{false};

  float current_gain_{0.0f};
  float target_gain_{0.0f};
  float dgain_{0.0f};
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

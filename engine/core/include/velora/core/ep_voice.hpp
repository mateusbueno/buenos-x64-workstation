#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "velora/core/engine.hpp"
#include "velora/core/ep_parameters.hpp"

namespace velora::core {

class EpVoice {
 public:
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr uint32_t kNumModes = 4;
  static constexpr float kHalfPedalHoldCc = 63.5f;
  static constexpr float kHalfPedalMinScale = 0.1f;

  static float half_pedal_scale(float cc64) noexcept {
    if (!(cc64 > 0.0f)) return 1.0f;
    if (cc64 >= kHalfPedalHoldCc) return 1.0f;
    return 1.0f - (1.0f - kHalfPedalMinScale) * (cc64 / kHalfPedalHoldCc);
  }

  EpVoice() = default;

  void init(float sample_rate) noexcept {
    sample_rate_ = sample_rate > 8000.0f ? sample_rate : 48000.0f;
    reset();
  }

  void reset() noexcept {
    active_ = false;
    channel_ = 0;
    note_ = 60;
    velocity_ = 0;
    release_velocity_ = 64;
    start_time_ = 0;
    state_ = VoiceState::kHeld;
    pedal_held_ = false;
    pedal_cc_ = 0.0f;
    current_level_ = 0.0f;

    stealing_ = false;
    steal_fade_step_ = 0.0f;
    steal_gain_ = 1.0f;

    for (uint32_t m = 0; m < kNumModes; ++m) {
      y1_[m] = 0.0f;
      y2_[m] = 0.0f;
      c1_[m] = 0.0f;
      c2_[m] = 0.0f;
      damp_[m] = 0.0f;
      release_damp_[m] = 0.0f;
      amp_[m] = 0.0f;
    }

    prev_tip_disp_ = 0.0f;
    hammer_sample_idx_ = 0;
    hammer_duration_ = 0;
    hammer_impulse_ = 0.0f;
    release_noise_samples_ = 0;
    noise_seed_ = 1234567;
  }

  bool is_active() const noexcept { return active_; }
  bool is_steal_fading() const noexcept { return stealing_; }
  uint32_t channel() const noexcept { return channel_; }
  uint32_t note() const noexcept { return note_; }
  VoiceState state() const noexcept { return state_; }
  uint64_t start_time() const noexcept { return start_time_; }
  float current_level() const noexcept { return current_level_; }

  void start(uint32_t channel, uint32_t note, uint32_t velocity16,
             const EpVoicingParameters& params, float pitch_bend_semitones,
             uint64_t frame_clock) noexcept {
    channel_ = channel;
    note_ = note;
    velocity_ = static_cast<uint8_t>(std::clamp<uint32_t>(velocity16 >> 8, 1, 127));
    start_time_ = frame_clock;
    state_ = VoiceState::kHeld;
    pedal_held_ = false;
    active_ = true;
    stealing_ = false;
    steal_gain_ = 1.0f;
    params_ = params;
    pitch_bend_semitones_ = pitch_bend_semitones;

    configure_modes();

    // Hammer mechanics
    float norm_vel = static_cast<float>(velocity_) / 127.0f;
    float hammer_vel = std::pow(norm_vel, 1.35f);
    float hardness = std::clamp(params.hammer_hardness, 0.0f, 1.0f);

    // Contact duration in frames: decreases with strike velocity and hardness
    float dur_ms = 4.0f * (1.0f - 0.5f * hardness) * (1.0f - 0.35f * norm_vel) + 0.8f;
    hammer_duration_ = std::max<uint32_t>(2, static_cast<uint32_t>(sample_rate_ * dur_ms * 0.001f));
    hammer_sample_idx_ = 0;
    hammer_impulse_ = hammer_vel * (1.0f + 1.5f * hardness);

    // Clear state
    for (uint32_t m = 0; m < kNumModes; ++m) {
      y1_[m] = 0.0f;
      y2_[m] = 0.0f;
    }
    prev_tip_disp_ = 0.0f;
    release_noise_samples_ = 0;
    current_level_ = 1.0f;
    noise_seed_ = 1234567u + note * 10139u + velocity_ * 31337u;
  }

  void set_half_pedal(float cc64) noexcept {
    pedal_cc_ = std::clamp(cc64, 0.0f, 127.0f);
    if (!active_ || state_ == VoiceState::kHeld || stealing_) return;

    if (pedal_cc_ >= kHalfPedalHoldCc) {
      state_ = VoiceState::kSustained;
      pedal_held_ = true;
    } else {
      state_ = VoiceState::kReleasing;
      pedal_held_ = false;
      float scale = half_pedal_scale(pedal_cc_);
      update_release_damping(scale);
    }
  }

  void stop(uint8_t release_velocity = 64) noexcept {
    if (!active_ || state_ == VoiceState::kReleasing || stealing_) return;
    release_velocity_ = release_velocity;
    if (pedal_held_ || pedal_cc_ >= kHalfPedalHoldCc) {
      state_ = VoiceState::kSustained;
      return;
    }
    float scale = half_pedal_scale(pedal_cc_);
    release(release_velocity, scale);
  }

  void release(uint8_t release_velocity = 64, float scale = 1.0f) noexcept {
    state_ = VoiceState::kReleasing;
    pedal_held_ = false;
    release_velocity_ = release_velocity;
    update_release_damping(scale);

    // Trigger subtle felt release scrape
    if (params_.damper_noise > 0.01f) {
      float norm_rel = static_cast<float>(std::clamp<uint8_t>(release_velocity, 1, 127)) / 127.0f;
      release_noise_samples_ = static_cast<uint32_t>(sample_rate_ * 0.035f);
      release_noise_gain_ = params_.damper_noise * 0.04f * (0.5f + 0.5f * norm_rel) * scale;
    }
  }

  void update_release_damping(float scale) noexcept {
    float norm_rel = static_cast<float>(std::clamp<uint8_t>(release_velocity_, 1, 127)) / 127.0f;
    float t60_base = 0.12f / (0.6f + 0.8f * norm_rel);
    float t60_eff = t60_base / std::max(0.05f, scale);
    for (uint32_t m = 0; m < kNumModes; ++m) {
      release_damp_[m] = std::exp(-3.0f / (t60_eff * sample_rate_));
    }
  }

  void set_sustain(bool sustain) noexcept {
    pedal_held_ = sustain;
    pedal_cc_ = sustain ? 127.0f : 0.0f;
    if (!sustain && state_ == VoiceState::kSustained) {
      release(release_velocity_, 1.0f);
    }
  }

  void set_pitch_bend(float semitones) noexcept {
    if (std::abs(pitch_bend_semitones_ - semitones) > 1e-4f) {
      pitch_bend_semitones_ = semitones;
      configure_modes();
    }
  }

  void begin_steal_fade(uint32_t ramp_frames) noexcept {
    stealing_ = true;
    ramp_frames = std::max(1u, ramp_frames);
    steal_fade_step_ = steal_gain_ / static_cast<float>(ramp_frames);
  }

  // Render 64 frames. Left and right are summed into existing buffers.
  void render_block(float* left, float* right, uint32_t frames) noexcept {
    if (!active_) return;

    float pu_pos = std::clamp(params_.pickup_position, 0.0f, 1.0f);
    float pu_level = std::clamp(params_.pickup_level, 0.0f, 1.0f);
    float y0 = (pu_pos - 0.5f) * 0.35f;  // pole alignment asymmetry
    float att_noise_gain = params_.attack_noise * 0.03f * (static_cast<float>(velocity_) / 127.0f);

    float max_sample_mag = 0.0f;

    for (uint32_t i = 0; i < frames; ++i) {
      // 1. Hammer force excitation
      float f_h = 0.0f;
      if (hammer_sample_idx_ < hammer_duration_) {
        float phase = static_cast<float>(hammer_sample_idx_) / static_cast<float>(hammer_duration_);
        float shape = std::sin(kPi * phase);
        f_h = hammer_impulse_ * shape * shape;
      }

      // Attack transient noise
      float attack_noise = 0.0f;
      if (hammer_sample_idx_ < hammer_duration_ + 32 && att_noise_gain > 0.0001f) {
        attack_noise = next_noise() * att_noise_gain * (1.0f - static_cast<float>(hammer_sample_idx_) / static_cast<float>(hammer_duration_ + 32));
      }

      if (hammer_sample_idx_ < hammer_duration_ + 32) {
        hammer_sample_idx_++;
      }

      // 2. Resonator bank update
      float tip_displacement = 0.0f;
      for (uint32_t m = 0; m < kNumModes; ++m) {
        if (amp_[m] < 1e-5f) continue;

        float d = (state_ == VoiceState::kReleasing) ? release_damp_[m] : damp_[m];
        float c1_eff = 2.0f * d * cos_omega_[m];
        float c2_eff = -(d * d);

        float yn = c1_eff * y1_[m] + c2_eff * y2_[m] + amp_[m] * (f_h + attack_noise);
        if (std::abs(yn) < 1e-15f) yn = 0.0f;
        y2_[m] = y1_[m];
        y1_[m] = yn;

        tip_displacement += yn * mode_phase_[m];
      }

      // 3. Transducer (Pickup)
      float tip_velocity = tip_displacement - prev_tip_disp_;
      prev_tip_disp_ = tip_displacement;

      float out_val = 0.0f;
      if (params_.instrument_type == EpInstrumentType::kReed) {
        // Electrostatic Variable Capacitance Pickup (Reed)
        // Polynomial non-linear displacement current: i(t) ~ v_tip * (1 + 2.2*xi + 3.2*xi^2 + 4*xi^3)
        float xi = (tip_displacement - y0) * 1.8f;
        float poly = 1.0f + 2.2f * xi + 3.2f * xi * xi + 4.0f * xi * xi * xi;
        poly = std::clamp(poly, -4.0f, 6.0f);
        float current = tip_velocity * poly * (pu_level * 2.5f);
        out_val = std::tanh(current);
      } else {
        // Electromagnetic Variable Reluctance Pickup (Tine)
        float gap = 1.0f - (tip_displacement - y0);
        float d_eff = gap;
        if (gap < 0.25f) {
          d_eff = 0.18f + 0.07f * std::tanh((gap - 0.18f) / 0.07f);
        }
        float emf = (tip_velocity / (d_eff * d_eff)) * (pu_level * 2.8f);
        out_val = std::tanh(emf);
      }

      // 4. Release felt scrape noise
      if (release_noise_samples_ > 0) {
        out_val += next_noise() * release_noise_gain_;
        release_noise_samples_--;
      }

      // 5. Steal fade envelope
      if (stealing_) {
        steal_gain_ -= steal_fade_step_;
        if (steal_gain_ <= 0.0f) {
          steal_gain_ = 0.0f;
          active_ = false;
        }
        out_val *= steal_gain_;
      }

      left[i] += out_val;
      right[i] += out_val;

      max_sample_mag = std::max(max_sample_mag, std::abs(out_val));
    }

    current_level_ = max_sample_mag;

    // Check voice death
    if (state_ == VoiceState::kReleasing && hammer_sample_idx_ >= hammer_duration_ &&
        release_noise_samples_ == 0) {
      float total_energy = 0.0f;
      for (uint32_t m = 0; m < kNumModes; ++m) {
        total_energy += std::abs(y1_[m]) + std::abs(y2_[m]);
      }
      if (total_energy < 1e-5f) {
        active_ = false;
        current_level_ = 0.0f;
      }
    }
  }

 private:
  void configure_modes() noexcept {
    // Equal temperament base frequency with pitch bend
    float effective_note = static_cast<float>(note_) + pitch_bend_semitones_;
    float f1 = 440.0f * std::pow(2.0f, (effective_note - 69.0f) / 12.0f);
    fundamental_hz_ = f1;

    // Modal configuration
    bool is_reed = (params_.instrument_type == EpInstrumentType::kReed);
    float decay_param = std::clamp(params_.decay_rate, 0.0f, 1.0f);
    float coupling = std::clamp(params_.tone_bar_coupling, 0.0f, 1.0f);

    float ratios[kNumModes];
    if (is_reed) {
      // Flat steel reed with tip solder mass: lower mode 2 ratio (~5.65) and compressed mode 3 (~16.2)
      ratios[0] = 1.0f;
      ratios[1] = 5.65f * (1.0f - 0.04f * coupling);
      ratios[2] = 16.20f * (1.0f - 0.03f * coupling);
      ratios[3] = 32.50f;
    } else {
      // Clamped-free Euler-Bernoulli cantilever tine mode ratios:
      // r1 = 1.000, r2 ~ 6.267, r3 ~ 17.548, r4 ~ 34.387
      ratios[0] = 1.0f;
      ratios[1] = 6.267f * (1.0f - 0.05f * coupling);
      ratios[2] = 17.548f * (1.0f - 0.03f * coupling);
      ratios[3] = 34.387f;
    }

    mode_phase_[0] = 1.0f;
    mode_phase_[1] = -0.7f;
    mode_phase_[2] = 0.4f;
    mode_phase_[3] = -0.2f;

    // Fundamental decay time scales with register: lower notes ring longer (5.0s to 12.0s)
    float note_offset = std::max(0.0f, static_cast<float>(note_) - 28.0f);
    float reg_decay = is_reed ? 4.8f * (1.0f - 0.45f * (note_offset / 72.0f))
                              : 7.0f * (1.0f - 0.4f * (note_offset / 72.0f));
    float t60_f1 = reg_decay * (0.6f + 0.8f * (1.0f - decay_param));

    float nyquist = 0.5f * sample_rate_;

    for (uint32_t m = 0; m < kNumModes; ++m) {
      float fm = f1 * ratios[m];
      mode_freq_hz_[m] = fm;

      // Band-limit: if mode exceeds 0.90 * Nyquist, suppress it completely to prevent aliasing
      if (fm >= 0.90f * nyquist) {
        amp_[m] = 0.0f;
        damp_[m] = 0.0f;
        release_damp_[m] = 0.0f;
        cos_omega_[m] = 0.0f;
        continue;
      }

      // Smooth roll-off near Nyquist (band-limiting filter)
      float bandwidth_gain = 1.0f;
      if (fm > 0.75f * nyquist) {
        bandwidth_gain = 0.5f * (1.0f + std::cos(kPi * (fm - 0.75f * nyquist) / (0.15f * nyquist)));
      }

      float omega = kTwoPi * fm / sample_rate_;
      cos_omega_[m] = std::cos(omega);

      // Modal decay times
      float t60_m = 0.0f;
      if (m == 0) {
        t60_m = t60_f1;
        amp_[m] = is_reed ? 0.09f : 0.08f;
      } else if (m == 1) {
        t60_m = (is_reed ? 0.16f : 0.28f) * (1.0f - 0.3f * decay_param);  // chime clang
        amp_[m] = (is_reed ? 0.065f : 0.05f) * bandwidth_gain;
      } else if (m == 2) {
        t60_m = (is_reed ? 0.05f : 0.08f) * (1.0f - 0.4f * decay_param);
        amp_[m] = (is_reed ? 0.02f : 0.025f) * bandwidth_gain;
      } else {
        t60_m = (is_reed ? 0.02f : 0.035f) * (1.0f - 0.4f * decay_param);
        amp_[m] = (is_reed ? 0.010f : 0.012f) * bandwidth_gain;
      }

      damp_[m] = std::exp(-3.0f / (t60_m * sample_rate_));
      release_damp_[m] = std::exp(-3.0f / (0.18f * sample_rate_));
    }
  }

  float next_noise() noexcept {
    noise_seed_ = noise_seed_ * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(noise_seed_)) / 2147483648.0f;
  }

  float sample_rate_{48000.0f};
  bool active_{false};
  uint32_t channel_{0};
  uint32_t note_{60};
  uint8_t velocity_{0};
  uint8_t release_velocity_{64};
  uint64_t start_time_{0};
  VoiceState state_{VoiceState::kHeld};
  bool pedal_held_{false};
  float pedal_cc_{0.0f};
  float current_level_{0.0f};

  bool stealing_{false};
  float steal_fade_step_{0.0f};
  float steal_gain_{1.0f};

  EpVoicingParameters params_{};
  float pitch_bend_semitones_{0.0f};
  float fundamental_hz_{440.0f};

  std::array<float, kNumModes> mode_freq_hz_{};
  std::array<float, kNumModes> cos_omega_{};
  std::array<float, kNumModes> damp_{};
  std::array<float, kNumModes> release_damp_{};
  std::array<float, kNumModes> amp_{};
  std::array<float, kNumModes> mode_phase_{};

  std::array<float, kNumModes> y1_{};
  std::array<float, kNumModes> y2_{};
  std::array<float, kNumModes> c1_{};
  std::array<float, kNumModes> c2_{};

  float prev_tip_disp_{0.0f};
  uint32_t hammer_sample_idx_{0};
  uint32_t hammer_duration_{0};
  float hammer_impulse_{0.0f};

  uint32_t release_noise_samples_{0};
  float release_noise_gain_{0.0f};
  uint32_t noise_seed_{1234567};
};

}  // namespace velora::core

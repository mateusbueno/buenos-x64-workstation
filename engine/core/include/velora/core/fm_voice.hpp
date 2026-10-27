#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "velora/core/fm_parameters.hpp"
#include "velora/core/fm_tables.hpp"
#include "velora/core/fm_envelope.hpp"
#include "velora/core/fm_lfo.hpp"

namespace velora::core {

class FmVoice {
 public:
  static constexpr float kTwoPi = 6.28318530717958647692f;

  void start(uint32_t channel, uint8_t note, uint16_t velocity16,
             const FmVoiceParameters& params, float sample_rate = 48000.0f) noexcept {
    channel_ = channel;
    note_ = note;
    uint8_t vel7 = static_cast<uint8_t>(std::clamp(static_cast<int>(velocity16 >> 9), 1, 127));
    velocity_ = vel7;
    sample_rate_ = sample_rate;
    active_ = true;
    held_ = true;
    sustained_ = false;
    is_steal_fading_ = false;
    steal_fade_gain_ = 1.0f;
    steal_fade_step_ = 0.0f;
    bright_cur_ = bright_target_ = static_cast<float>(params.brightness_offset);
    start_time_ = 0; // will be updated by engine frame clock

    alg_def_ = get_fm_algorithm_def(params.algorithm);
    fb_scale_ = fm_feedback_scale(params.feedback);

    // Initialize pitch envelope & LFO
    pitch_env_.init(params.pitch_env_rates, params.pitch_env_levels, sample_rate_);
    lfo_.init(params.lfo_speed, params.lfo_delay, params.lfo_wave, params.lfo_key_sync, sample_rate_);
    lfo_pitch_sens_ = params.lfo_pitch_mod_sens;
    lfo_pitch_depth_ = static_cast<float>(params.lfo_pitch_mod_depth) / 99.0f;
    lfo_amp_depth_ = static_cast<float>(params.lfo_amp_mod_depth) / 99.0f;

    // Base pitch of note (A4 = 440 Hz at note 69)
    float base_pitch = 440.0f * std::pow(2.0f, (static_cast<float>(note_) + params.transpose - 69.0f) / 12.0f);
    base_freq_ = base_pitch;

    // Initialize each operator
    for (int i = 0; i < 6; ++i) {
      const auto& op_params = params.operators[i];
      op_envs_[i].init(op_params.rates, op_params.levels, op_params.total_level,
                       note_, velocity_, op_params.velocity_sens,
                       op_params.kbd_rate_scaling, op_params.kbd_breakpoint,
                       op_params.left_depth, op_params.right_depth,
                       op_params.left_curve, op_params.right_curve, sample_rate_);

      if (params.osc_key_sync) op_phases_[i] = 0.0f;  // key sync off: the oscillator keeps running from where the slot's last note left it
      op_fb_history_[i][0] = 0.0f;
      op_fb_history_[i][1] = 0.0f;

      // Calculate operator base frequency ratio or fixed frequency
      if (op_params.fixed_freq_mode) {
        // Fixed frequency mode: 1 Hz to 9772 Hz
        float coarse_base = std::pow(10.0f, static_cast<float>(op_params.freq_coarse % 4));
        float fine_mult = 1.0f + (static_cast<float>(op_params.freq_fine) / 99.0f) * 8.772f;
        op_base_freqs_[i] = coarse_base * fine_mult;
        op_is_fixed_[i] = true;
      } else {
        int eff_note = static_cast<int>(note_) + params.transpose;
        int detune_raw = static_cast<int>(op_params.detune) + 7;
        op_base_freqs_[i] = fm_osc_freq(eff_note, op_params.freq_coarse, op_params.freq_fine, detune_raw, sample_rate_);
        op_is_fixed_[i] = false;
      }

      op_amp_sens_[i] = op_params.amp_mod_sens;
    }
  }

  void stop() noexcept {
    held_ = false;
    if (!sustained_ && !is_steal_fading_) {
      trigger_release();
    }
  }

  void key_released() noexcept {
    held_ = false;
    if (!sustained_ && !is_steal_fading_) {
      trigger_release();
    }
  }

  void set_sustain(bool sus) noexcept {
    if (sustained_ && !sus && !held_) {
      sustained_ = false;
      trigger_release();
    } else {
      sustained_ = sus;
    }
  }

  void set_pitch_bend(float semitones) noexcept {
    pitch_bend_semitones_ = semitones;
  }

  // fm_brightness moved while the note sounds: the modulator gain factor glides to the new value (one pole, ~5 ms) instead of stepping.
  void set_brightness_target(uint8_t brightness) noexcept {
    bright_target_ = static_cast<float>(brightness);
  }

  void begin_steal_fade(uint32_t ramp_frames) noexcept {
    is_steal_fading_ = true;
    steal_fade_step_ = (ramp_frames > 0) ? (steal_fade_gain_ / static_cast<float>(ramp_frames)) : 1.0f;
  }

  // Renders up to 'frames' samples, adding into output_buffer
  void render(float* output_buffer, uint32_t frames) noexcept {
    if (!active_) return;

    // Begin block for all operator envelopes
    for (int i = 0; i < 6; ++i) {
      op_envs_[i].begin_block();
    }

    float pb_ratio = std::pow(2.0f, pitch_bend_semitones_ / 12.0f);
    const float bright_k = 1.0f - std::exp(-1.0f / (0.005f * sample_rate_));

    for (uint32_t f = 0; f < frames; ++f) {
      if (bright_cur_ != bright_target_) {  // smoothed brightness (only while it moves)
        bright_cur_ += bright_k * (bright_target_ - bright_cur_);
        if (std::abs(bright_target_ - bright_cur_) < 0.01f) bright_cur_ = bright_target_;
      }
      const float bright_factor = bright_cur_ != 64.0f ? std::pow(2.0f, (bright_cur_ - 64.0f) / 32.0f) : 1.0f;
      // 1. Advance LFO & Pitch Envelope
      float lfo_pitch = 0.0f, lfo_amp = 0.0f;
      lfo_.step(lfo_pitch, lfo_amp);

      float pitch_env_ratio = pitch_env_.step();
      float lfo_pitch_ratio = std::pow(2.0f, (lfo_pitch * lfo_pitch_depth_ * static_cast<float>(lfo_pitch_sens_)) / 12.0f);
      float global_pitch_mod = pb_ratio * pitch_env_ratio * lfo_pitch_ratio;

      // 2. Compute operator frequencies & envelope levels
      std::array<float, 6> op_outputs{};
      std::array<float, 6> op_gains{};

      for (int i = 0; i < 6; ++i) {
        float g = op_envs_[i].step();
        if (i >= 1 && bright_cur_ != 64.0f) {
          g = std::clamp(g * bright_factor, 0.0f, 2.0f);
        }

        // Apply LFO AM
        if (op_amp_sens_[i] > 0 && lfo_amp_depth_ > 0.0f) {
          float am_factor = 1.0f - (lfo_amp * lfo_amp_depth_ * (static_cast<float>(op_amp_sens_[i]) / 3.0f));
          g *= std::clamp(am_factor, 0.0f, 1.0f);
        }
        op_gains[i] = g;
      }

      // 3. Process operators in topological order (OP6 down to OP1)
      for (int op = 5; op >= 0; --op) {
        // Collect phase modulation from operators that modulate 'op' (scaled by 2.0 to match DX7 Q24 modulation depth)
        float mod_in = 0.0f;
        for (int src = 0; src < 6; ++src) {
          if (alg_def_.mod_matrix[op][src]) {
            mod_in += op_outputs[src] * 2.0f;
          }
        }

        // Add feedback if this is the feedback operator
        if (op == alg_def_.feedback_op && fb_scale_ > 0.0f) {
          float fb_avg = (op_fb_history_[op][0] + op_fb_history_[op][1]) * 0.5f;
          mod_in += fb_avg * fb_scale_;
        }

        // Output = Gain * sin(2*pi*phase + mod_in * 2*pi)
        float total_phase = (op_phases_[op] + mod_in) * kTwoPi;
        float y = op_gains[op] * std::sin(total_phase);
        op_outputs[op] = y;

        // Update feedback history
        if (op == alg_def_.feedback_op) {
          op_fb_history_[op][1] = op_fb_history_[op][0];
          op_fb_history_[op][0] = y;
        }
      }

      // Advance operator phases after evaluating this sample
      for (int i = 0; i < 6; ++i) {
        float f_op = op_is_fixed_[i] ? op_base_freqs_[i] : (op_base_freqs_[i] * global_pitch_mod);
        op_phases_[i] += f_op / sample_rate_;
        if (op_phases_[i] >= 1.0f) op_phases_[i] -= std::floor(op_phases_[i]);
      }

      // 4. Sum carrier operators to voice output
      float voice_sample = 0.0f;
      for (int i = 0; i < 6; ++i) {
        if (alg_def_.is_carrier[i]) {
          voice_sample += op_outputs[i];
        }
      }

      // 5. Apply steal fade if active
      if (is_steal_fading_) {
        voice_sample *= steal_fade_gain_;
        steal_fade_gain_ -= steal_fade_step_;
        if (steal_fade_gain_ <= 0.0f) {
          steal_fade_gain_ = 0.0f;
          active_ = false;
          output_buffer[f] += voice_sample;
          break;
        }
      }

      // Scale output down to prevent clipping from additive carrier sums
      output_buffer[f] += voice_sample * 0.25f;
      current_level_ = std::abs(voice_sample);
    }

    // Check if voice has completely finished
    if (!held_ && !sustained_) {
      bool all_carriers_finished = true;
      for (int i = 0; i < 6; ++i) {
        if (alg_def_.is_carrier[i] && !op_envs_[i].is_finished()) {
          all_carriers_finished = false;
          break;
        }
      }
      if (all_carriers_finished) {
        active_ = false;
      }
    }
  }

  bool is_active() const noexcept { return active_; }
  bool is_held() const noexcept { return held_; }
  bool is_sustained() const noexcept { return sustained_; }
  bool is_steal_fading() const noexcept { return is_steal_fading_; }
  uint32_t channel() const noexcept { return channel_; }
  uint8_t note() const noexcept { return note_; }
  float current_level() const noexcept { return current_level_; }
  uint64_t start_time() const noexcept { return start_time_; }
  void set_start_time(uint64_t t) noexcept { start_time_ = t; }

 private:
  void trigger_release() noexcept {
    pitch_env_.note_off();
    for (int i = 0; i < 6; ++i) {
      op_envs_[i].note_off();
    }
  }

  bool active_{false};
  bool held_{false};
  bool sustained_{false};
  bool is_steal_fading_{false};
  float steal_fade_gain_{1.0f};
  float steal_fade_step_{0.0f};
  float bright_cur_{64.0f};
  float bright_target_{64.0f};

  uint32_t channel_{0};
  uint8_t note_{60};
  uint8_t velocity_{64};
  float sample_rate_{48000.0f};
  uint64_t start_time_{0};
  float current_level_{0.0f};
  float base_freq_{440.0f};
  float pitch_bend_semitones_{0.0f};

  FmAlgorithmDef alg_def_{};
  float fb_scale_{0.0f};

  std::array<FmEnvelope, 6> op_envs_{};
  std::array<float, 6> op_phases_{};
  std::array<float, 6> op_base_freqs_{};
  std::array<bool, 6> op_is_fixed_{};
  std::array<float, 6> op_base_gains_{};
  std::array<uint8_t, 6> op_amp_sens_{};
  std::array<std::array<float, 2>, 6> op_fb_history_{};

  FmPitchEnvelope pitch_env_{};
  FmLfo lfo_{};
  uint8_t lfo_pitch_sens_{3};
  float lfo_pitch_depth_{0.0f};
  float lfo_amp_depth_{0.0f};
};

} // namespace velora::core

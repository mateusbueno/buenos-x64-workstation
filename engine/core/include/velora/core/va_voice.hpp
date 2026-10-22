#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "velora/core/engine.hpp"
#include "velora/core/va_parameters.hpp"
#include "velora/core/va_polyblep.hpp"
#include "velora/core/va_filter.hpp"
#include "velora/core/va_envelope.hpp"
#include "velora/core/va_lfo.hpp"

namespace velora::core {

class VaVoice {
 public:
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr uint32_t kMaxUnison = 8;

  void start(uint32_t channel, uint8_t note, uint16_t velocity16,
             const VaVoiceParameters& params, float sample_rate = 48000.0f) noexcept {
    channel_ = channel;
    note_ = note;
    velocity_ = static_cast<uint8_t>(std::clamp(static_cast<int>(velocity16 >> 9), 1, 127));
    sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
    active_ = true;
    held_ = true;
    sustained_ = false;
    is_steal_fading_ = false;
    steal_fade_gain_ = 1.0f;
    steal_fade_step_ = 0.0f;
    start_time_ = 0;
    params_ = params;

    // Base pitch frequency (A4 = 440 Hz at note 69)
    float target_freq = 440.0f * std::pow(2.0f, (static_cast<float>(note_) - 69.0f) / 12.0f);
    if (current_freq_ <= 0.0f || params_.glide_time_ms <= 0.0f) {
      current_freq_ = target_freq;
    }
    target_freq_ = target_freq;

    // Glide coefficient
    if (params_.glide_time_ms > 0.0f) {
      float glide_samples = (params_.glide_time_ms * 0.001f) * sample_rate_;
      glide_coeff_ = 1.0f - std::exp(-3.0f / std::max(glide_samples, 1.0f));
    } else {
      glide_coeff_ = 1.0f;
      current_freq_ = target_freq_;
    }

    // Velocity scale
    float vel_frac = static_cast<float>(velocity_) / 127.0f;
    amp_vel_scale_ = 1.0f - params_.amp_velocity_sens * (1.0f - vel_frac);
    filter_vel_scale_ = 1.0f - params_.filter_velocity_sens * (1.0f - vel_frac);

    // Initialize envelopes
    amp_env_.init(params_.amp_attack_ms, params_.amp_decay_ms,
                  params_.amp_sustain_level, params_.amp_release_ms, sample_rate_);
    amp_env_.trigger();

    filter_env_.init(params_.filter_attack_ms, params_.filter_decay_ms,
                     params_.filter_sustain_level, params_.filter_release_ms, sample_rate_);
    filter_env_.trigger();

    // Initialize LFO
    lfo_.init(params_.lfo_wave, params_.lfo_rate_hz, params_.lfo_delay_ms,
              params_.lfo_key_sync, sample_rate_);
    if (params_.lfo_key_sync) {
      lfo_.reset();
    }

    // Reset filter and noise
    filter_.reset();
    pink_filter_.reset();

    // Reset oscillator phases
    uint32_t num_unison = std::clamp(static_cast<uint32_t>(params_.unison_voices), 1u, kMaxUnison);
    for (uint32_t u = 0; u < num_unison; ++u) {
      osc1_phases_[u] = 0.0f;
      osc2_phases_[u] = 0.0f;
      sub_phases_[u] = 0.0f;
      tri1_states_[u] = 0.0f;
      tri2_states_[u] = 0.0f;
    }
  }

  void legato_retune(uint8_t note, uint16_t velocity16) noexcept {
    note_ = note;
    velocity_ = static_cast<uint8_t>(std::clamp(static_cast<int>(velocity16 >> 9), 1, 127));
    target_freq_ = 440.0f * std::pow(2.0f, (static_cast<float>(note_) - 69.0f) / 12.0f);
    if (params_.glide_time_ms > 0.0f) {
      float glide_samples = (params_.glide_time_ms * 0.001f) * sample_rate_;
      glide_coeff_ = 1.0f - std::exp(-3.0f / std::max(glide_samples, 1.0f));
    } else {
      current_freq_ = target_freq_;
    }
  }

  void stop() noexcept {
    held_ = false;
    if (!sustained_) {
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

  void trigger_release() noexcept {
    amp_env_.release();
    filter_env_.release();
  }

  void set_pitch_bend(float semitones) noexcept {
    pitch_bend_semitones_ = semitones;
  }

  void set_brightness_offset(uint8_t brightness) noexcept {
    params_.brightness_offset = brightness;
  }

  void begin_steal_fade(uint32_t ramp_frames) noexcept {
    is_steal_fading_ = true;
    steal_fade_step_ = (ramp_frames > 0) ? (steal_fade_gain_ / static_cast<float>(ramp_frames)) : 1.0f;
  }

  void set_start_time(uint64_t clock) noexcept {
    start_time_ = clock;
  }

  uint64_t start_time() const noexcept { return start_time_; }
  uint32_t channel() const noexcept { return channel_; }
  uint32_t note() const noexcept { return note_; }
  bool is_active() const noexcept { return active_; }
  bool is_held() const noexcept { return held_; }
  bool is_sustained() const noexcept { return sustained_; }
  float last_level() const noexcept { return last_level_; }

  VoiceState voice_state() const noexcept {
    if (held_) return VoiceState::kHeld;
    if (sustained_) return VoiceState::kSustained;
    return VoiceState::kReleasing;
  }

  // Render stereo audio frames into out_left and out_right
  void render(float* out_left, float* out_right, uint32_t frames) noexcept {
    if (!active_) return;

    uint32_t num_unison = std::clamp(static_cast<uint32_t>(params_.unison_voices), 1u, kMaxUnison);
    float inv_unison = 1.0f / std::sqrt(static_cast<float>(num_unison));
    float pb_ratio = std::pow(2.0f, pitch_bend_semitones_ / 12.0f);

    for (uint32_t f = 0; f < frames; ++f) {
      if (!amp_env_.is_active()) {
        active_ = false;
        break;
      }

      // Smooth glide towards target frequency
      current_freq_ += glide_coeff_ * (target_freq_ - current_freq_);

      // Step envelopes & LFO
      float amp_env_val = amp_env_.step();
      float flt_env_val = filter_env_.step();
      float lfo_val = lfo_.step();

      // Pitch modulation from LFO and Pitch Bend
      float lfo_pitch_mod = lfo_val * params_.lfo_to_pitch; // in semitones
      float base_voice_freq = current_freq_ * pb_ratio * std::pow(2.0f, lfo_pitch_mod / 12.0f);

      // PWM modulation from LFO
      float pwm1 = std::clamp(params_.osc1_pulse_width + lfo_val * params_.lfo_to_pwm * 0.4f, 0.02f, 0.98f);
      float pwm2 = std::clamp(params_.osc2_pulse_width + lfo_val * params_.lfo_to_pwm * 0.4f, 0.02f, 0.98f);

      // Render unison oscillators
      float mono_mix = 0.0f;
      float stereo_left = 0.0f;
      float stereo_right = 0.0f;

      for (uint32_t u = 0; u < num_unison; ++u) {
        float detune_offset_cents = 0.0f;
        float pan = 0.5f;
        if (num_unison > 1) {
          float spread_idx = (static_cast<float>(u) / static_cast<float>(num_unison - 1)) * 2.0f - 1.0f;
          detune_offset_cents = spread_idx * params_.unison_detune_cents;
          pan = std::clamp(0.5f + 0.5f * spread_idx * params_.unison_spread, 0.0f, 1.0f);
        }

        // Osc 1 frequency
        float osc1_pitch_cents = (params_.osc1_octave * 12.0f + params_.osc1_semitone) * 100.0f +
                                 params_.osc1_detune_cents + detune_offset_cents;
        float f1 = base_voice_freq * std::pow(2.0f, osc1_pitch_cents / 1200.0f);
        float dt1 = f1 / sample_rate_;

        // Osc 2 frequency
        float osc2_pitch_cents = (params_.osc2_octave * 12.0f + params_.osc2_semitone) * 100.0f +
                                 params_.osc2_detune_cents + detune_offset_cents;
        float f2 = base_voice_freq * std::pow(2.0f, osc2_pitch_cents / 1200.0f);
        float dt2 = f2 / sample_rate_;

        // Advance Osc 1
        float s1 = render_osc(params_.osc1_wave, osc1_phases_[u], dt1, pwm1, tri1_states_[u]);
        osc1_phases_[u] += dt1;
        bool osc1_wrapped = false;
        if (osc1_phases_[u] >= 1.0f) {
          osc1_phases_[u] -= 1.0f;
          osc1_wrapped = true;
        }

        // Advance Osc 2 (with hard sync if enabled)
        if (params_.osc2_hard_sync && osc1_wrapped) {
          osc2_phases_[u] = osc1_phases_[u] * (dt2 / std::max(dt1, 1e-6f));
        }
        float s2 = render_osc(params_.osc2_wave, osc2_phases_[u], dt2, pwm2, tri2_states_[u]);
        osc2_phases_[u] += dt2;
        if (osc2_phases_[u] >= 1.0f) {
          osc2_phases_[u] -= 1.0f;
        }

        // Sub Oscillator (derived from Osc 1 frequency)
        float sub_sig = 0.0f;
        if (params_.sub_enabled) {
          float sub_f = f1 * (params_.sub_octave <= -2 ? 0.25f : 0.5f);
          float sub_dt = sub_f / sample_rate_;
          if (params_.sub_is_square) {
            sub_sig = (sub_phases_[u] < 0.5f ? 1.0f : -1.0f) + poly_blep(sub_phases_[u], sub_dt)
                      - poly_blep(std::fmod(sub_phases_[u] + 0.5f, 1.0f), sub_dt);
          } else {
            sub_sig = std::sin(kTwoPi * sub_phases_[u]);
          }
          sub_phases_[u] += sub_dt;
          if (sub_phases_[u] >= 1.0f) sub_phases_[u] -= 1.0f;
        }

        // Sub-voice mix
        float voice_osc_mix = (s1 * params_.mix_osc1 + s2 * params_.mix_osc2 + sub_sig * params_.mix_sub) * inv_unison;

        stereo_left += voice_osc_mix * (1.0f - pan);
        stereo_right += voice_osc_mix * pan;
      }

      // Noise generator
      float noise_sig = 0.0f;
      if (params_.noise_enabled || params_.mix_noise > 0.0f) {
        float white = generate_white_noise(rng_state_);
        noise_sig = params_.noise_is_pink ? pink_filter_.process(white) : white;
        noise_sig *= params_.mix_noise;
      }

      float pre_filter_mix = stereo_left + stereo_right + noise_sig;

      // Filter cutoff calculation with modulations
      float base_cutoff = params_.cutoff_hz;
      // Knob 2 Brightness: centered at 64
      if (params_.brightness_offset != 64) {
        float bright_mult = std::pow(2.0f, (static_cast<float>(params_.brightness_offset) - 64.0f) / 32.0f);
        base_cutoff *= bright_mult;
      }
      // Key tracking
      float key_offset = (static_cast<float>(note_) - 60.0f) * params_.filter_key_track / 12.0f;
      base_cutoff *= std::pow(2.0f, key_offset);
      // Envelope modulation with velocity scaling
      float env_mod_octaves = flt_env_val * params_.filter_env_amount * filter_vel_scale_ * 5.0f;
      base_cutoff *= std::pow(2.0f, env_mod_octaves);
      // LFO modulation
      float lfo_filter_octaves = lfo_val * params_.lfo_to_filter * 2.0f;
      base_cutoff *= std::pow(2.0f, lfo_filter_octaves);

      // Process through resonant filter
      float filtered_sig = filter_.process(pre_filter_mix, params_.filter_type,
                                           base_cutoff, params_.resonance,
                                           params_.drive, sample_rate_);

      // Apply Amp ADSR, Velocity, and Tremolo
      float tremolo = 1.0f + lfo_val * params_.lfo_to_amp * 0.5f;
      float voice_out = filtered_sig * amp_env_val * amp_vel_scale_ * std::max(tremolo, 0.0f);

      // Steal fade ramp
      if (is_steal_fading_) {
        voice_out *= steal_fade_gain_;
        steal_fade_gain_ -= steal_fade_step_;
        if (steal_fade_gain_ <= 0.0f) {
          steal_fade_gain_ = 0.0f;
          active_ = false;
        }
      }

      last_level_ = std::abs(voice_out);

      // Accumulate into output buffers
      out_left[f] += voice_out;
      out_right[f] += voice_out;
    }
  }

 private:
  float render_osc(VaWaveform wave, float phase, float dt, float pw, float& tri_state) noexcept {
    switch (wave) {
      case VaWaveform::kSaw: {
        float saw = (2.0f * phase - 1.0f) - poly_blep(phase, dt);
        return saw;
      }

      case VaWaveform::kPulse: {
        float pulse = (phase < pw ? 1.0f : -1.0f) + poly_blep(phase, dt)
                      - poly_blep(std::fmod(phase - pw + 1.0f, 1.0f), dt);
        return pulse;
      }

      case VaWaveform::kTriangle: {
        // Leaky integration of bandlimited pulse
        float square = (phase < 0.5f ? 1.0f : -1.0f) + poly_blep(phase, dt)
                       - poly_blep(std::fmod(phase + 0.5f, 1.0f), dt);
        tri_state = tri_state * 0.999f + 4.0f * dt * square;
        return tri_state;
      }

      case VaWaveform::kSine:
        return std::sin(kTwoPi * phase);

      default:
        return 0.0f;
    }
  }

  uint32_t channel_{0};
  uint8_t note_{60};
  uint8_t velocity_{100};
  float sample_rate_{48000.0f};
  bool active_{false};
  bool held_{false};
  bool sustained_{false};
  bool is_steal_fading_{false};
  float steal_fade_gain_{1.0f};
  float steal_fade_step_{0.0f};
  uint64_t start_time_{0};
  float last_level_{0.0f};

  float current_freq_{0.0f};
  float target_freq_{0.0f};
  float glide_coeff_{1.0f};
  float pitch_bend_semitones_{0.0f};

  float amp_vel_scale_{1.0f};
  float filter_vel_scale_{1.0f};

  VaVoiceParameters params_{};
  VaAdsrEnvelope amp_env_{};
  VaAdsrEnvelope filter_env_{};
  VaLfo lfo_{};
  VaFilter filter_{};
  PinkNoiseFilter pink_filter_{};

  std::array<float, kMaxUnison> osc1_phases_{};
  std::array<float, kMaxUnison> osc2_phases_{};
  std::array<float, kMaxUnison> sub_phases_{};
  std::array<float, kMaxUnison> tri1_states_{};
  std::array<float, kMaxUnison> tri2_states_{};

  uint32_t rng_state_{0x12345678};
};

} // namespace velora::core

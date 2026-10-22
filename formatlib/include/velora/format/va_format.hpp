#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace velora::format {

enum class VaWaveform : uint8_t {
  kSaw = 0,
  kPulse = 1,
  kTriangle = 2,
  kSine = 3,
};

enum class VaFilterType : uint8_t {
  kLowPass24 = 0,
  kLowPass12 = 1,
  kBandPass12 = 2,
  kHighPass12 = 3,
};

enum class VaLfoWaveform : uint8_t {
  kSine = 0,
  kTriangle = 1,
  kSawUp = 2,
  kSawDown = 3,
  kSquare = 4,
  kSampleAndHold = 5,
};

enum class VaVoiceMode : uint8_t {
  kPoly = 0,
  kMono = 1,
  kLegato = 2,
};

enum class VaGlideMode : uint8_t {
  kAlways = 0,
  kLegatoOnly = 1,
};

struct VaVoiceParameters {
  std::string name{"Clean VA Init"};

  // Oscillator 1
  VaWaveform osc1_wave{VaWaveform::kSaw};
  int8_t osc1_octave{0};        // -2 .. +2
  int8_t osc1_semitone{0};      // -24 .. +24
  float osc1_detune_cents{0.0f}; // -50.0 .. +50.0
  float osc1_pulse_width{0.5f}; // 0.01 .. 0.99

  // Oscillator 2
  VaWaveform osc2_wave{VaWaveform::kPulse};
  int8_t osc2_octave{0};
  int8_t osc2_semitone{0};
  float osc2_detune_cents{5.0f};
  float osc2_pulse_width{0.5f};
  bool osc2_hard_sync{false};

  // Sub Oscillator
  bool sub_enabled{true};
  int8_t sub_octave{-1};        // -1 or -2 octaves relative to Osc 1
  bool sub_is_square{true};     // true: square, false: sine

  // Noise Generator
  bool noise_enabled{false};
  bool noise_is_pink{false};    // false: white, true: pink

  // Mixer (0.0 .. 1.0)
  float mix_osc1{0.8f};
  float mix_osc2{0.65f};
  float mix_sub{0.2f};
  float mix_noise{0.0f};

  // Filter
  VaFilterType filter_type{VaFilterType::kLowPass24};
  float cutoff_hz{2000.0f};     // 20 Hz .. 20000 Hz
  float resonance{0.22f};       // 0.0 .. 1.0 (self-oscillation near 1.0)
  float drive{0.1f};            // 0.0 .. 1.0
  float filter_key_track{0.5f}; // 0.0 .. 1.0
  float filter_env_amount{0.4f};// -1.0 .. +1.0
  float filter_velocity_sens{0.35f}; // 0.0 .. 1.0

  // Amp ADSR (times in milliseconds, sustain in 0.0 .. 1.0)
  float amp_attack_ms{5.0f};
  float amp_decay_ms{300.0f};
  float amp_sustain_level{0.7f};
  float amp_release_ms{200.0f};
  float amp_velocity_sens{0.5f};

  // Filter ADSR
  float filter_attack_ms{10.0f};
  float filter_decay_ms{400.0f};
  float filter_sustain_level{0.5f};
  float filter_release_ms{250.0f};

  // LFO
  VaLfoWaveform lfo_wave{VaLfoWaveform::kTriangle};
  float lfo_rate_hz{4.0f};      // 0.1 .. 50.0 Hz
  float lfo_delay_ms{0.0f};     // 0.0 .. 2000.0 ms
  bool lfo_key_sync{false};
  float lfo_to_pitch{0.05f};    // Vibrato depth (semitones)
  float lfo_to_filter{0.2f};    // Cutoff modulation
  float lfo_to_amp{0.0f};       // Tremolo depth
  float lfo_to_pwm{0.0f};       // Pulse width mod depth

  // Voice & Performance
  VaVoiceMode voice_mode{VaVoiceMode::kPoly};
  uint8_t unison_voices{1};     // 1 .. 8
  float unison_detune_cents{15.0f}; // 0 .. 50 cents
  float unison_spread{0.5f};    // Stereo spread 0.0 .. 1.0
  float glide_time_ms{0.0f};    // 0 .. 2000 ms
  VaGlideMode glide_mode{VaGlideMode::kLegatoOnly};

  // Real-time offset (Knob 2 Brightness: 0..127, 64 is nominal)
  uint8_t brightness_offset{64};

  friend bool operator==(const VaVoiceParameters&, const VaVoiceParameters&) = default;
};

inline constexpr size_t kVaPackedVoiceSize = 160;

void pack_va_voice(const VaVoiceParameters& in, std::span<uint8_t, kVaPackedVoiceSize> out);
bool unpack_va_voice(std::span<const uint8_t, kVaPackedVoiceSize> in, VaVoiceParameters& out);

} // namespace velora::format

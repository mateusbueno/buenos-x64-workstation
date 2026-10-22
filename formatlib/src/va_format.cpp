#include "velora/format/va_format.hpp"

#include <cstring>
#include <algorithm>

namespace velora::format {

namespace {

void put_f32(uint8_t* p, float v) {
  std::memcpy(p, &v, sizeof(float));
}

float get_f32(const uint8_t* p) {
  float v = 0.0f;
  std::memcpy(&v, p, sizeof(float));
  return v;
}

} // namespace

void pack_va_voice(const VaVoiceParameters& in, std::span<uint8_t, kVaPackedVoiceSize> out) {
  std::fill(out.begin(), out.end(), uint8_t{0});
  uint8_t* p = out.data();

  // Name (32 bytes)
  size_t name_len = std::min<size_t>(in.name.size(), 31);
  std::memcpy(p, in.name.data(), name_len);

  // Osc 1
  p[32] = static_cast<uint8_t>(in.osc1_wave);
  p[33] = static_cast<uint8_t>(in.osc1_octave);
  p[34] = static_cast<uint8_t>(in.osc1_semitone);
  put_f32(p + 36, in.osc1_detune_cents);
  put_f32(p + 40, in.osc1_pulse_width);

  // Osc 2
  p[44] = static_cast<uint8_t>(in.osc2_wave);
  p[45] = static_cast<uint8_t>(in.osc2_octave);
  p[46] = static_cast<uint8_t>(in.osc2_semitone);
  p[47] = in.osc2_hard_sync ? 1 : 0;
  put_f32(p + 48, in.osc2_detune_cents);
  put_f32(p + 52, in.osc2_pulse_width);

  // Sub & Noise
  p[56] = in.sub_enabled ? 1 : 0;
  p[57] = static_cast<uint8_t>(in.sub_octave);
  p[58] = in.sub_is_square ? 1 : 0;
  p[59] = in.noise_enabled ? 1 : 0;
  p[60] = in.noise_is_pink ? 1 : 0;

  // Mix
  put_f32(p + 64, in.mix_osc1);
  put_f32(p + 68, in.mix_osc2);
  put_f32(p + 72, in.mix_sub);
  put_f32(p + 76, in.mix_noise);

  // Filter
  p[80] = static_cast<uint8_t>(in.filter_type);
  put_f32(p + 84, in.cutoff_hz);
  put_f32(p + 88, in.resonance);
  put_f32(p + 92, in.drive);
  put_f32(p + 96, in.filter_key_track);
  put_f32(p + 100, in.filter_env_amount);
  put_f32(p + 104, in.filter_velocity_sens);

  // Amp ADSR
  put_f32(p + 108, in.amp_attack_ms);
  put_f32(p + 112, in.amp_decay_ms);
  put_f32(p + 116, in.amp_sustain_level);
  put_f32(p + 120, in.amp_release_ms);
  put_f32(p + 124, in.amp_velocity_sens);

  // Filter ADSR
  put_f32(p + 128, in.filter_attack_ms);
  put_f32(p + 132, in.filter_decay_ms);
  put_f32(p + 136, in.filter_sustain_level);
  put_f32(p + 140, in.filter_release_ms);

  // LFO & Mode
  p[144] = static_cast<uint8_t>(in.lfo_wave);
  p[145] = in.lfo_key_sync ? 1 : 0;
  p[146] = static_cast<uint8_t>(in.voice_mode);
  p[147] = in.unison_voices;
  put_f32(p + 148, in.lfo_rate_hz);
  put_f32(p + 152, in.lfo_to_pitch);
  p[156] = static_cast<uint8_t>(in.glide_mode);
  p[157] = in.brightness_offset;
}

bool unpack_va_voice(std::span<const uint8_t, kVaPackedVoiceSize> in, VaVoiceParameters& out) {
  const uint8_t* p = in.data();

  // Name
  char name_buf[33]{};
  std::memcpy(name_buf, p, 32);
  out.name = std::string(name_buf);

  // Osc 1
  out.osc1_wave = static_cast<VaWaveform>(p[32] % 4);
  out.osc1_octave = static_cast<int8_t>(p[33]);
  out.osc1_semitone = static_cast<int8_t>(p[34]);
  out.osc1_detune_cents = get_f32(p + 36);
  out.osc1_pulse_width = get_f32(p + 40);

  // Osc 2
  out.osc2_wave = static_cast<VaWaveform>(p[44] % 4);
  out.osc2_octave = static_cast<int8_t>(p[45]);
  out.osc2_semitone = static_cast<int8_t>(p[46]);
  out.osc2_hard_sync = p[47] != 0;
  out.osc2_detune_cents = get_f32(p + 48);
  out.osc2_pulse_width = get_f32(p + 52);

  // Sub & Noise
  out.sub_enabled = p[56] != 0;
  out.sub_octave = static_cast<int8_t>(p[57]);
  out.sub_is_square = p[58] != 0;
  out.noise_enabled = p[59] != 0;
  out.noise_is_pink = p[60] != 0;

  // Mix
  out.mix_osc1 = get_f32(p + 64);
  out.mix_osc2 = get_f32(p + 68);
  out.mix_sub = get_f32(p + 72);
  out.mix_noise = get_f32(p + 76);

  // Filter
  out.filter_type = static_cast<VaFilterType>(p[80] % 4);
  out.cutoff_hz = get_f32(p + 84);
  out.resonance = get_f32(p + 88);
  out.drive = get_f32(p + 92);
  out.filter_key_track = get_f32(p + 96);
  out.filter_env_amount = get_f32(p + 100);
  out.filter_velocity_sens = get_f32(p + 104);

  // Amp ADSR
  out.amp_attack_ms = get_f32(p + 108);
  out.amp_decay_ms = get_f32(p + 112);
  out.amp_sustain_level = get_f32(p + 116);
  out.amp_release_ms = get_f32(p + 120);
  out.amp_velocity_sens = get_f32(p + 124);

  // Filter ADSR
  out.filter_attack_ms = get_f32(p + 128);
  out.filter_decay_ms = get_f32(p + 132);
  out.filter_sustain_level = get_f32(p + 136);
  out.filter_release_ms = get_f32(p + 140);

  // LFO & Mode
  out.lfo_wave = static_cast<VaLfoWaveform>(p[144] % 6);
  out.lfo_key_sync = p[145] != 0;
  out.voice_mode = static_cast<VaVoiceMode>(p[146] % 3);
  out.unison_voices = std::clamp<uint8_t>(p[147], 1, 8);
  out.lfo_rate_hz = get_f32(p + 148);
  out.lfo_to_pitch = get_f32(p + 152);
  out.glide_mode = static_cast<VaGlideMode>(p[156] % 2);
  out.brightness_offset = p[157];

  return true;
}

} // namespace velora::format

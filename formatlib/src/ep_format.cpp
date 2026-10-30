#include "velora/format/ep_format.hpp"

#include <algorithm>
#include <cstring>

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

}  // namespace

void pack_ep_voice(const EpVoiceParameters& in, std::span<uint8_t, kEpPackedVoiceSize> out) {
  std::fill(out.begin(), out.end(), uint8_t{0});
  uint8_t* p = out.data();

  // Name (32 bytes)
  size_t name_len = std::min<size_t>(in.name.size(), 31);
  std::memcpy(p, in.name.data(), name_len);

  // Type & offset
  p[32] = static_cast<uint8_t>(in.instrument_type);
  p[33] = in.brightness_offset;

  // Voicing floats (each 4 bytes, aligned)
  put_f32(p + 36, in.hammer_hardness);
  put_f32(p + 40, in.pickup_position);
  put_f32(p + 44, in.pickup_level);
  put_f32(p + 48, in.damper_noise);
  put_f32(p + 52, in.attack_noise);
  put_f32(p + 56, in.decay_rate);
  put_f32(p + 60, in.tone_bar_coupling);
}

bool unpack_ep_voice(std::span<const uint8_t, kEpPackedVoiceSize> in, EpVoiceParameters& out) {
  const uint8_t* p = in.data();

  // Name
  char name_buf[33]{};
  std::memcpy(name_buf, p, 32);
  out.name = std::string(name_buf);

  uint8_t raw_type = p[32];
  out.instrument_type = (raw_type == 1) ? EpInstrumentType::kReed : EpInstrumentType::kTine;
  out.brightness_offset = p[33];

  out.hammer_hardness = get_f32(p + 36);
  out.pickup_position = get_f32(p + 40);
  out.pickup_level = get_f32(p + 44);
  out.damper_noise = get_f32(p + 48);
  out.attack_noise = get_f32(p + 52);
  out.decay_rate = get_f32(p + 56);
  out.tone_bar_coupling = get_f32(p + 60);

  return true;
}

}  // namespace velora::format

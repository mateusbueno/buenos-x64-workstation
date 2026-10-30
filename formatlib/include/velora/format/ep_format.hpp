#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace velora::format {

enum class EpInstrumentType : uint8_t {
  kTine = 0,
  kReed = 1,
};

struct EpVoiceParameters {
  std::string name{"Stage 73 Pure"};
  EpInstrumentType instrument_type{EpInstrumentType::kTine};
  float hammer_hardness{0.5f};     // [0.0, 1.0]: strike contact stiffness
  float pickup_position{0.5f};     // [0.0, 1.0]: harmonic bark / pickup placement
  float pickup_level{0.7f};        // [0.0, 1.0]: transducer saturation drive
  float damper_noise{0.3f};        // [0.0, 1.0]: felt scrape damping level
  float attack_noise{0.3f};        // [0.0, 1.0]: mechanical hammer strike click
  float decay_rate{0.5f};          // [0.0, 1.0]: sustain damping rate
  float tone_bar_coupling{0.5f};   // [0.0, 1.0]: tone bar resonance coupling
  uint8_t brightness_offset{64};   // [0, 127], 64 nominal

  friend bool operator==(const EpVoiceParameters&, const EpVoiceParameters&) = default;
};

inline constexpr size_t kEpPackedVoiceSize = 64;

void pack_ep_voice(const EpVoiceParameters& in, std::span<uint8_t, kEpPackedVoiceSize> out);
bool unpack_ep_voice(std::span<const uint8_t, kEpPackedVoiceSize> in, EpVoiceParameters& out);

}  // namespace velora::format

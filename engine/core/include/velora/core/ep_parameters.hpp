#pragma once

#include <cstdint>

namespace velora::core {

enum class EpInstrumentType : uint8_t {
  kTine = 0,
  kReed = 1,
};

struct EpVoicingParameters {
  EpInstrumentType instrument_type = EpInstrumentType::kTine;
  float hammer_hardness = 0.5f;     // [0.0, 1.0]: strike contact stiffness and high-mode excitation
  float pickup_position = 0.5f;     // [0.0, 1.0]: geometric offset relative to pole (even vs odd harmonics, bark)
  float pickup_level = 0.7f;        // [0.0, 1.0]: pre-transducer gain & saturation drive
  float damper_noise = 0.3f;        // [0.0, 1.0]: felt scrape & damping transient level
  float attack_noise = 0.3f;        // [0.0, 1.0]: mechanical hammer strike click transient level
  float decay_rate = 0.5f;          // [0.0, 1.0]: overall sustain/damping scaling
  float tone_bar_coupling = 0.5f;   // [0.0, 1.0]: resonance reflection supporting fundamental sustain
};

inline EpVoicingParameters create_default_tine_parameters() {
  EpVoicingParameters p{};
  p.instrument_type = EpInstrumentType::kTine;
  p.hammer_hardness = 0.5f;
  p.pickup_position = 0.5f;
  p.pickup_level = 0.7f;
  p.damper_noise = 0.3f;
  p.attack_noise = 0.3f;
  p.decay_rate = 0.5f;
  p.tone_bar_coupling = 0.5f;
  return p;
}

inline EpVoicingParameters create_default_reed_parameters() {
  EpVoicingParameters p{};
  p.instrument_type = EpInstrumentType::kReed;
  p.hammer_hardness = 0.6f;
  p.pickup_position = 0.4f;
  p.pickup_level = 0.75f;
  p.damper_noise = 0.25f;
  p.attack_noise = 0.35f;
  p.decay_rate = 0.55f;
  p.tone_bar_coupling = 0.4f;
  return p;
}

}  // namespace velora::core

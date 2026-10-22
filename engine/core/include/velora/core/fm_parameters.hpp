#pragma once

#include <cstdint>
#include <array>
#include <string>

namespace velora::core {

struct FmOperatorParameters {
  // Envelopes: 4 rates (0..99) and 4 levels (0..99)
  std::array<uint8_t, 4> rates{99, 99, 99, 99};
  std::array<uint8_t, 4> levels{99, 99, 99, 0};

  // Keyboard Level Scaling
  uint8_t kbd_breakpoint{60}; // C3 (note 60)
  uint8_t left_depth{0};      // 0..99
  uint8_t right_depth{0};     // 0..99
  uint8_t left_curve{0};      // 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN
  uint8_t right_curve{0};     // 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN

  // Scaling & Sensitivity
  uint8_t kbd_rate_scaling{0}; // 0..7
  uint8_t amp_mod_sens{0};     // 0..3
  uint8_t velocity_sens{0};    // 0..7
  uint8_t total_level{99};     // 0..99 (carrier volume or modulation index)

  // Frequency
  bool fixed_freq_mode{false}; // false: ratio (tracked), true: fixed Hz
  uint8_t freq_coarse{1};      // 0..31
  uint8_t freq_fine{0};        // 0..99
  int8_t detune{0};            // -7..+7
};

struct FmVoiceParameters {
  // 6 Operators: index 0 = OP1, index 5 = OP6
  std::array<FmOperatorParameters, 6> operators{};

  // Pitch Envelope: 4 rates (0..99) and 4 levels (0..99, 50 = center)
  std::array<uint8_t, 4> pitch_env_rates{99, 99, 99, 99};
  std::array<uint8_t, 4> pitch_env_levels{50, 50, 50, 50};

  // Algorithm & Feedback
  uint8_t algorithm{5}; // 1..32
  uint8_t feedback{4};  // 0..7
  bool osc_key_sync{true};

  // LFO
  uint8_t lfo_speed{35};             // 0..99
  uint8_t lfo_delay{0};              // 0..99
  uint8_t lfo_pitch_mod_depth{0};    // 0..99
  uint8_t lfo_amp_mod_depth{0};      // 0..99
  bool lfo_key_sync{true};
  uint8_t lfo_wave{1};               // 0: Tri, 1: SawDown, 2: SawUp, 3: Sqr, 4: Sine, 5: S&H
  uint8_t lfo_pitch_mod_sens{3};     // 0..7

  int8_t transpose{0};               // -24..+24 semitones
  char name[11]{"INIT VOICE"};

  // Velora extension
  uint8_t brightness_offset{64};     // 0..127 (Knob 2 Brightness mapping)
};

// Creates a default initialization voice
inline FmVoiceParameters create_init_fm_voice() {
  FmVoiceParameters p{};
  // OP1: Carrier, ratio 1.0, full level
  p.operators[0].total_level = 99;
  p.operators[0].freq_coarse = 1;
  p.operators[0].freq_fine = 0;
  p.operators[0].rates = {99, 99, 99, 99};
  p.operators[0].levels = {99, 99, 99, 0};

  // OP2..OP6: Modulators, off by default until raised
  for (int i = 1; i < 6; ++i) {
    p.operators[i].total_level = 0;
    p.operators[i].freq_coarse = 1;
    p.operators[i].freq_fine = 0;
    p.operators[i].rates = {99, 99, 99, 99};
    p.operators[i].levels = {99, 99, 99, 0};
  }
  return p;
}

// Creates classic FM Electric Piano voice preset
inline FmVoiceParameters create_ep_fm_voice() {
  FmVoiceParameters p = create_init_fm_voice();
  std::snprintf(p.name, sizeof(p.name), "FM E.PIANO");
  p.algorithm = 5;
  p.feedback = 6;

  // OP1 (carrier, ratio 1.0)
  p.operators[0].total_level = 99;
  p.operators[0].freq_coarse = 1;
  p.operators[0].rates = {95, 30, 20, 45};
  p.operators[0].levels = {99, 90, 75, 0};
  p.operators[0].velocity_sens = 2;

  // OP2 (modulator for OP1, ratio 14.0 for tine chime)
  p.operators[1].total_level = 76;
  p.operators[1].freq_coarse = 14;
  p.operators[1].rates = {95, 60, 40, 50};
  p.operators[1].levels = {99, 70, 0, 0};
  p.operators[1].velocity_sens = 4;

  // OP3 (carrier, ratio 1.0)
  p.operators[2].total_level = 90;
  p.operators[2].freq_coarse = 1;
  p.operators[2].rates = {90, 35, 25, 45};
  p.operators[2].levels = {99, 85, 70, 0};
  p.operators[2].velocity_sens = 3;

  // OP4 (modulator for OP3, ratio 1.0 warm body)
  p.operators[3].total_level = 82;
  p.operators[3].freq_coarse = 1;
  p.operators[3].rates = {92, 40, 30, 48};
  p.operators[3].levels = {99, 80, 0, 0};
  p.operators[3].velocity_sens = 5;

  // OP5 & OP6 (carrier & modulator for soft attack warmth)
  p.operators[4].total_level = 84;
  p.operators[4].freq_coarse = 1;
  p.operators[4].rates = {88, 30, 20, 40};
  p.operators[4].levels = {99, 80, 60, 0};

  p.operators[5].total_level = 78;
  p.operators[5].freq_coarse = 1;
  p.operators[5].rates = {90, 45, 35, 45};
  p.operators[5].levels = {99, 75, 0, 0};
  p.operators[5].velocity_sens = 3;

  return p;
}

} // namespace velora::core

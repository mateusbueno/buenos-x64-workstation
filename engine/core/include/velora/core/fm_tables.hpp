#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace velora::core {

struct FmAlgorithmDef {
  uint8_t feedback_op;          // 0..5 (0 = OP1, 5 = OP6)
  std::array<bool, 6> is_carrier; // which operators output to audio
  // mod_matrix[dst][src]: true if operator 'src' modulates operator 'dst'
  std::array<std::array<bool, 6>, 6> mod_matrix;
};

// Generates the table of all 32 standard 6-operator algorithms
inline FmAlgorithmDef get_fm_algorithm_def(uint8_t alg_1_based) {
  FmAlgorithmDef d{};
  for (int i = 0; i < 6; ++i) {
    d.is_carrier[i] = false;
    for (int j = 0; j < 6; ++j) {
      d.mod_matrix[i][j] = false;
    }
  }

  uint8_t a = (alg_1_based < 1 || alg_1_based > 32) ? 1 : alg_1_based;
  // Operator index mapping: 0=OP1, 1=OP2, 2=OP3, 3=OP4, 4=OP5, 5=OP6

  switch (a) {
    case 1:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 2:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 3:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 4:
      d.feedback_op = 3;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 5:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 6:
      d.feedback_op = 4;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 7:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 8:
      d.feedback_op = 3;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 9:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 10:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 11:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 12:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 13:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 14:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[1][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 15:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[1][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 16:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 17:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 18:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 19:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 20:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 21:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 22:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[0][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 23:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 24:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 25:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 26:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[2][5] = true;
      break;
    case 27:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[2][5] = true;
      break;
    case 28:
      d.feedback_op = 4;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 29:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 30:
      d.feedback_op = 4;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 31:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 32:
    default:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.is_carrier[5] = true;
      break;
  }
  return d;
}

// Converts operator level (0..99) to linear output/modulation gain
inline float fm_level_to_gain(uint8_t level) {
  if (level == 0) return 0.0f;
  if (level >= 99) return 1.0f;
  // Approximately 0.75 dB per step below 99
  float db = -0.75f * static_cast<float>(99 - level);
  return std::pow(10.0f, db / 20.0f);
}

// Converts velocity (1..127) and velocity sensitivity (0..7) to gain multiplier
inline float fm_velocity_to_gain(uint8_t vel, uint8_t sens) {
  if (sens == 0) return 1.0f;
  float norm_v = static_cast<float>(std::clamp(static_cast<int>(vel), 1, 127)) / 127.0f;
  float power = 0.4f * static_cast<float>(sens);
  return std::pow(norm_v, power);
}

// Feedback scale multiplier for feedback levels 0..7
inline float fm_feedback_scale(uint8_t fb) {
  if (fb == 0) return 0.0f;
  // Level 1: ~0.06 pi, Level 7: ~4.0 pi
  return 0.0625f * static_cast<float>(1 << fb);
}

// Pitch envelope level (0..99, 50 = center) to frequency ratio offset
inline float fm_pitch_env_to_ratio(uint8_t level) {
  if (level == 50) return 1.0f;
  // Level 0: -48 semitones, Level 99: +48 semitones
  float semitones = (static_cast<float>(level) - 50.0f) * (48.0f / 49.0f);
  return std::pow(2.0f, semitones / 12.0f);
}

} // namespace velora::core

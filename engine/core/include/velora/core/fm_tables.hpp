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
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 2:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
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
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 11:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 12:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[2][5] = true;
      break;
    case 13:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[2][4] = true;
      d.mod_matrix[2][5] = true;
      break;
    case 14:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 15:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 16:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[0][4] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 17:
      d.feedback_op = 1;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[0][4] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 18:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[0][3] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 19:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 20:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 21:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[0][2] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 22:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][5] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 23:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 24:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[2][5] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 25:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[3][5] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 26:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 27:
      d.feedback_op = 2;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[3] = true;
      d.mod_matrix[1][2] = true;
      d.mod_matrix[3][4] = true;
      d.mod_matrix[3][5] = true;
      break;
    case 28:
      d.feedback_op = 4;
      d.is_carrier[0] = true;
      d.is_carrier[2] = true;
      d.is_carrier[5] = true;
      d.mod_matrix[0][1] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      break;
    case 29:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[4] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[4][5] = true;
      break;
    case 30:
      d.feedback_op = 4;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[5] = true;
      d.mod_matrix[2][3] = true;
      d.mod_matrix[3][4] = true;
      break;
    case 31:
      d.feedback_op = 5;
      d.is_carrier[0] = true;
      d.is_carrier[1] = true;
      d.is_carrier[2] = true;
      d.is_carrier[3] = true;
      d.is_carrier[4] = true;
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
// DX7 level scaling: 8 steps per octave (0.752575 dB/step), exact base-2 power
inline float fm_level_to_gain(uint8_t level) {
  if (level == 0) return 0.0f;
  if (level >= 99) return 1.0f;
  if (level < 20) {
    static const uint8_t lut[20] = {
      0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46
    };
    int eff = static_cast<int>(lut[level]) - 28;
    return std::pow(2.0f, static_cast<float>(eff - 99) / 8.0f);
  }
  return std::pow(2.0f, static_cast<float>(static_cast<int>(level) - 99) / 8.0f);
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
  // Level 1: 2^(-7) cycles, Level 7: 0.5 cycles (pi rad)
  return static_cast<float>(1 << fb) / 256.0f;
}

// Pitch envelope level (0..99, 50 = center) to frequency ratio offset
inline float fm_pitch_env_to_ratio(uint8_t level) {
  if (level == 50) return 1.0f;
  // Level 0: -48 semitones, Level 99: +48 semitones
  float semitones = (static_cast<float>(level) - 50.0f) * (48.0f / 49.0f);
  return std::pow(2.0f, semitones / 12.0f);
}

// Computes operator oscillator frequency in Hz matching standard Yamaha DX7 log-frequency LUT
inline float fm_osc_freq(int midinote, int coarse, int fine, int detune, float sample_rate) {
  static const int32_t coarsemul[32] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622
  };
  const int base = 50857777;
  const int step = 1398101;
  int32_t logfreq = base + step * midinote + coarsemul[coarse & 31];
  if (fine) {
    logfreq += static_cast<int32_t>(std::floor(24204406.323123 * std::log(1.0 + 0.01 * fine) + 0.5));
  }
  int32_t detune_delta = (detune - 7) << 7;
  logfreq += detune_delta;

  int ix = (logfreq & 0xffffff) >> 14;
  int lowbits = logfreq & 0x3fff;
  double y = (static_cast<double>(1ULL << 44)) / static_cast<double>(sample_rate);
  double y0 = std::floor(y * std::pow(2.0, ix / 1024.0) + 0.5);
  double y1 = std::floor(y * std::pow(2.0, (ix + 1) / 1024.0) + 0.5);
  int64_t y_interp = static_cast<int64_t>(y0) + (((static_cast<int64_t>(y1 - y0) * lowbits)) >> 14);
  int hibits = logfreq >> 24;
  int32_t freq_int = static_cast<int32_t>(y_interp >> (20 - hibits));
  return static_cast<float>(freq_int) * (sample_rate / 16777216.0f);
}

} // namespace velora::core

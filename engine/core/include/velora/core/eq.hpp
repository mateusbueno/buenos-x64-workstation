#pragma once
// Three-band EQ (Story 2.8): Low shelf, Mid peak with Mid freq, High shelf, gains +-15 dB. One DSP, two uses: the `eq` insert
// effect (make_effect(EffectType::kEq), parameters are the four normalised registry insert params) and the master EQ of the
// Mixer (physical units from the master_eq_* registry parameters, ids 500-503). The four controls are the fixed encoder
// functions LOW, MID, MID FREQ, HIGH of the panel (insert_param_1..4 in that order).
//
// Mapping (the one place; engine/core/README.md repeats it as a table):
//   insert_param_1 (LOW)      0..1 -> low shelf gain  -15..+15 dB, linear  (0.5 = 0 dB)   == master_eq_low_gain  (dB)
//   insert_param_2 (MID)      0..1 -> mid peak gain   -15..+15 dB, linear  (0.5 = 0 dB)   == master_eq_mid_gain  (dB)
//   insert_param_3 (MID FREQ) 0..1 -> mid frequency   200..8000 Hz, log    (0.5 = 1265 Hz) == master_eq_mid_freq (Hz)
//   insert_param_4 (HIGH)     0..1 -> high shelf gain -15..+15 dB, linear  (0.5 = 0 dB)   == master_eq_high_gain (dB)
//   insert_param_5            unused by the EQ.
//
// Design (fixed engine rate kEqSampleRate = 48 kHz): low shelf at 100 Hz and high shelf at 4 kHz with shelf slope S = 1 (Q = 1/sqrt(2)),
// peak with Q = 1. The shelves are the public RBJ "Audio EQ Cookbook" biquads (bilinear, pre-warped at the corner); the peak is the RBJ
// analog prototype realised with the matched-magnitude design of M. Vicanek (see eq.cpp), because the bilinear peak is pulled down far above
// its centre when the centre is high.
// "Design curve" = the magnitude of the ANALOG RBJ prototype H(s) (s normalised to the corner / centre frequency) evaluated at the real
// frequency. tests/eq_compressor_test.cpp compares the digital response with it: within +-0.5 dB from 20 Hz to 16 kHz (0.33 x fs) for
// every mid frequency 200..8000 Hz at +-15 dB, and within 1 dB at 20 kHz (0.42 x fs).
//
// Smoothing and identity: the physical targets are approached by a one-pole per block (about 10 ms time constant); the coefficients are
// recomputed once per block while anything moves and held within the block. A band whose current and target gain are exactly 0 dB is
// skipped (and its state cleared), so an EQ at identity settings is bit-exact transparent.
#include <array>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

constexpr std::uint32_t kEqSampleRate = 48000;  // = kSampleRate (checked in eq.cpp)

namespace eqmap {
constexpr float kMinGainDb = -15.0f, kMaxGainDb = 15.0f;
constexpr float kMinMidHz = 200.0f, kMaxMidHz = 8000.0f;
constexpr float kLowShelfHz = 100.0f, kHighShelfHz = 4000.0f, kMidQ = 1.0f;
float clamp01(float v) noexcept;  // NaN -> 0.5
float gain_db(float norm) noexcept;
float mid_hz(float norm) noexcept;
}  // namespace eqmap

class EqEffect final : public Effect {
 public:
  EqEffect() noexcept;
  EffectType type() const noexcept override { return EffectType::kEq; }
  std::uint32_t cost_units() const noexcept override { return effect_cost_units(EffectType::kEq); }
  void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept override;
  void reset() noexcept VELORA_NONBLOCKING override;
  // Normalised insert parameters, applied immediately (control thread, before the hand-over).
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override;
  // Physical units (dB, Hz); clamped to the registry ranges. instant = no smoothing (setup, first generation).
  void set_physical(float low_db, float mid_db, float mid_hz, float high_db, bool instant) noexcept VELORA_NONBLOCKING;
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;

  // True when every band is at exactly 0 dB (current and target): process() leaves a block untouched.
  bool is_identity() const noexcept { return all_zero(gain_cur_) && all_zero(gain_tgt_); }
  // |H(e^jw)| of the CURRENT coefficients in dB at `hz` (tests, later the UI curve). Not RT-critical, no allocation.
  double magnitude_db(double hz) const noexcept;

 private:
  static bool all_zero(const std::array<float, 3>& g) noexcept { return g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f; }
  struct Coef {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct State {
    double z1[2] = {0, 0}, z2[2] = {0, 0};
  };
  void design() noexcept VELORA_NONBLOCKING;
  void step_smoothing() noexcept VELORA_NONBLOCKING;

  std::array<float, 3> gain_cur_{}, gain_tgt_{};  // low, mid, high in dB
  float freq_cur_ = 1000.0f, freq_tgt_ = 1000.0f;  // mid, Hz
  std::array<Coef, 3> coef_{};
  std::array<State, 3> state_{};
  std::array<bool, 3> active_{};  // band was non-zero last block (state is live)
  bool moving_ = false;
};

}  // namespace velora::core

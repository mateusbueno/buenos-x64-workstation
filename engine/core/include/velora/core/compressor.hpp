#pragma once
// Feed-forward compressor (Story 2.8): threshold, ratio, attack, release, makeup gain, and the gain reduction published for the UI.
// One DSP, two uses: the `compressor` insert effect and the master compressor of the Mixer (master_comp_* registry parameters,
// ids 510-514, physical units).
//
// Mapping (the one place; engine/core/README.md repeats it as a table):
//   insert_param_1 THRESHOLD 0..1 -> -60..0 dB, linear   (0.5 = -30 dB)        == master_comp_threshold (dB)
//   insert_param_2 RATIO     0..1 -> 1..20 :1, log 20^p  (0 = 1:1, 0.5 = 4.5:1) == master_comp_ratio (ratio)
//   insert_param_3 ATTACK    0..1 -> 0.1..100 ms, log    (0.5 = 3.2 ms)         == master_comp_attack (ms)
//   insert_param_4 RELEASE   0..1 -> 10..1000 ms, log    (0.5 = 100 ms)         == master_comp_release (ms)
//   insert_param_5 MAKEUP    0..1 -> 0..24 dB, linear    (default 0 = 0 dB)     == master_comp_makeup (dB)
//
// Detector and law (all per sample, stereo-linked so the image does not shift):
//   level  x = max(|L|, |R|)                        peak detector, no extra smoothing (the gain smoother below is the only dynamics)
//   x_dB   = 20 log10(x)
//   over   = x_dB - threshold
//   target = over > 0 ? -over * (1 - 1/ratio) : 0   hard-knee static curve, gain change in dB (<= 0)
//   g      = target + c * (g - target)              log-domain "branching" smoother: c = c_attack when target < g (more reduction),
//                                                  c_release otherwise;  c_t = exp(-1 / (t * fs))
//   out    = in * 10^((g + makeup)/20)
// Attack / release definition: a first-order exponential in dB, so with the input stepped to a level that needs a final gain
// reduction G, GR(t) = G (1 - exp(-t/attack)): the attack time is the time to reach 63.2 % (1 - 1/e) of G, the release time the
// time to fall to 36.8 % (1/e) of the reduction it started from, i.e. 63.2 % of the way back to the new target. The test measures
// exactly that on a DC step.
//
// Published value: gain_reduction_db() = -g >= 0 (dB of reduction), updated at the end of every block (an atomic, readable by any
// thread). Makeup is ramped linearly over each block when it changes. Threshold, ratio, attack and release are applied at once: the
// gain smoother turns any jump of the target into a click-free move.
//
// Non-finite input samples (NaN, inf) are replaced by 0 in the output, like in the EQ, once the compressor works on the block.
// Identity: ratio exactly 1 with makeup exactly 0 dB (and the reduction settled at 0) leaves the block untouched, bit for bit.
#include <array>
#include <atomic>
#include <cstdint>

#include "velora/core/fx.hpp"

namespace velora::core {

namespace compmap {
constexpr float kMinThresholdDb = -60.0f, kMaxThresholdDb = 0.0f;
constexpr float kMinRatio = 1.0f, kMaxRatio = 20.0f;
constexpr float kMinAttackMs = 0.1f, kMaxAttackMs = 100.0f;
constexpr float kMinReleaseMs = 10.0f, kMaxReleaseMs = 1000.0f;
constexpr float kMaxMakeupDb = 24.0f;
float threshold_db(float norm) noexcept;
float ratio(float norm) noexcept;
float attack_ms(float norm) noexcept;
float release_ms(float norm) noexcept;
float makeup_db(float norm) noexcept;
}  // namespace compmap

class CompressorEffect final : public Effect {
 public:
  CompressorEffect() noexcept;
  EffectType type() const noexcept override { return EffectType::kCompressor; }
  std::uint32_t cost_units() const noexcept override { return effect_cost_units(EffectType::kCompressor); }
  void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept override;
  void reset() noexcept VELORA_NONBLOCKING override;
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override;
  // Physical units; clamped to the registry ranges. instant: the makeup applies without a ramp.
  void set_physical(float threshold_db, float ratio, float attack_ms, float release_ms, float makeup_db, bool instant) noexcept
      VELORA_NONBLOCKING;
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;
  float gain_reduction_db() const noexcept VELORA_NONBLOCKING override { return gr_published_.load(std::memory_order_relaxed); }

  // The current settings (tests, UI read-back).
  float threshold_db() const noexcept { return threshold_; }
  float ratio() const noexcept { return ratio_; }
  float attack_ms() const noexcept { return attack_ms_; }
  float release_ms() const noexcept { return release_ms_; }
  float makeup_db() const noexcept { return makeup_tgt_; }

  // True when process() leaves a block untouched (see "Identity" above).
  bool is_identity() const noexcept { return ratio_ == 1.0f && makeup_cur_ == 0.0f && makeup_tgt_ == 0.0f && g_db_ == 0.0; }

 private:
  void set_times() noexcept VELORA_NONBLOCKING;

  float threshold_ = -12.0f, ratio_ = 1.0f, attack_ms_ = 10.0f, release_ms_ = 100.0f;
  float makeup_cur_ = 0.0f, makeup_tgt_ = 0.0f;
  double att_coef_ = 0.0, rel_coef_ = 0.0;
  double g_db_ = 0.0;  // smoothed gain change, dB, <= 0
  std::atomic<float> gr_published_{0.0f};
};

}  // namespace velora::core

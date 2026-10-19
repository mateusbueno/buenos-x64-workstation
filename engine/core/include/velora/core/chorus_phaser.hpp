#pragma once
// Chorus / phaser insert effect (Story 2.9). Clean-room, public-fact design: a chorus is a short delay modulated by a slow LFO and mixed
// with the dry signal; a phaser is a chain of first-order all-pass stages whose break frequency is swept by an LFO (with optional
// feedback around the chain) and mixed with the dry signal, so the all-pass phase shifts turn into notches.
//
// Mapping (the one place; engine/core/README.md repeats it as a table). The five normalised insert params (registry insert_param_1..5):
//   insert_param_1 MODE     0..1 -> chorus for p <= 0.5, phaser for p > 0.5 (one threshold, no hysteresis; NaN -> 0.5 -> chorus)
//   insert_param_2 RATE     0..1 -> LFO rate 0.1..10 Hz, logarithmic, 0.1 * 100^p   (0.5 = 1 Hz)
//   insert_param_3 DEPTH    0..1 -> sweep depth, linear (0 = no modulation, 1 = full sweep)
//   insert_param_4 FEEDBACK 0..1 -> phaser feedback -0.8..+0.8, linear, bipolar (0.5 = 0); unused by the chorus
//   insert_param_5 MIX      0..1 -> wet amount, linear: out = dry + mix * (wet - dry)   (default 0 = dry: bit-exact identity)
//
// Chorus: per channel one delay line (2048 samples) read with a 4-point Lagrange interpolator at  d = 15 ms + 10 ms * depth * lfo
// (the delay stays inside 5..25 ms). Phaser: per channel four first-order all-pass stages  H(z) = (a + z^-1) / (1 + a z^-1),
// a = (t - 1) / (t + 1), t = tan(pi f / fs) (phase -90 degrees at f), swept exponentially  f = 894 Hz * exp(depth * ln(4000/200)/2 * lfo)  (200 Hz..4 kHz at depth 1);
// the feedback takes the chain output of the previous sample back to its input. Stereo: both channels run the same LFO with a quarter
// cycle (90 degrees) phase offset, so the left and the right wet signals are decorrelated. The LFO is a parabolic sine approximation
// (error below 0.1 %), the phaser coefficient is held for 16 frames.
//
// Mode change: the phaser weight w ramps linearly between 0 (chorus) and 1 (phaser) over kModeRampFrames frames; the wet signal is
// (1 - w) * chorus + w * phaser, so a live mode change cross-fades without a step. Both modes run only while their weight is non-zero;
// the chorus delay line is always written so it holds history when the chorus comes back. (A mode change that arrives as a structural
// change builds a new chain, which the Mixer cross-fades as for every insert; this ramp covers a set_params() on a running instance.)
//
// Smoothing: rate, depth, feedback and mix approach their targets by a one-pole per 64-frame block (about 10 ms) and are interpolated
// linearly per sample inside the block. Identity: mix exactly 0 (current and target) leaves the block untouched, bit for bit, and clears the
// state. Non-finite input samples are replaced by 0. Reset clears the state and puts the LFO at its start phase.
#include <array>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

enum class ChorusPhaserMode : std::uint8_t { kChorus = 0, kPhaser = 1 };

namespace cpmap {
constexpr float kModeThreshold = 0.5f;  // p > threshold -> phaser
constexpr float kMinRateHz = 0.1f, kMaxRateHz = 10.0f;
constexpr float kMaxFeedback = 0.8f;
float clamp01(float v) noexcept;  // NaN -> 0.5
ChorusPhaserMode mode(float norm) noexcept;
float rate_hz(float norm) noexcept;
float depth(float norm) noexcept;
float feedback(float norm) noexcept;
float mix(float norm) noexcept;
}  // namespace cpmap

class ChorusPhaserEffect final : public Effect {
 public:
  static constexpr std::uint32_t kLineSize = 2048;           // samples per channel (power of two), > 25 ms + interpolator margin
  static constexpr float kChorusCentreMs = 15.0f, kChorusSwingMs = 10.0f;  // delay 5..25 ms
  static constexpr float kPhaserMinHz = 200.0f, kPhaserMaxHz = 4000.0f;
  static constexpr std::uint32_t kStages = 4;
  static constexpr std::uint32_t kModeRampFrames = 480;      // 10 ms mode cross-fade
  static constexpr std::uint32_t kCoefHold = 16;             // frames the phaser coefficient is held
  static constexpr float kChannelPhaseOffset = 0.25f;        // right LFO = left LFO + 90 degrees

  ChorusPhaserEffect() noexcept;
  EffectType type() const noexcept override { return EffectType::kChorusPhaser; }
  std::uint32_t cost_units() const noexcept override { return effect_cost_units(EffectType::kChorusPhaser); }
  void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept override;
  void reset() noexcept VELORA_NONBLOCKING override;
  // Normalised insert parameters. Before the first process() they apply at once; on a running instance they are smoothed.
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override;
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;
  // False after prepare() with a sample rate other than kSampleRate or max_block_frames > kFxBlockFrames: the effect is then a pass-through.
  bool prepare_ok() const noexcept { return prepare_ok_; }

  // True when the effect would leave a block untouched (mix exactly 0, current and target).
  bool is_identity() const noexcept { return mix_cur_ == 0.0f && mix_tgt_ == 0.0f; }
  ChorusPhaserMode target_mode() const noexcept { return mode_tgt_; }
  float phaser_weight() const noexcept { return w_; }
  float lfo_phase() const noexcept { return static_cast<float>(phase_); }  // left channel, cycles in [0, 1)

 private:
  struct Lines {
    std::array<float, kLineSize> line{};
    std::array<double, kStages> z{};  // all-pass states
    double fb = 0.0;                  // previous chain output (feedback)
    double a = 0.0;                   // held all-pass coefficient
  };
  void clear_state() noexcept VELORA_NONBLOCKING;
  void snap() noexcept VELORA_NONBLOCKING;

  float rate_cur_ = 1.0f, rate_tgt_ = 1.0f, depth_cur_ = 0.5f, depth_tgt_ = 0.5f;
  float fb_cur_ = 0.0f, fb_tgt_ = 0.0f, mix_cur_ = 0.0f, mix_tgt_ = 0.0f;
  float w_ = 0.0f;  // phaser weight 0..1
  ChorusPhaserMode mode_tgt_ = ChorusPhaserMode::kChorus;
  double phase_ = 0.0;
  std::uint32_t pos_ = 0;
  std::uint32_t hold_ = 0;  // frames since the phaser coefficient was last computed (>= kCoefHold: due)
  bool started_ = false, active_ = false, ph_dirty_ = false, prepare_ok_ = true;
  std::array<Lines, 2> ch_{};
};

}  // namespace velora::core

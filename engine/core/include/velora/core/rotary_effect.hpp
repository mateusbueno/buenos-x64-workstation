#pragma once
// Rotary speaker insert effect (Story 2.9): the RotarySpeaker DSP of Story 2.6 (crossover, horn and drum rotors, Doppler, amplitude
// modulation, two virtual microphones) run as an insert on the stereo part bus.
//
// Mono/stereo handling (explicit, the one place): the rotary unit takes ONE mono signal and produces a stereo pair (the organ's built-in
// rotary receives the mono organ signal of the part). The insert receives the part bus (stereo), so it first sums it to mono,
// m = (L + R) / 2 (a mono source on both channels passes unchanged; a hard-panned source arrives at half level), runs the rotary unit on m
// and writes the stereo result back, scaled by the WIDTH parameter: out_mid = (l + r) / 2, out_side = (l - r) / 2 * width.
// The stereo image of the input is therefore replaced by the rotary's own spatialisation (the way a rotary speaker miked in a room works).
//
// Mapping (the one place; engine/core/README.md repeats it as a table):
//   insert_param_1 SPEED  0..1 -> stop for p < 0.25, slow for 0.25 <= p < 0.75, fast for p >= 0.75 (thresholds, no hysteresis;
//                                 NaN -> 0.5 -> slow). The registry rotary_speed enum order (stop, slow, fast) is kept.
//   insert_param_2 WIDTH  0..1 -> stereo width of the rotary output, 0 = mono, 0.5 = as designed (1.0), 1 = twice the side signal
//   insert_param_3..5     unused
//
// Speed changes ramp exactly like the organ's: on a running instance set_params() sets the rotary unit's target, and the horn and the
// drum approach it with their own acceleration and braking time constants (RotaryConfig, ORGAN.md); before the first process() the
// speed is applied at once (the rotors start settled at it). A structural change (the chain is rebuilt) starts a fresh instance, which
// the Mixer cross-fades as for every insert. Reset puts the rotors back at rest angle and the speed settled at the current target.
//
// State publishing: the insert does not publish rotary state (the organ's built-in rotary does, OrganEngine::rotary_status()); the speed
// of an insert is the structural parameter in the snapshot. There is exactly one rotor pair per rotary insert: an organ part keeps its
// built-in rotary and gets no second instance unless the user inserts one (live_instances() counts the inserts alive).
// Non-finite input samples are replaced by 0.
#include <array>
#include <atomic>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rotary_speaker.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

namespace rotmap {
constexpr float kStopBelow = 0.25f, kFastFrom = 0.75f;
float clamp01(float v) noexcept;  // NaN -> 0.5
RotarySpeed speed(float norm) noexcept;
float width(float norm) noexcept;  // 0..2
}  // namespace rotmap

class RotaryEffect final : public Effect {
 public:
  RotaryEffect() noexcept;
  ~RotaryEffect() override;
  EffectType type() const noexcept override { return EffectType::kRotary; }
  std::uint32_t cost_units() const noexcept override { return effect_cost_units(EffectType::kRotary); }
  void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept override;
  void reset() noexcept VELORA_NONBLOCKING override;
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override;
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;

  // False after prepare() with a sample rate other than kSampleRate or max_block_frames > kFxBlockFrames: the effect is then a pass-through.
  bool prepare_ok() const noexcept { return prepare_ok_; }
  const RotarySpeaker& rotary() const noexcept { return rot_; }
  RotarySpeed target_speed() const noexcept { return speed_tgt_; }
  float width() const noexcept { return width_cur_; }
  // Rotary insert instances alive in the process (tests: no automatic duplication next to the organ's built-in rotary).
  static std::int64_t live_instances() noexcept { return live_.load(std::memory_order_relaxed); }

 private:
  RotarySpeaker rot_;
  RotarySpeed speed_tgt_ = RotarySpeed::kSlow;
  float width_cur_ = 1.0f, width_tgt_ = 1.0f;
  bool started_ = false, prepare_ok_ = true;
  std::array<float, kFxBlockFrames> mono_{}, wl_{}, wr_{};
  static inline std::atomic<std::int64_t> live_{0};
};

}  // namespace velora::core

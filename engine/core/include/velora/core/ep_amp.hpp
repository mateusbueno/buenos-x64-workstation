#pragma once
// Electric-piano amp / drive insert effect (Story 2.9). Clean-room, public-fact design: a pre-gain into a soft-clipping waveshaper
// (a tanh-like odd-symmetric curve), 4x oversampled so the harmonics the clipper creates above the audio band are removed instead of
// folding back as aliases; then a tone tilt, a fixed amp voicing (high-pass and presence peak), an optional cabinet (high-pass and low-pass
// that band-limit like a speaker) and the output level.
//
// Mapping (the one place; engine/core/README.md repeats it as a table). Normalised insert params (registry insert_param_1..5):
//   insert_param_1 DRIVE    0..1 -> pre-gain 0..30 dB, linear in dB (0 = 0 dB, 0.5 = 15 dB)
//   insert_param_2 TONE     0..1 -> tilt +-6 dB around 1 kHz, linear (0.5 = flat): low band -t, high band +t
//   insert_param_3 PRESENCE 0..1 -> peak at 3 kHz (Q 0.9) 0..+9 dB, linear (0.5 = 4.5 dB)
//   insert_param_4 LEVEL    0..1 -> output level -12..+12 dB, linear (0.5 = 0 dB)
//   insert_param_5 CABINET  0..1 -> amount of the cabinet filter set blended in: out = x + c * (cab(x) - x)   (default 0 = off)
//
// Signal flow per channel: x -> pre-gain g -> [4x up -> shaper s -> 4x down] -> * 1/sqrt(g) -> high-pass 40 Hz (2nd order Butterworth,
// removes subsonic/DC) -> tone tilt (one-pole split at 1 kHz: y = gh x + (gl - gh) lp, gl = 10^(-t/20), gh = 10^(t/20), lp the one-pole low-pass of x) -> presence peak (RBJ) ->
// cabinet blend (high-pass 80 Hz and low-pass 5 kHz, both 2nd-order Butterworth) -> * level.
// Shaper: s(x) = x (27 + x^2) / (27 + 9 x^2) for |x| < 3 and sign(x) for |x| >= 3 (a Pade approximation of tanh, odd, bounded by 1, with a
// continuous slope at the knee); the 1/sqrt(g) after it keeps the loudness roughly constant as the drive rises (the small-signal gain
// is sqrt(g), the clipping ceiling is 1/sqrt(g)).
// Oversampling: two cascaded half-band stages (48 -> 96 -> 192 kHz and back), Kaiser-windowed half-band FIRs (47 taps at 96 kHz, 15 taps
// at 192 kHz), polyphase (only the non-zero taps are evaluated). The stages are linear phase; the whole oversampler delays the signal by
// kLatencyFrames samples (25.75, measured by the test). Cost: declared in effect_cost_units() (oversampling included).
//
// Smoothing: every target approaches by a one-pole per 64-frame block (about 10 ms); gains are interpolated linearly per sample, the
// presence coefficients are recomputed per block while the value moves. Non-finite input samples are replaced by 0. There is no identity
// setting (the shaper is always in the path); bypass is the slot flag, which is bit-exact. Reset clears all filter states.
#include <array>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

namespace epmap {
constexpr float kMaxDriveDb = 30.0f, kTiltDb = 6.0f, kMaxPresenceDb = 9.0f, kLevelRangeDb = 12.0f;
float clamp01(float v) noexcept;  // NaN -> 0.5
float drive_db(float norm) noexcept;
float tone_db(float norm) noexcept;
float presence_db(float norm) noexcept;
float level_db(float norm) noexcept;
float cabinet(float norm) noexcept;
// The waveshaper (also used by the tests as the non-oversampled reference).
float shape(float x) noexcept;
}  // namespace epmap

// A 2x half-band interpolator/decimator pair with 4P - 1 taps (P non-zero odd taps per side), block based so the compiler can vectorise the
// dot products. The state is the exact input history, so the result does not depend on how a signal is split into blocks. Fixed storage.
template <std::uint32_t P>
class HalfBand2x {
 public:
  static constexpr std::uint32_t kMaxIn = 256;  // the largest input block of up() / output block of down() ... see below
  HalfBand2x() noexcept;
  void clear() noexcept;
  // n input samples -> 2n output samples at twice the rate (out[2i]: interpolated, out[2i + 1]: the delayed input). n <= kMaxIn.
  void up(const float* in, std::uint32_t n, float* out) noexcept;
  // 2n input samples at the high rate -> n output samples at the low rate. n <= kMaxIn.
  void down(const float* in, std::uint32_t n, float* out) noexcept;
  static constexpr std::uint32_t kTaps = 4 * P - 1;

 private:
  static constexpr std::uint32_t kUpHist = 2 * P - 1, kEvenHist = P - 1, kOddHist = 2 * P - 1;
  std::array<float, P> c_{};  // interpolator weights 2 h(2s - 1)
  std::array<float, P> h_{};  // decimator weights h(2s - 1)
  std::array<float, kUpHist + kMaxIn> uw_{};        // interpolator input history + block
  std::array<float, kEvenHist + kMaxIn> de_{};      // decimator: even polyphase history + block
  std::array<float, kOddHist + kMaxIn> do_{};       // decimator: odd polyphase history + block
  std::array<float, kMaxIn> acc_{};
};

class EpAmpEffect final : public Effect {
 public:
  static constexpr std::uint32_t kOversampling = 4;
  // Group delay of the oversampler in frames at 48 kHz: 25.75 (two 47-tap and two 15-tap linear-phase half-band stages; the decimators keep the newest sample of each pair).
  static constexpr float kLatencyFrames = 25.75f;
  static constexpr std::uint32_t kMaxFrames = kFxBlockFrames;
  static constexpr float kHighPassHz = 40.0f, kTiltHz = 1000.0f, kPresenceHz = 3000.0f, kPresenceQ = 0.9f, kCabHighPassHz = 80.0f,
                         kCabLowPassHz = 5000.0f;

  EpAmpEffect() noexcept;
  EffectType type() const noexcept override { return EffectType::kEpAmp; }
  std::uint32_t cost_units() const noexcept override { return effect_cost_units(EffectType::kEpAmp); }
  void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept override;
  void reset() noexcept VELORA_NONBLOCKING override;
  // Normalised insert parameters. Before the first process() they apply at once; on a running instance they are smoothed.
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override;
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;
  // False after prepare() with a sample rate other than kSampleRate or max_block_frames > kFxBlockFrames: the effect is then a pass-through.
  bool prepare_ok() const noexcept { return prepare_ok_; }

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct BiquadState {
    double z1 = 0, z2 = 0;
    double run(const Biquad& k, double x) noexcept {
      const double y = k.b0 * x + z1;
      z1 = k.b1 * x - k.a1 * y + z2;
      z2 = k.b2 * x - k.a2 * y;
      return y;
    }
  };
  struct Channel {
    HalfBand2x<12> hb1;  // 48 <-> 96 kHz
    HalfBand2x<4> hb2;   // 96 <-> 192 kHz
    BiquadState hp, pres, cab_hp, cab_lp;
    double tilt_lp = 0.0;
  };
  void design_presence() noexcept VELORA_NONBLOCKING;
  void clear_state() noexcept VELORA_NONBLOCKING;
  void snap() noexcept VELORA_NONBLOCKING;

  // Physical targets / current values: drive dB, tone dB, presence dB, level dB, cabinet amount.
  std::array<float, 5> cur_{}, tgt_{};
  bool started_ = false;
  Biquad hp_, pres_, cab_hp_, cab_lp_;
  double tilt_a_ = 0.0;
  std::array<Channel, 2> ch_{};
  bool prepare_ok_ = true;
  std::array<float, 4 * kMaxFrames> sa_{}, sb_{};  // scratch: 1x / 2x / 4x signal (per channel, reused)
};

}  // namespace velora::core

#pragma once
// Per-part tone stage (Story 9.9 follow-up): the registry parameters part_brightness (402) and part_eq_low / mid / mid_freq / high (405..408), for EVERY
// engine (organ, sample, FM, VA). It sits on the part bus in the Mixer, before the insert chain: engine voices -> part bus -> PartTone -> inserts -> ...
//
//   part_eq_*        the Story 2.8 three-band EQ (EqEffect: low shelf 100 Hz, mid peak at part_eq_mid_freq, high shelf 4 kHz, +-15 dB), physical units.
//   part_brightness  0..100 %, 50 = neutral: a high shelf at 2.5 kHz (the same RBJ shelf, slope S = 1) whose gain is (value - 50) / 50 * 9 dB, i.e. -9 dB at
//                    0 %, +9 dB at 100 %. This is the engine-independent tone control; the engines keep their own timbre brightness (fm_brightness, va_brightness).
//
// Identity and smoothing: at the defaults (brightness 50, every EQ gain 0 dB) the stage is exactly transparent and the Mixer treats it as trivial routing, so
// existing renders are bit-identical. Targets are approached by a one-pole per 64-frame block (about 10 ms); coefficients are recomputed once per block while
// anything moves. Filters run in double with a transposed direct form II; non-finite input is replaced by 0, the state and the output are flushed to 0 below 1e-30 (-600 dB), so a
// NaN never sticks and a tail never ends in denormals (the EQ states are flushed the same way). Parameter values that come back through the 32-bit event scale
// (0 dB as ~1e-9 dB, 50 % as 50.000000012) are inside a 0.001 dB dead band and count as neutral. RT-safe: no allocation, no lock.
#include <array>
#include <cstdint>

#include "velora/core/eq.hpp"
#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

namespace tonemap {
constexpr float kBrightnessShelfHz = 2500.0f;
constexpr float kBrightnessMaxDb = 9.0f;
// part_brightness (percent, 0..100, NaN -> neutral) -> shelf gain in dB.
float brightness_db(float percent) noexcept;
}  // namespace tonemap

class PartTone {
 public:
  PartTone() noexcept;
  // Physical units as in the registry (brightness percent, dB, Hz); clamped to the registry ranges. instant = no smoothing (first generation / setup).
  void set(float brightness_percent, float low_db, float mid_db, float mid_hz, float high_db, bool instant) noexcept VELORA_NONBLOCKING;
  // True when the stage cannot change the signal (brightness 0 dB and every EQ band 0 dB, current and target).
  bool is_identity() const noexcept { return bright_cur_ == 0.0f && bright_tgt_ == 0.0f && eq_.is_identity(); }
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING;
  void reset() noexcept VELORA_NONBLOCKING;
  // |H| of the current response in dB at `hz` (tests). No allocation.
  double magnitude_db(double hz) const noexcept;

 private:
  struct Coef {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  void design() noexcept VELORA_NONBLOCKING;
  static void flush_tiny(float* const* ch, std::uint32_t frames) noexcept VELORA_NONBLOCKING;

  EqEffect eq_;
  float bright_cur_ = 0.0f, bright_tgt_ = 0.0f;  // shelf gain in dB
  bool bright_moving_ = false;
  bool bright_active_ = false;
  Coef coef_{};
  double z1_[2] = {0, 0}, z2_[2] = {0, 0};
};

}  // namespace velora::core

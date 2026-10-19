#pragma once
// Rotary speaker DSP unit (Story 2.6). Clean-room; the public facts and design values are in engine/core/ORGAN.md
// ("Rotary speaker"). A mono signal goes through a crossover into a horn band and a drum band. Each band feeds a rotor:
// a rotating radiator seen by two virtual microphones (left and right). Per rotor and per microphone the rotor angle drives
//   * amplitude modulation: a directivity gain  g = 1 - depth * (1 - cos(angle - mic)) / 2   (1 facing the mic, 1 - depth away),
//   * Doppler: a delay that follows the source distance,  d = d0 + k * (1 - cos(angle - mic)),  read with a 4-point Lagrange
//     interpolator from a fixed-size delay line (a moving source's pitch shift is the derivative of that delay),
//   * stereo spread: the two microphones sit at +-mic_half_angle, so the two channels are modulated out of phase.
// Each rotor has its own speed state: slow, fast or stop targets, reached by a first-order approach with its own acceleration and
// braking time constants (the horn is faster than the drum). Stop brakes to a standstill and leaves the rotor at the angle where it
// stopped; the amplitude, delay and pan then stay constant, so nothing steps.
// Rest angle: both rotors stop at the SAME angle (0, facing the front), so the two bands always meet there with equal delay: while
// stopping, a rotor that falls below kRestCreepHz keeps creeping at that speed until its angle reaches 0, then stops dead (a speed
// step of 0.25 Hz, i.e. a pitch step of 0.01 percent). It adds at most 4 s to the braking time.
//
// The unit is self-contained (no organ dependency) so Story 2.9 uses it as an insert (rotary_effect.hpp). It is one instance per part; fixed
// storage, no allocation, no locks: safe on the audio thread. Control calls (set_target, set_speed_immediate) and process()/skip()
// run on the same thread, in frame order, so a speed change lands on the exact sample where the caller makes it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace velora::core {

// Same order as the registry enum rotary_speed (stop, slow, fast).
enum class RotarySpeed : std::uint8_t { kStop = 0, kSlow = 1, kFast = 2 };

struct RotorSpec {
  double slow_hz = 0.0, fast_hz = 0.0;   // rotation speed at the SLOW and FAST settings
  double accel_tau_s = 0.0;              // time constant when the speed rises toward its target
  double brake_tau_s = 0.0;              // time constant when the speed falls toward its target (also used for STOP)
  float am_depth = 0.0f;                 // 0..1: the gain is 1 - am_depth facing away from a microphone
  double radius_m = 0.0;                 // distance swing of the radiator; sets the Doppler depth (0 = none)
};

struct RotaryConfig {
  // Design values, see ORGAN.md. horn: high band, fast and light; drum: low band, slow to start and to stop.
  RotorSpec horn{0.8, 6.8, 0.6, 1.2, 0.60f, 0.17};
  RotorSpec drum{0.67, 5.7, 2.0, 3.0, 0.35f, 0.12};
  double crossover_hz = 800.0;
  double mic_half_angle_rad = 1.0471975511965976;  // microphones at +-60 degrees from the front
};

class RotarySpeaker {
 public:
  static constexpr double kSampleRateHz = 48000.0;
  static constexpr double kSpeedOfSoundMps = 343.0;
  static constexpr std::uint32_t kDelayLine = 128;         // samples per rotor (power of two)
  static constexpr std::uint32_t kMinDelaySamples = 4;      // the smallest delay a rotor ever uses
  static constexpr std::uint32_t kTailFrames = 1024;        // frames of zeros that drain the filters and the delay lines
  static constexpr double kRestCreepHz = 0.25;             // while stopping, a rotor never goes slower than this until it reaches the rest angle
  static constexpr double kMaxRadiusM = 0.25;               // config clamp, derived below
  static constexpr double kStopOvershootHz = 0.2;           // STOP aims at -0.2 Hz and clamps at 0, so it ends in finite time
  static constexpr std::uint32_t kCostUnits = 24;           // arbiter cost per part (placeholder until measured on the board)

  RotarySpeaker() noexcept : RotarySpeaker(RotaryConfig{}) {}
  explicit RotarySpeaker(const RotaryConfig& cfg) noexcept;
  // The largest delay (samples) either rotor can ask of its line, plus the 3 samples the interpolator reads beyond it: always < kDelayLine.
  double max_delay_samples() const noexcept {
    return 3.0 + std::max(horn_.delay_base + 2.0 * horn_.delay_swing, drum_.delay_base + 2.0 * drum_.delay_swing);
  }

  // Speed target for both rotors; each rotor ramps with its own time constant from the next processed frame on.
  void set_target(RotarySpeed s) noexcept { target_ = s; }
  // Target and speeds at once, no ramp (setup, or a stage that is bypassed and inaudible).
  void set_speed_immediate(RotarySpeed s) noexcept;
  RotarySpeed target() const noexcept { return target_; }

  // Mono in, stereo out (overwritten). Advances the rotors frame by frame.
  void process(const float* in, float* out_left, float* out_right, std::uint32_t frames) noexcept;
  // Advances the rotors (speed and angle) exactly as process() would, without audio.
  void skip(std::uint32_t frames) noexcept;
  // Clears the crossover and delay-line signal state (the rotors keep their speed and angle).
  void clear_signal() noexcept;

  // --- introspection ---------------------------------------------------------------------------------------
  double horn_hz() const noexcept { return horn_.speed; }
  double drum_hz() const noexcept { return drum_.speed; }
  double horn_angle_cycles() const noexcept { return horn_.phase; }   // 0..1
  double drum_angle_cycles() const noexcept { return drum_.phase; }
  // Angles in cycles; any finite value wraps into [0, 1), a non-finite one becomes 0.
  void set_angles(double horn_cycles, double drum_cycles) noexcept {
    horn_.phase = wrap_cycles(horn_cycles);
    drum_.phase = wrap_cycles(drum_cycles);
  }
  const RotaryConfig& config() const noexcept { return cfg_; }
  static double target_hz(const RotorSpec& s, RotarySpeed t) noexcept {
    return t == RotarySpeed::kStop ? 0.0 : t == RotarySpeed::kSlow ? s.slow_hz : s.fast_hz;
  }

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) noexcept {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  struct Rotor {
    RotorSpec spec;
    double speed = 0.0, phase = 0.0;          // Hz, cycles in [0, 1)
    double accel = 0.0, brake = 0.0;          // per-sample approach coefficients
    double delay_base = 0.0, delay_swing = 0.0;  // samples: d = base + swing * (1 - cos)
    std::array<float, kDelayLine> line{};
    void step(RotarySpeed target) noexcept;
  };
  static double wrap_cycles(double c) noexcept {
    if (!std::isfinite(c)) return 0.0;
    const double w = c - std::floor(c);
    return w >= 1.0 ? 0.0 : w;
  }
  static void design_lowpass(Biquad& b, double fc) noexcept;
  static void design_highpass(Biquad& b, double fc) noexcept;
  float tap(const Rotor& r, double delay) const noexcept;

  RotaryConfig cfg_;
  RotarySpeed target_ = RotarySpeed::kSlow;
  Biquad lp_[2], hp_[2];  // Linkwitz-Riley 4th order = two cascaded Butterworth sections per band
  Rotor horn_, drum_;
  std::uint32_t pos_ = 0;
};

}  // namespace velora::core

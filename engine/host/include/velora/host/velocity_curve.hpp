// Velocity chain (Story 3.6, FR70 / FR71): key travel time -> keybed calibration -> the player's selected curve -> MIDI 2.0 16-bit velocity.
// This is the ONLY place a velocity curve is applied (once, in the keybed reader's VelocityMapper). The MCU, the engine core and the sample engine never apply one.
//
//   1. calibration (per keybed model, data in KeybedCalibration): the travel time range [fast_ns, slow_ns] and the shape of the time axis, which is
//      logarithmic: position s = ln(slow / dt) / ln(slow / fast) in [0, 1], 1 = fastest. (Key speed is 1 / time, and equal ratios of speed are equal steps
//      of loudness to the player, so the curves below see an axis in which a doubling of key speed is the same distance anywhere.)
//   2. curve (the sysd velocity_curve setting): c = curve(s), monotonic increasing, curve(0) = 0, curve(1) = 1:
//        Normal  c = s            (the reference)
//        Light   c = s^0.7        (a light touch gives a higher velocity than Normal at the same time)
//        Heavy   c = s^1.4        (a firm touch is needed: lower velocity than Normal at the same time)
//        Fixed   a constant velocity (kFixedVelocity7 = 100 unless set), independent of time
//   3. 16-bit velocity v = 512 + round((65535 - 512) * c): the slowest key gives MIDI velocity 1 (512), the fastest 127 (65535). Times faster than fast_ns /
//      slower than slow_ns are clamped. Because the curves are continuous and strictly monotonic in s, the 127 seven-bit velocities (the sample engine rounds the
//      16-bit value to 7 bits: (v * 127 + 32767) / 65535, never 0) are all reachable with no gap and no plateau.
// Time is injected (no clocks). Allocation-free; the curve and the fixed velocity may be changed from another thread (atomics, effective for the next note).
#pragma once

#include <atomic>
#include <cstdint>

#include "velora/host/keybed_calibration.hpp"

namespace velora::host {

enum class VelocityCurve : std::uint8_t { kNormal = 0, kLight = 1, kHeavy = 2, kFixed = 3 };

inline constexpr std::uint16_t kMinVelocity16 = 512;     // MIDI velocity 1 as a 16-bit value
inline constexpr std::uint16_t kMaxVelocity16 = 0xFFFF;  // MIDI velocity 127
inline constexpr std::uint8_t kFixedVelocity7 = 100;      // the Fixed curve's velocity (sysd has no field for it; a documented default)

// 7-bit MIDI velocity <-> 16-bit. 7 -> 16 is LINEAR, (v * 65535 + 63) / 127 (1 -> 516, 127 -> 65535), not the MIDI 2.0 bit-repeat upscaling; 16 -> 7 is the sample
// engine's own rounding, (v * 127 + 32767) / 65535 (never 0). The chain's slowest value is 512, which that rounding maps to 1.
std::uint16_t velocity7_to_16(std::uint8_t v7) noexcept;
std::uint8_t velocity16_to_7(std::uint16_t v16) noexcept;

// Step 1: position of `dt_ns` on the calibrated time axis, 1 = at or faster than fast_ns, 0 = at or slower than slow_ns.
double travel_position(const VelocityRange& range, std::uint64_t dt_ns) noexcept;
// Step 2: the curve over a position (Fixed is not a function of the position and returns it unchanged here).
double apply_velocity_curve(VelocityCurve curve, double position) noexcept;
// Steps 1 to 3.
std::uint16_t chain_velocity16(const VelocityRange& range, VelocityCurve curve, std::uint8_t fixed7, std::uint64_t dt_ns) noexcept;

// Composes "calibration range -> selected curve" behind the reader's VelocityMapper interface.
class CalibratedVelocityMapper final : public VelocityMapper {
 public:
  explicit CalibratedVelocityMapper(VelocityRange range = {}, VelocityCurve curve = VelocityCurve::kNormal) noexcept : range_(range), curve_(static_cast<std::uint8_t>(curve)) {}
  std::uint16_t velocity16(std::uint64_t dt_ns) const noexcept override {
    return chain_velocity16(range_, curve(), fixed7_.load(std::memory_order_relaxed), dt_ns);
  }
  // Thread-safe (any thread, lock-free): takes effect for the next Note On; a sounding note keeps its velocity.
  void set_curve(VelocityCurve c) noexcept { curve_.store(static_cast<std::uint8_t>(c), std::memory_order_relaxed); }
  VelocityCurve curve() const noexcept { return static_cast<VelocityCurve>(curve_.load(std::memory_order_relaxed)); }
  void set_fixed_velocity(std::uint8_t v7) noexcept { fixed7_.store(v7 < 1 ? 1 : v7 > 127 ? 127 : v7, std::memory_order_relaxed); }
  // Setup time, on the thread that calls velocity16() (a calibration swap): not atomic.
  void set_range(VelocityRange r) noexcept { range_ = r; }
  const VelocityRange& range() const noexcept { return range_; }

 private:
  VelocityRange range_;
  std::atomic<std::uint8_t> curve_;
  std::atomic<std::uint8_t> fixed7_{kFixedVelocity7};
};

}  // namespace velora::host

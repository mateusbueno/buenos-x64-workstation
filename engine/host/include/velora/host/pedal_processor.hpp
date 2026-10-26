// Pedal processing (Story 3.11, FR42, AR39): the pure and thread-safe pieces the panel reader uses to turn the raw 12-bit pedal values of the panel MCU
// (PEDAL_SUSTAIN 0x28, PEDAL_EXPRESSION 0x29, PEDAL_SWITCH 0x2A; mcu/common/MCU-ID-MAP.md section 6) into controllers. No I/O, no allocation.
//
//   * Direction (polarity). Every pedal reports "pressed = higher" after normalisation. sysd's PedalPolarity maps to PedalDirection: UNSPECIFIED and
//     NORMALLY_OPEN keep the raw direction (raw rises when the pedal is pressed; a switch reads closed on raw 1), NORMALLY_CLOSED inverts it (raw falls
//     when the pedal is pressed; a switch reads closed on raw 0). Boot-time detection is Story 3.8 and does not exist: an unspecified polarity IS the raw direction.
//   * Calibration. The calibrated raw range is [min, max] (sysd PedalCalibration: 12-bit, min < max, span >= kPedalMinSpan). min == max == 0, or any range
//     the engine cannot trust (min >= max, span below kPedalMinSpan, above 4095), is "uncalibrated" and means the full 0..4095. The pedal maps over that
//     range to the full continuous 32-bit range: position 0 -> 0, fully down -> 0xFFFFFFFF, halfway -> half.
//   * Deadband and hysteresis (documented constants below). The calibrated span has a dead zone at each end of db = max(kPedalDeadbandMin, span / 64)
//     counts: a position inside it reads exactly 0 (up end) or exactly 0xFFFFFFFF (down end), and the travel between the two zones is rescaled linearly,
//     so the mapping is clamped and monotonic and the fully-up / fully-down pedal reads exactly 0 / max whatever the ADC noise at rest. A move of less than
//     kPedalHysteresis counts from the last accepted raw value is ignored unless it enters or leaves an end state, so a resting pedal does not jitter.
#pragma once

#include <atomic>
#include <cstdint>

namespace velora::host {

enum class PedalId : std::uint8_t { kSustain = 0, kExpression = 1, kSwitch = 2 };
inline constexpr std::size_t kPedalCount = 3;

enum class PedalDirection : std::uint8_t { kNormal = 0, kInverted = 1 };

struct PedalCal {
  std::uint16_t min = 0, max = 0;  // raw 12-bit range; 0 / 0 = not calibrated
  PedalDirection direction = PedalDirection::kNormal;
  friend bool operator==(const PedalCal&, const PedalCal&) = default;
};

inline constexpr std::uint16_t kPedalRawMax = 4095;
inline constexpr std::uint16_t kPedalMinSpan = 256;        // a calibrated range narrower than this is refused (sysd) and ignored (engine)
inline constexpr std::uint16_t kPedalDeadbandMin = 4;      // dead zone at each end: max(kPedalDeadbandMin, span / kPedalDeadbandDivisor) counts
inline constexpr std::uint16_t kPedalDeadbandDivisor = 64;
inline constexpr std::uint16_t kPedalHysteresis = 2;       // raw counts

struct PedalRange {
  std::uint16_t lo = 0, hi = kPedalRawMax;
};
// The range the mapping uses for a calibration (the full range for an uncalibrated or untrustworthy one).
PedalRange effective_range(const PedalCal& cal) noexcept;
// The dead zone (counts) at each end of a range.
std::uint16_t pedal_deadband(const PedalRange& r) noexcept;
// Raw value -> 0..0xFFFFFFFF, pressed = higher. Pure; clamped and monotonic in the pressed direction.
std::uint32_t map_pedal(std::uint16_t raw, const PedalCal& cal) noexcept;
// A switch pedal: closed (pressed) for a raw value, with the direction applied.
inline bool switch_closed(std::uint16_t raw, PedalDirection d) noexcept { return (raw != 0) != (d == PedalDirection::kInverted); }

// One continuous pedal's mapping with hysteresis. Single thread (the panel input thread).
class PedalChannel {
 public:
  // Feeds a raw value. True when the mapped value changed (value() is the new one); the first value always counts.
  bool update(std::uint16_t raw, const PedalCal& cal) noexcept;
  // Re-maps the last raw value with a new calibration (config change at runtime). True when the value changed.
  bool remap(const PedalCal& cal) noexcept;
  bool has_value() const noexcept { return have_; }
  std::uint32_t value() const noexcept { return value_; }
  std::uint16_t last_raw() const noexcept { return raw_; }

 private:
  bool have_ = false;
  std::uint16_t raw_ = 0;
  std::uint32_t value_ = 0;
};

// The pedal configuration as the engine applies it: written by the control thread (sysd config callback), read by the panel input thread, lock-free.
class PedalSettings {
 public:
  void set(PedalId id, const PedalCal& cal) noexcept;
  PedalCal get(PedalId id) const noexcept;
  // Bumps when a value actually changed; the reader compares it with the one it applied.
  std::uint32_t version() const noexcept { return version_.load(std::memory_order_acquire); }

 private:
  std::atomic<std::uint64_t> packed_[kPedalCount] = {};
  std::atomic<std::uint32_t> version_{0};
};

// What the calibration routine measured.
struct PedalMeasurement {
  bool valid = false;           // false: the reader has not reported anything for this run yet
  std::uint16_t min = 0, max = 0;
  std::uint32_t samples = 0;
};

// Raw min / max collector for the calibration routine. begin() / cancel() / read() are for the control thread, observe() for the panel input thread (the
// single writer of the measurement); they meet through atomics only, so nothing locks on the input thread. A run is identified by a generation: the
// reader restarts its running min / max the first time it sees a new generation, and read() ignores anything of an older one.
class PedalCalibrationProbe {
 public:
  void begin(PedalId id) noexcept;
  void cancel() noexcept;
  bool active() const noexcept { return (state_.load(std::memory_order_acquire) & 0xFFu) != 0; }
  PedalMeasurement read() const noexcept;
  // Panel input thread: one raw value of pedal `id`.
  void observe(PedalId id, std::uint16_t raw) noexcept;

 private:
  // generation << 8 | (PedalId + 1), 0 in the low byte = idle: the run and the pedal it measures change together, in one atomic word (control thread writes it).
  std::atomic<std::uint32_t> state_{0};
  std::atomic<std::uint64_t> result_{0}; // gen (16) | samples (24) | min (12) | max (12), written by the reader only
  std::uint32_t local_gen_ = 0;          // reader-owned
  std::uint16_t lmin_ = 0, lmax_ = 0;
  std::uint32_t lsamples_ = 0;
};

}  // namespace velora::host

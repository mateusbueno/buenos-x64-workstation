#pragma once
// Story 7.1: the RT plane's single tempo and transport clock.
//
// Musical position is an exact integer rational, never an accumulated float: at 960 PPQN and 48 kHz one frame at tempo T (in tenths of a bpm) is
// T / 30000 ticks, so the position is kept in units of 1/30000 tick (`units`) and advances by `frames x tenths` per rendered stretch. A tempo
// change only changes the multiplier from that point on, so the position stays continuous. Every query is integer arithmetic.
#include <cstdint>

namespace velora::core {

class TransportClock {
 public:
  static constexpr std::uint32_t kPpqn = 960;
  static constexpr std::uint32_t kRateHz = 48000;  // == kSampleRate (block_renderer.hpp; checked in transport_clock.cpp)
  static constexpr std::int32_t kMinTenths = 200;   // 20.0 bpm
  static constexpr std::int32_t kMaxTenths = 3000;  // 300.0 bpm
  static constexpr std::int32_t kDefaultTenths = 1200;
  // units per tick: 60 s x 48000 Hz x 10 (tenths) / 960 PPQN
  static constexpr std::int64_t kUnitsPerTick = static_cast<std::int64_t>(60) * kRateHz * 10 / kPpqn;

  // bpm -> tenths: NaN/inf -> 120, otherwise clamped to 20..300 and rounded to 0.1 (same clamp as delaymap::tempo_bpm).
  static std::int32_t tenths_from_bpm(float bpm) noexcept;

  // Sets the tempo used from the current position on (clamped to 200..3000). Position stays continuous.
  void set_tempo_tenths(std::int32_t tenths) noexcept;
  std::int32_t tempo_tenths() const noexcept { return tenths_; }

  // Advances the block-start position by `frames` at the current tempo.
  void advance(std::uint32_t frames) noexcept;

  // Position of the block start, in 1/30000 tick units, and the frames advanced since reset.
  std::int64_t position_units() const noexcept { return units_; }
  std::uint64_t frames() const noexcept { return frames_; }

  // Whole ticks (floor) at `frame` frames after the block start (current tempo).
  std::int64_t ticks_at(std::uint32_t frame) const noexcept;
  // `tick` must stay within about +-1e12 (tick x 30000 must not overflow int64); a musical position never gets near this.
  // Frame offset from the block start (may be negative for past ticks) that is nearest to `tick` at the current tempo; ties round up.
  std::int64_t frame_of_tick(std::int64_t tick) const noexcept;

  // RT thread only (the members are not atomic). Restores position 0 and the default tempo.
  void reset() noexcept { units_ = 0; frames_ = 0; tenths_ = kDefaultTenths; }

 private:
  std::int64_t units_ = 0;
  std::uint64_t frames_ = 0;
  std::int32_t tenths_ = kDefaultTenths;
};

}  // namespace velora::core

#include "velora/core/transport_clock.hpp"

#include <cmath>

#include "velora/core/block_renderer.hpp"
#include "velora/core/delay.hpp"

namespace velora::core {

static_assert(TransportClock::kRateHz == kSampleRate, "the clock grid assumes the engine sample rate");
static_assert(60 * 48000 * 10 % 960 == 0, "units per tick must be an integer");

std::int32_t TransportClock::tenths_from_bpm(float bpm) noexcept {
  return static_cast<std::int32_t>(std::lround(static_cast<double>(delaymap::tempo_bpm(bpm)) * 10.0));
}

void TransportClock::set_tempo_tenths(std::int32_t tenths) noexcept {
  tenths_ = tenths < kMinTenths ? kMinTenths : (tenths > kMaxTenths ? kMaxTenths : tenths);
}

void TransportClock::advance(std::uint32_t frames) noexcept {
  units_ += static_cast<std::int64_t>(frames) * tenths_;
  frames_ += frames;
}

std::int64_t TransportClock::ticks_at(std::uint32_t frame) const noexcept {
  return (units_ + static_cast<std::int64_t>(frame) * tenths_) / kUnitsPerTick;  // units_ >= 0
}

std::int64_t TransportClock::frame_of_tick(std::int64_t tick) const noexcept {
  const std::int64_t delta = tick * kUnitsPerTick - units_;
  const std::int64_t t = tenths_;
  // round(delta / t), ties up, correct for negative delta
  const std::int64_t num = 2 * delta + t, den = 2 * t;
  std::int64_t q = num / den;
  if (num % den != 0 && ((num < 0) != (den < 0))) --q;
  return q;
}

}  // namespace velora::core

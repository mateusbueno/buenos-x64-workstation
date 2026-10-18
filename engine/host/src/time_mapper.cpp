#include "velora/host/time_mapper.hpp"

namespace velora::host {

MappedEvent TimeMapper::map(std::uint64_t event_ns, std::uint64_t block_start_ns) const noexcept {
  // Saturating add: a huge timestamp must not wrap around into the past.
  const std::uint64_t due =
      event_ns > ~0ull - cfg_.delay_ns ? ~0ull : event_ns + cfg_.delay_ns;
  if (due < block_start_ns) return {0, EventTiming::kLate};
  const std::uint64_t offset = due - block_start_ns;
  // Anything past the horizon (plus a block of rounding slack) is clamped without risking overflow.
  const std::uint64_t horizon_ns = frames_to_ns(cfg_.horizon_frames, cfg_.sample_rate);
  if (offset >= horizon_ns) return {0, EventTiming::kBeyondHorizon};
  const std::uint64_t frame = (offset * cfg_.sample_rate + 500'000'000ull) / 1'000'000'000ull;
  if (frame < cfg_.block_frames) return {static_cast<std::uint32_t>(frame), EventTiming::kInBlock};
  if (frame < cfg_.horizon_frames) return {static_cast<std::uint32_t>(frame), EventTiming::kDeferred};
  return {0, EventTiming::kBeyondHorizon};
}

}  // namespace velora::host

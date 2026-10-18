#pragma once

#include <cstdint>

#include "velora/core/block_renderer.hpp"
#include "velora/core/rt.hpp"

namespace velora::host {

// Fixed-scheduling-delay rule (same shape as the live engine: device time -> frame, fixed delay):
//
//   due      = event_ns + delay_ns             (when the event should become audible)
//   offset   = due - block_start_ns            (block_start_ns: when the block's first frame is heard)
//   frame    = round(offset * sample_rate / 1e9)
//
//   due < block_start_ns          -> kLate:          applied at frame 0 of this block (counted)
//   frame < block_frames          -> kInBlock:       applied at exactly `frame`
//   frame < horizon_frames        -> kDeferred:      frame >= block_frames; the renderer carries it
//                                                    to a later block (counted as informational)
//   frame >= horizon_frames       -> kBeyondHorizon: the caller must HOLD the event (and everything
//                                    behind it) until it comes within the horizon; `frame` is
//                                    meaningless. Holding, not clamping, keeps events in order.
//
// The mapping is monotone in event_ns, and beyond-horizon events are held in order, so events are
// never reordered.
struct TimeMapperConfig {
  std::uint32_t sample_rate = core::kSampleRate;
  std::uint32_t block_frames = core::kBlockFrames;
  std::uint64_t delay_ns = 3'000'000;      // fixed scheduling delay (3 ms, about 2 blocks of slack)
  // 100 ms at 48 kHz. Deferred events wait in the renderer's pending list, which holds at most
  // BlockRenderer::kPendingCapacity events (events, not blocks); overflow is reported by the renderer.
  std::uint32_t horizon_frames = 4800;
};

enum class EventTiming : std::uint8_t { kInBlock, kLate, kDeferred, kBeyondHorizon };

struct MappedEvent {
  std::uint32_t frame = 0;
  EventTiming timing = EventTiming::kInBlock;
};

class TimeMapper {
 public:
  // Sanitises the config: zero rate/block become the engine's, horizon is at least block + 1.
  explicit TimeMapper(TimeMapperConfig cfg = {}) noexcept : cfg_(cfg) {
    if (cfg_.sample_rate == 0) cfg_.sample_rate = core::kSampleRate;
    if (cfg_.block_frames == 0) cfg_.block_frames = core::kBlockFrames;
    if (cfg_.horizon_frames <= cfg_.block_frames) cfg_.horizon_frames = cfg_.block_frames + 1;
  }
  const TimeMapperConfig& config() const noexcept { return cfg_; }
  MappedEvent map(std::uint64_t event_ns, std::uint64_t block_start_ns) const noexcept VELORA_NONBLOCKING;

 private:
  TimeMapperConfig cfg_;
};

// Duration of `frames` at `sample_rate`, in nanoseconds (rounded to nearest).
constexpr std::uint64_t frames_to_ns(std::uint64_t frames, std::uint32_t sample_rate) noexcept {
  return (frames * 1'000'000'000ull + sample_rate / 2) / sample_rate;
}

}  // namespace velora::host

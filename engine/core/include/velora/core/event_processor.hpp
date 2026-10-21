#pragma once
// Story 7.2 (AR17): per-part event-processor framework in the serial phase of the block loop.
//
// Each part (0..7) has an EventProcessor slot that consumes the part's UMP input events (with in-block frame offset 0..kBlockFrames-1)
// and emits TimedUmp events with exact in-block frame positions in frame-sorted order before voice allocation. All methods run on the RT
// plane without dynamic allocation or locks (VELORA_NONBLOCKING). The same slot interface serves the Arpeggiator and a future sequencer.
#include <array>
#include <cstddef>
#include <cstdint>

#include "velora/core/rt.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

// Fixed-capacity event buffer for per-part serial-phase UMP processing. Maintains events in stable ascending target_frame order.
class EventBuffer {
 public:
  static constexpr std::size_t kCapacity = 256;

  constexpr void clear() noexcept VELORA_NONBLOCKING {
    count_ = 0;
    dropped_ = 0;
  }

  bool push(const TimedUmp& ev) noexcept VELORA_NONBLOCKING {
    if (count_ >= kCapacity) {
      ++dropped_;
      return false;
    }
    std::size_t pos = count_;
    while (pos > 0 && events_[pos - 1].target_frame > ev.target_frame) {
      events_[pos] = events_[pos - 1];
      --pos;
    }
    events_[pos] = ev;
    ++count_;
    return true;
  }

  bool push(const Ump& ump, std::uint32_t target_frame) noexcept VELORA_NONBLOCKING {
    return push(TimedUmp{ump, target_frame});
  }

  constexpr std::size_t size() const noexcept VELORA_NONBLOCKING { return count_; }
  constexpr bool empty() const noexcept VELORA_NONBLOCKING { return count_ == 0; }
  constexpr const TimedUmp& operator[](std::size_t i) const noexcept VELORA_NONBLOCKING { return events_[i]; }
  constexpr const TimedUmp* begin() const noexcept VELORA_NONBLOCKING { return events_.data(); }
  constexpr const TimedUmp* end() const noexcept VELORA_NONBLOCKING { return events_.data() + count_; }
  constexpr std::size_t dropped() const noexcept VELORA_NONBLOCKING { return dropped_; }

 private:
  std::array<TimedUmp, kCapacity> events_{};
  std::size_t count_ = 0;
  std::size_t dropped_ = 0;
};

// Abstract per-part event processor slot in the serial phase of BlockRenderer.
class EventProcessor {
 public:
  virtual ~EventProcessor() = default;

  // Clears held notes and internal state, returning the processor to its initial deterministic state.
  virtual void reset() noexcept VELORA_NONBLOCKING = 0;

  // Consumes a single input UMP event for this part at in-block offset `frame` (0..kBlockFrames-1).
  virtual void process_input(const Ump& ump, std::uint32_t frame, const TransportClock& clock,
                             EventBuffer& out) noexcept VELORA_NONBLOCKING = 0;

  // Advances the processor across a block of `frames` (e.g. kBlockFrames = 64) given the part's input events `in`
  // (sorted by target_frame in 0..frames-1) and the block's TransportClock, emitting UMP events with exact in-block
  // frame offsets into `out`.
  virtual void process_block(const EventBuffer& in, const TransportClock& clock, std::uint32_t frames,
                             EventBuffer& out) noexcept VELORA_NONBLOCKING {
    (void)frames;
    for (std::size_t i = 0; i < in.size(); ++i) {
      process_input(in[i].ump, in[i].target_frame, clock, out);
    }
  }
};

}  // namespace velora::core

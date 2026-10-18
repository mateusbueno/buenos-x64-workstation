#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/core/spsc_queue.hpp"
#include "velora/core/ump.hpp"
#include "velora/ports/midi_input.hpp"

namespace velora::host {

// A normalised UMP note event stamped with host time (ns) when it arrived at the adapter.
struct StampedUmp {
  core::Ump ump{};
  std::uint64_t host_ns = 0;
};

// MIDI thread (single producer) -> audio thread (single consumer).
using HostEventQueue = core::SpscQueue<StampedUmp, 1024>;

// Scale a 7-bit MIDI 1.0 velocity to 16 bits (bit-repeat, 127 -> 65535, 0 -> 0).
constexpr std::uint32_t scale_velocity_7_to_16(std::uint32_t v) noexcept {
  v &= 0x7F;
  return v == 0 ? 0 : ((v << 9) | (v << 2) | (v >> 5));
}

// Words in a UMP packet of the given message type (0..15).
constexpr std::size_t ump_packet_words(std::uint32_t message_type) noexcept {
  switch (message_type) {
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
    case 0xB: case 0xC: return 3;
    case 0x5: case 0xD: case 0xE: case 0xF: return 4;
    default: return 1;  // 0x0, 0x1, 0x2, 0x6, 0x7
  }
}

// Normalise the first packet of `words` to a MIDI 2.0 channel-voice note on/off.
// Always sets `consumed` (>=1 when count>0). Returns true and fills `out` for note on/off; false
// for any other message. Velocity-0 policy: a MIDI 1.0 (type 2) note-on with velocity 0 is a
// note-off (MIDI 1.0 convention); a MIDI 2.0 (type 4) note-on with velocity 0 is passed through
// UNCHANGED as a note-on (MIDI 2.0 has real note-offs; the engine treats velocity 0 as minimum).
bool normalise_packet(const std::uint32_t* words, std::size_t count, std::size_t& consumed,
                      core::Ump& out) noexcept;

struct MidiIngestCounters {
  std::atomic<std::uint64_t> notes{0};          // note on/off events queued
  std::atomic<std::uint64_t> ignored{0};        // non-note messages (or truncated packets)
  std::atomic<std::uint64_t> queue_full{0};     // dropped because the audio side is behind
};

// UmpSink that normalises, stamps and queues events. Runs on the MIDI adapter thread.
class MidiIngest final : public ports::UmpSink {
 public:
  explicit MidiIngest(HostEventQueue& q) noexcept : queue_(q) {}
  void on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept override;
  const MidiIngestCounters& counters() const noexcept { return counters_; }
  std::uint64_t last_stamp_ns() const noexcept { return last_stamp_ns_.load(std::memory_order_relaxed); }

 private:
  HostEventQueue& queue_;
  MidiIngestCounters counters_;
  std::atomic<std::uint64_t> last_stamp_ns_{0};
};

}  // namespace velora::host

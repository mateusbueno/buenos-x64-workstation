#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "velora/core/param_events.hpp"
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

// Lock-free queue for 64-bit SysEx UMP packets (MIDI input thread -> control thread).
using SysexPacketQueue = core::SpscQueue<core::Ump, 4096>;

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

// Normalise the first packet of `words` to a MIDI 2.0 channel-voice note on/off, a control change the
// parameter registry maps (ParamDef::cc) to a performance parameter UMP (channel = part), the sustain pedal, or any other
// control change as the MIDI 2.0 control change itself (Story 5.2: the sample engine's CC crossfades read them).
// Always sets `consumed` (>=1 when count>0). Returns true and fills `out` for those; false for any other message. Velocity-0 policy: a MIDI 1.0 (type 2) note-on with velocity 0 is a
// note-off (MIDI 1.0 convention); a MIDI 2.0 (type 4) note-on with velocity 0 is passed through
// UNCHANGED as a note-on (MIDI 2.0 has real note-offs; the engine treats velocity 0 as minimum).
bool normalise_packet(const std::uint32_t* words, std::size_t count, std::size_t& consumed,
                      core::Ump& out) noexcept;

// Slots of the event queue that only notes and the sustain pedal may use (controller floods stop short of them).
inline constexpr std::size_t kNoteHeadroom = 128;

struct MidiIngestCounters {
  std::atomic<std::uint64_t> notes{0};          // note on/off events queued
  std::atomic<std::uint64_t> params{0};         // mapped control changes queued as parameter UMPs
  std::atomic<std::uint64_t> controllers{0};    // control changes the registry does not map (not the pedal), queued as MIDI 2.0 control changes (Story 5.2)
  std::atomic<std::uint64_t> controllers_dropped{0};  // controllers (and mapped parameter events) shed because the queue had only note headroom left
  std::atomic<std::uint64_t> sysex_packets{0};  // 64-bit SysEx UMP packets queued for control thread
  std::atomic<std::uint64_t> sysex_dropped{0};  // SysEx packets dropped because sysex queue was full
  std::atomic<std::uint64_t> ignored{0};        // messages that are not notes, controllers, pedal or sysex (or truncated packets)
  std::atomic<std::uint64_t> queue_full{0};     // dropped because the audio side is behind
};

// UmpSink that normalises, stamps and queues events. Runs on the MIDI adapter thread.
class MidiIngest final : public ports::UmpSink {
 public:
  explicit MidiIngest(HostEventQueue& q, SysexPacketQueue* sysex_q = nullptr) noexcept
      : queue_(q), sysex_queue_(sysex_q) {}
  void on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept override;
  const MidiIngestCounters& counters() const noexcept { return counters_; }
  std::uint64_t last_stamp_ns() const noexcept { return last_stamp_ns_.load(std::memory_order_relaxed); }

  void set_sysex_queue(SysexPacketQueue* q) noexcept { sysex_queue_ = q; }
  SysexPacketQueue* sysex_queue() const noexcept { return sysex_queue_; }

 private:
  HostEventQueue& queue_;
  SysexPacketQueue* sysex_queue_{nullptr};
  MidiIngestCounters counters_;
  std::atomic<std::uint64_t> last_stamp_ns_{0};
};

}  // namespace velora::host

#pragma once
// Key contact events (Story 2.5): the vendor per-note `contact_k` controller, the format the Epic 3 keybed reader emits.
//
//   MIDI 2.0 assignable per-note controller (message type 4, status 0x1), group 0
//     word0: [31:28]=4 | [27:24]=0 | [23:20]=1 | [19:16]=channel | [15:8]=note (0..127) | [7:0]=contact index k (1..255 on the wire)
//     word1: 0xFFFFFFFF = contact k closed, 0x00000000 = contact k open; any value >= 0x80000000 is read as closed
//
// One event per contact transition, timestamped like every other UMP (its in-block frame is the moment the contact changed).
// Contact 1 is the first to close when a key goes down and the last to open when it comes up; a keybed with 2 or 3
// contacts per key uses k = 1..2 or 1..3 (the wire accepts 1..8) (the organ's contact groups, see engine/core/ORGAN.md). The organ accepts
// k = 1..OrganEngine::contact_count(); other indices are ignored and counted. The per-note controller is MIDI 2.0's own mechanism
// for vendor values attached to one note, so no new message type is introduced. The channel is the organ part, as for Note On.
#include <cstdint>

#include "velora/core/ump.hpp"

namespace velora::core {

// Per-note controller indices 1..kMaxWireContact are contact_k; any other index (0, or above) is some other per-note controller
// and is not ours (the block loop counts it as an ignored UMP). The organ serves 1..OrganEngine::contact_count().
constexpr std::uint32_t kMaxWireContact = 8;

struct ContactEvent {
  std::uint32_t channel = 0;
  std::uint32_t note = 0;     // 0..127 (a wire value above 127 is refused by decode_contact_event)
  std::uint32_t contact = 0;  // k as sent; 0 and out-of-range values are the engine's to refuse
  bool closed = false;
};

// A contact above 255 does not fit the index byte (it would alias onto another contact): the result is the zero Ump, which no
// decoder accepts.
constexpr Ump make_contact_event(std::uint32_t group, std::uint32_t channel, std::uint32_t note, std::uint32_t contact,
                                 bool closed) noexcept {
  if (contact > 255) return Ump{};
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | ((group & 0xF) << 24) | (Ump::kStatusAssignablePerNoteController << 20) |
                 ((channel & 0xF) << 16) | ((note & 0xFF) << 8) | (contact & 0xFF),
             closed ? 0xFFFFFFFFu : 0u};
}

// True when `ump` is a per-note controller on group 0 (the only group the core serves). `out.note` is the 7-bit note;
// a note field above 127 yields false (such an event is counted as dropped by the block loop, never aliased onto another key).
constexpr bool decode_contact_event(const Ump& ump, ContactEvent& out) noexcept {
  if (ump.group() != 0 || !ump.is_assignable_per_note_controller()) return false;
  const std::uint32_t note = (ump.word0 >> 8) & 0xFFu;
  if (note > 127u) return false;
  out.channel = ump.channel();
  out.note = note;
  out.contact = ump.word0 & 0xFFu;
  if (out.contact < 1 || out.contact > kMaxWireContact) return false;
  out.closed = ump.word1 >= 0x80000000u;
  return true;
}

}  // namespace velora::core

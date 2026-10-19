#pragma once

#include <cstdint>

namespace velora::core {

// MIDI 2.0 Universal MIDI Packet, 64-bit channel voice message (message type 0x4).
// This is the ONLY event type the core accepts; there is no MIDI 1.0 byte path.
//   word0: [31:28] mt=4 | [27:24] group | [23:20] status | [19:16] channel | [15:8] note | [7:0] attr type
//   word1: [31:16] velocity (16 bit) | [15:0] attribute
struct Ump {
  static constexpr std::uint32_t kMessageTypeChannelVoice2 = 0x4;
  static constexpr std::uint32_t kStatusNoteOff = 0x8;
  static constexpr std::uint32_t kStatusNoteOn = 0x9;
  static constexpr std::uint32_t kStatusControlChange = 0xB;
  static constexpr std::uint32_t kControllerSustain = 64;

  std::uint32_t word0 = 0;
  std::uint32_t word1 = 0;

  constexpr std::uint32_t message_type() const noexcept { return word0 >> 28; }
  constexpr std::uint32_t group() const noexcept { return (word0 >> 24) & 0xF; }
  constexpr std::uint32_t status() const noexcept { return (word0 >> 20) & 0xF; }
  constexpr std::uint32_t channel() const noexcept { return (word0 >> 16) & 0xF; }
  constexpr std::uint32_t note() const noexcept { return (word0 >> 8) & 0x7F; }
  constexpr std::uint32_t velocity16() const noexcept { return word1 >> 16; }

  // MIDI 2.0 control change: word0 [15:8] = controller index, word1 = 32-bit value.
  constexpr std::uint32_t controller_index() const noexcept { return (word0 >> 8) & 0x7F; }
  constexpr std::uint32_t controller_value32() const noexcept { return word1; }
  constexpr bool is_control_change() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusControlChange;
  }
  constexpr bool is_note_on() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusNoteOn;
  }
  constexpr bool is_note_off() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusNoteOff;
  }
};

constexpr Ump make_note_on(std::uint32_t group, std::uint32_t channel, std::uint32_t note,
                           std::uint32_t velocity16) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusNoteOn << 20) | ((channel & 0xF) << 16) | ((note & 0x7F) << 8),
             (velocity16 & 0xFFFF) << 16};
}

constexpr Ump make_note_off(std::uint32_t group, std::uint32_t channel, std::uint32_t note,
                            std::uint32_t velocity16 = 0) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusNoteOff << 20) | ((channel & 0xF) << 16) | ((note & 0x7F) << 8),
             (velocity16 & 0xFFFF) << 16};
}

constexpr Ump make_control_change(std::uint32_t group, std::uint32_t channel, std::uint32_t index,
                                  std::uint32_t value32) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusControlChange << 20) | ((channel & 0xF) << 16) | ((index & 0x7F) << 8),
             value32};
}

// A UMP with the frame (relative to the start of the next rendered block) at which it applies.
struct TimedUmp {
  Ump ump{};
  std::uint32_t target_frame = 0;
};

}  // namespace velora::core

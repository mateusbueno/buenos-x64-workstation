#pragma once

#include <cstdint>

namespace velora::core {

// MIDI 2.0 Universal MIDI Packet, 64-bit channel voice message (message type 0x4).
// This is the ONLY event type the core accepts; there is no MIDI 1.0 byte path.
//   word0: [31:28] mt=4 | [27:24] group | [23:20] status | [19:16] channel | [15:8] note | [7:0] attr type
//   word1: [31:16] velocity (16 bit) | [15:0] attribute
struct Ump {
  static constexpr std::uint32_t kMessageTypeSystemRealTime = 0x1;
  static constexpr std::uint32_t kMessageTypeData64 = 0x3;
  static constexpr std::uint32_t kMessageTypeChannelVoice2 = 0x4;
  static constexpr std::uint32_t kStatusNoteOff = 0x8;
  static constexpr std::uint32_t kStatusNoteOn = 0x9;
  static constexpr std::uint32_t kStatusAssignableController = 0x3;  // MIDI 2.0 assignable controller (NRPN)
  static constexpr std::uint32_t kStatusAssignablePerNoteController = 0x1;  // MIDI 2.0 assignable per-note controller
  static constexpr std::uint32_t kStatusControlChange = 0xB;
  static constexpr std::uint32_t kControllerSustain = 64;

  // SysEx7 Status (Form) bits: word0 [23:20]
  static constexpr std::uint32_t kSysex7FormComplete = 0x0;
  static constexpr std::uint32_t kSysex7FormStart = 0x1;
  static constexpr std::uint32_t kSysex7FormContinue = 0x2;
  static constexpr std::uint32_t kSysex7FormEnd = 0x3;

  // System Real Time status bytes (Message Type 0x1)
  static constexpr std::uint32_t kStatusTimingClock = 0xF8;
  static constexpr std::uint32_t kStatusStart = 0xFA;
  static constexpr std::uint32_t kStatusContinue = 0xFB;
  static constexpr std::uint32_t kStatusStop = 0xFC;
  static constexpr std::uint32_t kStatusActiveSensing = 0xFE;
  static constexpr std::uint32_t kStatusReset = 0xFF;

  std::uint32_t word0 = 0;
  std::uint32_t word1 = 0;

  constexpr std::uint32_t message_type() const noexcept { return word0 >> 28; }
  constexpr std::uint32_t group() const noexcept { return (word0 >> 24) & 0xF; }
  constexpr std::uint32_t status() const noexcept { return (word0 >> 20) & 0xF; }
  constexpr std::uint32_t channel() const noexcept { return (word0 >> 16) & 0xF; }
  constexpr std::uint32_t note() const noexcept { return (word0 >> 8) & 0x7F; }
  constexpr std::uint32_t velocity16() const noexcept { return word1 >> 16; }

  // System Real Time / Common status byte: word0 [23:16]
  constexpr std::uint32_t status_byte() const noexcept { return (word0 >> 16) & 0xFF; }
  constexpr bool is_system_realtime() const noexcept {
    return message_type() == kMessageTypeSystemRealTime && status_byte() >= 0xF8;
  }
  constexpr bool is_timing_clock() const noexcept {
    return message_type() == kMessageTypeSystemRealTime && status_byte() == kStatusTimingClock;
  }
  constexpr bool is_start() const noexcept {
    return message_type() == kMessageTypeSystemRealTime && status_byte() == kStatusStart;
  }
  constexpr bool is_continue() const noexcept {
    return message_type() == kMessageTypeSystemRealTime && status_byte() == kStatusContinue;
  }
  constexpr bool is_stop() const noexcept {
    return message_type() == kMessageTypeSystemRealTime && status_byte() == kStatusStop;
  }

  // MIDI 2.0 control change: word0 [15:8] = controller index, word1 = 32-bit value.
  constexpr std::uint32_t controller_index() const noexcept { return (word0 >> 8) & 0x7F; }
  constexpr std::uint32_t controller_value32() const noexcept { return word1; }
  constexpr bool is_control_change() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusControlChange;
  }
  constexpr bool is_assignable_controller() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusAssignableController;
  }
  // MIDI 2.0 assignable per-note controller (status 0x1): word0 [15:8] = note (8 bits on the wire), [7:0] = controller index.
  constexpr bool is_assignable_per_note_controller() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusAssignablePerNoteController;
  }
  constexpr bool is_note_on() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusNoteOn;
  }
  constexpr bool is_note_off() const noexcept {
    return message_type() == kMessageTypeChannelVoice2 && status() == kStatusNoteOff;
  }

  // SysEx7 (Message Type 0x3) accessors
  constexpr bool is_sysex7() const noexcept {
    return message_type() == kMessageTypeData64;
  }
  constexpr std::uint32_t sysex7_form() const noexcept {
    return (word0 >> 20) & 0xF;
  }
  constexpr std::uint32_t sysex7_byte_count() const noexcept {
    return (word0 >> 16) & 0xF;
  }
  constexpr std::uint8_t sysex7_byte(std::size_t idx) const noexcept {
    switch (idx) {
      case 0: return static_cast<std::uint8_t>((word0 >> 8) & 0xFF);
      case 1: return static_cast<std::uint8_t>(word0 & 0xFF);
      case 2: return static_cast<std::uint8_t>((word1 >> 24) & 0xFF);
      case 3: return static_cast<std::uint8_t>((word1 >> 16) & 0xFF);
      case 4: return static_cast<std::uint8_t>((word1 >> 8) & 0xFF);
      case 5: return static_cast<std::uint8_t>(word1 & 0xFF);
      default: return 0;
    }
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

constexpr Ump make_timing_clock(std::uint32_t group = 0) noexcept {
  return Ump{(Ump::kMessageTypeSystemRealTime << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusTimingClock << 16),
             0};
}

constexpr Ump make_start(std::uint32_t group = 0) noexcept {
  return Ump{(Ump::kMessageTypeSystemRealTime << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusStart << 16),
             0};
}

constexpr Ump make_continue(std::uint32_t group = 0) noexcept {
  return Ump{(Ump::kMessageTypeSystemRealTime << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusContinue << 16),
             0};
}

constexpr Ump make_stop(std::uint32_t group = 0) noexcept {
  return Ump{(Ump::kMessageTypeSystemRealTime << 28) | ((group & 0xF) << 24) |
                 (Ump::kStatusStop << 16),
             0};
}

constexpr Ump make_sysex7(std::uint32_t group, std::uint32_t form,
                          const std::uint8_t* bytes, std::size_t num_bytes) noexcept {
  const std::uint32_t count = static_cast<std::uint32_t>(num_bytes > 6 ? 6 : num_bytes);
  std::uint32_t w0 = (Ump::kMessageTypeData64 << 28) | ((group & 0xF) << 24) |
                     ((form & 0xF) << 20) | ((count & 0xF) << 16);
  if (count >= 1) w0 |= (static_cast<std::uint32_t>(bytes[0]) << 8);
  if (count >= 2) w0 |= static_cast<std::uint32_t>(bytes[1]);

  std::uint32_t w1 = 0;
  if (count >= 3) w1 |= (static_cast<std::uint32_t>(bytes[2]) << 24);
  if (count >= 4) w1 |= (static_cast<std::uint32_t>(bytes[3]) << 16);
  if (count >= 5) w1 |= (static_cast<std::uint32_t>(bytes[4]) << 8);
  if (count >= 6) w1 |= static_cast<std::uint32_t>(bytes[5]);

  return Ump{w0, w1};
}

// A UMP with the frame (relative to the start of the next rendered block) at which it applies.
struct TimedUmp {
  Ump ump{};
  std::uint32_t target_frame = 0;
};

}  // namespace velora::core

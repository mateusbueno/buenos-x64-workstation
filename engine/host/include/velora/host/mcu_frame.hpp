// C++ wrapper over the portable MCU frame protocol library (mcu/common) for the SoC side (Stories 3.2, 3.5, 3.10).
// Control / I-O thread code: it allocates (std::vector). The C library underneath never allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "mcu/mcu_frame.h"
#include "velora/host/mcu_ids.hpp"

namespace velora::host::mcu {

inline constexpr std::uint8_t kProtocolVersion = MCU_PROTOCOL_VERSION;
inline constexpr std::size_t kMaxPayload = MCU_MAX_PAYLOAD;
static_assert(mcu_ids::kMapProtocolVersion == MCU_PROTOCOL_VERSION, "ids.json protocol_version must match mcu_frame.h");

enum class FrameType : std::uint8_t {
  kHello = MCU_TYPE_HELLO,
  kKeyContact = MCU_TYPE_KEY_CONTACT,
  kAftertouch = MCU_TYPE_AFTERTOUCH,
  kControl = MCU_TYPE_CONTROL,
  kButton = MCU_TYPE_BUTTON,
  kLedSet = MCU_TYPE_LED_SET,
  kTimeSync = MCU_TYPE_TIME_SYNC,
  kEnterBootloader = MCU_TYPE_ENTER_BOOTLOADER,
};

// Same values as mcu_status (negative = error).
enum class Error : int {
  kOk = MCU_OK,
  kBufferTooSmall = MCU_ERR_BUFFER_TOO_SMALL,
  kPayloadTooLong = MCU_ERR_PAYLOAD_TOO_LONG,
  kUnknownType = MCU_ERR_UNKNOWN_TYPE,
  kBadLength = MCU_ERR_BAD_LENGTH,
  kBadValue = MCU_ERR_BAD_VALUE,
  kNull = MCU_ERR_NULL,
};

struct Frame {
  std::uint8_t version = kProtocolVersion;
  std::uint8_t type = 0;  // raw type byte; the encoder and decoder reject unknown types (counted)
  std::uint16_t seq = 0;
  std::uint32_t tick = 0;
  std::vector<std::uint8_t> payload;
  friend bool operator==(const Frame&, const Frame&) = default;
};

// Typed messages (payload layouts in mcu_frame.h and MCU-ID-MAP.md).
struct Hello { std::uint16_t fw_version; std::uint8_t protocol_version; std::uint8_t board_id; std::uint8_t keybed_contacts; bool aftertouch = false; friend bool operator==(const Hello&, const Hello&) = default; };
struct KeyContact { std::uint8_t key; std::uint8_t contact; bool closed; friend bool operator==(const KeyContact&, const KeyContact&) = default; };
struct Aftertouch { std::uint8_t target; std::uint16_t value; friend bool operator==(const Aftertouch&, const Aftertouch&) = default; };  // target 0xFF = channel
struct Control { std::uint8_t id; std::uint16_t value; friend bool operator==(const Control&, const Control&) = default; };  // encoders: int16 delta in two's complement
struct Button { std::uint8_t id; bool pressed; friend bool operator==(const Button&, const Button&) = default; };
struct LedSet { std::uint8_t id; std::uint8_t brightness; std::uint8_t arg; friend bool operator==(const LedSet&, const LedSet&) = default; };
struct TimeSync { bool reply; std::uint32_t token; std::uint32_t tick; friend bool operator==(const TimeSync&, const TimeSync&) = default; };
struct EnterBootloader { friend bool operator==(const EnterBootloader&, const EnterBootloader&) = default; };
using Message = std::variant<Hello, KeyContact, Aftertouch, Control, Button, LedSet, TimeSync, EnterBootloader>;

inline std::int16_t control_delta(const Control& c) { return static_cast<std::int16_t>(c.value); }
// Encoder step delta, clamped to the documented -127..127.
inline Control make_delta(std::uint8_t id, std::int16_t delta) {
  if (delta > 127) delta = 127;
  if (delta < -127) delta = -127;
  return Control{id, static_cast<std::uint16_t>(delta)};
}

// Builds a frame from a typed message. Returns nullopt when a field is out of its documented range (board_id, contacts,
// control value for its ID, unknown control/button/LED ID, ...).
std::optional<Frame> make_frame(const Message& m, std::uint16_t seq, std::uint32_t tick,
                                std::uint8_t version = kProtocolVersion);
// Parses a frame's payload into a typed message (nullopt on unknown type / wrong length / bad value).
std::optional<Message> parse_message(const Frame& f);

// Encodes into a fresh byte vector; on failure returns an empty optional and sets *err when given.
std::optional<std::vector<std::uint8_t>> encode(const Frame& f, Error* err = nullptr);

struct DecoderCounters {
  std::uint32_t frames_ok = 0, crc_errors = 0, framing_errors = 0, resyncs = 0, dropped_bytes = 0,
                version_mismatches = 0, bad_lengths = 0, unknown_types = 0, seq_gaps = 0, seq_lost = 0,
                overflow_bytes = 0, seq_reorders = 0;
  std::uint64_t bytes_in = 0;
};

enum class Event { kFrame, kCrcError, kFramingError, kVersionRejected, kBadLength, kUnknownType };

// One decoder event. For kFrame and the intact-but-rejected events (kVersionRejected, kBadLength, kUnknownType)
// version, type and seq describe the frame so the SoC can log it; they are 0 for kCrcError and kFramingError.
struct EventInfo {
  Event event = Event::kFrame;
  std::uint8_t version = 0;
  std::uint8_t type = 0;
  std::uint16_t seq = 0;
};

// Streaming decoder: feed any chunking of bytes, get the delivered frames. Never delivers a corrupt frame.
class StreamDecoder {
 public:
  explicit StreamDecoder(std::uint8_t current_version = kProtocolVersion);
  // Appends delivered frames to *frames (may be null) and every event to *events (may be null).
  // feed(nullptr, len > 0) is a no-op.
  void feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>* frames,
            std::vector<EventInfo>* events = nullptr);
  // See mcu_decoder_flush_stale(): abandons a candidate frame stalled on a bogus length and rescans (call after an
  // idle gap). Repeats until nothing is stalled.
  void flush_stale(std::vector<Frame>* frames, std::vector<EventInfo>* events = nullptr);
  std::vector<Frame> feed(const std::vector<std::uint8_t>& bytes);
  DecoderCounters counters() const;
  std::uint16_t last_seq_lost() const { return dec_.last_seq_lost; }  // frames lost before the latest delivered frame
  void reset_stream();

 private:
  mcu_decoder dec_;
};

// SoC-side helper for Stories 3.2/3.5: numbers frames with a per-link seq and encodes them.
class FrameWriter {
 public:
  explicit FrameWriter(std::uint16_t first_seq = 0) : seq_(first_seq) {}
  std::vector<std::uint8_t> write(const Message& m, std::uint32_t tick);  // empty on invalid message
  std::uint16_t next_seq() const { return seq_; }

 private:
  std::uint16_t seq_;
};

// Loopback: a writer wired to a decoder (the "simulated MCU" for tests and the Mac virtual panel).
class Loopback {
 public:
  explicit Loopback(std::uint16_t first_seq = 0) : writer_(first_seq) {}
  // Encodes one message, decodes the wire bytes and returns the delivered messages.
  std::vector<Message> send(const Message& m, std::uint32_t tick);
  std::vector<Message> send_bytes(const std::vector<std::uint8_t>& wire);
  StreamDecoder& decoder() { return decoder_; }
  FrameWriter& writer() { return writer_; }

 private:
  FrameWriter writer_;
  StreamDecoder decoder_;
};

}  // namespace velora::host::mcu

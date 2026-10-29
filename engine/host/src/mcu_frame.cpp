#include "velora/host/mcu_frame.hpp"

#include <cstring>
#include <type_traits>

namespace velora::host::mcu {

namespace {
Frame from_c(const mcu_frame& c) {
  Frame f;
  f.version = c.version;
  f.type = c.type;
  f.seq = c.seq;
  f.tick = c.tick;
  f.payload.assign(c.payload, c.payload + c.length);
  return f;
}

bool to_c(const Frame& f, mcu_frame* c) {
  if (f.payload.size() > MCU_MAX_PAYLOAD) return false;
  mcu_frame_init(c, f.type, f.seq, f.tick);
  c->version = f.version;
  c->length = static_cast<std::uint8_t>(f.payload.size());
  if (!f.payload.empty()) std::memcpy(c->payload, f.payload.data(), f.payload.size());
  return true;
}
}  // namespace

std::optional<Frame> make_frame(const Message& m, std::uint16_t seq, std::uint32_t tick, std::uint8_t version) {
  mcu_frame c;
  mcu_frame_init(&c, 0, seq, tick);
  int r = MCU_ERR_UNKNOWN_TYPE;
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Hello>) {
          if ((v.keybed_contacts & MCU_KEYBED_AFTERTOUCH_FLAG) != 0u) {
            r = MCU_ERR_BAD_VALUE;
            return;
          }
          const std::uint8_t contacts_byte =
              static_cast<std::uint8_t>(v.keybed_contacts | (v.aftertouch ? MCU_KEYBED_AFTERTOUCH_FLAG : 0u));
          mcu_hello x{v.fw_version, v.protocol_version, v.board_id, contacts_byte};
          r = mcu_pack_hello(&c, &x);
        } else if constexpr (std::is_same_v<T, KeyContact>) {
          mcu_key_contact x{v.key, v.contact, static_cast<std::uint8_t>(v.closed ? 1 : 0)};
          r = mcu_pack_key_contact(&c, &x);
        } else if constexpr (std::is_same_v<T, Aftertouch>) {
          mcu_aftertouch x{v.target, v.value};
          r = mcu_pack_aftertouch(&c, &x);
        } else if constexpr (std::is_same_v<T, Control>) {
          mcu_control x{v.id, v.value};
          r = mcu_pack_control(&c, &x);
        } else if constexpr (std::is_same_v<T, Button>) {
          mcu_button x{v.id, static_cast<std::uint8_t>(v.pressed ? 1 : 0)};
          r = mcu_pack_button(&c, &x);
        } else if constexpr (std::is_same_v<T, LedSet>) {
          mcu_led_set x{v.id, v.brightness, v.arg};
          r = mcu_pack_led_set(&c, &x);
        } else if constexpr (std::is_same_v<T, TimeSync>) {
          mcu_time_sync x{static_cast<std::uint8_t>(v.reply ? 1 : 0), v.token, v.tick};
          r = mcu_pack_time_sync(&c, &x);
        } else {
          r = mcu_pack_enter_bootloader(&c);
        }
      },
      m);
  if (r != MCU_OK) return std::nullopt;
  c.version = version;
  c.seq = seq;
  c.tick = tick;
  return from_c(c);
}

std::optional<Message> parse_message(const Frame& f) {
  mcu_frame c;
  if (!to_c(f, &c)) return std::nullopt;
  switch (f.type) {
    case MCU_TYPE_HELLO: {
      mcu_hello x;
      if (mcu_unpack_hello(&c, &x) != MCU_OK) return std::nullopt;
      return Message{Hello{x.fw_version, x.protocol_version, x.board_id,
                           static_cast<std::uint8_t>(x.keybed_contacts & MCU_KEYBED_CONTACTS_MASK),
                           (x.keybed_contacts & MCU_KEYBED_AFTERTOUCH_FLAG) != 0u}};
    }
    case MCU_TYPE_KEY_CONTACT: {
      mcu_key_contact x;
      if (mcu_unpack_key_contact(&c, &x) != MCU_OK) return std::nullopt;
      return Message{KeyContact{x.key, x.contact, x.closed != 0}};
    }
    case MCU_TYPE_AFTERTOUCH: {
      mcu_aftertouch x;
      if (mcu_unpack_aftertouch(&c, &x) != MCU_OK) return std::nullopt;
      return Message{Aftertouch{x.target, x.value}};
    }
    case MCU_TYPE_CONTROL: {
      mcu_control x;
      if (mcu_unpack_control(&c, &x) != MCU_OK) return std::nullopt;
      return Message{Control{x.id, x.value}};
    }
    case MCU_TYPE_BUTTON: {
      mcu_button x;
      if (mcu_unpack_button(&c, &x) != MCU_OK) return std::nullopt;
      return Message{Button{x.id, x.pressed != 0}};
    }
    case MCU_TYPE_LED_SET: {
      mcu_led_set x;
      if (mcu_unpack_led_set(&c, &x) != MCU_OK) return std::nullopt;
      return Message{LedSet{x.id, x.brightness, x.arg}};
    }
    case MCU_TYPE_TIME_SYNC: {
      mcu_time_sync x;
      if (mcu_unpack_time_sync(&c, &x) != MCU_OK) return std::nullopt;
      return Message{TimeSync{x.reply != 0, x.token, x.tick}};
    }
    case MCU_TYPE_ENTER_BOOTLOADER:
      if (mcu_unpack_enter_bootloader(&c) != MCU_OK) return std::nullopt;
      return Message{EnterBootloader{}};
    default:
      return std::nullopt;
  }
}

std::optional<std::vector<std::uint8_t>> encode(const Frame& f, Error* err) {
  mcu_frame c;
  if (!to_c(f, &c)) {
    if (err) *err = Error::kPayloadTooLong;
    return std::nullopt;
  }
  std::vector<std::uint8_t> out(MCU_MAX_FRAME);
  const int n = mcu_encode(&c, out.data(), out.size());
  if (n < 0) {
    if (err) *err = static_cast<Error>(n);
    return std::nullopt;
  }
  out.resize(static_cast<std::size_t>(n));
  if (err) *err = Error::kOk;
  return out;
}

StreamDecoder::StreamDecoder(std::uint8_t current_version) { mcu_decoder_init(&dec_, current_version); }

namespace {
struct Ctx {
  std::vector<Frame>* frames;
  std::vector<EventInfo>* events;
};
void on_event(void* user, mcu_event ev, const mcu_frame* fr) {
  auto* c = static_cast<Ctx*>(user);
  EventInfo info;
  switch (ev) {
    case MCU_EV_FRAME: info.event = Event::kFrame; break;
    case MCU_EV_CRC_ERROR: info.event = Event::kCrcError; break;
    case MCU_EV_FRAMING_ERROR: info.event = Event::kFramingError; break;
    case MCU_EV_VERSION_REJECTED: info.event = Event::kVersionRejected; break;
    case MCU_EV_BAD_LENGTH: info.event = Event::kBadLength; break;
    case MCU_EV_UNKNOWN_TYPE: info.event = Event::kUnknownType; break;
    case MCU_EV_NONE:
    default: return;
  }
  if (fr) {
    info.version = fr->version;
    info.type = fr->type;
    info.seq = fr->seq;
    if (ev == MCU_EV_FRAME && c->frames) c->frames->push_back(from_c(*fr));
  }
  if (c->events) c->events->push_back(info);
}
}  // namespace

void StreamDecoder::feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>* frames,
                         std::vector<EventInfo>* events) {
  if (data == nullptr || len == 0) return;
  Ctx ctx{frames, events};
  mcu_decoder_push(&dec_, data, len, on_event, &ctx);
}

void StreamDecoder::flush_stale(std::vector<Frame>* frames, std::vector<EventInfo>* events) {
  Ctx ctx{frames, events};
  mcu_frame f;
  while (mcu_decoder_pending(&dec_) > 0) {
    mcu_event ev = mcu_decoder_flush_stale(&dec_, &f);
    while (ev != MCU_EV_NONE) {
      const bool has_frame = ev == MCU_EV_FRAME || ev == MCU_EV_VERSION_REJECTED || ev == MCU_EV_BAD_LENGTH ||
                             ev == MCU_EV_UNKNOWN_TYPE;
      on_event(&ctx, ev, has_frame ? &f : nullptr);
      ev = mcu_decoder_poll(&dec_, &f);
    }
  }
}

std::vector<Frame> StreamDecoder::feed(const std::vector<std::uint8_t>& bytes) {
  std::vector<Frame> out;
  feed(bytes.data(), bytes.size(), &out);
  return out;
}

DecoderCounters StreamDecoder::counters() const {
  const mcu_decoder_counters& c = dec_.counters;
  DecoderCounters o;
  o.bytes_in = c.bytes_in;
  o.frames_ok = c.frames_ok;
  o.crc_errors = c.crc_errors;
  o.framing_errors = c.framing_errors;
  o.resyncs = c.resyncs;
  o.dropped_bytes = c.dropped_bytes;
  o.version_mismatches = c.version_mismatches;
  o.bad_lengths = c.bad_lengths;
  o.unknown_types = c.unknown_types;
  o.seq_gaps = c.seq_gaps;
  o.seq_lost = c.seq_lost;
  o.overflow_bytes = c.overflow_bytes;
  o.seq_reorders = c.seq_reorders;
  return o;
}

void StreamDecoder::reset_stream() { mcu_decoder_reset_stream(&dec_); }

std::vector<std::uint8_t> FrameWriter::write(const Message& m, std::uint32_t tick) {
  auto f = make_frame(m, seq_, tick);
  if (!f) return {};
  auto bytes = encode(*f);
  if (!bytes) return {};
  ++seq_;
  return *bytes;
}

std::vector<Message> Loopback::send(const Message& m, std::uint32_t tick) {
  return send_bytes(writer_.write(m, tick));
}

std::vector<Message> Loopback::send_bytes(const std::vector<std::uint8_t>& wire) {
  std::vector<Message> out;
  for (const Frame& f : decoder_.feed(wire)) {
    if (auto m = parse_message(f)) out.push_back(*m);
  }
  return out;
}

}  // namespace velora::host::mcu

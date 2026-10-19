// Unit tests of the portable MCU frame library (mcu/common): CRC, encoder, round trips, golden frames, the streaming
// decoder (chunking, noise, truncation, bad CRC, resync, seq gaps, version policy, length vs type).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "mcu/mcu_frame.h"
#include "mcu/mcu_ids.h"

static int g_fail = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes enc(const mcu_frame& f) {
  std::uint8_t buf[MCU_MAX_FRAME];
  const int n = mcu_encode_unchecked(&f, buf, sizeof buf);
  return n > 0 ? Bytes(buf, buf + n) : Bytes{};
}

struct Collected {
  std::vector<mcu_frame> frames;
  std::vector<mcu_event> events;
};
void collect_cb(void* user, mcu_event ev, const mcu_frame* f) {
  auto* c = static_cast<Collected*>(user);
  c->events.push_back(ev);
  if (ev == MCU_EV_FRAME && f) c->frames.push_back(*f);
}
Collected push(mcu_decoder& d, const Bytes& b) {
  Collected c;
  mcu_decoder_push(&d, b.data(), b.size(), collect_cb, &c);
  return c;
}
Collected push_bytewise(mcu_decoder& d, const Bytes& b) {
  Collected c;
  for (auto x : b) mcu_decoder_push(&d, &x, 1, collect_cb, &c);
  return c;
}
void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

mcu_frame make(std::uint8_t type, std::uint16_t seq, std::uint32_t tick, std::initializer_list<std::uint8_t> pl) {
  mcu_frame f;
  mcu_frame_init(&f, type, seq, tick);
  f.length = static_cast<std::uint8_t>(pl.size());
  std::size_t i = 0;
  for (auto b : pl) f.payload[i++] = b;
  return f;
}
bool same(const mcu_frame& a, const mcu_frame& b) {
  return a.version == b.version && a.type == b.type && a.seq == b.seq && a.tick == b.tick && a.length == b.length &&
         std::memcmp(a.payload, b.payload, a.length) == 0;
}

// Golden frames (computed independently with a reference CRC-16/CCITT-FALSE in Python).
struct Golden {
  const char* name;
  mcu_frame frame;
  Bytes wire;
};
std::vector<Golden> goldens() {
  return {
      {"HELLO", make(MCU_TYPE_HELLO, 0x0102, 0x11223344, {0x01, 0x02, 0x10, 2, 3}),
       {0xA5, 0x10, 0x01, 0x05, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11, 0x01, 0x02, 0x10, 0x02, 0x03, 0x5C, 0xA5}},
      {"KEY_CONTACT", make(MCU_TYPE_KEY_CONTACT, 1, 1000, {60, 1, 1}),
       {0xA5, 0x10, 0x02, 0x03, 0x01, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x3C, 0x01, 0x01, 0x8C, 0x9F}},
      {"AFTERTOUCH", make(MCU_TYPE_AFTERTOUCH, 0xFFFF, 0xFFFFFFFFu, {0xFF, 0x34, 0x12}),
       {0xA5, 0x10, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x34, 0x12, 0x6D, 0xB6}},
      {"CONTROL", make(MCU_TYPE_CONTROL, 300, 5, {16, 0xFF, 0x0F}),
       {0xA5, 0x10, 0x04, 0x03, 0x2C, 0x01, 0x05, 0x00, 0x00, 0x00, 0x10, 0xFF, 0x0F, 0xCA, 0xCE}},
      {"BUTTON", make(MCU_TYPE_BUTTON, 7, 8, {23, 1}),
       {0xA5, 0x10, 0x05, 0x02, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x17, 0x01, 0xBA, 0xC5}},
      {"LED_SET", make(MCU_TYPE_LED_SET, 9, 10, {16, 255, 128}),
       {0xA5, 0x10, 0x06, 0x03, 0x09, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x10, 0xFF, 0x80, 0x14, 0x13}},
      {"TIME_SYNC", make(MCU_TYPE_TIME_SYNC, 11, 12, {1, 0x78, 0x56, 0x34, 0x12, 0xEF, 0xBE, 0xAD, 0xDE}),
       {0xA5, 0x10, 0x07, 0x09, 0x0B, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x78, 0x56, 0x34, 0x12, 0xEF, 0xBE, 0xAD, 0xDE,
        0x14, 0x25}},
      {"ENTER_BOOTLOADER", make(MCU_TYPE_ENTER_BOOTLOADER, 0, 0, {0x42, 0x4F, 0x4F, 0x54}),
       {0xA5, 0x10, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x4F, 0x4F, 0x54, 0xBC, 0xC1}},
  };
}

// Tiny deterministic PRNG for the chunking tests.
struct Rng {
  std::uint64_t s;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 11);
  }
};

void test_crc() {
  CHECK(mcu_crc16(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0x29B1);  // CCITT-FALSE check value
  CHECK(mcu_crc16(nullptr, 0) == 0xFFFF);
  std::uint16_t c = 0xFFFF;
  for (char ch : std::string("123456789")) c = mcu_crc16_update(c, static_cast<std::uint8_t>(ch));
  CHECK(c == 0x29B1);
}

void test_golden() {
  for (const auto& g : goldens()) {
    const Bytes e = enc(g.frame);
    if (e != g.wire) std::printf("golden mismatch: %s\n", g.name);
    CHECK(e == g.wire);
    std::uint8_t buf[MCU_MAX_FRAME];
    CHECK(mcu_encode(&g.frame, buf, sizeof buf) == static_cast<int>(g.wire.size()));
    mcu_decoder d;
    mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
    const auto c = push(d, g.wire);
    CHECK(c.frames.size() == 1 && c.events.size() == 1 && same(c.frames[0], g.frame));
    CHECK(d.counters.crc_errors == 0 && d.counters.dropped_bytes == 0);
  }
}

void test_roundtrip_typed() {
  mcu_frame f;
  mcu_frame_init(&f, 0, 0, 0);
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  std::uint16_t seq = 0;
  auto rt = [&](const mcu_frame& in) {
    const Bytes w = enc(in);
    const auto c = push(d, w);
    CHECK(c.frames.size() == 1);
    return c.frames.empty() ? mcu_frame{} : c.frames[0];
  };
  for (int tickcase = 0; tickcase < 3; ++tickcase) {
    const std::uint32_t tick = tickcase == 0 ? 0u : tickcase == 1 ? 1u : 0xFFFFFFFFu;
    for (unsigned fw : {0u, 1u, 0xFFFFu}) {
      mcu_hello in{static_cast<std::uint16_t>(fw), 0x10, 2, 3}, out{};
      CHECK(mcu_pack_hello(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_hello(&r, &out) == MCU_OK && out.fw_version == in.fw_version && out.protocol_version == 0x10 &&
            out.board_id == 2 && out.keybed_contacts == 3);
    }
    {
      mcu_aftertouch in{0xFF, 0xFFFF}, out{};
      CHECK(mcu_pack_aftertouch(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_aftertouch(&r, &out) == MCU_OK && out.value == 0xFFFF);
    }
    for (unsigned closed : {0u, 1u}) {
      for (unsigned key : {0u, 72u, 255u}) {
        mcu_key_contact in{static_cast<std::uint8_t>(key), 2, static_cast<std::uint8_t>(closed)}, out{};
        CHECK(mcu_pack_key_contact(&f, &in) == MCU_OK);
        f.seq = seq++; f.tick = tick;
        const mcu_frame r = rt(f);
        CHECK(mcu_unpack_key_contact(&r, &out) == MCU_OK && out.key == key && out.contact == 2 && out.closed == closed);
      }
    }
    for (unsigned v : {0u, 1u, 0x0123u, 4095u}) {
      for (unsigned target : {0u, 72u, 0xFFu}) {
        mcu_aftertouch in{static_cast<std::uint8_t>(target), static_cast<std::uint16_t>(v)}, out{};
        CHECK(mcu_pack_aftertouch(&f, &in) == MCU_OK);
        f.seq = seq++; f.tick = tick;
        const mcu_frame r = rt(f);
        CHECK(mcu_unpack_aftertouch(&r, &out) == MCU_OK && out.target == target && out.value == v);
      }
      mcu_control in{MCU_CTRL_FADER_9, static_cast<std::uint16_t>(v)}, out{};
      CHECK(mcu_pack_control(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_control(&r, &out) == MCU_OK && out.id == MCU_CTRL_FADER_9 && out.value == v);
    }
    {  // negative encoder deltas survive as two's complement
      mcu_control in{MCU_CTRL_ENCODER_3, static_cast<std::uint16_t>(static_cast<std::int16_t>(-127))}, out{};
      CHECK(mcu_pack_control(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_control(&r, &out) == MCU_OK && static_cast<std::int16_t>(out.value) == -127);
    }
    for (unsigned p : {0u, 1u}) {
      mcu_button in{MCU_BTN_TOGGLE_PART_ALL_ASSIGN, static_cast<std::uint8_t>(p)}, out{};
      CHECK(mcu_pack_button(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_button(&r, &out) == MCU_OK && out.id == MCU_BTN_TOGGLE_PART_ALL_ASSIGN && out.pressed == p);
    }
    for (unsigned br : {0u, 128u, 255u}) {
      mcu_led_set in{MCU_LED_ENCODER_RING_8, static_cast<std::uint8_t>(br), 255}, out{};
      CHECK(mcu_pack_led_set(&f, &in) == MCU_OK);
      f.seq = seq++; f.tick = tick;
      const mcu_frame r = rt(f);
      CHECK(mcu_unpack_led_set(&r, &out) == MCU_OK && out.id == MCU_LED_ENCODER_RING_8 && out.brightness == br && out.arg == 255);
    }
    for (unsigned reply : {0u, 1u}) {
      for (std::uint32_t t : {0u, 1u, 0xFFFFFFFFu}) {
        mcu_time_sync in{static_cast<std::uint8_t>(reply), 0xDEADBEEFu, t}, out{};
        CHECK(mcu_pack_time_sync(&f, &in) == MCU_OK);
        f.seq = seq++; f.tick = tick;
        const mcu_frame r = rt(f);
        CHECK(mcu_unpack_time_sync(&r, &out) == MCU_OK && out.reply == reply && out.token == 0xDEADBEEFu && out.tick == t);
      }
    }
    CHECK(mcu_pack_enter_bootloader(&f) == MCU_OK);
    f.seq = seq++; f.tick = tick;
    const mcu_frame r = rt(f);
    CHECK(mcu_unpack_enter_bootloader(&r) == MCU_OK);
  }
  CHECK(d.counters.crc_errors == 0 && d.counters.seq_gaps == 0 && d.counters.version_mismatches == 0);
}

void test_pack_validation() {
  mcu_frame f;
  mcu_frame_init(&f, 0, 0, 0);
  mcu_key_contact kc{1, 1, 2};
  CHECK(mcu_pack_key_contact(&f, &kc) == MCU_ERR_BAD_VALUE);
  mcu_button b{1, 2};
  CHECK(mcu_pack_button(&f, &b) == MCU_ERR_BAD_VALUE);
  mcu_time_sync ts{2, 0, 0};
  CHECK(mcu_pack_time_sync(&f, &ts) == MCU_ERR_BAD_VALUE);
  CHECK(mcu_pack_button(nullptr, &b) == MCU_ERR_NULL);
  // unpack rejects wrong type, wrong length, bad values, bad magic
  mcu_frame g = make(MCU_TYPE_BUTTON, 0, 0, {1, 5});
  mcu_button ob;
  CHECK(mcu_unpack_button(&g, &ob) == MCU_ERR_BAD_VALUE);
  g = make(MCU_TYPE_BUTTON, 0, 0, {1});
  CHECK(mcu_unpack_button(&g, &ob) == MCU_ERR_BAD_LENGTH);
  g = make(MCU_TYPE_CONTROL, 0, 0, {1, 0, 0});
  CHECK(mcu_unpack_button(&g, &ob) == MCU_ERR_UNKNOWN_TYPE);
  g = make(MCU_TYPE_ENTER_BOOTLOADER, 0, 0, {1, 2, 3, 4});
  CHECK(mcu_unpack_enter_bootloader(&g) == MCU_ERR_BAD_VALUE);

  // pack keeps the caller's version, seq and tick
  mcu_frame_init(&f, 0, 1234, 0xCAFEF00Du);
  f.version = 0x21;
  mcu_button ok{MCU_BTN_EXIT, 1};
  CHECK(mcu_pack_button(&f, &ok) == MCU_OK && f.version == 0x21 && f.seq == 1234 && f.tick == 0xCAFEF00Du &&
        f.type == MCU_TYPE_BUTTON && f.length == 2);
  mcu_frame_init(&f, 0, 77, 88);
  CHECK(mcu_pack_enter_bootloader(&f) == MCU_OK && f.seq == 77 && f.tick == 88 && f.version == MCU_PROTOCOL_VERSION);

  // HELLO: board_id in {1,2}, keybed_contacts in {0,2,3}
  for (unsigned board = 0; board < 5; ++board) {
    for (unsigned contacts = 0; contacts < 5; ++contacts) {
      mcu_hello h{1, MCU_PROTOCOL_VERSION, static_cast<std::uint8_t>(board), static_cast<std::uint8_t>(contacts)};
      const bool want = (board == 1 || board == 2) && (contacts == 0 || contacts == 2 || contacts == 3);
      CHECK((mcu_pack_hello(&f, &h) == MCU_OK) == want);
    }
  }
  g = make(MCU_TYPE_HELLO, 0, 0, {1, 0, 0x10, 3, 2});  // board 3
  mcu_hello oh;
  CHECK(mcu_unpack_hello(&g, &oh) == MCU_ERR_BAD_VALUE);
  g = make(MCU_TYPE_HELLO, 0, 0, {1, 0, 0x10, 1, 1});  // 1 contact
  CHECK(mcu_unpack_hello(&g, &oh) == MCU_ERR_BAD_VALUE);

  // CONTROL ranges per kind: faders / wheels / pedals unsigned 12 bit, pedal switch 0..1, encoders -127..127, unknown IDs
  auto ctl = [&](unsigned id, unsigned value) {
    mcu_control c{static_cast<std::uint8_t>(id), static_cast<std::uint16_t>(value)};
    return mcu_pack_control(&f, &c);
  };
  CHECK(ctl(MCU_CTRL_FADER_1, 4095) == MCU_OK && ctl(MCU_CTRL_FADER_1, 4096) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(MCU_CTRL_WHEEL_PITCH, 0) == MCU_OK && ctl(MCU_CTRL_WHEEL_MOD, 0xFFFF) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(MCU_CTRL_PEDAL_SUSTAIN, 4095) == MCU_OK && ctl(MCU_CTRL_PEDAL_EXPRESSION, 4096) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(MCU_CTRL_PEDAL_SWITCH, 1) == MCU_OK && ctl(MCU_CTRL_PEDAL_SWITCH, 2) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(MCU_CTRL_ENCODER_1, 127) == MCU_OK && ctl(MCU_CTRL_ENCODER_1, 128) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(MCU_CTRL_VALUE_ENCODER, 0xFFFFu - 126u) == MCU_OK);                       // -127
  CHECK(ctl(MCU_CTRL_VALUE_ENCODER, 0xFFFFu - 127u) == MCU_ERR_BAD_VALUE);            // -128
  CHECK(ctl(MCU_CTRL_ENCODER_2, 0x8000) == MCU_ERR_BAD_VALUE);
  CHECK(ctl(0, 0) == MCU_ERR_BAD_VALUE && ctl(200, 0) == MCU_ERR_BAD_VALUE);
  g = make(MCU_TYPE_CONTROL, 0, 0, {MCU_CTRL_FADER_2, 0x00, 0x10});  // 4096
  mcu_control oc;
  CHECK(mcu_unpack_control(&g, &oc) == MCU_ERR_BAD_VALUE);
  // BUTTON / LED IDs must exist
  mcu_button ub{0, 1};
  CHECK(mcu_pack_button(&f, &ub) == MCU_ERR_BAD_VALUE);
  ub.id = 99;
  CHECK(mcu_pack_button(&f, &ub) == MCU_ERR_BAD_VALUE);
  mcu_led_set ul{0, 1, 1}, okl{MCU_LED_PART_SCENE_LED_1, 255, 255};
  CHECK(mcu_pack_led_set(&f, &ul) == MCU_ERR_BAD_VALUE && mcu_pack_led_set(&f, &okl) == MCU_OK);
  g = make(MCU_TYPE_LED_SET, 0, 0, {99, 1, 1});
  mcu_led_set ol;
  CHECK(mcu_unpack_led_set(&g, &ol) == MCU_ERR_BAD_VALUE);
}

void test_encoder_errors() {
  std::uint8_t buf[MCU_MAX_FRAME + 8];
  mcu_frame f = make(MCU_TYPE_BUTTON, 1, 2, {1, 1});
  CHECK(mcu_encode(&f, buf, 13) == MCU_ERR_BUFFER_TOO_SMALL);  // needs 14
  CHECK(mcu_encode(&f, buf, 14) == 14);
  CHECK(mcu_encode(&f, buf, 0) == MCU_ERR_BUFFER_TOO_SMALL);
  CHECK(mcu_encode(&f, nullptr, 100) == MCU_ERR_NULL);
  CHECK(mcu_encode(nullptr, buf, 100) == MCU_ERR_NULL);
  f.length = MCU_MAX_PAYLOAD + 1;
  CHECK(mcu_encode(&f, buf, sizeof buf) == MCU_ERR_PAYLOAD_TOO_LONG);
  CHECK(mcu_encode_unchecked(&f, buf, sizeof buf) == MCU_ERR_PAYLOAD_TOO_LONG);
  f.length = 3;  // BUTTON wants 2
  CHECK(mcu_encode(&f, buf, sizeof buf) == MCU_ERR_BAD_LENGTH);
  CHECK(mcu_encode_unchecked(&f, buf, sizeof buf) == 15);
  f = make(99, 1, 2, {});
  CHECK(mcu_encode(&f, buf, sizeof buf) == MCU_ERR_UNKNOWN_TYPE);
  // max payload (unchecked, unknown type) fits exactly MCU_MAX_FRAME
  f.length = MCU_MAX_PAYLOAD;
  CHECK(mcu_encode_unchecked(&f, buf, MCU_MAX_FRAME) == static_cast<int>(MCU_MAX_FRAME));
  CHECK(mcu_encode_unchecked(&f, buf, MCU_MAX_FRAME - 1) == MCU_ERR_BUFFER_TOO_SMALL);
  // no write past the stated capacity
  std::uint8_t guard[20];
  std::memset(guard, 0xEE, sizeof guard);
  f = make(MCU_TYPE_BUTTON, 1, 2, {1, 1});
  CHECK(mcu_encode(&f, guard, 14) == 14);
  CHECK(guard[14] == 0xEE && guard[19] == 0xEE);
}

void test_version_policy() {
  // current 1.0: only major 1 (N-1 = 0 is never valid), any minor.
  CHECK(mcu_version_accepted(0x10, 0x10) && mcu_version_accepted(0x1F, 0x10) && mcu_version_accepted(0x11, 0x10));
  CHECK(!mcu_version_accepted(0x00, 0x10) && !mcu_version_accepted(0x0F, 0x10));
  CHECK(!mcu_version_accepted(0x20, 0x10) && !mcu_version_accepted(0xF0, 0x10));
  // current 2.1: majors 2 and 1 accepted, 0 and 3 rejected (the N and N-1 boundaries).
  CHECK(mcu_version_accepted(0x21, 0x21) && mcu_version_accepted(0x20, 0x21) && mcu_version_accepted(0x2F, 0x21));
  CHECK(mcu_version_accepted(0x10, 0x21) && mcu_version_accepted(0x1F, 0x21));
  CHECK(!mcu_version_accepted(0x0F, 0x21) && !mcu_version_accepted(0x30, 0x21) && !mcu_version_accepted(0x3F, 0x21));
  CHECK(MCU_VERSION_MAJOR(MCU_PROTOCOL_VERSION) == 1 && MCU_VERSION_MINOR(MCU_PROTOCOL_VERSION) == 0);

  // Decoder: unknown major rejected with a typed event and a counter; the frame is consumed, the stream continues.
  mcu_decoder d;
  mcu_decoder_init(&d, 0x21);
  mcu_frame bad = make(MCU_TYPE_BUTTON, 1, 1, {1, 1});
  bad.version = 0x30;
  mcu_frame n1 = make(MCU_TYPE_BUTTON, 2, 1, {1, 1});
  n1.version = 0x10;  // N-1
  mcu_frame cur = make(MCU_TYPE_BUTTON, 3, 1, {1, 0});
  cur.version = 0x21;
  Bytes w;
  append(w, enc(bad));
  append(w, enc(n1));
  append(w, enc(cur));
  const auto c = push(d, w);
  CHECK(c.events.size() == 3 && c.events[0] == MCU_EV_VERSION_REJECTED && c.events[1] == MCU_EV_FRAME &&
        c.events[2] == MCU_EV_FRAME);
  CHECK(c.frames.size() == 2 && c.frames[0].version == 0x10 && c.frames[1].version == 0x21);
  CHECK(d.counters.version_mismatches == 1 && d.counters.crc_errors == 0 && d.counters.frames_ok == 2);
  // The rejected frame consumed seq 1, so 1, 2, 3 is gapless.
  CHECK(d.counters.seq_gaps == 0);
  // current version with major 0 is invalid and replaced by the library default
  mcu_decoder dz;
  mcu_decoder_init(&dz, 0x05);
  CHECK(dz.current_version == MCU_PROTOCOL_VERSION);
  mcu_decoder_init(&dz, 0x21);
  CHECK(dz.current_version == 0x21);
}

void test_length_vs_type() {
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes w;
  append(w, enc(make(MCU_TYPE_BUTTON, 0, 0, {1, 1, 1})));  // BUTTON must be 2 bytes
  append(w, enc(make(MCU_TYPE_HELLO, 1, 0, {})));          // HELLO must be 5
  append(w, enc(make(77, 2, 0, {1})));                      // unknown type
  append(w, enc(make(MCU_TYPE_BUTTON, 3, 0, {2, 1})));      // fine
  const auto c = push(d, w);
  CHECK(c.events.size() == 4 && c.events[0] == MCU_EV_BAD_LENGTH && c.events[1] == MCU_EV_BAD_LENGTH &&
        c.events[2] == MCU_EV_UNKNOWN_TYPE && c.events[3] == MCU_EV_FRAME);
  CHECK(c.frames.size() == 1 && d.counters.bad_lengths == 2 && d.counters.unknown_types == 1);
  CHECK(d.counters.frames_ok == 1 && d.counters.crc_errors == 0);
}

void test_max_payload_and_framing() {
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  // A max-size payload frame of an unknown type is intact: reported, never overflows.
  mcu_frame f = make(77, 0, 0, {});
  f.length = MCU_MAX_PAYLOAD;
  for (unsigned i = 0; i < MCU_MAX_PAYLOAD; ++i) f.payload[i] = static_cast<std::uint8_t>(i ^ 0xA5);
  auto c = push(d, enc(f));
  CHECK(c.events.size() == 1 && c.events[0] == MCU_EV_UNKNOWN_TYPE);
  // Length field above the maximum: framing error, resync onto the following valid frame.
  Bytes w = {0xA5, 0x10, 0x05, static_cast<std::uint8_t>(MCU_MAX_PAYLOAD + 1), 1, 2, 3};
  append(w, enc(make(MCU_TYPE_BUTTON, 5, 0, {4, 1})));
  c = push(d, w);
  CHECK(c.frames.size() == 1 && c.frames[0].seq == 5);
  CHECK(d.counters.framing_errors == 1);
}

void test_decoder_chunking() {
  Rng rng{0x1234567};
  Bytes stream;
  std::vector<mcu_frame> sent;
  for (unsigned i = 0; i < 300; ++i) {
    mcu_frame f = make(MCU_TYPE_CONTROL, static_cast<std::uint16_t>(i), i * 1000u,
                       {static_cast<std::uint8_t>(i % 23), static_cast<std::uint8_t>(i), 0xA5});
    sent.push_back(f);
    append(stream, enc(f));
  }
  // one byte at a time
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  auto c = push_bytewise(d, stream);
  CHECK(c.frames.size() == sent.size());
  for (std::size_t i = 0; i < sent.size() && i < c.frames.size(); ++i) CHECK(same(c.frames[i], sent[i]));
  CHECK(d.counters.frames_ok == 300 && d.counters.dropped_bytes == 0 && d.counters.seq_gaps == 0 &&
        d.counters.bytes_in == stream.size());
  // random chunking, many seeds
  for (unsigned seed = 1; seed <= 50; ++seed) {
    mcu_decoder d2;
    mcu_decoder_init(&d2, MCU_PROTOCOL_VERSION);
    Collected cc;
    rng.s = seed * 7919u;
    std::size_t pos = 0;
    while (pos < stream.size()) {
      std::size_t n = 1 + rng.next() % 40;
      if (n > stream.size() - pos) n = stream.size() - pos;
      mcu_decoder_push(&d2, stream.data() + pos, n, collect_cb, &cc);
      pos += n;
    }
    CHECK(cc.frames.size() == sent.size());
    bool ok = cc.frames.size() == sent.size();
    for (std::size_t i = 0; ok && i < sent.size(); ++i) ok = same(cc.frames[i], sent[i]);
    CHECK(ok);
  }
}

void test_noise_truncation_crc() {
  const mcu_frame a = make(MCU_TYPE_BUTTON, 0, 10, {1, 1});
  const mcu_frame b = make(MCU_TYPE_CONTROL, 1, 20, {2, 0x34, 0x12});
  const mcu_frame cfr = make(MCU_TYPE_LED_SET, 2, 30, {3, 4, 5});
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);

  // noise before and between frames (including 0xA5 noise that does not form a frame)
  Bytes w = {0x00, 0xFF, 0x13, 0xA5, 0xA5, 0xA5, 0x01};
  append(w, enc(a));
  append(w, Bytes{0x55, 0xAA, 0xA5});
  append(w, enc(b));
  append(w, Bytes(5, 0x00));
  append(w, enc(cfr));
  auto c = push(d, w);
  CHECK(c.frames.size() == 3 && same(c.frames[0], a) && same(c.frames[1], b) && same(c.frames[2], cfr));
  CHECK(d.counters.dropped_bytes > 0 && d.counters.seq_gaps == 0);

  // bad CRC: delivers nothing, counts, and the next frame still arrives
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes bad = enc(a);
  bad[bad.size() - 1] ^= 0x01;
  Bytes w2 = bad;
  append(w2, enc(b));
  c = push(d, w2);
  CHECK(c.frames.size() == 1 && same(c.frames[0], b));
  CHECK(d.counters.crc_errors == 1 && d.counters.frames_ok == 1);
  // every single-bit corruption of a frame is rejected (CRC-16 detects all single-bit errors)
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  const Bytes good = enc(b);
  unsigned delivered_bad = 0;
  for (std::size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes m = good;
      m[byte] ^= static_cast<std::uint8_t>(1u << bit);
      const auto r = push(d, m);
      for (const auto& fr : r.frames)
        if (!same(fr, b)) ++delivered_bad;
      // flush any waiting candidate with a clean frame so the next case starts clean
      append(m, Bytes());
      push(d, Bytes(MCU_MAX_FRAME, 0x00));
    }
  }
  CHECK(delivered_bad == 0);

  // truncated frame followed by a complete one: the complete one still arrives
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes tr = enc(a);
  tr.resize(tr.size() - 4);
  Bytes w3 = tr;
  append(w3, enc(b));
  append(w3, enc(cfr));
  c = push(d, w3);
  CHECK(c.frames.size() == 2 && same(c.frames[0], b) && same(c.frames[1], cfr));
  CHECK(d.counters.crc_errors >= 1);

  // false sync byte inside a payload does not break resync; a frame carrying 0xA5 in every field decodes fine
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  mcu_frame sync_heavy = make(MCU_TYPE_CONTROL, 0xA5A5, 0xA5A5A5A5u, {0xA5, 0xA5, 0xA5});
  Bytes w4;
  append(w4, enc(sync_heavy));
  mcu_frame sh2 = make(MCU_TYPE_TIME_SYNC, 0xA5A6, 1, {1, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5});
  append(w4, enc(sh2));
  c = push_bytewise(d, w4);
  CHECK(c.frames.size() == 2 && same(c.frames[0], sync_heavy) && same(c.frames[1], sh2));
  CHECK(d.counters.crc_errors == 0 && d.counters.dropped_bytes == 0);

  // a corrupted frame whose payload contains a whole valid frame: the inner frame is recovered
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  const Bytes inner = enc(a);
  mcu_frame outer = make(77, 9, 9, {});
  outer.length = static_cast<std::uint8_t>(inner.size());
  std::memcpy(outer.payload, inner.data(), inner.size());
  Bytes wo = enc(outer);
  wo[wo.size() - 1] ^= 0xFF;  // outer CRC broken
  c = push(d, wo);
  CHECK(c.frames.size() == 1 && same(c.frames[0], a) && d.counters.crc_errors == 1);

  // false header claiming a long payload swallows following real frames: all of them are recovered after the CRC check
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes w5 = {0xA5, 0x10, 0x05, 60};
  const mcu_frame f1 = make(MCU_TYPE_BUTTON, 0, 1, {1, 1}), f2 = make(MCU_TYPE_BUTTON, 1, 2, {1, 0}),
                  f3 = make(MCU_TYPE_BUTTON, 2, 3, {2, 1});
  append(w5, enc(f1));
  append(w5, enc(f2));
  append(w5, enc(f3));
  append(w5, Bytes(40, 0x00));  // a false header waits for its claimed length: the stream must go on for it to resolve
  c = push(d, w5);
  CHECK(c.frames.size() == 3 && same(c.frames[0], f1) && same(c.frames[1], f2) && same(c.frames[2], f3));
}

void test_seq() {
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  auto send = [&](std::uint16_t seq) {
    push(d, enc(make(MCU_TYPE_BUTTON, seq, 0, {1, 1})));
  };
  send(100);
  CHECK(d.counters.seq_gaps == 0);  // first frame sets the base
  send(101);
  CHECK(d.counters.seq_gaps == 0 && d.last_seq_lost == 0);
  send(105);  // 102,103,104 lost
  CHECK(d.counters.seq_gaps == 1 && d.counters.seq_lost == 3 && d.last_seq_lost == 3);
  send(106);
  CHECK(d.counters.seq_gaps == 1 && d.last_seq_lost == 0);
  // wrap: 65534, 65535, 0, 1 is gapless
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(65534); send(65535); send(0); send(1);
  CHECK(d.counters.seq_gaps == 0);
  // gap across the wrap: 65535 -> 2 loses 0 and 1
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(65535); send(2);
  CHECK(d.counters.seq_gaps == 1 && d.counters.seq_lost == 2 && d.last_seq_lost == 2);
  // a repeated or older seq is a duplicate/reorder, not a loss, and does not move the expectation
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(7); send(7);
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_lost == 0 && d.counters.seq_reorders == 1);
  send(8);  // still in order relative to 7
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 1 && d.last_seq == 8);
  send(5);  // older
  CHECK(d.counters.seq_reorders == 2 && d.counters.seq_gaps == 0 && d.last_seq == 8);
  send(9);
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 2);
  // boundary: delta 0x8000 is a gap, 0x8001 a reorder
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(0); send(static_cast<std::uint16_t>(1 + 0x8000));
  CHECK(d.counters.seq_gaps == 1 && d.counters.seq_lost == 0x8000);
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(0); send(static_cast<std::uint16_t>(1 + 0x8001));
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 1);
  // rejected frames (unknown type, bad length, unsupported major) still consume a seq: no spurious gap
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  {
    Bytes w;
    append(w, enc(make(MCU_TYPE_BUTTON, 10, 0, {1, 1})));
    append(w, enc(make(77, 11, 0, {1})));                // unknown type (a newer minor's addition)
    append(w, enc(make(MCU_TYPE_BUTTON, 12, 0, {1})));   // bad length
    mcu_frame v = make(MCU_TYPE_BUTTON, 13, 0, {1, 1});
    v.version = 0x30;                                      // unsupported major
    append(w, enc(v));
    append(w, enc(make(MCU_TYPE_BUTTON, 14, 0, {1, 0})));
    push(d, w);
    CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 0 && d.counters.frames_ok == 2);
    CHECK(d.counters.unknown_types == 1 && d.counters.bad_lengths == 1 && d.counters.version_mismatches == 1);
  }
  // a delivered HELLO resets the expectation (MCU reboot): no gap, no reorder
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  send(500); send(501);
  push(d, enc(make(MCU_TYPE_HELLO, 0, 0, {1, 0, 0x10, 1, 2})));
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 0 && d.last_seq == 0);
  send(1);
  CHECK(d.counters.seq_gaps == 0 && d.counters.seq_reorders == 0);
  // a rejected HELLO (bad length) does not reset: it is an ordinary seq-consuming frame (here a gap of 898)
  push(d, enc(make(MCU_TYPE_HELLO, 900, 0, {1})));
  CHECK(d.counters.bad_lengths == 1 && d.last_seq == 900 && d.counters.seq_gaps == 1 && d.counters.seq_lost == 898);
  // frames lost to corruption show up as a gap on the next delivered frame
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes w = enc(make(MCU_TYPE_BUTTON, 0, 0, {1, 1}));
  Bytes lost = enc(make(MCU_TYPE_BUTTON, 1, 0, {1, 1}));
  lost[5] ^= 0x40;
  append(w, lost);
  append(w, enc(make(MCU_TYPE_BUTTON, 2, 0, {1, 1})));
  push(d, w);
  CHECK(d.counters.crc_errors == 1 && d.counters.seq_gaps == 1 && d.counters.seq_lost == 1);
  // reset_stream forgets the sequence base but keeps counters
  mcu_decoder_reset_stream(&d);
  send(500);
  CHECK(d.counters.seq_gaps == 1);
}

void test_back_to_back_and_poll_contract() {
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes w;
  for (unsigned i = 0; i < 100; ++i) append(w, enc(make(MCU_TYPE_BUTTON, static_cast<std::uint16_t>(i), i, {1, static_cast<std::uint8_t>(i & 1)})));
  auto c = push(d, w);
  CHECK(c.frames.size() == 100 && d.counters.dropped_bytes == 0);

  // A caller that ignores the poll contract never causes an out-of-bounds write: bytes are refused and counted.
  mcu_decoder d2;
  mcu_decoder_init(&d2, MCU_PROTOCOL_VERSION);
  mcu_frame out;
  for (unsigned i = 0; i < 4 * MCU_MAX_FRAME; ++i) (void)mcu_decoder_feed(&d2, 0xA5, &out);
  CHECK(d2.n <= sizeof d2.buf);
  // poll with nothing buffered is a no-op
  mcu_decoder d3;
  mcu_decoder_init(&d3, MCU_PROTOCOL_VERSION);
  CHECK(mcu_decoder_poll(&d3, &out) == MCU_EV_NONE);
}

void test_flush_stale() {
  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  // A false header claiming 60 payload bytes, then a real short frame: the decoder stalls waiting for the claimed length.
  Bytes w = {0xA5, 0x10, 0x05, 60};
  const mcu_frame real = make(MCU_TYPE_BUTTON, 3, 4, {2, 1});
  append(w, enc(real));
  auto c = push(d, w);
  CHECK(c.frames.empty() && mcu_decoder_pending(&d) == w.size());
  mcu_frame out;
  mcu_event ev = mcu_decoder_flush_stale(&d, &out);
  CHECK(ev == MCU_EV_FRAME && same(out, real));
  CHECK(mcu_decoder_poll(&d, &out) == MCU_EV_NONE && mcu_decoder_pending(&d) == 0);
  CHECK(d.counters.resyncs == 1);
  // nothing buffered: no-op; a complete pending frame is delivered before anything is abandoned
  CHECK(mcu_decoder_flush_stale(&d, &out) == MCU_EV_NONE && d.counters.resyncs == 1);
  // two stacked false headers: repeated flushes drain both and still deliver the real frame
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  Bytes w2 = {0xA5, 0x10, 0x05, 60, 0xA5, 0x10, 0x05, 50};
  append(w2, enc(real));
  push(d, w2);
  unsigned frames = 0;
  for (int i = 0; i < 4 && mcu_decoder_pending(&d) > 0; ++i) {
    for (ev = mcu_decoder_flush_stale(&d, &out); ev != MCU_EV_NONE; ev = mcu_decoder_poll(&d, &out)) frames += ev == MCU_EV_FRAME;
  }
  CHECK(frames == 1 && mcu_decoder_pending(&d) == 0);
  // counters: bytes_in is 64-bit
  CHECK(sizeof(d.counters.bytes_in) == 8);
}

void test_id_maps() {
  CHECK(MCU_CTRL_ENCODER_1 == 1 && MCU_CTRL_VALUE_ENCODER == 9 && MCU_CTRL_FADER_1 == 16 && MCU_CTRL_PEDAL_SWITCH == 42);
  CHECK(MCU_BTN_VALUE_PUSH == 23 && MCU_BTN_TOGGLE_PART_ALL_ASSIGN == 34);
  CHECK(MCU_LED_ENCODER_RING_1 == 16 && MCU_LED_TOGGLE_LED_ASSIGN == 38);
}

}  // namespace

int main() {
  test_crc();
  test_golden();
  test_roundtrip_typed();
  test_pack_validation();
  test_encoder_errors();
  test_version_policy();
  test_length_vs_type();
  test_max_payload_and_framing();
  test_decoder_chunking();
  test_noise_truncation_crc();
  test_seq();
  test_back_to_back_and_poll_contract();
  test_flush_stale();
  test_id_maps();
  if (g_fail == 0) std::printf("mcu_frame_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}

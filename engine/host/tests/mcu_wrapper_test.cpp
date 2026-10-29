// C++ wrapper of the MCU frame library: typed messages, range validation, malformed input, StreamDecoder events and
// counters, FrameWriter, Loopback, generated ID table.
#include <cstdio>
#include <vector>

#include "velora/host/mcu_frame.hpp"

using namespace velora::host::mcu;
namespace mcu_ids = velora::host::mcu_ids;

static int g_fail = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

using Bytes = std::vector<std::uint8_t>;

// Builds wire bytes without any type / length validation (what a misbehaving MCU could send).
static Bytes raw(std::uint8_t type, std::uint16_t seq, const Bytes& payload, std::uint8_t version = kProtocolVersion) {
  mcu_frame f;
  mcu_frame_init(&f, type, seq, 0);
  f.version = version;
  f.length = static_cast<std::uint8_t>(payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) f.payload[i] = payload[i];
  std::uint8_t buf[MCU_MAX_FRAME];
  const int n = mcu_encode_unchecked(&f, buf, sizeof buf);
  return Bytes(buf, buf + n);
}

static Frame frame_of(std::uint8_t type, const Bytes& payload) {
  Frame f;
  f.type = type;
  f.payload = payload;
  return f;
}

static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

static void test_roundtrip_and_ranges() {
  const std::vector<Message> all = {
      Hello{0x0102, kProtocolVersion, 1, 3}, Hello{0x0102, kProtocolVersion, 1, 3, true},
      Hello{1, kProtocolVersion, 2, 0},      KeyContact{72, 2, true},
      KeyContact{0, 0, false},               Aftertouch{0xFF, 0xFFFF},         Control{mcu_ids::controls::FADER_9, 4095},
      make_delta(mcu_ids::controls::ENCODER_1, -127), make_delta(mcu_ids::controls::VALUE_ENCODER, 127),
      Button{mcu_ids::buttons::VALUE_PUSH, true},     LedSet{mcu_ids::leds::ENCODER_RING_8, 200, 128},
      TimeSync{false, 7, 0},                 TimeSync{true, 7, 123456},        EnterBootloader{}};
  Loopback lb(65530);  // seq wraps during the test without a reported gap
  for (const Message& m : all) {
    auto got = lb.send(m, 1000);
    CHECK(got.size() == 1 && got[0] == m);
  }
  CHECK(lb.decoder().counters().frames_ok == all.size() && lb.decoder().counters().seq_gaps == 0);
  CHECK(control_delta(make_delta(1, -127)) == -127);
  CHECK(control_delta(make_delta(1, 1000)) == 127 && control_delta(make_delta(1, -1000)) == -127);  // clamped

  // out-of-range or unknown fields: no frame
  CHECK(!make_frame(Hello{1, kProtocolVersion, 3, 2}, 0, 0));
  CHECK(!make_frame(Hello{1, kProtocolVersion, 1, 1}, 0, 0));
  CHECK(!make_frame(Hello{1, kProtocolVersion, 2, 0, true}, 0, 0));  // panel MCU cannot advertise keybed aftertouch
  CHECK(!make_frame(Hello{1, kProtocolVersion, 1, 0x83, false}, 0, 0));
  CHECK(!make_frame(Control{mcu_ids::controls::FADER_1, 4096}, 0, 0));
  CHECK(make_frame(Control{mcu_ids::controls::FADER_1, 4095}, 0, 0));
  CHECK(!make_frame(Control{mcu_ids::controls::PEDAL_SWITCH, 2}, 0, 0));
  CHECK(!make_frame(Control{mcu_ids::controls::ENCODER_1, 128}, 0, 0));
  CHECK(!make_frame(Control{0, 0}, 0, 0));
  CHECK(!make_frame(Button{0, true}, 0, 0));
  CHECK(!make_frame(LedSet{0, 1, 1}, 0, 0));
  CHECK(make_frame(KeyContact{1, 1, true}, 0, 0));

  static_assert(static_cast<int>(Error::kNull) < 0);
}

static void test_malformed() {
  Loopback lb;
  // parse_message: wrong value, wrong length, unknown type, bad magic, oversize payload
  CHECK(!parse_message(frame_of(static_cast<std::uint8_t>(FrameType::kButton), {1, 5})));
  CHECK(!parse_message(frame_of(static_cast<std::uint8_t>(FrameType::kHello), {1, 0, 0x10})));
  CHECK(!parse_message(frame_of(99, {1})));
  CHECK(!parse_message(frame_of(static_cast<std::uint8_t>(FrameType::kEnterBootloader), {1, 2, 3, 4})));
  CHECK(!parse_message(frame_of(static_cast<std::uint8_t>(FrameType::kButton), Bytes(65, 0))));
  CHECK(parse_message(frame_of(static_cast<std::uint8_t>(FrameType::kEnterBootloader), {0x42, 0x4F, 0x4F, 0x54})));

  // the same through the wire: nothing comes out of Loopback::send_bytes
  CHECK(lb.send_bytes(raw(5, 0, {1, 5})).empty());                      // BUTTON value 5
  CHECK(lb.send_bytes(raw(1, 1, {1, 0, 0x10})).empty());                // HELLO wrong length
  CHECK(lb.send_bytes(raw(99, 2, {1})).empty());                        // unknown type
  CHECK(lb.send_bytes(raw(8, 3, {1, 2, 3, 4})).empty());                // wrong magic
  Bytes huge = {0xA5, kProtocolVersion, 5, 65};
  huge.resize(4 + 65 + 2 + 10, 0);
  CHECK(lb.send_bytes(huge).empty());                                   // 65-byte payload: framing error
  CHECK(lb.decoder().counters().framing_errors >= 1 && lb.decoder().counters().bad_lengths == 1 &&
        lb.decoder().counters().unknown_types == 1);
  CHECK(!encode(frame_of(5, Bytes(65, 0))));

  // a good message after all that still arrives
  auto ok = lb.send(Button{1, true}, 5);
  CHECK(ok.size() == 1);
}

static void test_events_and_counters() {
  StreamDecoder dec;
  Bytes w = {0x00, 0x11};                       // 2 noise bytes
  append(w, raw(5, 0, {1, 1, 1}));              // BUTTON, wrong length (15 bytes)
  append(w, raw(99, 1, {1}));                   // unknown type (13 bytes)
  append(w, Bytes{0xA5, kProtocolVersion, 5, 65});  // framing error header (4 bytes)
  append(w, raw(5, 3, {2, 1}));                 // good BUTTON, seq 3 (14 bytes): seq 2 was never seen
  std::vector<Frame> frames;
  std::vector<EventInfo> events;
  dec.feed(w.data(), w.size(), &frames, &events);
  CHECK(frames.size() == 1 && frames[0].seq == 3);
  CHECK(events.size() == 4);
  if (events.size() == 4) {
    CHECK(events[0].event == Event::kBadLength && events[0].type == 5 && events[0].seq == 0 && events[0].version == kProtocolVersion);
    CHECK(events[1].event == Event::kUnknownType && events[1].type == 99 && events[1].seq == 1);
    CHECK(events[2].event == Event::kFramingError && events[2].type == 0 && events[2].seq == 0);
    CHECK(events[3].event == Event::kFrame && events[3].type == 5 && events[3].seq == 3);
  }
  const DecoderCounters c = dec.counters();
  CHECK(c.bytes_in == 48);
  CHECK(c.frames_ok == 1);
  CHECK(c.crc_errors == 0);
  CHECK(c.framing_errors == 1);
  CHECK(c.resyncs == 1);
  CHECK(c.dropped_bytes == 6);  // 2 noise + the false sync + 3 header bytes behind it
  CHECK(c.version_mismatches == 0);
  CHECK(c.bad_lengths == 1);
  CHECK(c.unknown_types == 1);
  CHECK(c.seq_gaps == 1);
  CHECK(c.seq_lost == 1);
  CHECK(c.seq_reorders == 0);
  CHECK(c.overflow_bytes == 0);
  CHECK(dec.last_seq_lost() == 1);

  // CRC error and a reorder
  StreamDecoder d2;
  Bytes bad = raw(5, 0, {1, 1});
  bad.back() ^= 1;
  Bytes w2 = bad;
  append(w2, raw(5, 4, {1, 1}));
  append(w2, raw(5, 4, {1, 0}));  // duplicate seq
  events.clear();
  frames.clear();
  d2.feed(w2.data(), w2.size(), &frames, &events);
  const DecoderCounters c2 = d2.counters();
  CHECK(frames.size() == 2 && c2.crc_errors == 1 && c2.resyncs == 1 && c2.dropped_bytes >= 1 && c2.seq_reorders == 1 &&
        c2.seq_gaps == 0 && c2.bytes_in == w2.size());
  CHECK(events.size() == 3 && events[0].event == Event::kCrcError);

  // feed(nullptr, n) is a safe no-op
  d2.feed(nullptr, 10, &frames, &events);
  CHECK(d2.counters().bytes_in == c2.bytes_in);

  // non-default current version: 2.1 accepts 1.x (N-1) and 2.x, rejects 3.x and carries the rejected frame's info
  StreamDecoder d3(0x21);
  Bytes w3;
  append(w3, raw(5, 0, {1, 1}, 0x10));
  append(w3, raw(5, 1, {1, 1}, 0x2F));
  append(w3, raw(5, 2, {1, 1}, 0x30));
  events.clear();
  frames.clear();
  d3.feed(w3.data(), w3.size(), &frames, &events);
  CHECK(frames.size() == 2 && events.size() == 3 && events[2].event == Event::kVersionRejected && events[2].version == 0x30 &&
        events[2].seq == 2);
  CHECK(d3.counters().version_mismatches == 1 && d3.counters().seq_gaps == 0);
  StreamDecoder d4(0x05);  // major 0 is invalid: falls back to the default
  frames.clear();
  d4.feed(raw(5, 0, {1, 1}).data(), 14, &frames);
  CHECK(frames.size() == 1);

  // flush_stale: a false header stalls the decoder, a real frame behind it is delivered after the flush
  StreamDecoder d5;
  Bytes w5 = {0xA5, kProtocolVersion, 5, 60};
  append(w5, raw(5, 0, {3, 1}));
  frames.clear();
  d5.feed(w5.data(), w5.size(), &frames);
  CHECK(frames.empty());
  d5.flush_stale(&frames);
  CHECK(frames.size() == 1 && frames[0].payload == Bytes({3, 1}));
}

static void test_writer_and_golden() {
  FrameWriter w(7);
  const Bytes golden = {0xA5, 0x10, 0x05, 0x02, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x17, 0x01, 0xBA, 0xC5};
  CHECK(w.write(Button{mcu_ids::buttons::VALUE_PUSH, true}, 8) == golden);
  CHECK(w.next_seq() == 8);
  CHECK(w.write(Button{0, true}, 8).empty() && w.next_seq() == 8);  // invalid message: nothing written, seq kept

  Frame f;
  f.type = static_cast<std::uint8_t>(FrameType::kButton);
  f.payload = {1, 1};
  Error err = Error::kOk;
  auto bytes = encode(f, &err);
  CHECK(bytes && bytes->size() == 14 && err == Error::kOk && (*bytes)[0] == 0xA5);
  f.payload.assign(kMaxPayload + 1, 0);
  CHECK(!encode(f, &err) && err == Error::kPayloadTooLong);
  f.payload = {1};
  CHECK(!encode(f, &err) && err == Error::kBadLength);
  f.type = 99;
  CHECK(!encode(f, &err) && err == Error::kUnknownType);

  Loopback lb;
  auto garbage = FrameWriter().write(Button{1, true}, 1);
  garbage.back() ^= 1;
  CHECK(lb.send_bytes(garbage).empty() && lb.decoder().counters().crc_errors == 1);

  CHECK(mcu_ids::kControlsCount == 23 && mcu_ids::kButtonsCount == 21 && mcu_ids::kLedsCount == 23);
  CHECK(mcu_ids::kControls[0].id == 1 && mcu_ids::kMapProtocolVersion == kProtocolVersion);
}

int main() {
  test_roundtrip_and_ranges();
  test_malformed();
  test_events_and_counters();
  test_writer_and_golden();
  if (g_fail == 0) std::printf("mcu_wrapper_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}

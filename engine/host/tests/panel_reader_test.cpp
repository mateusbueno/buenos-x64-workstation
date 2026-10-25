// Panel reader (Story 3.10): CONTROL / BUTTON frames of the panel MCU -> device time -> pitch bend / modulation UMP for every keyboard-on part, raw panel
// control events, BUTTON sink; frame errors counted; drift-tracked time like the keybed reader; an offline BlockRenderer render of the wheels reaching
// ALL keyboard-on parts. Bare main, CHECK style as the other host tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "velora/core/assign_events.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/engine.hpp"
#include "velora/core/part_router.hpp"
#include "velora/host/panel_reader.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint64_t kMs = 1'000'000ull;
constexpr std::uint64_t kSec = 1'000'000'000ull;

// A simulated panel MCU: a tick counter running at 1 MHz * (1 + ppm), with its own frame numbering.
struct SimPanel {
  double ppm = 0;
  std::uint32_t tick0 = 987654321u;
  std::uint64_t t0_ns = 0;
  mcu::FrameWriter writer;
  std::uint32_t tick_at(std::uint64_t true_ns) const {
    const double us = static_cast<double>(true_ns - t0_ns) * (1.0 + ppm * 1e-6) / 1000.0;
    return tick0 + static_cast<std::uint32_t>(static_cast<std::uint64_t>(us));
  }
  std::vector<std::uint8_t> frame(const mcu::Message& m, std::uint64_t true_ns) { return writer.write(m, tick_at(true_ns)); }
};

struct Rig {
  HostEventQueue q;
  KeyboardOnParts parts;
  SimPanel mcu;
  std::unique_ptr<PanelReader> reader;
  std::vector<StampedUmp> out;
  std::vector<PanelButtonEvent> buttons;
  std::uint64_t host0 = 10 * kSec;
  explicit Rig(std::uint16_t mask = 0b100101, double ppm = 0) {  // parts 0, 2 and 5 sound with the keyboard
    mcu.ppm = ppm;
    mcu.t0_ns = host0;
    parts.set_mask(mask);
    reader = std::make_unique<PanelReader>(q, parts);
    reader->set_button_sink([this](const PanelButtonEvent& e) { buttons.push_back(e); });
  }
  void drain() {
    StampedUmp s;
    while (q.pop(s)) out.push_back(s);
  }
  // The frame reaches the reader 300 us after the event.
  void feed(const mcu::Message& m, std::uint64_t true_ns) {
    const auto wire = mcu.frame(m, true_ns);
    reader->on_bytes(wire.data(), wire.size(), true_ns + 300'000);
    drain();
  }
  void control(std::uint8_t id, std::uint16_t v, std::uint64_t t) { feed(mcu::Control{id, v}, t); }
};

namespace ids = mcu_ids::controls;

void test_wheels() {
  Rig r;
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::WHEEL_PITCH, 2048, t);
  CHECK(r.out.size() == 3);
  std::vector<std::uint32_t> chans;
  for (const StampedUmp& s : r.out) {
    CHECK(s.ump.is_pitch_bend() && s.ump.group() == 0 && s.ump.pitch_bend_value32() == 0x80000000u);
    CHECK(std::llabs(static_cast<long long>(s.host_ns) - static_cast<long long>(t)) < 400'000);  // device time (the bootstrap clock), not rx time + something else
    chans.push_back(s.ump.channel());
  }
  CHECK((chans == std::vector<std::uint32_t>{0, 2, 5}));
  r.out.clear();
  r.control(ids::WHEEL_PITCH, 0, t + kMs);
  r.control(ids::WHEEL_PITCH, 4095, t + 2 * kMs);
  r.control(ids::WHEEL_PITCH, 3072, t + 3 * kMs);
  CHECK(r.out.size() == 9);
  CHECK(r.out[0].ump.pitch_bend_value32() == 0 && r.out[3].ump.pitch_bend_value32() == 0xFFFFFFFFu);
  const std::uint32_t q3 = r.out[6].ump.pitch_bend_value32();
  CHECK(q3 > 0xBFFFFFF0u - 0x400000u && q3 < 0xC0000000u + 0x400000u);  // three quarters up
  CHECK(r.out[0].host_ns <= r.out[3].host_ns && r.out[3].host_ns <= r.out[6].host_ns);
  CHECK(PanelReader::pitch_bend_value32(2047) < 0x80000000u && PanelReader::pitch_bend_value32(2049) > 0x80000000u);
  // mod wheel -> CC 1
  r.out.clear();
  r.control(ids::WHEEL_MOD, 4095, t + 4 * kMs);
  r.control(ids::WHEEL_MOD, 0, t + 5 * kMs);
  CHECK(r.out.size() == 6);
  for (std::size_t i = 0; i < r.out.size(); ++i) {
    CHECK(r.out[i].ump.is_control_change() && r.out[i].ump.controller_index() == 1);
    CHECK(r.out[i].ump.controller_value32() == (i < 3 ? 0xFFFFFFFFu : 0u));
  }
  CHECK(r.reader->counters().pitch_bends == 4 && r.reader->counters().modulations == 2);
  // the parts follow the control thread's update
  r.parts.set_mask(0b10);
  r.out.clear();
  r.control(ids::WHEEL_MOD, 2000, t + 6 * kMs);
  // parts 0, 2, 5 left the keyboard: centre pitch bend and zero modulation; part 1 joined: the current wheels, then the new frame
  CHECK(r.out.size() == 9);
  if (r.out.size() == 9) {
    CHECK(r.out[0].ump.is_pitch_bend() && r.out[0].ump.channel() == 1 && r.out[0].ump.pitch_bend_value32() == PanelReader::pitch_bend_value32(3072));
    for (int i = 1; i <= 3; ++i) CHECK(r.out[i].ump.is_pitch_bend() && r.out[i].ump.pitch_bend_value32() == 0x80000000u && r.out[i].ump.channel() != 1);
    CHECK(r.out[4].ump.is_control_change() && r.out[4].ump.channel() == 1 && r.out[4].ump.controller_value32() == 0);
    for (int i = 5; i <= 7; ++i) CHECK(r.out[i].ump.is_control_change() && r.out[i].ump.controller_value32() == 0 && r.out[i].ump.channel() != 1);
    CHECK(r.out[8].ump.is_control_change() && r.out[8].ump.channel() == 1 && r.out[8].ump.controller_value32() == PanelReader::scale12_to_32(2000));
  }
}

// Near a full queue the controllers are shed, but a shed wheel is re-sent with its CURRENT value, and pedals are never shed.
void test_full_queue() {
  Rig r(0b101);
  const std::uint64_t t = r.host0;
  r.control(ids::WHEEL_PITCH, 2048, t);  // known state
  r.out.clear();
  StampedUmp filler{core::make_note_off(0, 7, 60), 0};
  while (r.q.size() + kNoteHeadroom < HostEventQueue::capacity()) CHECK(r.q.push(filler));
  auto raw = [&](std::uint8_t id, std::uint16_t v, std::uint64_t at) {  // no drain: the queue stays full
    const auto wire = r.mcu.frame(mcu::Control{id, v}, at);
    r.reader->on_bytes(wire.data(), wire.size(), at + 300'000);
  };
  raw(ids::WHEEL_PITCH, 0, t + kMs);     // shed on both parts
  raw(ids::WHEEL_PITCH, 4095, t + 2 * kMs);  // shed again: only this one may come back
  raw(ids::FADER_1, 9, t + 3 * kMs);     // a controller: shed
  raw(ids::PEDAL_SUSTAIN, 4095, t + 4 * kMs);  // a pedal: kept
  CHECK(r.reader->counters().controllers_dropped == 5);
  StampedUmp s;
  std::vector<StampedUmp> pedals;
  while (r.q.pop(s)) {
    if (s.ump.is_assignable_controller()) pedals.push_back(s);
  }
  CHECK(pedals.size() == 1 && core::panel_control_id(pedals[0].ump) == ids::PEDAL_SUSTAIN);
  r.out.clear();                 // (the fillers were drained into it by r.control)
  r.reader->poll(t + 10 * kMs);  // space again: the shed wheel is re-sent
  r.drain();
  CHECK(r.out.size() == 2);
  for (const StampedUmp& u : r.out) CHECK(u.ump.is_pitch_bend() && u.ump.pitch_bend_value32() == 0xFFFFFFFFu);
  CHECK(r.reader->counters().wheel_replays == 1);
  r.out.clear();
  r.reader->poll(t + 20 * kMs);  // nothing pending any more
  r.drain();
  CHECK(r.out.empty());
}

void test_unknown_button() {
  Rig r;
  const std::uint64_t t = r.host0;
  mcu::Frame bad;
  bad.type = static_cast<std::uint8_t>(mcu::FrameType::kButton);
  bad.seq = r.mcu.writer.next_seq();
  bad.tick = r.mcu.tick_at(t);
  bad.payload = {0x77, 0x01};
  const auto wire = mcu::encode(bad);
  CHECK(wire.has_value());
  if (wire) {
    r.mcu.writer = mcu::FrameWriter(static_cast<std::uint16_t>(bad.seq + 1));
    r.reader->on_bytes(wire->data(), wire->size(), t);
  }
  CHECK(r.buttons.empty() && r.reader->counters().unknown_button == 1 && r.reader->counters().button_frames == 0);
  r.feed(mcu::Button{mcu_ids::buttons::EXIT, true}, t + kMs);
  CHECK(r.buttons.size() == 1 && r.reader->counters().seq_gaps == 0);
}

void test_no_keyboard_parts() {
  Rig r(0);
  r.control(ids::WHEEL_PITCH, 1000, r.host0 + kMs);
  r.control(ids::WHEEL_MOD, 1000, r.host0 + 2 * kMs);
  CHECK(r.out.empty() && r.reader->counters().wheel_no_parts == 2);
  r.control(ids::FADER_1, 1000, r.host0 + 3 * kMs);  // other controls do not depend on the keyboard
  CHECK(r.out.size() == 1);
}

void test_raw_controls() {
  Rig r;
  const std::uint64_t t = r.host0;
  r.control(ids::FADER_3, 1234, t + kMs);
  CHECK(r.out.size() == 1);
  if (r.out.size() == 1) {
    std::uint32_t rid = 0;
    CHECK(core::decode_reserved_event_id(r.out[0].ump, rid) && rid == core::kPanelControlEventId);
    CHECK(core::panel_control_id(r.out[0].ump) == ids::FADER_3 && core::panel_control_value16(r.out[0].ump) == 1234);
  }
  r.out.clear();
  // an encoder: the raw event carries the signed delta, and encoders 1..8 also produce the Story 4.10 step event
  r.feed(mcu::make_delta(ids::ENCODER_2, -5), t + 2 * kMs);
  CHECK(r.out.size() == 2);
  if (r.out.size() == 2) {
    CHECK(core::panel_control_id(r.out[0].ump) == ids::ENCODER_2 && static_cast<std::int16_t>(core::panel_control_value16(r.out[0].ump)) == -5);
    std::uint32_t rid = 0;
    CHECK(core::decode_reserved_event_id(r.out[1].ump, rid) && rid == core::kEncoderStepFirstId + 1 && static_cast<std::int32_t>(r.out[1].ump.word1) == -5);
  }
  r.out.clear();
  r.feed(mcu::make_delta(ids::VALUE_ENCODER, 3), t + 3 * kMs);  // VALUE: raw only
  CHECK(r.out.size() == 1 && static_cast<std::int16_t>(core::panel_control_value16(r.out[0].ump)) == 3);
  r.out.clear();
  // pedals: raw events with ID and value, nothing interpreted
  r.control(ids::PEDAL_SUSTAIN, 4095, t + 4 * kMs);
  r.control(ids::PEDAL_EXPRESSION, 17, t + 5 * kMs);
  r.control(ids::PEDAL_SWITCH, 1, t + 6 * kMs);
  CHECK(r.out.size() == 3);
  if (r.out.size() == 3) {
    CHECK(core::panel_control_id(r.out[0].ump) == ids::PEDAL_SUSTAIN && core::panel_control_value16(r.out[0].ump) == 4095);
    CHECK(core::panel_control_id(r.out[1].ump) == ids::PEDAL_EXPRESSION && core::panel_control_value16(r.out[1].ump) == 17);
    CHECK(core::panel_control_id(r.out[2].ump) == ids::PEDAL_SWITCH && core::panel_control_value16(r.out[2].ump) == 1);
  }
  CHECK(r.reader->counters().raw_controls == 6 && r.reader->counters().encoder_steps == 1);
  // an unknown control ID (a valid frame, an ID the map does not have) is dropped and counted; the reader goes on
  r.out.clear();
  mcu::Frame bad;
  bad.type = static_cast<std::uint8_t>(mcu::FrameType::kControl);
  bad.seq = r.mcu.writer.next_seq();
  bad.tick = r.mcu.tick_at(t + 7 * kMs);
  bad.payload = {0x77, 0x10, 0x00};
  const auto wire = mcu::encode(bad);
  CHECK(wire.has_value());
  if (wire) {
    r.mcu.writer = mcu::FrameWriter(static_cast<std::uint16_t>(bad.seq + 1));
    r.reader->on_bytes(wire->data(), wire->size(), t + 7 * kMs);
  }
  r.drain();
  CHECK(r.out.empty() && r.reader->counters().unknown_control == 1 && r.reader->counters().seq_gaps == 0);
  r.control(ids::FADER_1, 5, t + 8 * kMs);
  CHECK(r.out.size() == 1);
}

void test_buttons() {
  Rig r;
  const std::uint64_t t = r.host0 + 20 * kMs;
  r.feed(mcu::Button{mcu_ids::buttons::MENU, true}, t);
  r.feed(mcu::Button{mcu_ids::buttons::MENU, false}, t + 80 * kMs);
  r.feed(mcu::Button{mcu_ids::buttons::TOGGLE_PARTS_SCENES, true}, t + 90 * kMs);
  CHECK(r.out.empty());  // nothing to the RT plane
  CHECK(r.buttons.size() == 3);
  if (r.buttons.size() == 3) {
    CHECK(r.buttons[0].id == mcu_ids::buttons::MENU && r.buttons[0].pressed && !r.buttons[1].pressed && r.buttons[2].id == 33);
    CHECK(r.buttons[1].device_ns - r.buttons[0].device_ns > 79 * kMs && r.buttons[1].device_ns - r.buttons[0].device_ns < 81 * kMs);
    CHECK(std::llabs(static_cast<long long>(r.buttons[0].device_ns) - static_cast<long long>(t)) < 400'000);
  }
  CHECK(r.reader->counters().button_frames == 3 && r.reader->counters().buttons_delivered == 3);
  // no sink: counted, nothing else happens
  Rig s;
  s.reader->set_button_sink(nullptr);
  s.feed(mcu::Button{1, true}, s.host0);
  CHECK(s.reader->counters().buttons_unsunk == 1 && s.out.empty());
}

void test_bad_frames() {
  Rig r;
  const std::uint64_t t = r.host0;
  r.control(ids::FADER_1, 10, t + kMs);
  CHECK(r.out.size() == 1);
  // a CRC error: the frame is lost, the next one shows the seq gap, the reader goes on
  auto w = r.mcu.frame(mcu::Control{ids::FADER_1, 20}, t + 2 * kMs);
  w[w.size() - 1] ^= 0x55;
  r.reader->on_bytes(w.data(), w.size(), t + 2 * kMs);
  r.control(ids::FADER_1, 30, t + 3 * kMs);
  r.control(ids::FADER_1, 40, t + 4 * kMs);
  CHECK(r.out.size() == 3);
  CHECK(r.reader->counters().crc_errors == 1 && r.reader->counters().seq_gaps == 1 && r.reader->counters().seq_lost == 1);
  // garbage between frames is skipped
  const std::vector<std::uint8_t> noise = {0x00, 0xA5, 0x13, 0xFF, 0x01};
  r.reader->on_bytes(noise.data(), noise.size(), t + 5 * kMs);
  r.control(ids::FADER_2, 50, t + 6 * kMs);
  r.drain();
  CHECK(r.out.size() == 4 && core::panel_control_id(r.out.back().ump) == ids::FADER_2);
  // frames of the keybed (key contacts) are not the panel's: ignored and counted
  const auto before = r.reader->counters().ignored_frames;
  r.feed(mcu::KeyContact{1, 0, true}, t + 7 * kMs);
  CHECK(r.reader->counters().ignored_frames == before + 1);
}

// The panel MCU's clock drifts: converted times follow the keybed reader's accuracy and are never reordered.
void test_drift(double ppm) {
  Rig r(0b1, ppm);
  std::vector<std::uint8_t> sent;
  r.reader->set_tx([&](const std::uint8_t* d, std::size_t n) { sent.assign(d, d + n); });
  mcu::StreamDecoder sim_rx;
  std::uint64_t rng = 0x9e3779b97f4a7c15ULL;
  auto jitter = [&](std::uint64_t span) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (rng >> 33) % span;
  };
  std::vector<std::uint64_t> expect;
  const std::uint64_t total = 2ull * 3600ull * kSec;
  std::uint64_t step = 0;
  for (std::uint64_t t = 0; t < total; t += 100 * kMs, ++step) {
    const std::uint64_t now = r.host0 + t;
    r.reader->poll(now);
    if (!sent.empty()) {
      const std::uint64_t up = 150'000 + jitter(40'000), down = 150'000 + jitter(40'000);
      auto frames = sim_rx.feed(sent);
      sent.clear();
      for (const mcu::Frame& f : frames) {
        auto m = mcu::parse_message(f);
        if (!m || !std::holds_alternative<mcu::TimeSync>(*m)) continue;
        const std::uint64_t at_mcu = now + up;
        const auto reply = r.mcu.frame(mcu::TimeSync{true, std::get<mcu::TimeSync>(*m).token, r.mcu.tick_at(at_mcu)}, at_mcu);
        r.reader->on_bytes(reply.data(), reply.size(), at_mcu + down);
      }
    }
    if (t > 60 * kSec && step % 70 == 3) {  // a fader move every 7 s
      const std::uint64_t te = now + 5 * kMs;
      const auto w = r.mcu.frame(mcu::Control{ids::FADER_4, static_cast<std::uint16_t>(step % 4096)}, te);
      r.reader->on_bytes(w.data(), w.size(), te + 100'000 + jitter(300'000));
      expect.push_back(te);
    }
    r.drain();
  }
  std::uint64_t worst = 0, prev = 0;
  bool ordered = true;
  std::size_t k = 0;
  for (const StampedUmp& s : r.out) {
    if (s.host_ns < prev) ordered = false;
    prev = s.host_ns;
    if (k < expect.size()) {
      const std::uint64_t d = s.host_ns > expect[k] ? s.host_ns - expect[k] : expect[k] - s.host_ns;
      worst = std::max(worst, d);
    }
    ++k;
  }
  std::printf("panel drift %+.0f ppm: %zu events, worst error %.1f us, fitted %+.1f ppm\n", ppm, r.out.size(), static_cast<double>(worst) / 1000.0,
              r.reader->clock().drift_ppm());
  CHECK(ordered && r.out.size() == expect.size() && !expect.empty());
  CHECK(worst <= 20'000);
  CHECK(std::fabs(r.reader->clock().drift_ppm() - ppm) < 5.0);
  const PanelReaderCounters c = r.reader->counters();
  CHECK(c.clock_clamps == 0 && c.missed_syncs == 0 && c.crc_errors == 0 && c.seq_gaps == 0 && c.queue_full == 0);
}

void test_hello_and_restart() {
  Rig r;
  const std::uint64_t t = r.host0;
  r.feed(mcu::Hello{0x0102, mcu::kProtocolVersion, 2, 0}, t);
  CHECK(r.reader->hello() && r.reader->hello()->board_id == 2);
  r.feed(mcu::Hello{0x0102, mcu::kProtocolVersion, 1, 3}, t + kMs);  // the keybed MCU's hello is not ours
  CHECK(r.reader->hello()->board_id == 2 && r.reader->counters().ignored_frames == 1);
  r.control(ids::FADER_1, 1, t + 2 * kMs);
  r.feed(mcu::Hello{0x0103, mcu::kProtocolVersion, 2, 0}, t + 3 * kMs);  // a different firmware: the MCU restarted
  CHECK(r.reader->counters().mcu_resets == 1);
  // a restart also resets the wheels of the keyboard-on parts: centre pitch bend and zero modulation
  Rig w;
  w.control(ids::WHEEL_PITCH, 4095, w.host0);
  w.control(ids::WHEEL_MOD, 4095, w.host0 + kMs);
  w.feed(mcu::Hello{0x0102, mcu::kProtocolVersion, 2, 0}, w.host0 + 2 * kMs);
  w.out.clear();
  w.feed(mcu::Hello{0x0103, mcu::kProtocolVersion, 2, 0}, w.host0 + 3 * kMs);
  CHECK(w.out.size() == 6);
  std::size_t centre = 0, zero = 0;
  for (const StampedUmp& u : w.out) {
    centre += u.ump.is_pitch_bend() && u.ump.pitch_bend_value32() == 0x80000000u;
    zero += u.ump.is_control_change() && u.ump.controller_index() == 1 && u.ump.controller_value32() == 0;
  }
  CHECK(centre == 3 && zero == 3);
}

// ---- an offline render: the wheels reach ALL keyboard-on parts, at their in-block offset -------------------------------------------------------------------

// A part engine that records pitch bend and controller deliveries with the absolute frame they were applied at.
class RecEngine final : public core::Engine {
 public:
  struct Rec { std::uint32_t part; bool pitch; std::uint32_t value; std::uint64_t frame; };
  core::EngineTypeId type_id() const noexcept override { return core::EngineTypeId::kFm; }
  std::uint32_t cost_units_per_voice() const noexcept override { return 1; }
  std::uint32_t voice_capacity() const noexcept override { return 1; }
  core::EngineStatus start_voice(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override { return core::EngineStatus::kOk; }
  core::EngineStatus stop_voice(std::uint32_t, std::uint32_t) noexcept override { return core::EngineStatus::kVoiceNotFound; }
  void set_param(std::uint32_t, float) noexcept override {}
  void render(float*, float*, std::uint32_t frames) noexcept override { clock_ += frames; }
  void render_split(float* const*, float* const*, std::uint32_t frames) noexcept override { clock_ += frames; }
  bool voice_info(std::uint32_t, core::VoiceInfo&) const noexcept override { return false; }
  bool begin_steal_fade(std::uint32_t, std::uint32_t) noexcept override { return false; }
  bool set_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept override {
    if (cc == 1) log.push_back({part, false, value32, clock_});
    return cc == 1;
  }
  bool set_part_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept override {
    log.push_back({part, true, value32, clock_});
    return true;
  }
  std::vector<Rec> log;

 private:
  std::uint64_t clock_ = 0;
};

void test_render_reaches_all_keyboard_parts() {
  auto br = std::make_unique<core::BlockRenderer>();
  RecEngine eng;
  CHECK(br->bind_engine(eng));
  for (std::uint32_t ch : {0u, 2u, 5u}) br->set_channel_engine(ch, static_cast<std::uint32_t>(core::EngineTypeId::kFm));
  HostEventQueue q;
  RenderDriver drv(*br, q);
  KeyboardOnParts parts;
  parts.set_mask(0b100101);
  PanelReader reader(q, parts);
  mcu::FrameWriter w;
  const std::uint64_t t0 = 50 * kSec;
  auto send = [&](const mcu::Message& m, std::uint64_t t) {
    const auto wire = w.write(m, static_cast<std::uint32_t>(t / 1000));
    reader.on_bytes(wire.data(), wire.size(), t);
  };
  send(mcu::Control{ids::WHEEL_PITCH, 4095}, t0 + 1 * kMs);
  send(mcu::Control{ids::WHEEL_MOD, 2048}, t0 + 2 * kMs);
  send(mcu::Control{ids::WHEEL_PITCH, 2048}, t0 + 3 * kMs);
  send(mcu::Control{ids::FADER_1, 100}, t0 + 3 * kMs);
  float l[64], r[64];
  for (int b = 0; b < 12; ++b) drv.render_block(l, r, t0 + static_cast<std::uint64_t>(b) * 1'333'333);
  std::vector<RecEngine::Rec> pb, cc;
  for (const auto& e : eng.log) (e.pitch ? pb : cc).push_back(e);
  CHECK(pb.size() == 6 && cc.size() == 3);
  for (std::uint32_t part : {0u, 2u, 5u}) {  // every keyboard-on part got both wheels, and no other part got anything
    int n_pb = 0, n_cc = 0;
    for (const auto& e : pb) n_pb += e.part == part;
    for (const auto& e : cc) n_cc += e.part == part;
    CHECK(n_pb == 2 && n_cc == 1);
  }
  for (const auto& e : eng.log) CHECK(e.part == 0 || e.part == 2 || e.part == 5);
  if (pb.size() == 6 && cc.size() == 3) {
    CHECK(pb[0].value == 0xFFFFFFFFu && pb[3].value == 0x80000000u);
    CHECK(std::llabs(static_cast<long long>(cc[0].value) - 0x80000000ll) < 0x100000);
    // device time -> in-block offset: events 1 ms apart land 48 frames apart (at 48 kHz), on every part
    CHECK(std::llabs(static_cast<long long>(cc[0].frame) - static_cast<long long>(pb[0].frame) - 48) <= 1);
    CHECK(std::llabs(static_cast<long long>(pb[3].frame) - static_cast<long long>(cc[0].frame) - 48) <= 1);
    CHECK(pb[0].frame == pb[1].frame && pb[1].frame == pb[2].frame);
  }
  CHECK(br->ignored_events() == 0);
  CHECK(br->panel_control_events() == 1 && br->panel_control_value(ids::FADER_1) == 100);  // the fader event reached the RT plane as a raw panel control
  CHECK(br->param_events_rejected() == 0);
}

// KeyboardOnParts::update_from follows the PartRouter rule exactly (ready, with an engine, keyboard on).
void test_keyboard_on_parts_from_snapshot() {
  core::Snapshot s = core::Snapshot::defaults();
  s.live_set = true;
  for (std::uint32_t p = 0; p < core::params::kPartCount; ++p) {
    s.parts[p].engine_type = 1;
    s.parts[p].state = core::PartState::kReady;
    s.parts[p].keyboard_on = 1;
  }
  s.parts[1].keyboard_on = 0;                              // keyboard off
  s.parts[2].engine_type = 0;                              // no engine
  s.parts[3].state = core::PartState::kMissingLibrary;              // not ready
  s.parts[5].keyboard_on = 0;
  s.parts[5].engine_type = 0;                              // both
  KeyboardOnParts k;
  k.update_from(s);
  core::PartRouter router;
  router.configure(s);
  std::uint16_t want = 0;
  for (std::uint32_t p = 0; p < core::params::kPartCount; ++p) {
    CHECK(((k.mask() >> p) & 1u) == (router.pedal_down_target(p) ? 1u : 0u));
    want = static_cast<std::uint16_t>(want | (router.pedal_down_target(p) ? 1u << p : 0u));
  }
  CHECK(k.mask() == want && k.mask() != 0 && k.mask() != 0xFF);
  CHECK((k.mask() & 0b101110) == 0);  // parts 1, 2, 3, 5 are out
}

void test_pitch_bend_ump() {
  const core::Ump u = core::make_pitch_bend(0, 3, 0x12345678u);
  CHECK(u.is_pitch_bend() && u.channel() == 3 && u.pitch_bend_value32() == 0x12345678u && !u.is_control_change() && !u.is_note_on());
  const core::Ump p = core::make_panel_control_event(0x2A, 0xFFFFu);
  CHECK(core::panel_control_id(p) == 0x2A && core::panel_control_value16(p) == 0xFFFFu && p.is_assignable_controller());
}

}  // namespace

// Story 3.12: set_led emits one LED_SET frame via the TX hook and stores the state; unknown ids are refused; a HELLO (first or after an MCU restart) replays the
// stored state; no TX hook still stores; the frames share the link's numbering with the time sync requests.
void test_leds() {
  Rig r(0b1);
  std::vector<mcu::Frame> tx_frames;
  mcu::StreamDecoder sim_rx;
  r.reader->set_tx([&](const std::uint8_t* d, std::size_t n) {
    for (auto& f : sim_rx.feed(std::vector<std::uint8_t>(d, d + n))) tx_frames.push_back(f);
  });
  auto leds_sent = [&] {
    std::vector<mcu::LedSet> v;
    for (const mcu::Frame& f : tx_frames) {
      auto m = mcu::parse_message(f);
      if (m && std::holds_alternative<mcu::LedSet>(*m)) v.push_back(std::get<mcu::LedSet>(*m));
    }
    return v;
  };
  namespace leds = mcu_ids::leds;
  // stored before the link is up of course also works (state kept, replayed below)
  CHECK(r.reader->set_led(leds::TOGGLE_LED_DRAWBARS, 255, 0, r.host0));
  CHECK(r.reader->set_led(leds::ENCODER_RING_3, 128, 77, r.host0 + kMs));
  CHECK(leds_sent().size() == 2);
  CHECK((leds_sent()[0] == mcu::LedSet{leds::TOGGLE_LED_DRAWBARS, 255, 0}) && (leds_sent()[1] == mcu::LedSet{leds::ENCODER_RING_3, 128, 77}));
  CHECK((r.reader->led(leds::ENCODER_RING_3) == LedValue{128, 77}));
  CHECK(!r.reader->led(leds::TOGGLE_LED_ALL));
  // unknown ids (a button id, an unused id) are refused: nothing stored, nothing sent
  CHECK(!r.reader->set_led(0x20 + 0x30, 255, 0));
  CHECK(!r.reader->set_led(9, 255, 0));
  CHECK(!r.reader->led(9));
  CHECK(leds_sent().size() == 2 && r.reader->counters().leds_rejected == 2);
  // the last value of an LED wins in the replay
  CHECK(r.reader->set_led(leds::TOGGLE_LED_DRAWBARS, 0, 0));
  CHECK(r.reader->set_led(leds::TOGGLE_LED_MIXER, 255, 0));
  tx_frames.clear();
  // the panel MCU says HELLO (boot): the stored state is replayed in id order, only LEDs that are not at the default (off, arg 0): DRAWBARS is off
  r.feed(mcu::Hello{1, mcu::kProtocolVersion, 2, 0}, r.host0 + 10 * kMs);
  auto rep = leds_sent();
  CHECK(rep.size() == 2);
  if (rep.size() == 2) {
    CHECK((rep[0] == mcu::LedSet{leds::ENCODER_RING_3, 128, 77}));
    CHECK((rep[1] == mcu::LedSet{leds::TOGGLE_LED_MIXER, 255, 0}));
  }
  CHECK(r.reader->counters().leds_replayed == 2);
  // the MCU restarts (a different HELLO): replayed again
  tx_frames.clear();
  r.feed(mcu::Hello{2, mcu::kProtocolVersion, 2, 0}, r.host0 + 20 * kMs);
  CHECK(leds_sent().size() == 2 && r.reader->counters().leds_replayed == 4);
  // a HELLO from another board (the keybed MCU) replays nothing
  tx_frames.clear();
  r.feed(mcu::Hello{1, mcu::kProtocolVersion, 1, 3}, r.host0 + 30 * kMs);
  CHECK(leds_sent().empty());
  // frame numbering is the link's: a time sync request and LED frames follow each other without a seq gap
  tx_frames.clear();
  r.reader->poll(r.host0 + 40 * kMs);
  CHECK(r.reader->set_led(leds::TOGGLE_LED_ALL, 10, 0));
  CHECK(tx_frames.size() == 2 && static_cast<std::uint16_t>(tx_frames[0].seq + 1) == tx_frames[1].seq);
  // no TX hook: stored, counted, nothing sent
  Rig n(0b1);
  CHECK(n.reader->set_led(leds::TOGGLE_LED_PART, 255, 0));
  CHECK((n.reader->led(leds::TOGGLE_LED_PART) == LedValue{255, 0}) && n.reader->counters().leds_unlinked == 1);
  // the RT plane is never involved
  StampedUmp u;
  CHECK(!n.q.pop(u));
}

int main() {
  test_pitch_bend_ump();
  test_keyboard_on_parts_from_snapshot();
  test_wheels();
  test_no_keyboard_parts();
  test_raw_controls();
  test_buttons();
  test_bad_frames();
  test_hello_and_restart();
  test_full_queue();
  test_unknown_button();
  test_leds();
  test_drift(+150.0);
  test_drift(-150.0);
  test_render_reaches_all_keyboard_parts();
  if (g_fail) std::printf("panel_reader_test: %d failure(s)\n", g_fail);
  else std::printf("panel_reader_test: ok\n");
  return g_fail ? 1 : 0;
}

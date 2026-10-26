// Pedals (Story 3.11): the mapping (polarity, calibration, deadband, hysteresis), the panel reader's pedal path (CC 64 / CC 11 to the keyboard-on parts in device
// time, the one-UMP-per-block coalescing, the note headroom lane, runtime config changes, the switch pedal), the calibration probe and routine (PedalCalibrator,
// with a fake sysd writer) and the ws.v1 routing of the three calibration commands. The end to end run with a real sysd and the virtual panel is
// sysd/tests/pedal_calibration_test.cpp; the half-pedal damping render is tools/offline-render/tests/pedal_half_pedal_test.cpp. Bare main, CHECK style.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/assign_events.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/panel_reader.hpp"
#include "velora/host/pedal_calibrator.hpp"
#include "velora/host/pedal_processor.hpp"

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
namespace ids = mcu_ids::controls;

struct SimPanel {
  std::uint32_t tick0 = 555000u;
  std::uint64_t t0_ns = 0;
  mcu::FrameWriter writer;
  std::vector<std::uint8_t> frame(const mcu::Message& m, std::uint64_t true_ns) {
    return writer.write(m, tick0 + static_cast<std::uint32_t>((true_ns - t0_ns) / 1000));
  }
};

struct Rig {
  HostEventQueue q;
  KeyboardOnParts parts;
  SimPanel mcu;
  std::unique_ptr<PanelReader> reader;
  std::vector<StampedUmp> out;
  std::vector<PanelButtonEvent> buttons;
  std::uint64_t host0 = 10 * kSec;
  explicit Rig(std::uint16_t mask = 0b100101) {  // parts 0, 2 and 5 sound with the keyboard
    mcu.t0_ns = host0;
    parts.set_mask(mask);
    reader = std::make_unique<PanelReader>(q, parts);
    reader->set_button_sink([this](const PanelButtonEvent& e) { buttons.push_back(e); });
  }
  void drain() {
    StampedUmp s;
    while (q.pop(s)) out.push_back(s);
  }
  void control(std::uint8_t id, std::uint16_t v, std::uint64_t t, bool drain_now = true) {
    const auto wire = mcu.frame(mcu::Control{id, v}, t);
    reader->on_bytes(wire.data(), wire.size(), t + 300'000);
    if (drain_now) drain();
  }
  // The controller UMPs (CC `cc`) of the output so far: (channel, value32, host_ns), in order.
  struct Cc {
    std::uint32_t ch, value;
    std::uint64_t ns;
  };
  std::vector<Cc> ccs(std::uint32_t cc) const {
    std::vector<Cc> v;
    for (const StampedUmp& s : out)
      if (s.ump.is_control_change() && s.ump.controller_index() == cc) v.push_back({s.ump.channel(), s.ump.controller_value32(), s.host_ns});
    return v;
  }
};

// --- the mapping ----------------------------------------------------------------------------------------

void test_mapping() {
  PedalCal full;
  CHECK(map_pedal(0, full) == 0 && map_pedal(4095, full) == 0xFFFFFFFFu);
  const PedalRange fr = effective_range(full);
  const std::uint16_t db = pedal_deadband(fr);
  CHECK(fr.lo == 0 && fr.hi == 4095 && db == 63);
  CHECK(map_pedal(db, full) == 0 && map_pedal(static_cast<std::uint16_t>(db + 1), full) > 0);  // dead zone at the up end
  CHECK(map_pedal(static_cast<std::uint16_t>(4095 - db), full) == 0xFFFFFFFFu && map_pedal(static_cast<std::uint16_t>(4095 - db - 1), full) < 0xFFFFFFFFu);
  const std::uint32_t mid = map_pedal(2048, full);
  CHECK(mid > 0x7FF00000u && mid < 0x80100000u);  // mid travel is mid value (within 0.05 %)
  // The knife edge: the arbiter holds notes from exactly half travel. Raw 2047 is still the half-pedal zone, 2048 is "down" (design limitation, README).
  CHECK(!core::Ump::sustain_down(map_pedal(2047, full)) && core::Ump::sustain_down(map_pedal(2048, full)));
  std::uint32_t prev = 0;
  bool mono = true;
  for (unsigned r = 0; r <= 4095; ++r) {
    const std::uint32_t v = map_pedal(static_cast<std::uint16_t>(r), full);
    mono = mono && v >= prev;
    prev = v;
  }
  CHECK(mono);
  // calibrated: the range is the player's, values outside it clamp
  PedalCal cal;
  cal.min = 500;
  cal.max = 3500;
  CHECK(map_pedal(500, cal) == 0 && map_pedal(0, cal) == 0 && map_pedal(3500, cal) == 0xFFFFFFFFu && map_pedal(4095, cal) == 0xFFFFFFFFu);
  const std::uint32_t cm = map_pedal(2000, cal);  // the middle of 500..3500
  CHECK(cm > 0x7FF00000u && cm < 0x80100000u);
  // inverted: the same travel, pressed = raw falls
  PedalCal inv = cal;
  inv.direction = PedalDirection::kInverted;
  CHECK(map_pedal(500, inv) == 0xFFFFFFFFu && map_pedal(3500, inv) == 0 && map_pedal(2000, inv) == cm);
  for (unsigned r = 500; r <= 3500; r += 37) CHECK(map_pedal(static_cast<std::uint16_t>(r), cal) == map_pedal(static_cast<std::uint16_t>(4000 - r), inv));
  // a range the engine cannot trust is uncalibrated (0 / 0, min >= max, a span under 256, above 4095)
  for (const PedalCal& bad : {PedalCal{0, 0}, PedalCal{900, 900}, PedalCal{900, 100}, PedalCal{1000, 1200}, PedalCal{0, 5000}}) {
    const PedalRange r = effective_range(bad);
    CHECK(r.lo == 0 && r.hi == 4095);
  }
  CHECK(effective_range(PedalCal{0, 256}).hi == 256);  // the smallest span accepted
  // the switch pedal
  CHECK(switch_closed(1, PedalDirection::kNormal) && !switch_closed(0, PedalDirection::kNormal));
  CHECK(!switch_closed(1, PedalDirection::kInverted) && switch_closed(0, PedalDirection::kInverted));
  // hysteresis: a resting pedal does not jitter, the ends still read exactly
  PedalChannel ch;
  CHECK(ch.update(2000, full) && !ch.update(2001, full) && !ch.update(1999, full) && ch.update(2002, full));
  PedalChannel rest;
  CHECK(rest.update(0, full) && rest.value() == 0);
  for (unsigned r : {1u, 0u, 3u, 1u, 2u, 0u, 5u, 2u}) CHECK(!rest.update(static_cast<std::uint16_t>(r), full) && rest.value() == 0);
  CHECK(rest.update(4095, full) && rest.value() == 0xFFFFFFFFu);
  CHECK(!rest.update(4094, full) && !rest.update(4093, full) && rest.value() == 0xFFFFFFFFu);
  CHECK(rest.update(0, full) && rest.value() == 0);
  // a one-count move that reaches an end state is not swallowed
  PedalChannel edge;
  edge.update(static_cast<std::uint16_t>(db + 1), full);
  CHECK(edge.value() > 0);
  CHECK(edge.update(db, full) && edge.value() == 0);
  // remap with a new calibration
  PedalChannel rm;
  rm.update(2048, full);
  const std::uint32_t before = rm.value();
  CHECK(rm.remap(cal) && rm.value() != before && rm.value() == map_pedal(2048, cal));
  CHECK(!rm.remap(cal));
}

// --- the reader --------------------------------------------------------------------------------------

// Two continuous sustain pedals that read in opposite directions (and have different travel) give the same CC 64 stream.
void test_opposite_polarity_sustain() {
  Rig a(0b1), b(0b1);
  PedalCal ca{300, 3800, PedalDirection::kNormal}, cb{200, 3700, PedalDirection::kInverted};
  a.reader->pedal_settings().set(PedalId::kSustain, ca);
  b.reader->pedal_settings().set(PedalId::kSustain, cb);
  std::uint64_t t = a.host0 + 5 * kMs;
  const double steps[] = {0.0, 0.02, 0.25, 0.5, 0.75, 0.98, 1.0, 0.6, 0.0};
  for (double f : steps) {
    a.control(ids::PEDAL_SUSTAIN, static_cast<std::uint16_t>(std::lround(300 + f * 3500)), t);
    b.control(ids::PEDAL_SUSTAIN, static_cast<std::uint16_t>(std::lround(3700 - f * 3500)), t);
    t += 3 * kMs;
  }
  const auto va = a.ccs(64), vb = b.ccs(64);
  CHECK(va.size() == vb.size() && va.size() >= 7);
  for (std::size_t i = 0; i < va.size() && i < vb.size(); ++i) CHECK(va[i].value == vb[i].value && va[i].ch == 0 && vb[i].ch == 0);
  CHECK(va.front().value == 0 && va.back().value == 0);
  bool saw_max = false, saw_mid = false;
  for (const auto& c : va) {
    saw_max = saw_max || c.value == 0xFFFFFFFFu;
    saw_mid = saw_mid || (c.value > 0x7F000000u && c.value < 0x81000000u);
  }
  CHECK(saw_max && saw_mid);  // fully down exactly max, mid travel mid value
  CHECK(a.reader->counters().sustain_ccs == va.size());
}

void test_expression_and_device_time() {
  Rig r;
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::PEDAL_EXPRESSION, 4095, t);
  r.control(ids::PEDAL_EXPRESSION, 0, t + 5 * kMs);
  const auto v = r.ccs(11);
  CHECK(v.size() == 6);
  std::vector<std::uint32_t> chans;
  for (std::size_t i = 0; i < v.size(); ++i) {
    CHECK(v[i].value == (i < 3 ? 0xFFFFFFFFu : 0u));
    CHECK(std::llabs(static_cast<long long>(v[i].ns) - static_cast<long long>(i < 3 ? t : t + 5 * kMs)) < 400'000);  // device time
    if (i < 3) chans.push_back(v[i].ch);
  }
  CHECK((chans == std::vector<std::uint32_t>{0, 2, 5}));
  for (const StampedUmp& s : r.out)
    if (s.ump.is_control_change()) CHECK(s.ump.group() == 0);
  CHECK(r.reader->counters().expression_ccs == 6);
  // the raw event is still published (Epic 4 binds it)
  std::size_t raws = 0;
  for (const StampedUmp& s : r.out)
    if (s.ump.is_assignable_controller() && core::panel_control_id(s.ump) == ids::PEDAL_EXPRESSION) ++raws;
  CHECK(raws == 2);
}

void test_no_keyboard_parts_then_join() {
  Rig r(0);
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::PEDAL_EXPRESSION, 3000, t);
  r.control(ids::PEDAL_SUSTAIN, 4095, t + 3 * kMs);
  CHECK(r.ccs(11).empty() && r.ccs(64).empty());
  CHECK(r.reader->counters().pedal_no_parts == 2);
  r.parts.set_mask(0b10);  // part 1 joins the keyboard: it takes the current pedal values with the next frame
  r.control(ids::FADER_1, 5, t + 6 * kMs);
  const auto e = r.ccs(11), s = r.ccs(64);
  CHECK(e.size() == 1 && e[0].ch == 1 && e[0].value == map_pedal(3000, PedalCal{}));
  CHECK(s.size() == 1 && s[0].ch == 1 && s[0].value == 0xFFFFFFFFu);
  CHECK(r.reader->counters().pedal_replays == 2);
}

// At most one UMP per pedal per part per block: values that arrive sooner are coalesced to the latest, which goes out with the next poll.
void test_coalescing() {
  Rig r(0b1);
  const std::uint64_t t = r.host0 + 5 * kMs;
  const std::uint16_t raws[] = {500, 900, 1400, 2000, 2600, 3200};
  std::uint64_t at = t;
  for (std::uint16_t v : raws) {
    r.control(ids::PEDAL_SUSTAIN, v, at);  // 100 us apart: all inside one block
    at += 100'000;
  }
  CHECK(r.ccs(64).size() == 1);  // the first goes at once, the rest wait
  CHECK(r.ccs(64)[0].value == map_pedal(500, PedalCal{}));
  r.reader->poll(t + 600'000);  // the interval (1.33 ms) has not passed since the first
  r.drain();
  CHECK(r.ccs(64).size() == 1);
  r.reader->poll(t + 2 * kMs);
  r.drain();
  const auto c = r.ccs(64);
  CHECK(c.size() == 2 && c[1].value == map_pedal(3200, PedalCal{}));  // the LATEST value
  CHECK(r.reader->counters().pedal_coalesced == 4);
  r.reader->poll(t + 4 * kMs);
  r.drain();
  CHECK(r.ccs(64).size() == 2);  // nothing pending any more
  // a burst of 200 frames at 50 us (10 ms): never more than one UMP per 1.33 ms
  Rig b(0b1);
  for (int i = 0; i < 200; ++i) b.control(ids::PEDAL_EXPRESSION, static_cast<std::uint16_t>(10 * i + 100), b.host0 + 5 * kMs + static_cast<std::uint64_t>(i) * 50'000);
  b.reader->poll(b.host0 + 30 * kMs);
  b.drain();
  const auto be = b.ccs(11);
  CHECK(be.size() <= 9 && be.size() >= 2);
  for (std::size_t i = 1; i + 1 < be.size(); ++i) CHECK(be[i].ns - be[i - 1].ns >= 1'333'333);
  CHECK(be.back().value == map_pedal(static_cast<std::uint16_t>(10 * 199 + 100), PedalCal{}));
}

// D1: two values in one block window, then silence: the trailing value is sent by the next on_bytes() once the window has passed (no poll needed), or by poll().
void test_trailing_value() {
  Rig r(0b1);
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::PEDAL_SUSTAIN, 500, t);
  r.control(ids::PEDAL_SUSTAIN, 3000, t + 100'000);  // same window: held back
  CHECK(r.ccs(64).size() == 1);
  r.control(ids::FADER_1, 1, t + 5 * kMs);  // any later frame, window passed
  CHECK(r.ccs(64).size() == 2 && r.ccs(64)[1].value == map_pedal(3000, PedalCal{}));
  Rig p(0b1);
  p.control(ids::PEDAL_EXPRESSION, 200, t);
  p.control(ids::PEDAL_EXPRESSION, 4095, t + 100'000);
  CHECK(p.ccs(11).size() == 1);
  p.reader->poll(t + 10 * kMs);
  p.drain();
  CHECK(p.ccs(11).size() == 2 && p.ccs(11)[1].value == 0xFFFFFFFFu);
}

// D2: the last keyboard-on part leaves while the sustain pedal is held: one CC 64 = 0 is owed (a pedal-up would otherwise reach nobody). A partial leave owes none.
void test_pedal_held_part_leaves() {
  Rig r(0b11);
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::PEDAL_SUSTAIN, 4095, t);
  r.parts.set_mask(0b10);  // part 0 leaves, part 1 stays: the router's pedal-up reaches every part by itself
  r.control(ids::FADER_1, 1, t + 4 * kMs);
  CHECK(r.ccs(64).size() == 2);
  r.parts.set_mask(0);  // the last one leaves
  r.control(ids::FADER_1, 2, t + 8 * kMs);
  const auto c = r.ccs(64);
  CHECK(c.size() == 3 && c[2].value == 0 && c[2].ch == 1);
  r.control(ids::PEDAL_SUSTAIN, 0, t + 12 * kMs);  // the pedal comes up with no part: nothing more, counted
  CHECK(r.ccs(64).size() == 3 && r.reader->counters().pedal_no_parts == 1);
  // no zero is owed for a pedal that is not held
  Rig u(0b1);
  u.control(ids::PEDAL_SUSTAIN, 0, t);
  u.parts.set_mask(0);
  u.control(ids::FADER_1, 1, t + 4 * kMs);
  CHECK(u.ccs(64).size() == 1);
}

// Pedals live in the note headroom: a queue that is nearly full sheds the faders but not the pedal; a queue that is completely full keeps the pedal owed and
// sends the LATEST value as soon as there is room.
void test_queue_pressure() {
  Rig r(0b101);
  const std::uint64_t t = r.host0;
  StampedUmp filler{core::make_note_off(0, 7, 60), 0};
  while (r.q.size() + kNoteHeadroom < HostEventQueue::capacity()) CHECK(r.q.push(filler));
  r.control(ids::FADER_1, 9, t + kMs, false);                 // shed
  r.control(ids::PEDAL_SUSTAIN, 4095, t + 2 * kMs, false);    // kept (note headroom)
  CHECK(r.reader->counters().controllers_dropped == 1);
  CHECK(r.reader->counters().sustain_ccs == 2);
  while (r.q.size() < HostEventQueue::capacity()) CHECK(r.q.push(filler));  // now completely full
  r.control(ids::PEDAL_SUSTAIN, 0, t + 5 * kMs, false);
  r.control(ids::PEDAL_SUSTAIN, 2400, t + 8 * kMs, false);
  CHECK(r.reader->counters().sustain_ccs == 2);  // nothing could be queued
  r.drain();
  r.out.clear();
  r.reader->poll(t + 20 * kMs);
  r.drain();
  const auto c = r.ccs(64);
  CHECK(c.size() == 2);  // one per part, the latest value (not the stale 4095 or 0)
  for (const auto& x : c) CHECK(x.value == map_pedal(2400, PedalCal{}));
}

// A new calibration / polarity from sysd applies from the next frame (or poll) with no restart, re-mapping the last raw value.
void test_runtime_config_change() {
  Rig r(0b1);
  const std::uint64_t t = r.host0 + 5 * kMs;
  r.control(ids::PEDAL_SUSTAIN, 2048, t);
  CHECK(r.ccs(64).size() == 1 && r.ccs(64)[0].value == map_pedal(2048, PedalCal{}));
  PedalCal cal{1000, 3000, PedalDirection::kNormal};
  r.reader->pedal_settings().set(PedalId::kSustain, cal);
  r.reader->poll(t + 10 * kMs);  // no pedal frame needed
  r.drain();
  CHECK(r.ccs(64).size() == 2 && r.ccs(64)[1].value == map_pedal(2048, cal));
  // polarity flipped by the player: the next frame uses it
  cal.direction = PedalDirection::kInverted;
  r.reader->pedal_settings().set(PedalId::kSustain, cal);
  r.control(ids::PEDAL_SUSTAIN, 1000, t + 20 * kMs);  // (the re-map of the old raw value went out first: this one is coalesced to the next poll)
  r.reader->poll(t + 25 * kMs);
  r.drain();
  CHECK(r.ccs(64).back().value == 0xFFFFFFFFu);
  // an identical setting changes nothing
  const std::size_t n = r.ccs(64).size();
  r.reader->pedal_settings().set(PedalId::kSustain, cal);
  r.reader->poll(t + 40 * kMs);
  r.drain();
  CHECK(r.ccs(64).size() == n);
}

void test_switch() {
  // NO wiring: raw 1 = closed. NC wiring: raw 0 = closed. The same presses give the same events.
  Rig no(0), nc(0);
  nc.reader->pedal_settings().set(PedalId::kSwitch, PedalCal{0, 0, PedalDirection::kInverted});
  std::uint64_t t = no.host0 + 5 * kMs;
  const int presses[] = {0, 1, 1, 0, 1, 0};  // closed? at each frame
  for (int p : presses) {
    no.control(ids::PEDAL_SWITCH, static_cast<std::uint16_t>(p), t);
    nc.control(ids::PEDAL_SWITCH, static_cast<std::uint16_t>(1 - p), t);
    t += 4 * kMs;
  }
  CHECK(no.buttons.size() == 4 && nc.buttons.size() == 4);
  for (std::size_t i = 0; i < no.buttons.size() && i < nc.buttons.size(); ++i) {
    CHECK(no.buttons[i].id == ids::PEDAL_SWITCH && nc.buttons[i].id == ids::PEDAL_SWITCH);
    CHECK(no.buttons[i].pressed == nc.buttons[i].pressed && no.buttons[i].pressed == (i % 2 == 0));  // press, release, press, release
  }
  // the raw event of both is normalised (1 = closed)
  std::vector<std::uint16_t> rn, rc;
  for (const StampedUmp& s : no.out) rn.push_back(core::panel_control_value16(s.ump));
  for (const StampedUmp& s : nc.out) rc.push_back(core::panel_control_value16(s.ump));
  CHECK(rn == rc && rn.size() == 6);
  CHECK(no.reader->counters().pedal_switch_events == 4);
  // a repeated state is not a new event
  no.control(ids::PEDAL_SWITCH, 0, t);
  CHECK(no.buttons.size() == 4);
}

void test_probe() {
  Rig r(0);
  PedalCalibrationProbe& p = r.reader->pedal_probe();
  CHECK(!p.active() && !p.read().valid);
  r.control(ids::PEDAL_SUSTAIN, 100, r.host0 + 5 * kMs);  // not armed: not measured
  CHECK(!p.read().valid);
  p.begin(PedalId::kSustain);
  CHECK(p.active() && !p.read().valid);
  std::uint64_t t = r.host0 + 10 * kMs;
  for (std::uint16_t v : {1500, 300, 3900, 2000}) {
    r.control(ids::PEDAL_SUSTAIN, v, t);
    t += 3 * kMs;
  }
  r.control(ids::PEDAL_EXPRESSION, 4000, t);  // another pedal: ignored
  PedalMeasurement m = p.read();
  CHECK(m.valid && m.min == 300 && m.max == 3900 && m.samples == 4);
  p.begin(PedalId::kSustain);  // a new run starts from nothing
  CHECK(!p.read().valid);
  r.control(ids::PEDAL_SUSTAIN, 2000, t + 3 * kMs);
  m = p.read();
  CHECK(m.valid && m.min == 2000 && m.max == 2000 && m.samples == 1);
  p.cancel();
  r.control(ids::PEDAL_SUSTAIN, 10, t + 6 * kMs);
  CHECK(p.read().min == 2000);
}

// D4: the run and the pedal it measures are one atomic word: switching runs never leaks the old pedal's frames into the new run.
void test_probe_run_switch() {
  PedalCalibrationProbe p;
  p.begin(PedalId::kSustain);
  p.observe(PedalId::kSustain, 100);
  p.observe(PedalId::kSustain, 900);
  CHECK(p.read().valid && p.read().min == 100 && p.read().max == 900);
  p.begin(PedalId::kExpression);  // a new run for another pedal
  CHECK(!p.read().valid);
  p.observe(PedalId::kSustain, 5);  // the old pedal is no longer measured
  CHECK(!p.read().valid);
  p.observe(PedalId::kExpression, 2000);
  p.observe(PedalId::kExpression, 2500);
  CHECK(p.read().valid && p.read().min == 2000 && p.read().max == 2500 && p.read().samples == 2);
  p.cancel();
  CHECK(!p.active());
  p.observe(PedalId::kExpression, 1);
  CHECK(p.read().min == 2000);
}

// --- the calibration routine -------------------------------------------------------------------------

struct CalRig {
  Rig rig{0b1};
  std::uint64_t now = 1 * kSec;
  int writes = 0;
  ws::v1::ConfigPatch last_patch;
  SysdSetResult next;
  PedalCalibrator cal;
  CalRig()
      : cal(rig.reader->pedal_probe(),
            [this](const ws::v1::ConfigPatch& p) {
              ++writes;
              last_patch = p;
              return next;
            },
            [this] { return now; }) {
    next.status = SysdSetResult::Status::kOk;
    next.revision = 7;
  }
  void press(PedalId id, std::initializer_list<std::uint16_t> values) {
    std::uint64_t t = rig.host0 + 5 * kMs + (++seq) * 100 * kMs;
    for (std::uint16_t v : values) {
      rig.control(id == PedalId::kSustain ? ids::PEDAL_SUSTAIN : ids::PEDAL_EXPRESSION, v, t);
      t += 3 * kMs;
    }
  }
  std::uint64_t seq = 0;
};

void test_calibrator() {
  using R = ws::v1::PedalCalibrationResult;
  {  // stored: only min / max of the pedal, the polarity is not part of the patch
    CalRig c;
    CHECK(c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN).result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING && c.cal.collecting());
    c.press(PedalId::kSustain, {2000, 150, 3900, 400});
    const auto r = c.cal.finish();
    CHECK(r.result() == R::PEDAL_CALIBRATION_RESULT_STORED && r.min() == 150 && r.max() == 3900 && r.config_revision() == 7);
    CHECK(c.writes == 1 && c.last_patch.has_sustain() && !c.last_patch.has_expression());
    CHECK(c.last_patch.sustain().min() == 150 && c.last_patch.sustain().max() == 3900 && !c.last_patch.sustain().has_polarity());
    CHECK(!c.cal.collecting() && !c.rig.reader->pedal_probe().active());
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
  }
  {  // expression goes to the expression field
    CalRig c;
    c.cal.start(ws::v1::PEDAL_KIND_EXPRESSION);
    c.press(PedalId::kExpression, {100, 3000});
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_STORED && c.last_patch.has_expression() && !c.last_patch.has_sustain());
  }
  {  // too small a span: typed refusal, nothing written
    CalRig c;
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {1000, 1255});  // 255 counts
    const auto r = c.cal.finish();
    CHECK(r.result() == R::PEDAL_CALIBRATION_RESULT_TOO_SMALL_SPAN && r.min() == 1000 && r.max() == 1255 && c.writes == 0);
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {1000, 1256});  // 256 is enough
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_STORED && c.writes == 1);
  }
  {  // no movement
    CalRig c;
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT);
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {777, 777, 777});
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT && c.writes == 0);
  }
  {  // cancel, a second start replaces the first, another pedal's frames do not count
    CalRig c;
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {0, 4000});
    CHECK(c.cal.cancel().result() == R::PEDAL_CALIBRATION_RESULT_CANCELLED && !c.cal.collecting());
    CHECK(c.cal.cancel().result() == R::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_NOT_RUNNING && c.writes == 0);
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {0, 4000});
    c.cal.start(ws::v1::PEDAL_KIND_EXPRESSION);  // replaces: the sustain frames above are not carried over
    c.press(PedalId::kSustain, {0, 4000});
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT);
  }
  {  // timeout
    CalRig c;
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {0, 4000});
    c.now += PedalCalibrator::kWindowNs + 1;
    CHECK(c.cal.finish().result() == R::PEDAL_CALIBRATION_RESULT_TIMED_OUT && c.writes == 0 && !c.cal.collecting());
  }
  {  // sysd problems: typed, nothing stored
    CalRig c;
    c.next.status = SysdSetResult::Status::kRefused;
    c.next.code = ws::v1::ERROR_CODE_INVALID_CONFIG;
    c.next.reason = "span_too_small";
    c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
    c.press(PedalId::kSustain, {0, 4000});
    auto r = c.cal.finish();
    CHECK(r.result() == R::PEDAL_CALIBRATION_RESULT_SYSD_REFUSED && r.detail().find("span_too_small") != std::string::npos);
    for (auto st : {SysdSetResult::Status::kNotConnected, SysdSetResult::Status::kTimeout, SysdSetResult::Status::kLost, SysdSetResult::Status::kBusy}) {
      c.next.status = st;
      c.cal.start(ws::v1::PEDAL_KIND_SUSTAIN);
      c.press(PedalId::kSustain, {0, 4000});
      r = c.cal.finish();
      CHECK(r.result() == R::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE && r.min() == 0 && r.max() == 4000);
    }
    PedalCalibrationProbe probe;
    PedalCalibrator nowriter(probe, {}, [] { return 0ull; });
    nowriter.start(ws::v1::PEDAL_KIND_SUSTAIN);
    probe.observe(PedalId::kSustain, 10);
    probe.observe(PedalId::kSustain, 4000);
    CHECK(nowriter.finish().result() == R::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE);
  }
  {  // the protocol: an unknown pedal is a typed error, the others reply
    CalRig c;
    ws::v1::ClientMessage m;
    ws::v1::ServerMessage rep;
    HandlerFailure hf;
    m.mutable_start_pedal_calibration()->set_pedal(ws::v1::PEDAL_KIND_UNSPECIFIED);
    CHECK(!c.cal.run(m, rep, hf) && hf.code == ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE && !c.cal.collecting());
    m.mutable_start_pedal_calibration()->set_pedal(ws::v1::PEDAL_KIND_SUSTAIN);
    CHECK(c.cal.run(m, rep, hf) && rep.pedal_calibration_reply().result() == R::PEDAL_CALIBRATION_RESULT_COLLECTING);
  }
}

// apply_pedal_config: sysd's Config -> the reader's settings.
void test_apply_config() {
  PedalSettings s;
  ws::v1::Config c = default_config();
  apply_pedal_config(c, s, PedalDirection::kInverted);
  CHECK(s.get(PedalId::kSustain) == PedalCal{} && s.get(PedalId::kExpression) == PedalCal{});
  CHECK(s.get(PedalId::kSwitch).direction == PedalDirection::kInverted);
  c.mutable_sustain()->set_min(120);
  c.mutable_sustain()->set_max(3900);
  c.mutable_sustain()->set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED);
  c.mutable_expression()->set_min(10);
  c.mutable_expression()->set_max(5000);  // not trusted: uncalibrated
  c.mutable_expression()->set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_OPEN);
  const std::uint32_t v0 = s.version();
  apply_pedal_config(c, s);
  CHECK(s.version() > v0);
  CHECK(s.get(PedalId::kSustain) == (PedalCal{120, 3900, PedalDirection::kInverted}));
  CHECK(s.get(PedalId::kExpression) == PedalCal{});
  const std::uint32_t v1 = s.version();
  apply_pedal_config(c, s);
  CHECK(s.version() == v1);  // same config: nothing changed
}

// --- the ws.v1 routing ---------------------------------------------------------------------------------

void test_plane_routing() {
  auto br = std::make_unique<core::BlockRenderer>();
  auto ctl_q = std::make_unique<HostEventQueue>();
  ControlPlane plane{br->snapshots(), br->last_values(), *ctl_q, [] { return 1ull; }};
  bool changed = false;
  ws::v1::ClientMessage m;
  m.set_request_id(5);
  m.mutable_start_pedal_calibration()->set_pedal(ws::v1::PEDAL_KIND_SUSTAIN);
  ws::v1::ServerMessage r = plane.handle(m, changed);  // no panel reader: a typed error
  CHECK(r.request_id() == 5 && r.has_error() && r.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND && r.error().details().at("reason") == "pedal_calibration_unavailable");
  PedalCalibrationProbe probe;
  std::uint64_t now = 0;
  int writes = 0;
  PedalCalibrator cal(probe, [&](const ws::v1::ConfigPatch&) {
    ++writes;
    SysdSetResult w;
    w.status = SysdSetResult::Status::kOk;
    w.revision = 3;
    return w;
  }, [&] { return now; });
  plane.set_pedal_handler(&cal);
  r = plane.handle(m, changed);
  CHECK(r.request_id() == 5 && r.has_pedal_calibration_reply() && r.pedal_calibration_reply().result() == ws::v1::PEDAL_CALIBRATION_RESULT_COLLECTING);
  probe.observe(PedalId::kSustain, 90);
  probe.observe(PedalId::kSustain, 3990);
  const std::uint64_t token = r.pedal_calibration_reply().run_token();
  CHECK(token != 0);
  ws::v1::ClientMessage f;
  f.set_request_id(6);
  f.mutable_finish_pedal_calibration()->set_run_token(token + 1);  // another client's Start cannot be ended with the wrong token
  r = plane.handle(f, changed);
  CHECK(r.has_error() && r.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND && r.error().details().at("reason") == "wrong_run_token" && writes == 0);
  f.mutable_finish_pedal_calibration()->set_run_token(0);
  CHECK(plane.handle(f, changed).has_error());
  ws::v1::ClientMessage cw;
  cw.mutable_cancel_pedal_calibration()->set_run_token(token + 1);
  CHECK(plane.handle(cw, changed).has_error() && cal.collecting());
  m.mutable_start_pedal_calibration()->set_pedal(ws::v1::PEDAL_KIND_SUSTAIN);  // a second Start replaces the run with a new token
  r = plane.handle(m, changed);
  CHECK(r.pedal_calibration_reply().run_token() != token && r.pedal_calibration_reply().run_token() != 0);
  CHECK(plane.handle(f, changed).has_error());  // the first owner's token is dead (token 0 / the old one)
  f.mutable_finish_pedal_calibration()->set_run_token(token);
  CHECK(plane.handle(f, changed).has_error());
  probe.observe(PedalId::kSustain, 90);
  probe.observe(PedalId::kSustain, 3990);
  f.mutable_finish_pedal_calibration()->set_run_token(r.pedal_calibration_reply().run_token());
  r = plane.handle(f, changed);
  CHECK(r.has_pedal_calibration_reply() && r.pedal_calibration_reply().result() == ws::v1::PEDAL_CALIBRATION_RESULT_STORED && r.pedal_calibration_reply().config_revision() == 3 && writes == 1);
  ws::v1::ClientMessage c;
  c.set_request_id(7);
  c.mutable_cancel_pedal_calibration()->set_run_token(1);
  r = plane.handle(c, changed);
  CHECK(r.has_pedal_calibration_reply() && r.pedal_calibration_reply().result() == ws::v1::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
  m.mutable_start_pedal_calibration()->set_pedal(static_cast<ws::v1::PedalKind>(9));
  r = plane.handle(m, changed);
  CHECK(r.has_error() && r.error().code() == ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  plane.set_pedal_handler(nullptr);
}

}  // namespace

int main() {
  test_mapping();
  test_opposite_polarity_sustain();
  test_expression_and_device_time();
  test_no_keyboard_parts_then_join();
  test_coalescing();
  test_trailing_value();
  test_pedal_held_part_leaves();
  test_queue_pressure();
  test_runtime_config_change();
  test_switch();
  test_probe();
  test_probe_run_switch();
  test_calibrator();
  test_apply_config();
  test_plane_routing();
  std::printf("pedal_test: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}

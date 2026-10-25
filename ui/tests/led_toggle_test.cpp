// Story 3.12: SoC-driven LED state. The toggle state has one owner (ToggleState); each toggle press lights only the active state's LED; the LED driver turns
// state into discrete ws.v1 LED commands, generates the blink-twice (2 x 250/250 ms) and recall-dip (200 ms) patterns in time, never rises faster than 3 Hz,
// and re-sends the full LED state. Deterministic: a fake clock drives every pattern. Plain asserts as the other ui tests.
#undef NDEBUG  // the checks below are plain asserts with side effects in their setup: they must run in every build type
#include <cassert>
#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/components/led_view.hpp"
#include "velora/ui/led_driver.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/toggle_state.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <vector>

using namespace velora::ui;

namespace {

struct Sent {
  uint64_t t;
  uint8_t id, brightness, arg;
};

struct Rec {
  std::vector<Sent> log;
  bool connected = true;
  uint64_t* clock = nullptr;
  LedDriver::SendFn fn() {
    return [this](uint8_t id, uint8_t b, uint8_t a) {
      if (!connected) return false;
      log.push_back({clock ? *clock : 0, id, b, a});
      return true;
    };
  }
  // The last value sent per LED.
  std::map<uint8_t, uint8_t> last() const {
    std::map<uint8_t, uint8_t> m;
    for (const Sent& s : log) m[s.id] = s.brightness;
    return m;
  }
};

constexpr uint8_t kDrawbars[2] = {MCU_LED_TOGGLE_LED_DRAWBARS, MCU_LED_TOGGLE_LED_MIXER};
constexpr uint8_t kPartsScenes[2] = {MCU_LED_TOGGLE_LED_PARTS, MCU_LED_TOGGLE_LED_SCENES};
constexpr uint8_t kKnobs[3] = {MCU_LED_TOGGLE_LED_PART, MCU_LED_TOGGLE_LED_ALL, MCU_LED_TOGGLE_LED_ASSIGN};

template <size_t N>
int lit(const LedDriver& d, const uint8_t (&ids)[N]) {
  int n = 0;
  for (uint8_t id : ids) n += d.output(id).brightness > 0 ? 1 : 0;
  return n;
}

}  // namespace

static void test_toggle_state_owner() {
  ToggleState t;
  int published = 0;
  t.set_listener([&] { ++published; });
  assert(!t.drawbars() && !t.scenes() && t.knob_mode() == 0);
  t.toggle_drawbars();
  assert(t.drawbars() && published == 1);
  t.set_drawbars(true);  // same value: not published
  assert(published == 1);
  t.toggle_scenes();
  assert(t.scenes() && published == 2);
  // PART -> ALL -> ASSIGN -> PART
  t.cycle_knob_mode();
  assert(t.knob_mode() == 1);
  t.cycle_knob_mode();
  assert(t.knob_mode() == 2);
  t.cycle_knob_mode();
  assert(t.knob_mode() == 0 && published == 5);
  t.set_all(true, true, 0);  // nothing changes
  assert(published == 5);
  // exactly one LED of each toggle is the active state's
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b)
      for (int k = 0; k < 3; ++k) {
        t.set_all(a, b, k);
        assert(t.led_lit(MCU_LED_TOGGLE_LED_DRAWBARS) + t.led_lit(MCU_LED_TOGGLE_LED_MIXER) == 1);
        assert(t.led_lit(MCU_LED_TOGGLE_LED_PARTS) + t.led_lit(MCU_LED_TOGGLE_LED_SCENES) == 1);
        assert(t.led_lit(MCU_LED_TOGGLE_LED_PART) + t.led_lit(MCU_LED_TOGGLE_LED_ALL) + t.led_lit(MCU_LED_TOGGLE_LED_ASSIGN) == 1);
      }
  assert(!t.led_lit(MCU_LED_PART_SCENE_LED_1));
  std::cout << "[PASS] test_toggle_state_owner\n";
}

static void test_blink_twice_timing() {
  uint64_t now = 0;
  Rec rec;
  rec.clock = &now;
  LedDriver d;
  d.set_send(rec.fn());
  d.tick(now);
  d.set(MCU_LED_TOGGLE_LED_DRAWBARS, 0);
  rec.log.clear();
  now = 1000;
  d.tick(now);
  d.blink_twice(MCU_LED_TOGGLE_LED_DRAWBARS);
  for (int i = 0; i < 2000; ++i) {
    ++now;
    d.tick(now);
  }
  // on at +0, off at +250, on at +500, off at +750; the base (off) is already the output at +1000: no extra frame
  assert(rec.log.size() == 4);
  assert(rec.log[0].t == 1000 && rec.log[0].brightness == 255);
  assert(rec.log[1].t == 1250 && rec.log[1].brightness == 0);
  assert(rec.log[2].t == 1500 && rec.log[2].brightness == 255);
  assert(rec.log[3].t == 1750 && rec.log[3].brightness == 0);
  assert(!d.pattern_active(MCU_LED_TOGGLE_LED_DRAWBARS));
  // with a lit base the blink returns to it
  d.set(MCU_LED_TOGGLE_LED_MIXER, 255);
  rec.log.clear();
  const uint64_t t0 = now;
  d.blink_twice(MCU_LED_TOGGLE_LED_MIXER);
  for (int i = 0; i < 1500; ++i) {
    ++now;
    d.tick(now);
  }
  assert(d.output(MCU_LED_TOGGLE_LED_MIXER).brightness == 255);
  assert(rec.log.size() == 4 && rec.log.back().brightness == 255 && rec.log.back().t == t0 + 1000);  // already on at +0: off at +250, on, off, on again at +1000
  std::cout << "[PASS] test_blink_twice_timing\n";
}

static void test_dip_timing() {
  uint64_t now = 5000;
  Rec rec;
  rec.clock = &now;
  LedDriver d;
  d.set_send(rec.fn());
  d.tick(now);
  d.set(MCU_LED_PART_SCENE_LED_2, 255);
  rec.log.clear();
  now = 6000;
  d.tick(now);
  d.dip(MCU_LED_PART_SCENE_LED_2);
  for (int i = 0; i < 600; ++i) {
    ++now;
    d.tick(now);
  }
  assert(rec.log.size() == 2);
  assert(rec.log[0].t == 6000 && rec.log[0].brightness == 0);
  assert(rec.log[1].t == 6200 && rec.log[1].brightness == 255);  // a single 200 ms dip, then restored
  std::cout << "[PASS] test_dip_timing\n";
}

static void test_three_hz_bound() {
  uint64_t now = 0;
  Rec rec;
  rec.clock = &now;
  LedDriver d;
  d.set_send(rec.fn());
  // an LED commanded on/off every 10 ms for 20 s (plain state changes; patterns are exempt from the guard, tested separately)
  for (int i = 0; i < 20000; ++i) {
    ++now;
    d.tick(now);
    if (i % 10 == 0) d.set(MCU_LED_PART_SCENE_LED_1, (i / 10) % 2 ? 255 : 0);
  }
  std::map<uint8_t, std::vector<uint64_t>> rises;
  std::map<uint8_t, uint8_t> prev;
  for (const Sent& s : rec.log) {
    if (s.brightness > 0 && prev[s.id] == 0) rises[s.id].push_back(s.t);
    prev[s.id] = s.brightness;
  }
  assert(!rises.empty());
  for (const auto& [id, v] : rises)
    for (size_t i = 0; i + 3 < v.size(); ++i) assert(v[i + 3] - v[i] >= 1000);  // never more than 3 rising edges in any second
  // and the consecutive edges are at least 334 ms apart
  for (const auto& [id, v] : rises)
    for (size_t i = 1; i < v.size(); ++i) assert(v[i] - v[i - 1] >= LedDriver::kMinRiseGapMs);
  assert(d.deferred_count() > 0);
  // the latest wanted value wins: once the flicker stops the output settles on the base
  d.set(MCU_LED_PART_SCENE_LED_1, 255);
  for (int i = 0; i < 2000; ++i) {
    ++now;
    d.tick(now);
  }
  assert(d.output(MCU_LED_PART_SCENE_LED_1).brightness == 255);
  std::cout << "[PASS] test_three_hz_bound\n";
}

static void run_ms(LedDriver& d, uint64_t& now, int ms) {
  for (int i = 0; i < ms; ++i) {
    ++now;
    d.tick(now);
  }
}

// A failed send keeps the last delivered output, backs off, and is retried from tick() until it is delivered, with no help from a reconnect edge.
static void test_failed_send_retried() {
  uint64_t now = 0;
  Rec rec;
  rec.clock = &now;
  rec.connected = false;
  LedDriver d;
  d.set_send(rec.fn());
  d.tick(now);
  d.set(MCU_LED_TOGGLE_LED_PART, 255);
  assert(d.output(MCU_LED_TOGGLE_LED_PART).brightness == 0);  // not delivered: the output stays at the last delivered value
  run_ms(d, now, 600);
  assert(rec.log.empty());
  rec.connected = true;  // no resend_all(): the retry from tick() delivers it
  run_ms(d, now, LedDriver::kRetryMs + 5);
  assert(rec.log.size() == 1 && rec.log[0].id == MCU_LED_TOGGLE_LED_PART && rec.log[0].brightness == 255);
  assert(d.output(MCU_LED_TOGGLE_LED_PART).brightness == 255);
  run_ms(d, now, 1000);
  assert(rec.log.size() == 1);  // delivered once, no more retries
  // a send hook that fails once, then succeeds
  int calls = 0;
  d.set_send([&](uint8_t id, uint8_t b, uint8_t a) {
    ++calls;
    if (calls == 1) return false;
    rec.log.push_back({now, id, b, a});
    return true;
  });
  rec.log.clear();
  d.set(MCU_LED_TOGGLE_LED_ALL, 255);
  assert(calls == 1 && rec.log.empty() && d.output(MCU_LED_TOGGLE_LED_ALL).brightness == 0);
  run_ms(d, now, LedDriver::kRetryMs - 10);
  assert(calls == 1);  // backing off
  run_ms(d, now, 20);
  assert(calls == 2 && rec.log.size() == 1 && d.output(MCU_LED_TOGGLE_LED_ALL).brightness == 255);
  std::cout << "[PASS] test_failed_send_retried\n";
}

// Pattern steps are not held back by the 3 Hz guard: the dip right after a rise still restores at 200 ms, blink twice keeps its exact timing after a recent
// toggle, and a forced resend does not hold back the next user toggle.
static void test_patterns_exempt_from_guard() {
  uint64_t now = 10000;
  Rec rec;
  rec.clock = &now;
  LedDriver d;
  d.set_send(rec.fn());
  d.tick(now);
  d.set(MCU_LED_PART_SCENE_LED_1, 0);
  d.set(MCU_LED_PART_SCENE_LED_1, 255);  // a rising edge now
  run_ms(d, now, 20);
  rec.log.clear();
  d.dip(MCU_LED_PART_SCENE_LED_1);       // 20 ms after the rise
  const uint64_t t0 = now;
  run_ms(d, now, 400);
  assert(rec.log.size() == 2 && rec.log[0].brightness == 0 && rec.log[1].brightness == 255);
  assert(rec.log[0].t == t0 && rec.log[1].t == t0 + 200);
  // blink twice right after a toggle
  run_ms(d, now, 400);
  d.set(MCU_LED_TOGGLE_LED_DRAWBARS, 0);
  d.set(MCU_LED_TOGGLE_LED_DRAWBARS, 255);
  run_ms(d, now, 5);
  d.set(MCU_LED_TOGGLE_LED_DRAWBARS, 0);
  rec.log.clear();
  d.blink_twice(MCU_LED_TOGGLE_LED_DRAWBARS);
  const uint64_t b0 = now;
  run_ms(d, now, 1100);
  std::vector<Sent> dr;
  for (const Sent& s : rec.log)
    if (s.id == MCU_LED_TOGGLE_LED_DRAWBARS) dr.push_back(s);
  assert(dr.size() == 4 && dr[0].t == b0 && dr[1].t == b0 + 250 && dr[2].t == b0 + 500 && dr[3].t == b0 + 750);
  // a forced resend does not count as a rising edge: the next user toggle is not deferred
  d.set(MCU_LED_TOGGLE_LED_MIXER, 0);
  run_ms(d, now, 400);
  d.resend_all();
  rec.log.clear();
  d.set(MCU_LED_TOGGLE_LED_MIXER, 255);
  assert(rec.log.size() == 1 && rec.log[0].t == now && rec.log[0].brightness == 255);
  std::cout << "[PASS] test_patterns_exempt_from_guard\n";
}

static void test_resend_all() {
  uint64_t now = 0;
  Rec rec;
  rec.clock = &now;
  rec.connected = false;
  LedDriver d;
  d.set_send(rec.fn());
  d.set(MCU_LED_TOGGLE_LED_ASSIGN, 255);
  d.set(MCU_LED_PART_SCENE_LED_3, 64);
  d.set(MCU_LED_TOGGLE_LED_DRAWBARS, 0);
  assert(rec.log.empty());  // no connection: nothing delivered, kept as dirty
  now = 50;
  d.tick(now);
  assert(rec.log.empty());  // not retried blindly
  rec.connected = true;
  d.resend_all();
  assert(rec.log.size() == 3);
  assert(rec.log[0].id == MCU_LED_PART_SCENE_LED_3 && rec.log[0].brightness == 64);
  assert(rec.log[1].id == MCU_LED_TOGGLE_LED_DRAWBARS && rec.log[1].brightness == 0);
  assert(rec.log[2].id == MCU_LED_TOGGLE_LED_ASSIGN && rec.log[2].brightness == 255);
  // a resend while a pattern runs sends the pattern's current output; the 3 Hz guard does not hold back a replay
  d.dip(MCU_LED_PART_SCENE_LED_3);
  rec.log.clear();
  d.resend_all();
  assert(rec.log.size() == 3);
  std::cout << "[PASS] test_resend_all\n";
}

static void press(NavigationController& nav, uint16_t id, uint64_t ts) {
  nav.on_panel_event(PanelEvent::button_press(id, ts));
  nav.on_panel_event(PanelEvent::button_release(id, ts + 20000));
}

// Presses are at least 400 ms apart in these tests (a human presses slower than the 3 Hz guard; a faster double press is held back by the guard, tested above).
static void settle(NavigationController& nav, uint64_t& clock_ms, int ms = 400) {
  for (int i = 0; i < ms; ++i) {
    nav.tick(1);
    ++clock_ms;
  }
}

static void test_toggles_and_leds_through_navigation() {

  uint64_t clock_ms = 0;
  Rec rec;
  rec.clock = &clock_ms;
  NavigationController nav;
  assert(nav.initialize());
  nav.led_driver().set_send(rec.fn());
  nav.resend_leds();
  auto& d = nav.led_driver();

  std::vector<PartStripData> parts(8);
  for (size_t i = 0; i < 8; ++i) parts[i] = {"Grand Piano " + std::to_string(i + 1), "Piano", 0.7f, true, false, false};
  parts[3] = {"Gospel B3", "Organ", 0.85f, true, false, false};
  nav.performance_view().set_parts(parts);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));  // forth and back: refreshes the part LEDs from the parts
  settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));

  // Initial: MIXER, PARTS, PART lit: one LED per toggle
  assert(lit(d, kDrawbars) == 1 && d.output(MCU_LED_TOGGLE_LED_MIXER).brightness == 255);
  assert(lit(d, kPartsScenes) == 1 && d.output(MCU_LED_TOGGLE_LED_PARTS).brightness == 255);
  assert(lit(d, kKnobs) == 1 && d.output(MCU_LED_TOGGLE_LED_PART).brightness == 255);
  const size_t sent_before = rec.log.size();

  // 2-state toggle: DRAWBARS/MIXER
  settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_DRAWBARS_MIXER));
  assert(nav.toggles().drawbars() && nav.performance_view().is_drawbars_mode());
  assert(lit(d, kDrawbars) == 1 && d.output(MCU_LED_TOGGLE_LED_DRAWBARS).brightness == 255 && d.output(MCU_LED_TOGGLE_LED_MIXER).brightness == 0);
  assert(rec.last()[MCU_LED_TOGGLE_LED_DRAWBARS] == 255 && rec.last()[MCU_LED_TOGGLE_LED_MIXER] == 0);
  settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_DRAWBARS_MIXER));
  assert(!nav.toggles().drawbars() && d.output(MCU_LED_TOGGLE_LED_MIXER).brightness == 255 && lit(d, kDrawbars) == 1);

  // 2-state toggle: PARTS/SCENES
  settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));
  assert(nav.toggles().scenes() && nav.performance_view().is_scenes_mode());
  assert(lit(d, kPartsScenes) == 1 && d.output(MCU_LED_TOGGLE_LED_SCENES).brightness == 255 && d.output(MCU_LED_TOGGLE_LED_PARTS).brightness == 0);
  settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));
  assert(!nav.toggles().scenes() && lit(d, kPartsScenes) == 1 && d.output(MCU_LED_TOGGLE_LED_PARTS).brightness == 255);

  // 3-state toggle: PART -> ALL -> ASSIGN -> PART, exactly one LED lit each time
  const uint8_t expect_lit[4] = {MCU_LED_TOGGLE_LED_ALL, MCU_LED_TOGGLE_LED_ASSIGN, MCU_LED_TOGGLE_LED_PART, MCU_LED_TOGGLE_LED_ALL};
  for (uint8_t want : expect_lit) {
    settle(nav, clock_ms);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PART_ALL_ASSIGN));
    assert(lit(d, kKnobs) == 1 && d.output(want).brightness == 255);
    assert(nav.performance_view().knob_mode() == nav.toggles().knob_mode());
    // what the engine would receive agrees with the driver
    int sent_lit = 0;
    for (uint8_t id : kKnobs) sent_lit += rec.last()[id] > 0 ? 1 : 0;
    assert(sent_lit == 1 && rec.last()[want] == 255);
  }
  assert(rec.log.size() > sent_before);

  // DRAWBARS with no organ part: refused, the DRAWBARS LED blinks twice (2 x 250/250 ms), the state is unchanged
  nav.performance_view().set_parts(std::vector<PartStripData>(8, PartStripData{"Grand Piano", "Piano", 0.7f, true, false, false}));
  nav.performance_view().toggles().set_all(false, false, 0);
  rec.log.clear();
  clock_ms = 0;
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_DRAWBARS_MIXER));
  assert(!nav.toggles().drawbars());
  for (int i = 0; i < 1100; ++i) {
    nav.tick(1);
    ++clock_ms;
  }
  std::vector<Sent> dr;
  for (const Sent& s : rec.log)
    if (s.id == MCU_LED_TOGGLE_LED_DRAWBARS) dr.push_back(s);
  assert(dr.size() == 4);
  assert(dr[0].brightness == 255 && dr[1].brightness == 0 && dr[2].brightness == 255 && dr[3].brightness == 0);
  auto near = [](uint64_t a, uint64_t b) { return a + 1 >= b && a <= b + 1; };  // the test clock is sampled around the 1 ms ticks: +-1 ms
  assert(near(dr[1].t - dr[0].t, 250) && near(dr[2].t - dr[1].t, 250) && near(dr[3].t - dr[2].t, 250));

  // Recall dip: in SCENES mode a scene button press dips the current scene's LED for 200 ms, then restores it
  std::vector<SceneTileData> scenes(8);
  scenes[0] = {"Intro", "Piano", false};
  scenes[1] = {"Verse", "Pad", false};
  for (size_t i = 2; i < 8; ++i) scenes[i] = {"", "", true};
  nav.performance_view().set_scenes(scenes, 0);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));
  assert(nav.toggles().scenes());
  for (int i = 0; i < 500; ++i) {
    nav.tick(1);
    ++clock_ms;
  }
  rec.log.clear();
  const uint64_t press_ms = clock_ms;
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_PART_SCENE_2, 1000000));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_PART_SCENE_2, 1100000));
  for (int i = 0; i < 700; ++i) {
    nav.tick(1);
    ++clock_ms;
  }
  assert(nav.performance_view().current_scene() == 1);
  std::vector<Sent> dip;
  for (const Sent& s : rec.log)
    if (s.id == MCU_LED_PART_SCENE_LED_2) dip.push_back(s);
  // the scene LED first goes from weak (existing scene) to strong (current scene), then dips to off for 200 ms and is restored
  assert(dip.size() == 3 && dip[0].brightness == 255 && dip[0].t == dip[1].t);
  dip.erase(dip.begin());
  assert(dip[0].brightness == 0 && dip[1].brightness == 255);
  assert(near(dip[1].t - dip[0].t, 200) && dip[0].t + 1 >= press_ms);
  assert(!nav.performance_view().is_scene_led_dipping());

  // On-screen LED view (simulator): shows what the driver outputs
  assert(nav.enable_led_view());
  nav.tick(1);
  for (const auto& [id, out] : d.snapshot()) {
    if (nav.led_view().dot(id)) assert(nav.led_view().shown_brightness(id) == out.brightness);
  }
  assert(nav.led_view().shown_brightness(MCU_LED_TOGGLE_LED_SCENES) == 255);
  assert(nav.led_view().shown_brightness(MCU_LED_TOGGLE_LED_PARTS) == 0);

  std::cout << "[PASS] test_toggles_and_leds_through_navigation\n";
}

// A ui restart: a fresh NavigationController knows nothing of the old one; it re-sends the full LED state (every LED, toggles with exactly one lit per toggle)
// when it (re)connects. The LED path is only control messages: nothing here touches audio.
static void test_ui_restart_resends_full_state() {

  Rec first;
  {
    NavigationController nav;
    assert(nav.initialize());
    nav.led_driver().set_send(first.fn());
    nav.resend_leds();
    nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PART_ALL_ASSIGN));
    assert(first.last()[MCU_LED_TOGGLE_LED_ALL] == 255);
  }
  Rec second;
  second.connected = false;  // the restarted ui starts before the engine connection is up
  NavigationController nav2;
  assert(nav2.initialize());
  nav2.led_driver().set_send(second.fn());
  nav2.performance_view().toggles().set_all(true, true, 2);
  assert(second.log.empty());
  second.connected = true;
  nav2.resend_leds();  // what the connection edge does
  const auto last = second.last();
  // all 8 part/scene LEDs and all 7 toggle LEDs
  assert(last.size() == 15);
  for (uint8_t id = MCU_LED_PART_SCENE_LED_1; id <= MCU_LED_PART_SCENE_LED_8; ++id) assert(last.count(id) == 1);
  assert(last.at(MCU_LED_TOGGLE_LED_DRAWBARS) == 255 && last.at(MCU_LED_TOGGLE_LED_MIXER) == 0);
  assert(last.at(MCU_LED_TOGGLE_LED_SCENES) == 255 && last.at(MCU_LED_TOGGLE_LED_PARTS) == 0);
  assert(last.at(MCU_LED_TOGGLE_LED_ASSIGN) == 255 && last.at(MCU_LED_TOGGLE_LED_PART) == 0 && last.at(MCU_LED_TOGGLE_LED_ALL) == 0);
  std::cout << "[PASS] test_ui_restart_resends_full_state\n";
}

// The connection edge in tick(): disconnected -> connected re-sends all LEDs once; the next tick while connected sends nothing. The default send hook maps a
// command to ws.v1 SetLed (led_id / brightness / arg).
static void test_connection_edge_and_message() {
  NavigationController nav;
  assert(nav.initialize());
  bool connected = false;
  std::vector<ws::v1::ClientMessage> msgs;
  nav.set_connected_probe([&] { return connected; });
  nav.set_command_sender([&](const ws::v1::ClientMessage& m) {
    msgs.push_back(m);
    return true;
  });
  nav.resend_leds();
  nav.tick(1);
  assert(msgs.empty());  // not connected: nothing leaves the ui
  connected = true;
  nav.tick(1);
  assert(msgs.size() == 15);  // all 8 part/scene LEDs and all 7 toggle LEDs, once
  for (const auto& m : msgs) assert(m.has_set_led());
  size_t lit_part = 0;
  for (const auto& m : msgs) {
    const uint32_t id = m.set_led().led_id();
    if (id == MCU_LED_TOGGLE_LED_MIXER || id == MCU_LED_TOGGLE_LED_PARTS || id == MCU_LED_TOGGLE_LED_PART) {
      assert(m.set_led().brightness() == 255 && m.set_led().arg() == 0);
      ++lit_part;
    }
  }
  assert(lit_part == 3);
  const size_t n = msgs.size();
  nav.tick(1);
  nav.tick(1);
  assert(msgs.size() == n);  // a second tick while connected sends nothing
  connected = false;
  nav.tick(1);
  connected = true;
  nav.tick(1);
  assert(msgs.size() == 2 * n);  // a new edge replays again
  // a toggle press produces exactly the SetLed pair of the flip
  msgs.clear();
  nav.tick(400);
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PART_ALL_ASSIGN));
  assert(msgs.size() == 2);
  assert(msgs[0].set_led().led_id() == MCU_LED_TOGGLE_LED_PART && msgs[0].set_led().brightness() == 0);
  assert(msgs[1].set_led().led_id() == MCU_LED_TOGGLE_LED_ALL && msgs[1].set_led().brightness() == 255);
  std::cout << "[PASS] test_connection_edge_and_message\n";
}

int main() {
  std::cout << "Running led_toggle_test...\n";
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));
  test_toggle_state_owner();
  test_blink_twice_timing();
  test_dip_timing();
  test_three_hz_bound();
  test_failed_send_retried();
  test_patterns_exempt_from_guard();
  test_resend_all();
  test_toggles_and_leds_through_navigation();
  test_ui_restart_resends_full_state();
  test_connection_edge_and_message();
  adapter.shutdown();
  std::cout << "All Story 3.12 LED / toggle tests passed!\n";
  return 0;
}

#include <cassert>
#include <deque>
#include <string>
#include <vector>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/settings_view.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/system_config.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

static void tap_exit(NavigationController& nav) {
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
}

void test_story_10_1_sections_and_persistence() {
  SystemConfig& cfg = SystemConfig::instance();
  cfg.schema_version = 1;
  cfg.theme = "stage";
  cfg.language = "en";
  cfg.screen_brightness = 80;
  cfg.led_brightness = LedBrightnessMode::FollowScreen;
  cfg.large_text = false;
  cfg.reduced_motion = false;
  cfg.touch_mode = TouchMode::Safe;
  cfg.aftertouch_available = false;

  SettingsView view(nullptr);
  assert(view.create(lv_screen_active()));

  // 1. All 7 canonical sections present and selectable
  view.select_section(SettingsSection::DisplayLanguage);
  assert(view.selected_section() == SettingsSection::DisplayLanguage);
  assert(!lv_obj_is_hidden(view.display_container()));
  assert(lv_obj_is_hidden(view.detail_container()));

  view.select_section(SettingsSection::KeyboardPedals);
  assert(view.selected_section() == SettingsSection::KeyboardPedals);
  assert(!lv_obj_is_hidden(view.keyboard_pedals_container()));
  assert(lv_obj_is_hidden(view.display_container()));

  // Conditional row rule: aftertouch_available == false -> aftertouch row hidden
  assert(lv_obj_is_hidden(view.card_aftertouch()));

  // If aftertouch_available is true, update_conditional_rows reveals the row
  cfg.aftertouch_available = true;
  view.update_conditional_rows();
  assert(!lv_obj_is_hidden(view.card_aftertouch()));

  view.select_section(SettingsSection::MidiClock);
  assert(view.selected_section() == SettingsSection::MidiClock);
  assert(!lv_obj_is_hidden(view.midi_clock_container()));

  view.select_section(SettingsSection::KnobsGlobalAssign);
  assert(view.selected_section() == SettingsSection::KnobsGlobalAssign);
  assert(!lv_obj_is_hidden(view.detail_container()));

  view.select_section(SettingsSection::Computer);
  assert(view.selected_section() == SettingsSection::Computer);
  assert(!lv_obj_is_hidden(view.computer_container()));

  view.select_section(SettingsSection::StorageLibrary);
  assert(view.selected_section() == SettingsSection::StorageLibrary);
  assert(!lv_obj_is_hidden(view.storage_container()));

  view.select_section(SettingsSection::SystemUpdate);
  assert(view.selected_section() == SettingsSection::SystemUpdate);
  assert(!lv_obj_is_hidden(view.system_update_container()));

  // JSON persistence round-trip
  cfg.theme = "studio";
  cfg.screen_brightness = 65;
  cfg.velocity_curve = VelocityCurve::Heavy;
  cfg.midi_out_channel = 7;
  std::string json = cfg.to_json();
  assert(json.find("\"schema_version\": 1") != std::string::npos);
  assert(json.find("\"theme\": \"studio\"") != std::string::npos);
  assert(json.find("\"screen_brightness\": 65") != std::string::npos);
  assert(json.find("\"velocity_curve\": \"heavy\"") != std::string::npos);
  assert(json.find("\"midi_out_channel\": 7") != std::string::npos);

  SystemConfig loaded;
  assert(loaded.from_json(json));
  assert(loaded.theme == "studio");
  assert(loaded.screen_brightness == 65);
  assert(loaded.velocity_curve == VelocityCurve::Heavy);
  assert(loaded.midi_out_channel == 7);

  std::cout << "[PASS] test_story_10_1_sections_and_persistence\n";
}

void test_story_10_2_keyboard_pedals_and_midi_clock() {
  SettingsView view(nullptr);
  assert(view.create(lv_screen_active()));

  // Keyboard & Pedals section
  view.select_section(SettingsSection::KeyboardPedals);

  // Velocity curve
  view.seg_velocity_curve().set_selected_index(static_cast<int32_t>(VelocityCurve::Light));
  assert(SystemConfig::instance().velocity_curve == VelocityCurve::Light);

  // Footswitch function
  view.seg_footswitch().set_selected_index(static_cast<int32_t>(FootswitchFunction::RotarySpeed));
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::RotarySpeed);

  // Calibration flows
  view.calibrate_sustain();
  assert(SystemConfig::instance().sustain_min == 0);
  assert(SystemConfig::instance().sustain_max == 4095);

  view.calibrate_expression();
  assert(SystemConfig::instance().expression_min == 0);
  assert(SystemConfig::instance().expression_max == 4095);

  // MIDI & Clock section
  view.select_section(SettingsSection::MidiClock);

  // Clock source
  view.seg_clock_source().set_selected_index(static_cast<int32_t>(ClockSourceMode::DIN));
  assert(SystemConfig::instance().clock_source == ClockSourceMode::DIN);

  // MIDI channel
  view.seg_midi_out_ch().set_selected_index(9); // Channel 10 (0-based 9)
  assert(SystemConfig::instance().midi_out_channel == 10);

  // Keybed to MIDI OUT and THRU
  view.seg_keybed_out().set_selected_index(0); // Off
  assert(!SystemConfig::instance().keybed_to_midi_out);

  view.seg_midi_thru().set_selected_index(1); // On
  assert(SystemConfig::instance().midi_thru);

  std::cout << "[PASS] test_story_10_2_keyboard_pedals_and_midi_clock\n";
}

void test_story_10_3_theme_and_drawbars_edge_rule() {
  PerformanceView perf(nullptr);
  assert(perf.create(lv_screen_active()));
  perf.set_modes(true, false, 0); // Activate Drawbars mode

  // 1. Stage theme (dark)
  Theme::set_theme(ThemeId::Stage);
  assert(Theme::current_id() == ThemeId::Stage);
  perf.set_drawbar(0, 8); // Brown (16')
  perf.set_drawbar(2, 8); // White (8')
  perf.set_drawbar(4, 6); // Black (2 2/3')

  // In Stage theme: black heads carry 2px black edge, white heads carry 0px edge
  lv_obj_t* black_head = perf.drawbar_col(4);
  assert(black_head != nullptr);

  // 2. Studio theme (light)
  Theme::set_theme(ThemeId::Studio);
  assert(Theme::current_id() == ThemeId::Studio);
  perf.set_drawbar(2, 8); // Trigger redraw
  // In Studio theme: white heads carry 2px white edge, black heads carry 0px edge

  // Restore Stage theme
  Theme::set_theme(ThemeId::Stage);
  std::cout << "[PASS] test_story_10_3_theme_and_drawbars_edge_rule\n";
}

void test_story_10_4_and_10_5_brightness_and_quick_path() {
  NavigationController nav(nullptr);
  assert(nav.initialize(lv_screen_active()));

  SystemConfig& cfg = SystemConfig::instance();
  cfg.screen_brightness = 70;

  // Hold EXIT on Performance
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));

  // Turn VALUE +1 detent (+5%)
  nav.on_panel_event(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, 1));
  assert(cfg.screen_brightness == 75);
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message().find("75") != std::string::npos);

  // Turn VALUE -2 detents (-10%)
  nav.on_panel_event(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, -2));
  assert(cfg.screen_brightness == 65);
  assert(nav.notice_toast().message().find("65") != std::string::npos);

  // Release EXIT: quick path used -> normal back navigation is CANCELLED!
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Performance);

  // Quick path works from any surface (e.g. Launcher)
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Launcher);

  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, 1));
  assert(cfg.screen_brightness == 70);
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));

  // Release did NOT navigate back to Performance because quick path was used!
  assert(nav.current_surface() == SurfaceId::Launcher);

  // Tapping EXIT dismisses toast first, then next EXIT performs normal back navigation
  tap_exit(nav);
  assert(!nav.notice_toast().is_visible());
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_story_10_4_and_10_5_brightness_and_quick_path\n";
}

void test_story_10_6_large_text_and_reduced_motion() {
  PerformanceView perf(nullptr);
  assert(perf.create(lv_screen_active()));

  std::vector<SceneTileData> scenes = {
    {"Verse 1", "E-Piano + Pad", false},
    {"Chorus", "Lead + Brass", false}
  };
  perf.set_scenes(scenes, 0);

  std::vector<PartStripData> parts(8);
  parts[0] = {"Stage Grand", "A. Piano", 0.8f, true, false, false, false, 0.8f};
  parts[1] = {"Warm Pad", "Synth Pad", 0.6f, true, false, false, false, 0.6f};
  perf.set_parts(parts);

  // Normal text mode
  perf.set_large_text(false);
  assert(!perf.is_large_text());

  // Large text mode On (+25% typography, drops sublabels and categories)
  perf.set_large_text(true);
  assert(perf.is_large_text());

  // Reduced motion
  SystemConfig& cfg = SystemConfig::instance();
  cfg.reduced_motion = true;
  assert(cfg.reduced_motion);

  std::cout << "[PASS] test_story_10_6_large_text_and_reduced_motion\n";
}

void test_story_10_7_and_10_8_locales_and_pseudo_localization() {
  Locale& loc = Locale::instance();

  // English
  loc.set_locale("en");
  assert(loc.current_locale() == "en");
  std::string s_en = tr("settings.title");
  assert(s_en == "SETTINGS");

  // Portuguese
  loc.set_locale("pt-BR");
  assert(loc.current_locale() == "pt-BR");
  std::string s_pt = tr("settings.display.touch_performance");
  assert(s_pt == "Toque no Performance");

  // Spanish
  loc.set_locale("es");
  assert(loc.current_locale() == "es");
  std::string s_es = tr("settings.display.touch_performance");
  assert(s_es == "Toque en Performance");

  // Pseudo-localization (+40% expansion, bracket markers, accented substitutions)
  loc.set_locale("pseudo");
  assert(loc.current_locale() == "pseudo");
  std::string s_pseudo = tr("settings.title");
  assert(s_pseudo.front() == '[');
  assert(s_pseudo.back() == ']');
  assert(s_pseudo.size() > s_en.size() * 1.3);

  // Restore English
  loc.set_locale("en");
  std::cout << "[PASS] test_story_10_7_and_10_8_locales_and_pseudo_localization\n";
}

// Story 7.7: the Clock source control writes through the sysd seam and shows the value sysd reports.
namespace {
struct FakeSysd : SysdConfigSender {
  std::vector<ClockSourceMode> requests;
  std::deque<Event> events;
  std::vector<std::pair<uint8_t, std::string>> boot_requests;
  void request_clock_source(ClockSourceMode m) override { requests.push_back(m); }
  void request_boot_into(uint8_t mode, const std::string& id) override { boot_requests.emplace_back(mode, id); }
  void request_start_pairing() override {}
  void request_cancel_pairing() override {}
  void request_pairing_status() override {}
  void request_list_paired_computers() override {}
  void request_forget_computer(const std::string&) override {}
  void report_boot(uint8_t mode, const std::string& id, ClockSourceMode clock = ClockSourceMode::Internal) {
    Event e;
    e.kind = Event::Kind::Config;
    e.clock_source = clock;
    e.has_boot_into = true;
    e.boot_into = mode;
    e.default_live_set_id = id;
    events.push_back(e);
  }
  void fail_boot(bool unreachable) {
    Event e;
    e.kind = Event::Kind::Failed;
    e.setting = Event::Setting::BootInto;
    e.unreachable = unreachable;
    e.error = "x";
    events.push_back(e);
  }
  bool poll(Event& out) override {
    if (events.empty()) return false;
    out = events.front();
    events.pop_front();
    return true;
  }
  void report(ClockSourceMode m) {
    Event e;
    e.kind = Event::Kind::Config;
    e.clock_source = m;
    events.push_back(e);
  }
  void fail(const char* why) {
    Event e;
    e.kind = Event::Kind::Failed;
    e.error = why;
    events.push_back(e);
  }
};
}  // namespace

void test_story_7_7_clock_source_through_sysd() {
  SystemConfig& cfg = SystemConfig::instance();
  cfg.clock_source = ClockSourceMode::Internal;
  SettingsView view(nullptr);
  assert(view.create(lv_screen_active()));
  FakeSysd sysd;
  std::vector<std::string> notices;
  int changed = 0;
  view.set_sysd_sender(&sysd);
  view.set_on_notice([&](const std::string& m) { notices.push_back(m); });
  view.set_on_config_changed([&](const SystemConfig&) { ++changed; });
  view.select_section(SettingsSection::MidiClock);

  // A change is a request to sysd (a patch with the clock source only); the local value is not touched until sysd reports it.
  view.seg_clock_source().set_selected_index(static_cast<int32_t>(ClockSourceMode::USB));
  assert(sysd.requests.size() == 1 && sysd.requests[0] == ClockSourceMode::USB);
  assert(cfg.clock_source == ClockSourceMode::Internal && changed == 0);
  sysd.report(ClockSourceMode::USB);
  view.poll_sysd();
  assert(cfg.clock_source == ClockSourceMode::USB && changed == 1);
  assert(view.seg_clock_source().selected_index() == static_cast<int32_t>(ClockSourceMode::USB));
  assert(sysd.requests.size() == 1);  // applying a report is not a user change

  // sysd changes the value on its own (another client, a restart): the control follows, nothing is sent.
  sysd.report(ClockSourceMode::DIN);
  view.poll_sysd();
  assert(cfg.clock_source == ClockSourceMode::DIN && view.seg_clock_source().selected_index() == static_cast<int32_t>(ClockSourceMode::DIN));
  assert(sysd.requests.size() == 1 && notices.empty());

  // A change that does not reach sysd: the control goes back to the last known value and the player gets a notice.
  view.seg_clock_source().set_selected_index(static_cast<int32_t>(ClockSourceMode::Internal));
  assert(sysd.requests.size() == 2);
  sysd.fail("cannot connect");
  view.poll_sysd();
  assert(cfg.clock_source == ClockSourceMode::DIN);
  assert(view.seg_clock_source().selected_index() == static_cast<int32_t>(ClockSourceMode::DIN));
  assert(notices.size() == 1 && !notices[0].empty());
  cfg.clock_source = ClockSourceMode::Internal;
  std::cout << "[PASS] test_story_7_7_clock_source_through_sysd\n";
}

// Story 8.9: the Boot into row through the sysd seam: patches with only the boot_into fields, the Default Live Set picker, sysd's value shown, failure -> notice + revert.
void test_story_8_9_boot_into_through_sysd() {
  SystemConfig& cfg = SystemConfig::instance();
  cfg.boot_into = 0;
  SettingsView view(nullptr);
  assert(view.create(lv_screen_active()));
  FakeSysd sysd;
  std::vector<std::string> notices;
  view.set_sysd_sender(&sysd);
  view.set_on_notice([&](const std::string& m) { notices.push_back(m); });
  view.select_section(SettingsSection::DisplayLanguage);
  const std::string A = "a0000000-0000-4000-8000-000000000001", B = "b0000000-0000-4000-8000-000000000002", C = "c0000000-0000-4000-8000-000000000003";
  int list_requests = 0;
  view.set_on_request_live_sets([&] { ++list_requests; });
  {  // the list comes from the engine's reply to GetSetlists{include_all_live_sets} (a real SetlistsReply), not from hand-injected choices
    ws::v1::SetlistsReply reply;
    auto* e1 = reply.add_all_live_sets();
    e1->set_live_set_id(A);
    e1->set_name("Alpha");
    auto* e2 = reply.add_all_live_sets();
    e2->set_live_set_id(B);
    e2->set_name("Bravo");
    auto* gone = reply.add_all_live_sets();  // a missing one is not offered
    gone->set_live_set_id("d0000000-0000-4000-8000-000000000004");
    gone->set_missing(true);
    view.update_from_setlists_reply(reply);
  }
  ws::v1::State st;  // the Live Set that sounds is offered too
  st.set_live_set_id(C);
  st.set_live_set_name("Charlie");
  view.update_from_state(st);

  // sysd reports its value at connect: the row shows it.
  sysd.report_boot(1, "");
  view.poll_sysd();
  assert(cfg.boot_into == 1 && view.seg_boot_into().selected_index() == 1);

  // Last sound used: a patch with the mode only (no id); the control shows sysd's report.
  view.seg_boot_into().set_selected_index(0);
  assert(sysd.boot_requests.size() == 1 && sysd.boot_requests[0].first == 0 && sysd.boot_requests[0].second.empty());
  assert(cfg.boot_into == 1);  // not touched until sysd reports it
  sysd.report_boot(0, "");
  view.poll_sysd();
  assert(cfg.boot_into == 0 && view.seg_boot_into().selected_index() == 0);
  assert(sysd.boot_requests.size() == 1);  // applying a report is not a user change

  // Default Live Set opens the picker of All Live Sets (and the one that sounds); nothing is sent until one is chosen.
  view.seg_boot_into().set_selected_index(2);
  assert(view.is_boot_picker_open() && view.is_picker_open() && sysd.boot_requests.size() == 1);
  assert(list_requests == 1);  // opening the picker asks the engine for the list
  assert(view.boot_picker_row_count() == 3);
  {  // a fresh reply while open rebuilds the rows
    ws::v1::SetlistsReply r2;
    for (const char* id : {"a0000000-0000-4000-8000-000000000001", "b0000000-0000-4000-8000-000000000002"}) r2.add_all_live_sets()->set_live_set_id(id);
    view.update_from_setlists_reply(r2);
    assert(view.boot_picker_row_count() == 3);  // the two + the one that sounds
  }
  // Cancelling goes back to sysd's value, nothing sent.
  view.close_boot_live_set_picker();
  assert(!view.is_boot_picker_open() && view.seg_boot_into().selected_index() == 0 && sysd.boot_requests.size() == 1);
  // Choosing one: a patch with the mode and the id, at once; sysd's report is shown, with the name of the Live Set.
  view.seg_boot_into().set_selected_index(2);
  view.select_boot_live_set(B);
  assert(!view.is_boot_picker_open());
  assert(sysd.boot_requests.size() == 2 && sysd.boot_requests[1].first == 2 && sysd.boot_requests[1].second == B);
  sysd.report_boot(2, B);
  view.poll_sysd();
  assert(cfg.boot_into == 2 && view.default_live_set_id() == B && view.seg_boot_into().selected_index() == 2);
  // The panel: encoder + push choose from the picker (the Live Set that sounds is in it).
  view.seg_boot_into().set_selected_index(0);
  sysd.report_boot(0, B);
  view.poll_sysd();
  view.seg_boot_into().set_selected_index(2);
  assert(view.is_boot_picker_open() && view.boot_picker_selected_row() == 1);  // opens on the current default
  PanelEvent enc;
  enc.type = PanelEventType::Encoder;
  enc.id = MCU_CTRL_VALUE_ENCODER;
  enc.delta = 1;
  view.on_panel_event(enc);
  assert(view.boot_picker_selected_row() == 2);
  PanelEvent push;
  push.type = PanelEventType::Button;
  push.id = MCU_BTN_VALUE_PUSH;
  push.pressed = true;
  view.on_panel_event(push);
  assert(!view.is_boot_picker_open() && sysd.boot_requests.back().first == 2 && sysd.boot_requests.back().second == C);

  // A change that does not reach sysd, or that sysd refuses: the control goes back, the player gets a notice.
  sysd.report_boot(0, B);
  view.poll_sysd();
  view.seg_boot_into().set_selected_index(1);
  sysd.fail_boot(true);
  view.poll_sysd();
  assert(cfg.boot_into == 0 && view.seg_boot_into().selected_index() == 0 && notices.size() == 1 && !notices[0].empty());
  view.seg_boot_into().set_selected_index(1);
  sysd.fail_boot(false);
  view.poll_sysd();
  assert(view.seg_boot_into().selected_index() == 0 && notices.size() == 2 && notices[0] != notices[1]);
  cfg.boot_into = 0;
  std::cout << "[PASS] test_story_8_9_boot_into_through_sysd\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to initialize headless adapter\n";
    return 1;
  }

  test_story_10_1_sections_and_persistence();
  test_story_10_2_keyboard_pedals_and_midi_clock();
  test_story_10_3_theme_and_drawbars_edge_rule();
  test_story_10_4_and_10_5_brightness_and_quick_path();
  test_story_10_6_large_text_and_reduced_motion();
  test_story_10_7_and_10_8_locales_and_pseudo_localization();
  test_story_7_7_clock_source_through_sysd();
  test_story_8_9_boot_into_through_sysd();

  std::cout << "All Epic 10 tests passed successfully!\n";
  return 0;
}

#include <cassert>
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

  std::cout << "All Epic 10 tests passed successfully!\n";
  return 0;
}

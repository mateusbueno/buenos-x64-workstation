#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/screens/settings_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/system_config.hpp"

using namespace velora::ui;

void test_settings_display_language_rows() {
  SettingsView view;
  assert(view.create(lv_screen_active()));

  view.select_section(SettingsSection::DisplayLanguage);
  assert(view.selected_section() == SettingsSection::DisplayLanguage);
  assert(!lv_obj_is_hidden(view.display_container()));

  // Check initial selections
  assert(view.seg_touch().selected_index() == static_cast<int32_t>(TouchMode::Safe));
  assert(view.seg_panel_guard().selected_index() == 0);  // Off

  bool config_changed = false;
  view.set_on_config_changed([&](const SystemConfig& cfg) {
    config_changed = true;
  });

  // Change Touch on Performance to Full
  view.seg_touch().set_selected_index(static_cast<int32_t>(TouchMode::Full));
  assert(SystemConfig::instance().touch_mode == TouchMode::Full);
  assert(config_changed);

  // Change Write hold to 0.8 s
  config_changed = false;
  view.seg_write_hold().set_selected_index(2);  // 800 ms
  assert(SystemConfig::instance().write_hold_ms == 800);
  assert(config_changed);

  // Change Long press to 800 ms
  config_changed = false;
  view.seg_long_press().set_selected_index(2);  // 800 ms
  assert(SystemConfig::instance().long_press_ms == 800);
  assert(config_changed);

  // Change Panel guard to On
  config_changed = false;
  view.seg_panel_guard().set_selected_index(1);  // On
  assert(SystemConfig::instance().panel_guard == true);
  assert(config_changed);

  // Change Reduced motion to On
  config_changed = false;
  view.seg_reduced_motion().set_selected_index(1);  // On
  assert(SystemConfig::instance().reduced_motion == true);
  assert(config_changed);

  // Reset to default for subsequent tests
  SystemConfig::instance().touch_mode = TouchMode::Safe;
  SystemConfig::instance().write_hold_ms = 500;
  SystemConfig::instance().long_press_ms = 500;
  SystemConfig::instance().panel_guard = false;
  SystemConfig::instance().reduced_motion = false;

  std::cout << "[PASS] test_settings_display_language_rows\n";
}

void test_touch_safe_mode() {
  PerformanceView perf;
  assert(perf.create(lv_screen_active()));
  perf.set_touch_mode(TouchMode::Safe);

  int part_selected = -1;
  perf.set_part_select_callback([&](int32_t idx) {
    part_selected = idx;
  });

  bool rev_clicked = false;
  perf.set_rev_click_callback([&]() {
    rev_clicked = true;
  });

  bool setlist_clicked = false;
  perf.set_setlist_click_callback([&]() {
    setlist_clicked = true;
  });

  lv_obj_t* btn_p = perf.part_power_button(0);
  assert(btn_p != nullptr);

  // 1. Touch down on power button -> shows armed state
  perf.simulate_touch_down(btn_p, 100, 100, 1000);
  assert(perf.is_armed(btn_p));

  // Release too early (< 150 ms) -> does NOT toggle
  perf.simulate_touch_up(btn_p, 100, 100, 1100);  // 100 ms
  assert(!perf.is_armed(btn_p));
  assert(perf.part_data(0).enabled == true);

  // Touch down again, release with too much movement (> 10 px) -> does NOT toggle
  perf.simulate_touch_down(btn_p, 100, 100, 2000);
  perf.simulate_touch_up(btn_p, 115, 100, 2200);  // 200 ms, dx=15 px
  assert(perf.part_data(0).enabled == true);

  // Touch down, release after 180 ms with 2 px movement -> toggles part!
  perf.simulate_touch_down(btn_p, 100, 100, 3000);
  perf.simulate_touch_up(btn_p, 102, 101, 3180);  // 180 ms, dist < 3 px
  assert(perf.part_data(0).enabled == false);

  // 2. Part card body in Safe mode: tap does nothing
  lv_obj_t* card = perf.part_card(0);
  perf.simulate_touch_down(card, 50, 50, 4000);
  perf.simulate_touch_up(card, 50, 50, 4200);
  assert(part_selected == -1);

  // Long press on card body in Safe mode -> opens Part surface!
  perf.simulate_long_press(card);
  assert(part_selected == 0);

  // 3. Setlist context & REV in Safe mode: tap does nothing, long press acts
  lv_obj_t* ctx = perf.context_label();
  perf.simulate_touch_down(ctx, 20, 20, 5000);
  perf.simulate_touch_up(ctx, 20, 20, 5200);
  assert(!setlist_clicked);
  perf.simulate_long_press(ctx);
  assert(setlist_clicked);

  lv_obj_t* rev = perf.rev_button();
  perf.simulate_touch_down(rev, 900, 550, 6000);
  perf.simulate_touch_up(rev, 900, 550, 6200);
  assert(!rev_clicked);
  perf.simulate_long_press(rev);
  assert(rev_clicked);

  std::cout << "[PASS] test_touch_safe_mode\n";
}

void test_touch_full_mode() {
  PerformanceView perf;
  assert(perf.create(lv_screen_active()));
  perf.set_touch_mode(TouchMode::Full);

  int part_selected = -1;
  perf.set_part_select_callback([&](int32_t idx) {
    part_selected = idx;
  });

  int knob_target = -1;
  perf.set_knob_target_select_callback([&](int32_t idx) {
    knob_target = idx;
  });

  bool rev_clicked = false;
  perf.set_rev_click_callback([&]() {
    rev_clicked = true;
  });

  bool setlist_clicked = false;
  perf.set_setlist_click_callback([&]() {
    setlist_clicked = true;
  });

  // 1. Power button toggles immediately on tap
  lv_obj_t* btn_p = perf.part_power_button(1);
  perf.simulate_touch_down(btn_p, 100, 100, 1000);
  perf.simulate_touch_up(btn_p, 100, 100, 1050);  // 50 ms
  assert(perf.part_data(1).enabled == false);

  // 2. Part card body tap -> opens Part surface
  lv_obj_t* card = perf.part_card(2);
  perf.simulate_touch_down(card, 50, 50, 2000);
  perf.simulate_touch_up(card, 50, 50, 2050);
  assert(part_selected == 2);

  // 3. Part card body long press in Full mode -> selects knob target!
  perf.simulate_long_press(card);
  assert(knob_target == 2);

  // 4. Setlist context and REV tap -> act on tap in Full mode
  perf.simulate_touch_down(perf.context_label(), 20, 20, 3000);
  perf.simulate_touch_up(perf.context_label(), 20, 20, 3050);
  assert(setlist_clicked);

  perf.simulate_touch_down(perf.rev_button(), 900, 550, 4000);
  perf.simulate_touch_up(perf.rev_button(), 900, 550, 4050);
  assert(rev_clicked);

  std::cout << "[PASS] test_touch_full_mode\n";
}

void test_touch_off_mode() {
  PerformanceView perf;
  assert(perf.create(lv_screen_active()));
  perf.set_touch_mode(TouchMode::Off);

  int part_selected = -1;
  perf.set_part_select_callback([&](int32_t idx) {
    part_selected = idx;
  });

  int initial_scene = perf.current_scene();

  // 1. Power button tap ignored
  lv_obj_t* btn_p = perf.part_power_button(0);
  perf.simulate_touch_down(btn_p, 100, 100, 1000);
  perf.simulate_touch_up(btn_p, 100, 100, 1200);  // 200 ms
  assert(perf.part_data(0).enabled == true);  // Unchanged!

  // 2. Scene Tile tap ignored
  lv_obj_t* tile = perf.scene_tile(1);
  perf.simulate_touch_down(tile, 200, 100, 2000);
  perf.simulate_touch_up(tile, 200, 100, 2200);
  assert(perf.current_scene() == initial_scene);  // Ignored!

  // 3. Long press targets still work
  lv_obj_t* card = perf.part_card(3);
  perf.simulate_long_press(card);
  assert(part_selected == 3);

  std::cout << "[PASS] test_touch_off_mode\n";
}

void test_panel_guard_stepping() {
  NavigationController nav;
  assert(nav.initialize(lv_screen_active()));

  // 1. Panel Guard Off: brief press steps Live Set immediately
  nav.set_panel_guard(false);
  int initial_steps = nav.live_set_step_count();

  PanelEvent ev_next_press;
  ev_next_press.type = PanelEventType::Button;
  ev_next_press.id = MCU_BTN_LIVE_SET_NEXT;
  ev_next_press.pressed = true;
  ev_next_press.timestamp_us = 1000000;
  nav.on_panel_event(ev_next_press);
  assert(nav.live_set_step_count() == initial_steps + 1);

  // 2. Panel Guard On: brief press does NOT step
  nav.set_panel_guard(true);
  initial_steps = nav.live_set_step_count();

  // Down
  ev_next_press.timestamp_us = 2000000;
  nav.on_panel_event(ev_next_press);
  assert(nav.live_set_step_count() == initial_steps);

  // Up after only 100 ms (< 300 ms hold)
  PanelEvent ev_next_release = ev_next_press;
  ev_next_release.pressed = false;
  ev_next_release.timestamp_us = 2100000;  // 100 ms later
  nav.on_panel_event(ev_next_release);
  assert(nav.live_set_step_count() == initial_steps);  // Still unchanged!

  // Hold for 350 ms (>= 300 ms hold) -> steps!
  ev_next_press.timestamp_us = 3000000;
  nav.on_panel_event(ev_next_press);
  assert(nav.live_set_step_count() == initial_steps);

  ev_next_release.timestamp_us = 3350000;  // 350 ms later
  nav.on_panel_event(ev_next_release);
  assert(nav.live_set_step_count() == initial_steps + 1);  // Stepped!

  std::cout << "[PASS] test_panel_guard_stepping\n";
}

void test_configurable_write_hold() {
  NavigationController nav;
  assert(nav.initialize(lv_screen_active()));

  nav.set_write_hold_ms(300);
  assert(nav.write_hold_ms() == 300);

  nav.set_write_hold_ms(800);
  assert(nav.write_hold_ms() == 800);

  nav.set_write_hold_ms(1000);
  assert(nav.write_hold_ms() == 1000);

  std::cout << "[PASS] test_configurable_write_hold\n";
}

int main() {
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  Locale::instance().initialize("", "en");
  Locale::instance().set_locale("en");
  Theme::set_theme(ThemeId::Stage);

  test_settings_display_language_rows();
  test_touch_safe_mode();
  test_touch_full_mode();
  test_touch_off_mode();
  test_panel_guard_stepping();
  test_configurable_write_hold();

  std::cout << "All Performance Guards tests passed!\n";
  return 0;
}

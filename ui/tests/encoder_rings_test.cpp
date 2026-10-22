#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/navigation_controller.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace velora::ui;

static void test_encoder_rings() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // Setup 8 parts
  std::vector<PartStripData> test_parts(8);
  for (size_t i = 0; i < 8; ++i) {
    test_parts[i] = {"Timbre " + std::to_string(i + 1), "Piano", 0.70f, true, false, false};
  }
  view.set_parts(test_parts);

  // 1. Initial Mode is PART (0)
  assert(view.knob_mode() == 0);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_PART) == LedLevel::Strong);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ALL) == LedLevel::Off);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ASSIGN) == LedLevel::Off);

  // Part 1 card is outlined with selection border
  assert(lv_obj_get_style_border_width(view.part_card(0), LV_PART_MAIN) == Theme::spacing().selection_border);
  assert(lv_obj_get_style_border_width(view.part_card(1), LV_PART_MAIN) == 1);

  // 2. Press TOGGLE_PART_ALL_ASSIGN -> Switch to ALL (1)
  PanelEvent toggle_ev = PanelEvent::button_press(MCU_BTN_TOGGLE_PART_ALL_ASSIGN);
  view.on_panel_event(toggle_ev);

  assert(view.knob_mode() == 1);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_PART) == LedLevel::Off);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ALL) == LedLevel::Strong);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ASSIGN) == LedLevel::Off);

  // In ALL mode, no single card is outlined with selection border
  assert(lv_obj_get_style_border_width(view.part_card(0), LV_PART_MAIN) == 1);
  assert(lv_obj_get_style_border_width(view.part_card(1), LV_PART_MAIN) == 1);

  // 3. Press TOGGLE_PART_ALL_ASSIGN -> Switch to ASSIGN (2)
  view.on_panel_event(toggle_ev);

  assert(view.knob_mode() == 2);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_PART) == LedLevel::Off);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ALL) == LedLevel::Off);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_ASSIGN) == LedLevel::Strong);
  assert(lv_obj_get_style_border_width(view.part_card(0), LV_PART_MAIN) == 1);

  // 4. Press TOGGLE_PART_ALL_ASSIGN -> Returns to PART (0)
  view.on_panel_event(toggle_ev);

  assert(view.knob_mode() == 0);
  assert(view.knob_mode_led(MCU_LED_TOGGLE_LED_PART) == LedLevel::Strong);
  assert(lv_obj_get_style_border_width(view.part_card(0), LV_PART_MAIN) == Theme::spacing().selection_border);

  // 5. Encoder Turns in PART Mode
  // Initial encoder 1 value is 0.5f
  assert(std::fabs(view.encoder_ring_value(0) - 0.5f) < 0.01f);
  assert(view.encoder_ring_fill(0) == 128);

  // Turn Encoder 1 by +20 steps (+0.2f)
  PanelEvent enc1_up = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_1, 20);
  view.on_panel_event(enc1_up);
  assert(std::fabs(view.encoder_ring_value(0) - 0.7f) < 0.01f);
  assert(view.encoder_ring_fill(0) == static_cast<uint8_t>(std::round(0.7f * 255.0f)));

  // Turn Encoder 1 by -50 steps (-0.5f)
  PanelEvent enc1_down = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_1, -50);
  view.on_panel_event(enc1_down);
  assert(std::fabs(view.encoder_ring_value(0) - 0.2f) < 0.01f);

  // Switch selected knob part to Part 2 (index 1)
  view.select_knob_part(1);
  assert(view.selected_knob_part() == 1);
  assert(lv_obj_get_style_border_width(view.part_card(0), LV_PART_MAIN) == 1);
  assert(lv_obj_get_style_border_width(view.part_card(1), LV_PART_MAIN) == Theme::spacing().selection_border);

  // Part 2 encoder 1 is untouched at 0.5f
  assert(std::fabs(view.encoder_ring_value(0) - 0.5f) < 0.01f);

  // 6. Test ALL Mode Relative Offset
  view.set_encoder_value(0, 0.40f); // Part 2 encoder 1 = 0.40f
  view.select_knob_part(0);
  view.set_encoder_value(0, 0.60f); // Part 1 encoder 1 = 0.60f

  // Enter ALL mode
  view.cycle_knob_mode();
  assert(view.knob_mode() == 1);

  // Ring shows relative offset centered at 0.5f
  assert(std::fabs(view.encoder_ring_value(0) - 0.5f) < 0.01f);

  // Turn encoder 1 by +15 steps (+0.15f)
  PanelEvent enc1_all = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_1, 15);
  view.on_panel_event(enc1_all);
  assert(std::fabs(view.encoder_ring_value(0) - 0.65f) < 0.01f);

  // Return to PART mode to inspect parts
  view.cycle_knob_mode(); // ASSIGN
  view.cycle_knob_mode(); // PART
  assert(view.knob_mode() == 0);

  // Part 1 encoder 1 was 0.60f + 0.15f = 0.75f
  view.select_knob_part(0);
  assert(std::fabs(view.encoder_ring_value(0) - 0.75f) < 0.01f);

  // Part 2 encoder 1 was 0.40f + 0.15f = 0.55f
  view.select_knob_part(1);
  assert(std::fabs(view.encoder_ring_value(0) - 0.55f) < 0.01f);

  // 7. Test ASSIGN Mode
  view.cycle_knob_mode(); // ALL
  view.cycle_knob_mode(); // ASSIGN
  assert(view.knob_mode() == 2);

  // Turn Encoder 3 (MCU_CTRL_ENCODER_3) by +30 (+0.30f)
  PanelEvent enc3_ev = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_3, 30);
  view.on_panel_event(enc3_ev);
  assert(std::fabs(view.encoder_ring_value(2) - 0.80f) < 0.01f);

  // 8. Test Global NavigationController Routing Across Surfaces
  NavigationController nav;
  assert(nav.initialize());

  // Switch to Launcher
  nav.navigate_to(SurfaceId::Launcher);
  assert(nav.current_surface() == SurfaceId::Launcher);

  // Toggle knob mode while on Launcher
  assert(nav.performance_view().knob_mode() == 0);
  nav.on_panel_event(toggle_ev);
  assert(nav.performance_view().knob_mode() == 1); // Cycled to ALL

  // Turn encoder while on Launcher
  nav.on_panel_event(enc1_up);

  // Switch to Effects
  nav.navigate_to(SurfaceId::Effects);
  assert(nav.current_surface() == SurfaceId::Effects);

  // Turn encoder while on Effects
  nav.on_panel_event(enc3_ev);

  std::cout << "test_encoder_rings passed" << std::endl;
}

int main() {
  test_encoder_rings();
  return 0;
}

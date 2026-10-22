#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_parts_scenes_toggle() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // 1. Initial state: PARTS mode
  assert(!view.is_scenes_mode());

  // 2. Test TOGGLE_PARTS_SCENES button
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));
  assert(view.is_scenes_mode());

  view.on_panel_event(PanelEvent::button_press(MCU_BTN_TOGGLE_PARTS_SCENES));
  assert(!view.is_scenes_mode());

  // 3. Test PARTS mode short press (toggle on/off)
  std::vector<PartStripData> test_parts(8);
  for (size_t i = 0; i < 8; ++i) {
    test_parts[i] = {"Sound " + std::to_string(i + 1), "Piano", 0.7f, true, false, false};
  }
  view.set_parts(test_parts);

  assert(view.part_data(0).enabled == true);
  assert(view.button_led_level(0) == LedLevel::Strong);

  // Short press button 1: press at 1000us, release at 150000us (149ms < 500ms)
  PanelEvent p1_down = {PanelEventType::Button, MCU_BTN_PART_SCENE_1, true, 0, 1000};
  PanelEvent p1_up = {PanelEventType::Button, MCU_BTN_PART_SCENE_1, false, 0, 150000};
  view.on_panel_event(p1_down);
  view.on_panel_event(p1_up);

  assert(view.part_data(0).enabled == false);
  assert(view.button_led_level(0) == LedLevel::Off);

  // Short press button 1 again: toggles back on
  PanelEvent p2_down = {PanelEventType::Button, MCU_BTN_PART_SCENE_1, true, 0, 200000};
  PanelEvent p2_up = {PanelEventType::Button, MCU_BTN_PART_SCENE_1, false, 0, 300000};
  view.on_panel_event(p2_down);
  view.on_panel_event(p2_up);

  assert(view.part_data(0).enabled == true);
  assert(view.button_led_level(0) == LedLevel::Strong);

  // 4. Test PARTS mode long press (knob target selection without toggling)
  bool part3_initial_state = view.part_data(2).enabled;
  assert(view.selected_knob_part() == 0);

  // Long press button 3: press at 400000us, release at 1000000us (600ms >= 500ms)
  PanelEvent p3_down = {PanelEventType::Button, MCU_BTN_PART_SCENE_3, true, 0, 400000};
  PanelEvent p3_up = {PanelEventType::Button, MCU_BTN_PART_SCENE_3, false, 0, 1000000};
  view.on_panel_event(p3_down);
  view.on_panel_event(p3_up);

  // Part 3 should not have toggled, but is now selected knob target
  assert(view.part_data(2).enabled == part3_initial_state);
  assert(view.selected_knob_part() == 2);

  // 5. Test SCENES mode
  view.toggle_parts_scenes_mode();
  assert(view.is_scenes_mode());

  std::vector<SceneTileData> scenes(8);
  scenes[0] = {"Intro", "Piano", false};
  scenes[1] = {"Verse", "Pad", false};
  scenes[2] = {"Chorus", "Full", false};
  for (size_t i = 3; i < 8; ++i) {
    scenes[i] = {"", "", true}; // Empty
  }
  view.set_scenes(scenes, 0);

  // Verify initial scene LEDs
  assert(view.button_led_level(0) == LedLevel::Strong); // Current scene
  assert(view.button_led_level(1) == LedLevel::Weak);   // Existing scene
  assert(view.button_led_level(2) == LedLevel::Weak);   // Existing scene
  assert(view.button_led_level(3) == LedLevel::Off);    // Empty scene
  assert(view.button_led_level(7) == LedLevel::Off);    // Empty scene

  // Press button 2 (Verse): short press
  PanelEvent s2_down = {PanelEventType::Button, MCU_BTN_PART_SCENE_2, true, 0, 1100000};
  PanelEvent s2_up = {PanelEventType::Button, MCU_BTN_PART_SCENE_2, false, 0, 1200000};
  view.on_panel_event(s2_down);
  view.on_panel_event(s2_up);

  assert(view.current_scene() == 1);
  assert(view.is_scene_led_dipping()); // 200 ms dip active

  // Press button 5 (empty slot): does nothing
  PanelEvent s5_down = {PanelEventType::Button, MCU_BTN_PART_SCENE_5, true, 0, 1300000};
  PanelEvent s5_up = {PanelEventType::Button, MCU_BTN_PART_SCENE_5, false, 0, 1400000};
  view.on_panel_event(s5_down);
  view.on_panel_event(s5_up);

  assert(view.current_scene() == 1); // Unchanged

  lv_timer_handler();
  adapter.shutdown();

  std::cout << "[PASS] test_parts_scenes_toggle\n";
}

int main() {
  std::cout << "Running parts_scenes_toggle_test...\n";
  test_parts_scenes_toggle();
  std::cout << "All parts / scenes toggle tests passed!\n";
  return 0;
}

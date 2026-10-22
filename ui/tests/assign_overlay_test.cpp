#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/navigation_controller.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_assign_overlay() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // Setup parts and scenes
  std::vector<PartStripData> test_parts(8);
  for (size_t i = 0; i < 8; ++i) {
    test_parts[i] = {"Sound " + std::to_string(i + 1), "Piano", 0.70f, true, false, false};
  }
  view.set_parts(test_parts);

  std::vector<SceneTileData> test_scenes(8);
  for (size_t i = 0; i < 8; ++i) {
    test_scenes[i] = {"Scene " + std::to_string(i + 1), "", false};
  }
  view.set_scenes(test_scenes, 0);

  // 1. PART mode (default): encoder turns do NOT show ASSIGN overlay
  assert(view.knob_mode() == 0);
  PanelEvent enc1_ev = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_1, 10, 1000000); // 1000 ms
  view.on_panel_event(enc1_ev);
  assert(!view.is_assign_overlay_visible());
  assert(lv_obj_is_hidden(view.assign_overlay()));

  // 2. ALL mode: encoder turns do NOT show ASSIGN overlay
  view.cycle_knob_mode();
  assert(view.knob_mode() == 1);
  view.on_panel_event(enc1_ev);
  assert(!view.is_assign_overlay_visible());
  assert(lv_obj_is_hidden(view.assign_overlay()));

  // 3. Switch to ASSIGN mode
  view.cycle_knob_mode();
  assert(view.knob_mode() == 2);
  assert(!view.is_assign_overlay_visible());

  // Turn Encoder 2 (K2) at t = 1000 ms
  PanelEvent enc2_ev = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_2, 15, 1000000); // 1000 ms
  view.on_panel_event(enc2_ev);

  assert(view.is_assign_overlay_visible());
  assert(!lv_obj_is_hidden(view.assign_overlay()));
  assert(view.active_assign_knob() == 1); // K2 is index 1

  // Verify K2 card has 3px accent border
  lv_obj_t* card2 = view.assign_slot_card(1);
  assert(card2 != nullptr);
  assert(lv_obj_get_style_border_width(card2, LV_PART_MAIN) == 3);

  // Verify K1 card is NOT active (border width 1)
  lv_obj_t* card1 = view.assign_slot_card(0);
  assert(card1 != nullptr);
  assert(lv_obj_get_style_border_width(card1, LV_PART_MAIN) == 1);

  // 4. Timer behavior: 2.5 s later (t = 3500 ms), overlay is still visible
  view.update_assign_timer(3500);
  assert(view.is_assign_overlay_visible());
  assert(!lv_obj_is_hidden(view.assign_overlay()));

  // Turn Encoder 4 (K4) at t = 3600 ms -> restarts timer, switches active knob
  PanelEvent enc4_ev = PanelEvent::encoder_turn(MCU_CTRL_ENCODER_4, 5, 3600000);
  view.on_panel_event(enc4_ev);
  assert(view.is_assign_overlay_visible());
  assert(view.active_assign_knob() == 3); // K4 is index 3

  lv_obj_t* card4 = view.assign_slot_card(3);
  assert(lv_obj_get_style_border_width(card4, LV_PART_MAIN) == 3);
  assert(lv_obj_get_style_border_width(card2, LV_PART_MAIN) == 1);

  // At t = 6000 ms (2.4s after K4 turn): still visible
  view.update_assign_timer(6000);
  assert(view.is_assign_overlay_visible());

  // At t = 6700 ms (>3.0s after K4 turn): closes automatically!
  view.update_assign_timer(6700);
  assert(!view.is_assign_overlay_visible());
  assert(lv_obj_is_hidden(view.assign_overlay()));

  // 5. Informational only: Scene and Live Set navigation work while overlay is open
  view.on_panel_event(enc2_ev); // Reopen overlay
  assert(view.is_assign_overlay_visible());

  // Advance scene
  PanelEvent scene_next_ev = PanelEvent::button_press(MCU_BTN_SCENE_NEXT);
  view.on_panel_event(scene_next_ev);
  assert(view.current_scene() == 1);
  assert(view.is_assign_overlay_visible()); // Overlay stays visible

  // 6. Switching knob mode immediately hides overlay
  PanelEvent toggle_ev = PanelEvent::button_press(MCU_BTN_TOGGLE_PART_ALL_ASSIGN);
  view.on_panel_event(toggle_ev); // Cycles ASSIGN -> PART
  assert(view.knob_mode() == 0);
  assert(!view.is_assign_overlay_visible());
  assert(lv_obj_is_hidden(view.assign_overlay()));

  std::cout << "test_assign_overlay passed" << std::endl;
}

int main() {
  test_assign_overlay();
  return 0;
}

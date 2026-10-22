#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/scenes_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_scenes_view_initial_state_and_selection() {
  ScenesView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);
  assert(view.used_slot_count() == 5);
  assert(view.scenes().size() == 8);
  assert(view.selected_slot() == 0);
  assert(view.current_scene() == 0);
  assert(!view.has_unsaved_edits());

  // Turn VALUE encoder (delta = 1) -> moves selection to slot 1
  PanelEvent enc_turn;
  enc_turn.type = PanelEventType::Encoder;
  enc_turn.id = MCU_CTRL_VALUE_ENCODER;
  enc_turn.delta = 1;
  view.on_panel_event(enc_turn);
  assert(view.selected_slot() == 1);
  assert(view.current_scene() == 0);

  // Press WRITE or VALUE_PUSH -> recalls slot 1
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.on_panel_event(write_ev);
  assert(view.current_scene() == 1);

  // Moving selection to an empty slot (slot 5) and pressing WRITE should NOT recall empty slot
  view.select_slot(5);
  assert(view.selected_slot() == 5);
  assert(view.scenes()[5].is_empty);
  view.on_panel_event(write_ev);
  assert(view.current_scene() == 1); // remains 1

  std::cout << "[PASS] test_scenes_view_initial_state_and_selection\n";
}

void test_scenes_view_actions() {
  ScenesView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);
  assert(view.used_slot_count() == 5);

  // 1. Create new scene (copies current scene 0 into next available empty slot 5)
  view.select_slot(0);
  bool created = view.create_new_scene();
  assert(created);
  assert(view.used_slot_count() == 6);
  assert(view.selected_slot() == 5);
  assert(!view.scenes()[5].is_empty);
  assert(view.has_unsaved_edits());

  // 2. Duplicate selected scene (slot 5 duplicated into slot 6)
  bool dup = view.duplicate_selected_scene();
  assert(dup);
  assert(view.used_slot_count() == 7);
  assert(view.selected_slot() == 6);
  assert(!view.scenes()[6].is_empty);

  // 3. Move scene left
  std::string name_at_6 = view.scenes()[6].name;
  bool moved = view.move_scene_left();
  assert(moved);
  assert(view.selected_slot() == 5);
  assert(view.scenes()[5].name == name_at_6);

  // 4. Move scene right
  moved = view.move_scene_right();
  assert(moved);
  assert(view.selected_slot() == 6);
  assert(view.scenes()[6].name == name_at_6);

  // 5. Clear slot 6
  bool cleared = view.clear_selected_scene();
  assert(cleared);
  assert(view.scenes()[6].is_empty);
  assert(view.used_slot_count() == 6);

  // 6. Test guard preventing clearing the last remaining populated scene
  // Clear slots until only 1 is left
  for (size_t i = 0; i < 8; ++i) {
    if (view.used_slot_count() > 1) {
      view.select_slot(i);
      view.clear_selected_scene();
    }
  }
  assert(view.used_slot_count() == 1);

  // Find the single remaining scene and try to clear it
  for (size_t i = 0; i < 8; ++i) {
    if (!view.scenes()[i].is_empty) {
      view.select_slot(i);
      bool can_clear_last = view.clear_selected_scene();
      assert(!can_clear_last); // MUST be refused
      assert(view.used_slot_count() == 1);
      assert(!view.scenes()[i].is_empty);
      break;
    }
  }

  std::cout << "[PASS] test_scenes_view_actions\n";
}

void test_scenes_view_on_screen_keyboard() {
  ScenesView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  assert(!view.keyboard().is_open());
  view.select_slot(0);
  bool renamed = view.rename_selected_scene();
  assert(renamed);
  assert(view.keyboard().is_open());

  // Panel event EXIT closes keyboard without throwing or corrupting
  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  view.on_panel_event(exit_ev);
  assert(!view.keyboard().is_open());

  std::cout << "[PASS] test_scenes_view_on_screen_keyboard\n";
}

void test_navigation_controller_scenes_integration() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize();
  assert(ok);
  assert(nav.current_surface() == SurfaceId::Performance);

  // Long press on scene button in SCENES mode opens Scenes surface
  nav.performance_view().set_modes(false, true, 0); // SCENES mode enabled
  assert(nav.performance_view().is_scenes_mode());

  nav.performance_view().handle_part_scene_press(2, true, 1000);
  nav.performance_view().handle_part_scene_press(2, false, 1000 + 600000); // 600ms = long press
  assert(nav.current_surface() == SurfaceId::Scenes);
  assert(nav.scenes_view().selected_slot() == 2);

  // EXIT returns to Performance
  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  nav.on_panel_event(exit_ev);
  assert(nav.current_surface() == SurfaceId::Performance);

  // Open Launcher via MENU button, highlight Scenes (index 4), activate
  PanelEvent menu_ev;
  menu_ev.type = PanelEventType::Button;
  menu_ev.id = MCU_BTN_MENU;
  menu_ev.pressed = true;
  nav.on_panel_event(menu_ev);
  assert(nav.current_surface() == SurfaceId::Launcher);

  nav.launcher_view().set_highlight(4); // Scenes tile
  nav.launcher_view().activate_highlighted();
  assert(nav.current_surface() == SurfaceId::Scenes);

  nav.on_panel_event(exit_ev);
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_navigation_controller_scenes_integration\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to initialize headless adapter\n";
    return 1;
  }

  test_scenes_view_initial_state_and_selection();
  test_scenes_view_actions();
  test_scenes_view_on_screen_keyboard();
  test_navigation_controller_scenes_integration();

  std::cout << "All ScenesView tests passed successfully!\n";
  return 0;
}

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/navigation_controller.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void tap_exit(NavigationController& nav) {
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
}

void test_navigation() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  NavigationController nav;
  assert(nav.initialize());
  lv_obj_update_layout(nav.performance_view().root_container());

  // 1. Initial surface is Performance
  assert(nav.current_surface() == SurfaceId::Performance);
  assert(!lv_obj_is_hidden(nav.performance_view().root_container()));
  assert(lv_obj_is_hidden(nav.launcher_view().skeleton().root_container()));
  assert(lv_obj_is_hidden(nav.effects_view().skeleton().root_container()));
  assert(lv_obj_is_hidden(nav.setlist_view().skeleton().root_container()));

  // 2. EXIT on Performance is a NO-OP
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 3. MENU toggles Launcher
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Launcher);
  assert(lv_obj_is_hidden(nav.performance_view().root_container()));
  assert(!lv_obj_is_hidden(nav.launcher_view().skeleton().root_container()));

  // Press MENU again to toggle back
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Performance);
  assert(!lv_obj_is_hidden(nav.performance_view().root_container()));

  // 4. Launcher navigation with VALUE and WRITE
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Launcher);
  assert(nav.launcher_view().highlight() == 0);

  // Turn VALUE to move highlight to Effects (index 3)
  nav.on_panel_event(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, 3));
  assert(nav.launcher_view().highlight() == 3);

  // Press WRITE to activate Effects
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Effects);
  assert(!lv_obj_is_hidden(nav.effects_view().skeleton().root_container()));

  // 5. EXIT rules from a surface opened via Launcher
  // Inside Effects: open parameter editor
  nav.effects_view().add_insert(ws::v1::INSERT_TYPE_EQ);
  nav.effects_view().open_param_editor(0);
  assert(nav.effects_view().current_mode() == EffectsViewMode::ParamEditor);

  // First EXIT closes ParamEditor, returning to ChainList
  tap_exit(nav);
  assert(nav.effects_view().current_mode() == EffectsViewMode::ChainList);
  assert(nav.current_surface() == SurfaceId::Effects);

  // Second EXIT returns to the surface Launcher was opened from (Performance)
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 5b. Launcher navigation to Setlist (index 1)
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Launcher);
  nav.launcher_view().set_highlight(1);
  assert(nav.launcher_view().is_tile_available(1));
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Setlist);
  assert(!lv_obj_is_hidden(nav.setlist_view().skeleton().root_container()));
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 6. Library tile in Launcher (Library = 2)
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  assert(nav.current_surface() == SurfaceId::Launcher);

  // Move to Library (index 2)
  nav.launcher_view().set_highlight(2);
  assert(nav.launcher_view().is_tile_available(2));

  // Press WRITE: navigates to Library
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Library);

  // EXIT returns back to Performance
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 7. Global Stepping: LIVE SET ◀ ▶ and SCENE ◀ ▶ work on ANY surface
  nav.navigate_to(SurfaceId::Effects);
  assert(nav.current_surface() == SurfaceId::Effects);

  int initial_live_set_steps = nav.live_set_step_count();
  int initial_scene_steps = nav.scene_step_count();

  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_LIVE_SET_NEXT));
  assert(nav.live_set_step_count() == initial_live_set_steps + 1);
  assert(nav.current_surface() == SurfaceId::Effects); // Doesn't leave current surface!

  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_LIVE_SET_PREV));
  assert(nav.live_set_step_count() == initial_live_set_steps);
  assert(nav.current_surface() == SurfaceId::Effects);

  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_SCENE_NEXT));
  assert(nav.scene_step_count() == initial_scene_steps + 1);
  assert(nav.current_surface() == SurfaceId::Effects);

  // 8. REV tap on Performance opens Effects
  nav.navigate_to(SurfaceId::Performance);
  assert(nav.current_surface() == SurfaceId::Performance);
  nav.performance_view().simulate_long_press(nav.performance_view().rev_button());
  assert(nav.current_surface() == SurfaceId::Effects);

  lv_timer_handler();

  std::cout << "[PASS] test_navigation\n";
}

int main() {
  std::cout << "Running navigation_test...\n";
  test_navigation();
  std::cout << "All navigation tests passed!\n";
  return 0;
}

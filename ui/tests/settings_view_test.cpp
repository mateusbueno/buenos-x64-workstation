#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/settings_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_settings_view_initial_state() {
  SettingsView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  assert(view.selected_section() == SettingsSection::KnobsGlobalAssign);
  assert(view.scope() == AssignScope::GlobalDefault);
  assert(view.selected_knob_slot() == 0);
  assert(!view.is_picker_open());
  assert(!view.has_unsaved_edits());

  // Check default factory assign map
  assert(view.global_default_slots().size() == 8);
  assert(view.global_default_slots()[0] == 401);
  assert(view.global_default_slots()[1] == 400);

  std::cout << "[PASS] test_settings_view_initial_state\n";
}

void test_settings_view_navigation_and_slots() {
  SettingsView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // Turn VALUE encoder to navigate slots
  PanelEvent enc_turn;
  enc_turn.type = PanelEventType::Encoder;
  enc_turn.id = MCU_CTRL_VALUE_ENCODER;
  enc_turn.delta = 1;
  view.on_panel_event(enc_turn);
  assert(view.selected_knob_slot() == 1);

  enc_turn.delta = 2;
  view.on_panel_event(enc_turn);
  assert(view.selected_knob_slot() == 3);

  // Scope toggling
  view.set_scope(AssignScope::ThisLiveSet);
  assert(view.scope() == AssignScope::ThisLiveSet);

  view.set_scope(AssignScope::GlobalDefault);
  assert(view.scope() == AssignScope::GlobalDefault);

  std::cout << "[PASS] test_settings_view_navigation_and_slots\n";
}

void test_settings_view_picker_and_assign_global() {
  SettingsView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  ws::v1::AssignTarget last_target = ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET;
  uint32_t last_slot = 0;
  uint32_t last_param = 0;

  view.set_on_assign_changed([&](ws::v1::AssignTarget target, uint32_t slot, uint32_t param) {
    last_target = target;
    last_slot = slot;
    last_param = param;
  });

  // Open picker for K1 (slot 0)
  view.open_param_picker(0);
  assert(view.is_picker_open());

  // Value turn in picker moves selected row
  int32_t init_row = view.picker_selected_row();
  PanelEvent enc_turn;
  enc_turn.type = PanelEventType::Encoder;
  enc_turn.id = MCU_CTRL_VALUE_ENCODER;
  enc_turn.delta = 1;
  view.on_panel_event(enc_turn);
  assert(view.picker_selected_row() == init_row + 1);

  // Select parameter 100 ("Drawbar 16′")
  view.select_param_in_picker(100);
  assert(!view.is_picker_open());
  assert(view.get_slot_param(AssignScope::GlobalDefault, 0) == 100);
  assert(last_target == ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT);
  assert(last_slot == 1);
  assert(last_param == 100);
  assert(!view.has_unsaved_edits()); // Global default does NOT mark Live Set dirty

  std::cout << "[PASS] test_settings_view_picker_and_assign_global\n";
}

void test_settings_view_picker_and_assign_live_set() {
  SettingsView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  ws::v1::AssignTarget last_target = ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT;
  uint32_t last_slot = 0;
  uint32_t last_param = 0;
  bool edit_called = false;

  view.set_on_assign_changed([&](ws::v1::AssignTarget target, uint32_t slot, uint32_t param) {
    last_target = target;
    last_slot = slot;
    last_param = param;
  });

  view.set_on_edit([&]() {
    edit_called = true;
  });

  view.set_scope(AssignScope::ThisLiveSet);

  // Open picker for K3 (slot 2)
  view.open_param_picker(2);
  assert(view.is_picker_open());

  // Select parameter 802 ("Tempo (BPM)")
  view.select_param_in_picker(802);
  assert(!view.is_picker_open());
  assert(view.get_slot_param(AssignScope::ThisLiveSet, 2) == 802);
  assert(last_target == ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET);
  assert(last_slot == 3);
  assert(last_param == 802);
  assert(view.has_unsaved_edits()); // Live Set ASSIGN edit marks dirty
  assert(edit_called);

  // Clear slot on K3
  view.open_param_picker(2);
  view.select_param_in_picker(0);
  assert(view.get_slot_param(AssignScope::ThisLiveSet, 2) == 0);
  assert(last_target == ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET);
  assert(last_slot == 3);
  assert(last_param == 0);

  std::cout << "[PASS] test_settings_view_picker_and_assign_live_set\n";
}

void test_settings_navigation_controller_integration() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize(lv_screen_active());
  assert(ok);

  assert(nav.current_surface() == SurfaceId::Performance);

  // Open Launcher via MENU
  PanelEvent menu_ev;
  menu_ev.type = PanelEventType::Button;
  menu_ev.id = MCU_BTN_MENU;
  menu_ev.pressed = true;
  nav.on_panel_event(menu_ev);
  assert(nav.current_surface() == SurfaceId::Launcher);

  // Navigate to Settings
  nav.navigate_to(SurfaceId::Settings);
  assert(nav.current_surface() == SurfaceId::Settings);

  // When picker is open, EXIT closes picker without leaving Settings
  nav.settings_view().open_param_picker(0);
  assert(nav.settings_view().is_picker_open());

  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  nav.on_panel_event(exit_ev);

  assert(!nav.settings_view().is_picker_open());
  assert(nav.current_surface() == SurfaceId::Settings);

  // Next EXIT returns to surface_before_launcher_ (Performance)
  nav.on_panel_event(exit_ev);
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_settings_navigation_controller_integration\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to initialize headless adapter\n";
    return 1;
  }

  test_settings_view_initial_state();
  test_settings_view_navigation_and_slots();
  test_settings_view_picker_and_assign_global();
  test_settings_view_picker_and_assign_live_set();
  test_settings_navigation_controller_integration();

  std::cout << "All SettingsView tests passed successfully!\n";
  return 0;
}

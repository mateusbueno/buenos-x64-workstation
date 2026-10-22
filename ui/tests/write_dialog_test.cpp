#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/components/write_dialog.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_write_dialog_creation_and_scopes() {
  WriteDialog dlg;
  bool ok = dlg.create(lv_screen_active());
  assert(ok);
  assert(!dlg.is_open());

  dlg.open("Ballad", false, "Part 1 Vol");
  assert(dlg.is_open());
  assert(dlg.is_scene_scope_available());
  assert(dlg.current_scope() == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);

  // Switch scope to Scene
  dlg.set_scope(ws::v1::WRITE_SCOPE_SCENE);
  assert(dlg.current_scope() == ws::v1::WRITE_SCOPE_SCENE);

  // Switch scope to Save As New
  dlg.set_scope(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  assert(dlg.current_scope() == ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  assert(!lv_obj_is_hidden(dlg.name_field_obj()));
  assert(dlg.current_name_input() == "Ballad 2");

  // Confirm write callback fires with correct arguments
  bool write_fired = false;
  ws::v1::WriteScope received_scope;
  std::string received_name;
  dlg.set_on_write([&](ws::v1::WriteScope s, const std::string& n) {
    write_fired = true;
    received_scope = s;
    received_name = n;
  });

  dlg.confirm_write();
  assert(write_fired);
  assert(received_scope == ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
  assert(received_name == "Ballad 2");
  assert(!dlg.is_open());

  std::cout << "[PASS] test_write_dialog_creation_and_scopes\n";
}

void test_write_dialog_timbre_edit_disables_scene_scope() {
  WriteDialog dlg;
  bool ok = dlg.create(lv_screen_active());
  assert(ok);

  dlg.open("Rock Organ", true /* has_timbre_edit */, "Part 2 Timbre");
  assert(dlg.is_open());
  assert(!dlg.is_scene_scope_available());
  assert(dlg.current_scope() == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);

  // Attempting to select Scene scope must be rejected
  dlg.set_scope(ws::v1::WRITE_SCOPE_SCENE);
  assert(dlg.current_scope() == ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);

  std::cout << "[PASS] test_write_dialog_timbre_edit_disables_scene_scope\n";
}

void test_write_dialog_errors() {
  WriteDialog dlg;
  bool ok = dlg.create(lv_screen_active());
  assert(ok);

  dlg.open("Ballad", false, "Part 1 Vol");
  assert(lv_obj_is_hidden(dlg.error_label_obj()));

  // Name in use error
  dlg.show_name_in_use_error("Ballad 2", "Ballad 3");
  assert(!lv_obj_is_hidden(dlg.error_label_obj()));
  assert(dlg.current_name_input() == "Ballad 3");

  // Storage full error
  dlg.show_storage_full_error();
  assert(!lv_obj_is_hidden(dlg.error_label_obj()));

  dlg.clear_error();
  assert(lv_obj_is_hidden(dlg.error_label_obj()));

  std::cout << "[PASS] test_write_dialog_errors\n";
}

void test_write_hold_navigation_integration() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize();
  assert(ok);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 1. With pending edits: Hold WRITE >= 500ms opens WriteDialog
  nav.set_pending_edits(true, false, "Part 1 EQ");
  assert(nav.has_pending_edits());
  assert(nav.performance_view().has_unsaved_edits());

  PanelEvent write_down;
  write_down.type = PanelEventType::Button;
  write_down.id = MCU_BTN_WRITE;
  write_down.pressed = true;
  write_down.timestamp_us = 1000;
  nav.on_panel_event(write_down);
  assert(!nav.write_dialog().is_open());

  PanelEvent write_up_hold;
  write_up_hold.type = PanelEventType::Button;
  write_up_hold.id = MCU_BTN_WRITE;
  write_up_hold.pressed = false;
  write_up_hold.timestamp_us = 1000 + 600000; // 600ms hold
  nav.on_panel_event(write_up_hold);
  assert(nav.write_dialog().is_open());

  // Confirm write inside dialog
  PanelEvent write_press_in_dlg;
  write_press_in_dlg.type = PanelEventType::Button;
  write_press_in_dlg.id = MCU_BTN_WRITE;
  write_press_in_dlg.pressed = true;
  nav.on_panel_event(write_press_in_dlg);
  assert(!nav.write_dialog().is_open());
  assert(!nav.has_pending_edits());
  assert(!nav.performance_view().has_unsaved_edits());

  // 2. Without pending edits: Hold WRITE >= 500ms does NOT open dialog and shows "Nothing to write"
  nav.on_panel_event(write_down);
  nav.on_panel_event(write_up_hold);
  assert(!nav.write_dialog().is_open());
  assert(nav.performance_view().is_warning_active());
  assert(nav.performance_view().warning_text() == tr("write_dialog.nothing_to_write"));

  std::cout << "[PASS] test_write_hold_navigation_integration\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_write_dialog_creation_and_scopes();
  test_write_dialog_timbre_edit_disables_scene_scope();
  test_write_dialog_errors();
  test_write_hold_navigation_integration();

  std::cout << "All WriteDialog tests passed successfully!\n";
  return 0;
}

#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/part_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_part_view_creation_and_navigation() {
  PartView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);
  assert(view.part_index() == 0);
  assert(view.current_section() == PartSection::Timbre);
  assert(view.is_powered_on());
  assert(!view.has_unsaved_edits());

  // Switch section to ZoneKeyboard
  view.set_section(PartSection::ZoneKeyboard);
  assert(view.current_section() == PartSection::ZoneKeyboard);

  // Switch section to LevelOutput
  view.set_section(PartSection::LevelOutput);
  assert(view.current_section() == PartSection::LevelOutput);

  // Power toggle
  view.set_power(false);
  assert(!view.is_powered_on());
  view.set_power(true);
  assert(view.is_powered_on());

  std::cout << "[PASS] test_part_view_creation_and_navigation\n";
}

void test_part_view_timbre_picker() {
  PartView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  assert(!view.is_timbre_picker_open());
  view.open_timbre_picker();
  assert(view.is_timbre_picker_open());
  assert(!lv_obj_is_hidden(view.timbre_picker_modal()));

  // Select timbre index 1: Rhodes Mark I 1975
  view.select_timbre(1);
  assert(!view.is_timbre_picker_open());
  assert(view.timbre_name() == "Rhodes Mark I 1975");
  assert(view.category_name() == "ELECTRIC PIANO");
  assert(view.has_unsaved_edits());

  // Test opening and cancelling with EXIT
  view.open_timbre_picker();
  assert(view.is_timbre_picker_open());
  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  view.on_panel_event(exit_ev);
  assert(!view.is_timbre_picker_open());

  std::cout << "[PASS] test_part_view_timbre_picker\n";
}

void test_part_view_zone_and_level_edits() {
  PartView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // Zone & Keyboard section
  view.set_section(PartSection::ZoneKeyboard);
  assert(view.current_section() == PartSection::ZoneKeyboard);

  // Field 0: Low key. Test setting via MIDI note
  view.on_midi_note(36); // C2 (MIDI 36)
  assert(view.low_key() == 36);
  assert(view.has_unsaved_edits());

  // Value encoder on Field 0 adjustments
  PanelEvent enc_up;
  enc_up.type = PanelEventType::Encoder;
  enc_up.id = MCU_CTRL_VALUE_ENCODER;
  enc_up.delta = 2;
  view.on_panel_event(enc_up);
  assert(view.low_key() == 38); // D2

  // Octave & Transpose
  view.set_zone(38, 100, 1, -2, true);
  assert(view.octave() == 1);
  assert(view.transpose() == -2);
  assert(view.keyboard_on());

  // Level & Output section
  view.set_section(PartSection::LevelOutput);
  assert(view.current_section() == PartSection::LevelOutput);
  assert(!view.is_aux_output());

  // Toggle Output via VALUE push (field 1 is Output)
  PanelEvent push_ev;
  push_ev.type = PanelEventType::Button;
  push_ev.id = MCU_BTN_VALUE_PUSH;
  push_ev.pressed = true;

  // Click / focus field 1
  view.set_level_output(0.80f, true);
  assert(view.is_aux_output());
  assert(view.volume() == 0.80f);

  std::cout << "[PASS] test_part_view_zone_and_level_edits\n";
}

void test_part_view_navigation_controller_integration() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize();
  assert(ok);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 1. VALUE push on Performance opens Part surface for selected knob part (part 1 -> index 0)
  PanelEvent push_ev;
  push_ev.type = PanelEventType::Button;
  push_ev.id = MCU_BTN_VALUE_PUSH;
  push_ev.pressed = true;
  nav.on_panel_event(push_ev);
  assert(nav.current_surface() == SurfaceId::Part);
  assert(nav.part_view().part_index() == 0);

  // EXIT returns to Performance
  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  nav.on_panel_event(exit_ev);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 2. Select knob part 3 and push VALUE -> opens Part surface for Part 3 (index 2)
  nav.performance_view().select_knob_part(2);
  nav.on_panel_event(push_ev);
  assert(nav.current_surface() == SurfaceId::Part);
  assert(nav.part_view().part_index() == 2);

  nav.on_panel_event(exit_ev);
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_part_view_navigation_controller_integration\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_part_view_creation_and_navigation();
  test_part_view_timbre_picker();
  test_part_view_zone_and_level_edits();
  test_part_view_navigation_controller_integration();

  std::cout << "All PartView tests passed successfully!\n";
  return 0;
}

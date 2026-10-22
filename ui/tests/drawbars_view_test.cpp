#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_drawbars_view() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // 1. Setup 8 parts without any organ
  std::vector<PartStripData> no_organ_parts(8);
  for (size_t i = 0; i < 8; ++i) {
    no_organ_parts[i] = {"Grand Piano " + std::to_string(i + 1), "Piano", 0.70f, true, false, false};
  }
  view.set_parts(no_organ_parts);

  assert(!view.has_organ_part());

  // Attempt DRAWBARS toggle without organ -> Refused!
  PanelEvent toggle_ev = PanelEvent::button_press(MCU_BTN_TOGGLE_DRAWBARS_MIXER);
  view.on_panel_event(toggle_ev);

  assert(!view.is_drawbars_mode());
  assert(view.drawbars_led_blinking());
  assert(view.is_warning_active());
  assert(view.warning_text().find("No organ in this Live Set") != std::string::npos);
  assert(lv_obj_is_hidden(view.drawbars_container()));
  assert(!lv_obj_is_hidden(view.mixer_container()));

  // 2. Setup parts where Part 4 is an Organ
  std::vector<PartStripData> with_organ_parts = no_organ_parts;
  with_organ_parts[3] = {"Gospel B3", "Organ", 0.85f, true, false, false};
  view.set_parts(with_organ_parts);

  assert(view.has_organ_part());

  // Clear warning takeover
  view.clear_warning_takeover();
  assert(!view.is_warning_active());

  // Attempt DRAWBARS toggle with organ -> Success!
  view.on_panel_event(toggle_ev);

  assert(view.is_drawbars_mode());
  assert(!view.drawbars_led_blinking());
  assert(!lv_obj_is_hidden(view.drawbars_container()));
  assert(lv_obj_is_hidden(view.mixer_container()));
  assert(view.organ_card() != nullptr);
  assert(!lv_obj_is_hidden(view.organ_card()));

  // Verify initial registration "88 8600 000"
  assert(view.registration_string() == "88 8600 000");

  // Verify 9 drawbar column widgets exist
  for (size_t i = 0; i < 9; ++i) {
    assert(view.drawbar_col(i) != nullptr);
  }

  // 3. Update drawbars directly via set_drawbar
  view.set_drawbar(3, 8); // Change 4' from 6 to 8
  assert(view.drawbar(3) == 8);
  assert(view.registration_string() == "88 8800 000");

  view.set_drawbar(0, 0); // Change 16' from 8 to 0
  assert(view.drawbar(0) == 0);
  assert(view.registration_string() == "08 8800 000");

  // 4. Test physical faders in DRAWBARS mode
  // Fader 1 (MCU_CTRL_FADER_1) -> Drawbar 0 (16') to max (4095 -> 8)
  PanelEvent fader1_ev = PanelEvent::fader_move(MCU_CTRL_FADER_1, 4095);
  view.on_panel_event(fader1_ev);
  assert(view.drawbar(0) == 8);

  // Fader 2 (MCU_CTRL_FADER_2) -> Drawbar 1 (5⅓') to 0
  PanelEvent fader2_ev = PanelEvent::fader_move(MCU_CTRL_FADER_2, 0);
  view.on_panel_event(fader2_ev);
  assert(view.drawbar(1) == 0);

  // Fader 9 (MCU_CTRL_FADER_9) -> Drawbar 8 (1') to max (4095 -> 8)
  PanelEvent fader9_ev = PanelEvent::fader_move(MCU_CTRL_FADER_9, 4095);
  view.on_panel_event(fader9_ev);
  assert(view.drawbar(8) == 8);

  assert(view.registration_string() == "80 8800 008");

  // 5. Test Rotary speed control
  PanelEvent rot_stop_ev = PanelEvent::button_press(MCU_BTN_ROTARY_STOP);
  view.on_panel_event(rot_stop_ev);
  assert(view.rotary_speed() == 0); // STOP

  PanelEvent rot_sf_ev = PanelEvent::button_press(MCU_BTN_ROTARY_SF);
  view.on_panel_event(rot_sf_ev);
  assert(view.rotary_speed() == 2); // FAST

  view.on_panel_event(rot_sf_ev);
  assert(view.rotary_speed() == 1); // SLOW

  // 6. Toggle back to MIXER
  view.on_panel_event(toggle_ev);
  assert(!view.is_drawbars_mode());
  assert(lv_obj_is_hidden(view.drawbars_container()));
  assert(!lv_obj_is_hidden(view.mixer_container()));

  std::cout << "test_drawbars_view passed" << std::endl;
}

int main() {
  test_drawbars_view();
  return 0;
}

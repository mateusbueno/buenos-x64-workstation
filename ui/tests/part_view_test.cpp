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

void test_part_view_sound_engine() {
  PartView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // 1. Sample Part Sound Engine
  view.set_timbre("German Grand D-274", "ACOUSTIC PIANO");
  assert(!view.is_organ());
  view.set_section(PartSection::SoundEngine);
  assert(view.current_section() == PartSection::SoundEngine);

  view.set_sample_sound(0.70f, 10000.0f, 0.30f, 0.85f);
  assert(view.brightness() == 0.70f);
  assert(view.cutoff_hz() == 10000.0f);
  assert(view.resonance() == 0.30f);
  assert(view.vel_sense() == 0.85f);

  // Physical Encoder 1 adjusts Brightness
  PanelEvent enc1;
  enc1.type = PanelEventType::Encoder;
  enc1.id = MCU_CTRL_ENCODER_1;
  enc1.delta = 2;
  view.on_panel_event(enc1);
  assert(view.brightness() > 0.70f);

  // 2. Organ Part Sound Engine
  view.set_timbre("B3 Tonewheel Organ", "ORGAN");
  assert(view.is_organ());
  view.set_section(PartSection::SoundEngine);

  uint8_t dbs[9] = {8, 8, 8, 0, 0, 0, 0, 0, 4};
  view.set_organ_drawbars(dbs);
  assert(view.drawbar(0) == 8);
  assert(view.drawbar(8) == 4);
  assert(view.registration_string() == "88 8000 004");

  view.set_organ_percussion(true);
  assert(view.organ_percussion());

  view.set_organ_rotary(2); // Fast
  assert(view.organ_rotary() == 2);

  std::cout << "[PASS] test_part_view_sound_engine\n";
}

void test_part_view_envelope_eq_and_effects() {
  PartView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // 1. Envelope & EQ
  view.set_section(PartSection::EnvelopeEQ);
  assert(view.current_section() == PartSection::EnvelopeEQ);

  view.set_envelope(20.0f, 350.0f, 0.65f, 400.0f);
  assert(view.attack_ms() == 20.0f);
  assert(view.decay_ms() == 350.0f);
  assert(view.sustain() == 0.65f);
  assert(view.release_ms() == 400.0f);

  view.set_eq(2.0f, -1.5f, 1500.0f, 3.0f);
  assert(view.eq_low_db() == 2.0f);
  assert(view.eq_mid_db() == -1.5f);
  assert(view.eq_mid_freq_hz() == 1500.0f);
  assert(view.eq_high_db() == 3.0f);

  // 2. Effects Sends & Open Inserts
  view.set_section(PartSection::EffectsSends);
  assert(view.current_section() == PartSection::EffectsSends);

  view.set_sends(0.40f, 0.20f);
  assert(view.reverb_send() == 0.40f);
  assert(view.delay_send() == 0.20f);

  bool inserts_opened = false;
  view.set_on_open_inserts([&](int32_t idx) {
    inserts_opened = true;
  });

  view.open_inserts_chain();
  assert(inserts_opened);

  std::cout << "[PASS] test_part_view_envelope_eq_and_effects\n";
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
  test_part_view_sound_engine();
  test_part_view_envelope_eq_and_effects();
  test_part_view_navigation_controller_integration();

  std::cout << "All PartView tests passed successfully!\n";
  return 0;
}

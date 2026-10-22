#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/screens/part_view.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/format/sysex_6op.hpp"

using namespace velora::ui;
namespace fmt = velora::format;

void test_story_12_10_engine_not_available_state() {
  std::cout << "Testing Story 12.10: Engine not-available state...\n";

  PartView pv(nullptr);
  bool ok = pv.create(lv_screen_active());
  assert(ok);

  pv.set_engine_unavailable(true, "1.4");
  assert(pv.is_engine_unavailable());
  assert(pv.required_version() == "1.4");

  // VALUE push or WRITE in unavailable state opens timbre picker
  assert(!pv.is_timbre_picker_open());
  PanelEvent push_ev;
  push_ev.type = PanelEventType::Button;
  push_ev.id = MCU_BTN_VALUE_PUSH;
  push_ev.pressed = true;
  pv.on_panel_event(push_ev);
  assert(pv.is_timbre_picker_open());
  pv.close_timbre_picker();
  assert(!pv.is_timbre_picker_open());

  // PerformanceView takeover warnings and part strips
  PerformanceView pf(nullptr);
  ok = pf.create(lv_screen_active());
  assert(ok);

  std::vector<PartStripData> parts(8);
  parts[0].timbre_name = "Stage Grand";
  parts[0].category = "A. Piano";

  // Single part needs update
  parts[1].timbre_name = "Modern FM Lead";
  parts[1].category = "FM";
  parts[1].needs_update = true;
  parts[1].needs_update_version = "1.4";

  pf.set_parts(parts);
  assert(pf.is_warning_active());
  assert(pf.warning_text().find("! PART 2 NEEDS UPDATE 1.4 · SILENT") != std::string::npos);

  // Two parts need update
  parts[3].timbre_name = "Hyper VA Pad";
  parts[3].category = "VA";
  parts[3].needs_update = true;
  parts[3].needs_update_version = "1.4";

  pf.set_parts(parts);
  assert(pf.is_warning_active());
  assert(pf.warning_text().find("! PARTS 2 · 4 NEED UPDATE 1.4 · SILENT") != std::string::npos);

  // Clear warnings
  parts[1].needs_update = false;
  parts[3].needs_update = false;
  pf.set_parts(parts);
  assert(!pf.is_warning_active());

  std::cout << "[PASS] Story 12.10: Engine not-available state\n";
}

void test_story_12_11_fm_sound_section() {
  std::cout << "Testing Story 12.11: FM Sound Section...\n";

  PartView pv(nullptr);
  bool ok = pv.create(lv_screen_active());
  assert(ok);

  // Set timbre with FM in category or name
  pv.set_timbre("Glass Keys DX", "FM");
  assert(pv.is_fm());
  assert(pv.engine_type() == EngineType::Fm);

  pv.set_section(PartSection::SoundEngine);
  assert(pv.current_section() == PartSection::SoundEngine);

  // Initial group is Algorithm
  assert(pv.fm_group() == FmSoundGroup::Algorithm);
  assert(pv.fm_settings().algorithm == 5);
  assert(pv.fm_settings().feedback == 4);

  // Select Operators sub-group
  pv.set_fm_group(FmSoundGroup::Operators);
  assert(pv.fm_group() == FmSoundGroup::Operators);

  // Modify Operator 1 ratio via API and check
  auto& fm = pv.fm_settings();
  fm.operators[0].ratio = 2.0f;
  assert(pv.fm_settings().operators[0].ratio == 2.0f);

  // Select Pitch Env sub-group
  pv.set_fm_group(FmSoundGroup::PitchEnv);
  assert(pv.fm_group() == FmSoundGroup::PitchEnv);
  fm.pitch_rates[0] = 75;
  fm.pitch_levels[0] = 60;
  assert(pv.fm_settings().pitch_rates[0] == 75);
  assert(pv.fm_settings().pitch_levels[0] == 60);

  // Select LFO sub-group
  pv.set_fm_group(FmSoundGroup::Lfo);
  assert(pv.fm_group() == FmSoundGroup::Lfo);
  fm.lfo_wave = 2; // Saw up
  fm.lfo_speed = 88;
  assert(pv.fm_settings().lfo_wave == 2);
  assert(pv.fm_settings().lfo_speed == 88);

  // Knob 2 (Physical Encoder 2: enc_idx == 1) adjusts Brightness offset on ANY sub-group
  int initial_bright = pv.fm_settings().brightness_offset;
  PanelEvent knob2_turn;
  knob2_turn.type = PanelEventType::Encoder;
  knob2_turn.id = MCU_CTRL_ENCODER_2; // Physical Knob 2
  knob2_turn.delta = 5;
  pv.on_panel_event(knob2_turn);
  assert(pv.fm_settings().brightness_offset == initial_bright + 5);

  // Switch to Algorithm sub-group, Knob 2 still works
  pv.set_fm_group(FmSoundGroup::Algorithm);
  pv.on_panel_event(knob2_turn);
  assert(pv.fm_settings().brightness_offset == initial_bright + 10);

  std::cout << "[PASS] Story 12.11: FM Sound Section\n";
}

void test_story_12_12_va_sound_section() {
  std::cout << "Testing Story 12.12: VA Sound Section...\n";

  PartView pv(nullptr);
  bool ok = pv.create(lv_screen_active());
  assert(ok);

  // Set timbre with VA in category or name
  pv.set_timbre("Warm Analog Strings", "VA");
  assert(pv.is_va());
  assert(pv.engine_type() == EngineType::Va);

  pv.set_section(PartSection::SoundEngine);
  assert(pv.current_section() == PartSection::SoundEngine);

  // Initial group is Oscillators
  assert(pv.va_group() == VaSoundGroup::Oscillators);
  assert(pv.va_settings().osc1_wave == 0); // Saw up
  assert(pv.va_settings().osc2_octave == 0);

  // Mixer group
  pv.set_va_group(VaSoundGroup::Mixer);
  assert(pv.va_group() == VaSoundGroup::Mixer);
  pv.va_settings().mix_sub = 45;
  assert(pv.va_settings().mix_sub == 45);

  // Filter group
  pv.set_va_group(VaSoundGroup::Filter);
  assert(pv.va_group() == VaSoundGroup::Filter);
  pv.va_settings().filter_type = 1; // LP 12
  pv.va_settings().cutoff = 80;
  assert(pv.va_settings().filter_type == 1);
  assert(pv.va_settings().cutoff == 80);

  // Envelopes group
  pv.set_va_group(VaSoundGroup::Envelopes);
  assert(pv.va_group() == VaSoundGroup::Envelopes);
  pv.va_settings().amp_attack = 12;
  pv.va_settings().filter_decay = 64;
  assert(pv.va_settings().amp_attack == 12);
  assert(pv.va_settings().filter_decay == 64);

  // LFO group
  pv.set_va_group(VaSoundGroup::Lfo);
  assert(pv.va_group() == VaSoundGroup::Lfo);
  pv.va_settings().lfo_wave = 3; // Square
  assert(pv.va_settings().lfo_wave == 3);

  // Voice group
  pv.set_va_group(VaSoundGroup::Voice);
  assert(pv.va_group() == VaSoundGroup::Voice);
  pv.va_settings().voice_mode = 2; // Legato
  pv.va_settings().unison_voices = 6;
  assert(pv.va_settings().voice_mode == 2);
  assert(pv.va_settings().unison_voices == 6);

  // Knob 2 (Physical Encoder 2: enc_idx == 1) directly adjusts brightness_offset / cutoff
  PanelEvent knob2_turn;
  knob2_turn.type = PanelEventType::Encoder;
  knob2_turn.id = MCU_CTRL_ENCODER_2;
  knob2_turn.delta = 8;
  pv.va_settings().brightness_offset = 50;
  pv.on_panel_event(knob2_turn);
  assert(pv.va_settings().brightness_offset == 58);
  assert(pv.va_settings().cutoff == 58);

  std::cout << "[PASS] Story 12.12: VA Sound Section\n";
}

void test_step_by_step_exit_navigation() {
  std::cout << "Testing Step-by-Step EXIT Navigation (Stories 12.11, 12.12)...\n";

  NavigationController nav(nullptr);
  bool ok = nav.initialize();
  assert(ok);

  // Set Part 1 to FM timbre and open Part surface
  nav.part_view().set_timbre("FM DX Glass", "FM");
  assert(nav.part_view().is_fm());
  nav.part_view().set_section(PartSection::SoundEngine);
  nav.navigate_to(SurfaceId::Part);
  assert(nav.current_surface() == SurfaceId::Part);

  // 1. Initially sound group has focus
  assert(nav.part_view().is_sound_group_focus());

  // 2. Press VALUE PUSH: Enters field focus
  PanelEvent push_ev;
  push_ev.type = PanelEventType::Button;
  push_ev.id = MCU_BTN_VALUE_PUSH;
  push_ev.pressed = true;
  nav.part_view().on_panel_event(push_ev);
  assert(!nav.part_view().is_sound_group_focus());
  assert(!nav.part_view().is_editing_field());

  // 3. Press VALUE PUSH again: Enters field editing mode
  nav.part_view().on_panel_event(push_ev);
  assert(nav.part_view().is_editing_field());

  // 4. First EXIT step: Exits editing mode, retains field focus
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Part);
  assert(!nav.part_view().is_editing_field());
  assert(!nav.part_view().is_sound_group_focus());

  // 5. Second EXIT step: Returns focus to Sound group tab strip
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Part);
  assert(nav.part_view().is_sound_group_focus());

  // 6. Third EXIT step: Leaves Part view, returns to Performance view
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] Step-by-Step EXIT Navigation\n";
}

void test_story_12_4_sysex_ui_notification_and_import() {
  std::cout << "Testing Story 12.4: SysEx UI Notice Toast and Import Flow...\n";

  NavigationController nav(nullptr);
  bool ok = nav.initialize();
  assert(ok);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 1. Valid single-voice SysEx received
  fmt::SysexParseResult single_dump;
  single_dump.status = fmt::SysexParseStatus::kOk;
  fmt::SysexVoiceDump single_v;
  single_v.slot = 1;
  single_v.name = "BASS 1";
  single_dump.voices.push_back(single_v);

  nav.on_sysex_dump_received(single_dump);
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "6-OP SysEx received: 1 voices");
  assert(nav.notice_toast().action_text() == "Import?");
  // Performance surface remains active, play is not blocked
  assert(nav.current_surface() == SurfaceId::Performance);

  // Dismiss without importing via EXIT
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(!nav.notice_toast().is_visible());
  assert(nav.current_surface() == SurfaceId::Performance);

  // 2. Valid 32-voice bulk dump received
  fmt::SysexParseResult bulk_dump;
  bulk_dump.status = fmt::SysexParseStatus::kOk;
  for (int i = 1; i <= 32; ++i) {
    fmt::SysexVoiceDump v;
    v.slot = static_cast<uint8_t>(i);
    v.name = "VOX " + std::to_string(i);
    bulk_dump.voices.push_back(v);
  }

  nav.on_sysex_dump_received(bulk_dump);
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "6-OP SysEx received: 32 voices");
  assert(nav.notice_toast().action_text() == "Import?");
  assert(nav.current_surface() == SurfaceId::Performance);

  // Confirming "Import?" opens Import rows
  lv_obj_send_event(nav.notice_toast().action_button(), LV_EVENT_CLICKED, nullptr);
  assert(!nav.notice_toast().is_visible());
  assert(nav.current_surface() == SurfaceId::Import);

  // Verify rows match Story 12.3: default category Synth Poly, format string, ready status
  assert(nav.import_view().items().size() == 32);
  for (size_t i = 0; i < 32; ++i) {
    const auto& it = nav.import_view().items()[i];
    assert(it.file_name == "VOX " + std::to_string(i + 1));
    assert(it.format_str == "Import 6-OP SysEx");
    assert(it.category == fmt::Category::kSynthPoly);
    assert(it.status == ImportItemStatus::Ready);
  }

  // 3. Damaged SysEx dump shows damaged notice copy with no action button
  nav.navigate_to(SurfaceId::Performance);
  nav.on_sysex_damaged();
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "SysEx received but damaged. Nothing was imported.");
  assert(nav.notice_toast().action_text().empty());
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] Story 12.4: SysEx UI Notice Toast and Import Flow\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_story_12_4_sysex_ui_notification_and_import();
  test_story_12_10_engine_not_available_state();
  test_story_12_11_fm_sound_section();
  test_story_12_12_va_sound_section();
  test_step_by_step_exit_navigation();

  std::cout << "All Epic 12 (Stories 12.4, 12.10, 12.11, 12.12) tests passed successfully!\n";
  return 0;
}

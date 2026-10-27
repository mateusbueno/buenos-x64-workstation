#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_performance_view() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // 1. Check container geometry matches DESIGN.md
  assert(lv_obj_get_width(view.root_container()) == 1024);
  assert(lv_obj_get_height(view.root_container()) == 600);

  assert(lv_obj_get_height(view.top_bar()) == 56);
  assert(lv_obj_get_y(view.top_bar()) == 0);

  assert(lv_obj_get_height(view.hero()) == 118);
  assert(lv_obj_get_y(view.hero()) == 56);

  assert(lv_obj_get_height(view.scene_strip()) == 72);
  assert(lv_obj_get_y(view.scene_strip()) == 174);

  assert(lv_obj_get_height(view.part_area()) == 298);
  assert(lv_obj_get_y(view.part_area()) == 246);

  assert(lv_obj_get_height(view.status_bar()) == 56);
  assert(lv_obj_get_y(view.status_bar()) == 544);

  // 2. Test Tempo display
  view.set_tempo(132.5f);
  assert(view.tempo() == 132.5f);

  // 3. Test Setlist context
  view.set_setlist_context("", 0, 0);
  view.set_setlist_context("Sunday Service", 2, 10);

  // 4. Test Mode mirrors
  view.set_modes(false, false, 0, 1);
  view.set_modes(true, true, 1, 1);
  view.set_modes(false, false, 2, 1);

  // 5. Test Live Set Hero and unsaved indicator
  view.set_live_set("Stage Grand", 1, true);
  assert(view.live_set_name() == "Stage Grand");
  assert(view.has_unsaved_edits() == true);

  view.set_next_live_set("Warm Pad", "● READY", true);

  // 6. Test Scene Strip
  std::vector<SceneTileData> scenes(8);
  scenes[0] = {"Intro", "Piano", false};
  scenes[1] = {"Verse", "Piano + Pad", false};
  scenes[2] = {"Chorus", "Full Band", false};
  scenes[3] = {"Bridge", "Strings", false};
  scenes[4] = {"", "", true}; // Empty slot
  scenes[5] = {"", "", true};
  scenes[6] = {"", "", true};
  scenes[7] = {"Outro", "Piano Solo", false};

  view.set_scenes(scenes, 1);
  assert(view.current_scene() == 1);

  // Direct recall
  view.recall_scene(3);
  assert(view.current_scene() == 3);

  // Panel navigation buttons
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_SCENE_PREV));
  assert(view.current_scene() == 2);

  view.on_panel_event(PanelEvent::button_press(MCU_BTN_SCENE_NEXT));
  assert(view.current_scene() == 3);

  // 7. Test Part Strips (Story 4.21)
  assert(view.part_count() == 8);

  // Check strip geometry and touch target min
  for (size_t i = 0; i < 8; ++i) {
    lv_obj_t* card = view.part_card(i);
    assert(card != nullptr);
    assert(lv_obj_get_width(card) == 115);
    assert(lv_obj_get_height(card) == 282);

    lv_obj_t* pwr = view.part_power_btn(i);
    assert(pwr != nullptr);
    // Touch target >= 56 px
    assert(lv_obj_get_width(pwr) >= 56);
    assert(lv_obj_get_height(pwr) >= 56);
  }

  // Populate parts with diverse states
  std::vector<PartStripData> test_parts(8);
  test_parts[0] = {"Concert Grand", "Piano", 0.85f, true, false, false};
  test_parts[1] = {"Warm Pad", "Synth Pad", 0.60f, true, false, false};
  test_parts[2] = {"Chamber Strings", "Strings", 0.50f, false, false, false}; // Off part
  test_parts[3] = {"Upright Bass", "A. Bass", 0.70f, true, false, false};
  test_parts[4] = {"", "", 0.0f, false, true, false}; // Empty part
  test_parts[5] = {"", "", 0.0f, false, true, false};
  test_parts[6] = {"", "", 0.0f, false, true, false};
  test_parts[7] = {"Solo Trumpet", "Brass", 0.40f, true, false, false};

  view.set_parts(test_parts);
  assert(view.part_data(0).timbre_name == "Concert Grand");
  assert(view.part_data(2).enabled == false);
  assert(view.part_data(4).is_empty == true);

  // Test callbacks
  int toggled_part = -1;
  bool toggled_state = false;
  view.set_part_toggle_callback([&](int32_t idx, bool state) {
    toggled_part = idx;
    toggled_state = state;
  });

  view.set_part_enabled(2, true);
  assert(view.part_data(2).enabled == true);
  assert(toggled_part == 2);
  assert(toggled_state == true);

  view.set_part_volume(0, 0.95f);
  assert(view.part_data(0).volume == 0.95f);

  // 8. Test Missing Library warning takeover
  assert(!view.is_warning_active());

  test_parts[1].missing_library = true;
  view.set_parts(test_parts);
  assert(view.is_warning_active());
  assert(!view.warning_text().empty());

  // Clear warning
  test_parts[1].missing_library = false;
  view.set_parts(test_parts);
  assert(!view.is_warning_active());

  // 9. Test Status Bar and REV button
  bool rev_clicked = false;
  view.set_rev_click_callback([&]() {
    rev_clicked = true;
  });

  view.set_status(32, 128, true, 1, 0.40f);

  assert(view.rev_button() != nullptr);
  assert(lv_obj_get_height(view.rev_button()) >= 56);

  // Simulate REV button click
  lv_obj_send_event(view.rev_button(), LV_EVENT_CLICKED, nullptr);
  assert(rev_clicked);

  // 10. Test state update from protobuf State
  ws::v1::State st;
  st.set_live_set_name("Sunday Morning");
  st.mutable_edits()->set_current_dirty(false);
  auto* pv = st.add_performance_init();
  pv->set_param_id(ws::v1::PARAM_TEMPO_BPM);
  pv->set_value(95.0f);

  st.set_scene_count(4);
  st.set_current_scene(0);

  auto* sl = st.mutable_setlists();
  sl->set_loaded_setlist_name("Easter 2026");
  sl->set_last_setlist_position(1);
  sl->set_loaded_setlist_size(5);

  view.update_from_state(st);
  assert(view.live_set_name() == "Sunday Morning");
  assert(view.tempo() == 95.0f);
  assert(view.current_scene() == 0);
  assert(view.has_unsaved_edits() == false);

  lv_timer_handler();
  adapter.shutdown();

  std::cout << "[PASS] test_performance_view\n";
}

static void test_top_bar_tempo_clock_source_and_clock_lost() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());

  // 1. Clock source Internal shows tempo
  view.set_clock_source("Internal");
  view.set_tempo(72.0f);
  assert(view.clock_source() == "Internal");
  assert(!view.is_clock_lost());
  assert(view.tempo() == 72.0f);

  // 2. Clock source DIN / USB
  view.set_clock_source("DIN");
  assert(view.clock_source() == "DIN");

  view.set_clock_source("USB");
  assert(view.clock_source() == "USB");

  // 3. Throttled tempo updates (twice per second max)
  view.set_tempo(73.0f, 1000);
  assert(view.tempo() == 73.0f);

  view.set_tempo(74.0f, 1200); // 200 ms later: throttled
  assert(view.tempo() == 73.0f);

  view.set_tempo(75.0f, 1600); // 600 ms later: updated
  assert(view.tempo() == 75.0f);

  // 4. Clock lost state
  view.set_clock_lost(true);
  assert(view.is_clock_lost());
  assert(view.is_warning_active());

  // Resume clock clears clock lost and warning takeover
  view.set_clock_lost(false);
  assert(!view.is_clock_lost());
  assert(!view.is_warning_active());

  adapter.shutdown();
  std::cout << "[PASS] test_top_bar_tempo_clock_source_and_clock_lost\n";
}

// Story 9.10: the Status Bar shows STUDIO while Velora Studio edits arrive (State.edits.studio_edit_active, which the engine clears after 3 s idle), and the dirty Live Set shows
// the diamond in the Hero. Headless: the label's visibility and text, not pixels (the board render is unverified).
static void test_studio_status_and_diamond() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));
  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());
  assert(!view.studio_active() && !view.studio_indicator_visible());  // nothing arrives: no STUDIO

  ws::v1::State st;
  st.set_live_set_name("Sunday Morning");
  st.mutable_edits()->set_studio_edit_active(true);  // an accepted Studio edit (and an unsaved one)
  st.mutable_edits()->set_current_dirty(true);
  view.update_from_state(st);
  assert(view.studio_active() && view.studio_indicator_visible());
  assert(view.has_unsaved_edits());  // the diamond: the Live Set carries unsaved edits

  st.mutable_edits()->set_studio_edit_active(false);  // idle 3 s: STUDIO goes out, the diamond stays until Write or power-off
  view.update_from_state(st);
  assert(!view.studio_active() && !view.studio_indicator_visible());
  assert(view.has_unsaved_edits());

  ws::v1::State no_edits;  // a State without edits (an engine without an edit session) never shows STUDIO
  no_edits.set_live_set_name("Sunday Morning");
  st.mutable_edits()->set_studio_edit_active(true);
  view.update_from_state(st);
  assert(view.studio_indicator_visible());
  view.update_from_state(no_edits);
  assert(!view.studio_indicator_visible());
  view.set_studio_active(true);
  assert(view.studio_indicator_visible());

  lv_timer_handler();
  adapter.shutdown();
  std::cout << "[PASS] test_studio_status_and_diamond\n";
}

int main() {
  std::cout << "Running performance_view_test...\n";
  test_studio_status_and_diamond();
  test_performance_view();
  test_top_bar_tempo_clock_source_and_clock_lost();
  std::cout << "All performance view tests passed!\n";
  return 0;
}

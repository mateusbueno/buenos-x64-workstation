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

  // 7. Test state update from protobuf State
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

int main() {
  std::cout << "Running performance_view_test...\n";
  test_performance_view();
  std::cout << "All performance view tests passed!\n";
  return 0;
}

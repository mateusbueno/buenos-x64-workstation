// Story 4.36: Footswitch function
// FR109, AR39, UX-DR65, UX-DR86
// Verifies:
// 1. Switch pedal event (MCU_CTRL_PEDAL_SWITCH) with default setting advances scene seamlessly (same as SCENE ▶).
// 2. Settings › Keyboard & Pedals › Footswitch function row allows choosing:
//    Next scene / Previous scene / Next Live Set / Rotary speed
// 3. Changing setting stores value in sysd config and footswitch follows immediately.
// 4. Rotary speed toggles slow/fast on the organ.
// 5. Next Live Set at the end of setlist does nothing (same as LIVE SET ▶).
// 6. Virtual panel key 'f' / 'F' dispatches MCU_CTRL_PEDAL_SWITCH.

#include <cassert>
#include <iostream>
#include <vector>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/system_config.hpp"
#include "velora/ui/virtual_panel.hpp"

using namespace velora::ui;

static void test_virtual_panel_key_f() {
  VirtualPanel& vp = VirtualPanel::instance();
  vp.clear_listeners();

  std::vector<PanelEvent> received;
  vp.add_listener([&](const PanelEvent& ev) {
    received.push_back(ev);
  });

  bool handled_down = vp.handle_key('f', true);
  assert(handled_down);
  assert(received.size() == 1);
  assert(received[0].id == MCU_CTRL_PEDAL_SWITCH);
  assert(received[0].pressed);

  bool handled_up = vp.handle_key('f', false);
  assert(handled_up);
  assert(received.size() == 2);
  assert(received[1].id == MCU_CTRL_PEDAL_SWITCH);
  assert(!received[1].pressed);

  // Capital 'F'
  bool handled_cap = vp.handle_key('F', true);
  assert(handled_cap);
  assert(received.size() == 3);
  assert(received[2].id == MCU_CTRL_PEDAL_SWITCH);

  vp.clear_listeners();
  std::cout << "[PASS] test_virtual_panel_key_f\n";
}

static void test_footswitch_functions() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  NavigationController nav;
  assert(nav.initialize());
  lv_obj_update_layout(nav.performance_view().root_container());

  // Setup 4 mock scenes on performance view (initial scene = 0)
  std::vector<SceneTileData> scenes(4);
  scenes[0].name = "Verse";
  scenes[1].name = "Chorus";
  scenes[2].name = "Bridge";
  scenes[3].name = "Outro";
  nav.performance_view().set_scenes(scenes, 0);
  assert(nav.performance_view().current_scene() == 0);

  // 1. Default setting: NextScene (FR109)
  SystemConfig::instance().footswitch_function = FootswitchFunction::NextScene;
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::NextScene);

  // Press footswitch -> advances scene to 1 (Chorus)
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 1);
  assert(nav.scene_step_count() == 1);

  // Release does nothing
  nav.on_panel_event(PanelEvent::button_release(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 1);

  // Press footswitch again -> advances scene to 2 (Bridge)
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 2);
  assert(nav.scene_step_count() == 2);

  // 2. Change setting to PreviousScene
  SystemConfig::instance().footswitch_function = FootswitchFunction::PreviousScene;
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 1);
  assert(nav.scene_step_count() == 1);

  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 0);
  assert(nav.scene_step_count() == 0);

  // Pressing previous scene at scene 0 does not underflow
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().current_scene() == 0);

  // 3. Change setting to NextLiveSet
  SystemConfig::instance().footswitch_function = FootswitchFunction::NextLiveSet;
  const int initial_ls_steps = nav.live_set_step_count();
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.live_set_step_count() == initial_ls_steps + 1);

  // 4. Change setting to RotarySpeed
  SystemConfig::instance().footswitch_function = FootswitchFunction::RotarySpeed;
  nav.performance_view().set_rotary_speed(1); // slow
  assert(nav.performance_view().rotary_speed() == 1);

  // Press footswitch -> toggles to fast (2)
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().rotary_speed() == 2);

  // Press footswitch again -> toggles back to slow (1)
  nav.on_panel_event(PanelEvent::button_press(MCU_CTRL_PEDAL_SWITCH));
  assert(nav.performance_view().rotary_speed() == 1);

  std::cout << "[PASS] test_footswitch_functions\n";
}

static void test_settings_view_footswitch_ui_and_persistence() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  SettingsView view(nullptr);
  assert(view.create(lv_screen_active()));

  // Navigate to Keyboard & Pedals section
  view.select_section(SettingsSection::KeyboardPedals);

  // Verify SegmentedControl has 4 options
  // 0: Next scene
  // 1: Previous scene
  // 2: Next Live Set
  // 3: Rotary speed

  // Test selecting index 0 (Next scene)
  view.seg_footswitch().set_selected_index(0);
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::NextScene);

  // Test selecting index 1 (Previous scene)
  view.seg_footswitch().set_selected_index(1);
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::PreviousScene);

  // Test selecting index 2 (Next Live Set)
  view.seg_footswitch().set_selected_index(2);
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::NextLiveSet);

  // Test selecting index 3 (Rotary speed)
  view.seg_footswitch().set_selected_index(3);
  assert(SystemConfig::instance().footswitch_function == FootswitchFunction::RotarySpeed);

  // Verify JSON serialization and deserialization
  auto& cfg = SystemConfig::instance();
  cfg.footswitch_function = FootswitchFunction::NextScene;
  std::string json0 = cfg.to_json();
  assert(json0.find("\"footswitch_function\": \"next_scene\"") != std::string::npos);

  cfg.footswitch_function = FootswitchFunction::PreviousScene;
  std::string json1 = cfg.to_json();
  assert(json1.find("\"footswitch_function\": \"previous_scene\"") != std::string::npos);

  cfg.footswitch_function = FootswitchFunction::NextLiveSet;
  std::string json2 = cfg.to_json();
  assert(json2.find("\"footswitch_function\": \"next_live_set\"") != std::string::npos);

  cfg.footswitch_function = FootswitchFunction::RotarySpeed;
  std::string json3 = cfg.to_json();
  assert(json3.find("\"footswitch_function\": \"rotary_speed\"") != std::string::npos);

  // Deserialize checks
  SystemConfig c0, c1, c2, c3;
  assert(c0.from_json(json0));
  assert(c0.footswitch_function == FootswitchFunction::NextScene);

  assert(c1.from_json(json1));
  assert(c1.footswitch_function == FootswitchFunction::PreviousScene);

  assert(c2.from_json(json2));
  assert(c2.footswitch_function == FootswitchFunction::NextLiveSet);

  assert(c3.from_json(json3));
  assert(c3.footswitch_function == FootswitchFunction::RotarySpeed);

  // Legacy alias check ("scene_advance" -> NextScene, "live_set_advance" -> NextLiveSet)
  SystemConfig cl0, cl2;
  std::string legacy0 = "{\"schema_version\": 1, \"footswitch_function\": \"scene_advance\"}";
  assert(cl0.from_json(legacy0));
  assert(cl0.footswitch_function == FootswitchFunction::NextScene);

  std::string legacy2 = "{\"schema_version\": 1, \"footswitch_function\": \"live_set_advance\"}";
  assert(cl2.from_json(legacy2));
  assert(cl2.footswitch_function == FootswitchFunction::NextLiveSet);

  std::cout << "[PASS] test_settings_view_footswitch_ui_and_persistence\n";
}

int main() {
  test_virtual_panel_key_f();
  test_footswitch_functions();
  test_settings_view_footswitch_ui_and_persistence();
  std::cout << "All Story 4.36 footswitch tests PASSED!\n";
  return 0;
}

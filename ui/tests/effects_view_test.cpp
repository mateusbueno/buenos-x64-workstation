#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/effects_view.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_effects_view() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  EffectsView view;
  assert(view.create());
  lv_obj_update_layout(view.skeleton().root_container());

  // 1. Part selection
  assert(view.selected_part() == 0);
  view.select_part(2);
  assert(view.selected_part() == 2);
  assert(view.slot_count() == 0);

  // 2. Add inserts
  view.add_insert(ws::v1::INSERT_TYPE_ROTARY);
  assert(view.slot_count() == 1);

  view.add_insert(ws::v1::INSERT_TYPE_EQ);
  assert(view.slot_count() == 2);

  // 3. Bypass
  view.toggle_bypass(0);

  // 4. Move insert
  view.move_insert(1, 0);
  assert(view.slot_count() == 2);

  // 5. Remove insert
  view.remove_insert(0);
  assert(view.slot_count() == 1);

  // 6. Parameter editor & EXIT
  view.open_param_editor(0);
  assert(view.current_mode() == EffectsViewMode::ParamEditor);

  // Send EXIT button press
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  assert(view.current_mode() == EffectsViewMode::ChainList);

  // 7. Error handling
  ws::v1::Error err;
  err.set_code(ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED);
  err.set_message("FX budget exceeded");
  view.handle_error(err);
  assert(view.skeleton().hint_bar().right_text() == "FX budget exceeded");

  // 8. Test Sends section
  view.select_section(EffectsSection::Sends);
  assert(view.current_section() == EffectsSection::Sends);
  view.set_part_send_levels(0, 0.45f, 0.20f);

  // 9. Test Master section
  view.select_section(EffectsSection::Master);
  assert(view.current_section() == EffectsSection::Master);
  view.set_master_eq(2.0f, -1.0f, 1500.0f, 3.0f, false);
  view.set_master_comp(-15.0f, 3.0f, 20.0f, 150.0f, 2.0f, false);
  view.set_line_in_level(0.75f);

  // 10. Test Outputs section
  view.select_section(EffectsSection::Outputs);
  assert(view.current_section() == EffectsSection::Outputs);
  view.set_part_output(0, ws::v1::PART_OUTPUT_AUX);
  view.set_part_output(1, ws::v1::PART_OUTPUT_MAIN);

  // 11. Switch back to Inserts
  view.select_section(EffectsSection::Inserts);
  assert(view.current_section() == EffectsSection::Inserts);

  lv_timer_handler();
  adapter.shutdown();

  std::cout << "[PASS] test_effects_view\n";
}

int main() {
  std::cout << "Running effects_view_test...\n";
  test_effects_view();
  std::cout << "All effects view tests passed!\n";
  return 0;
}

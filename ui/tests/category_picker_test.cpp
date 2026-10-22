#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/components/category_picker.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_category_picker_initial_state() {
  CategoryPicker picker;
  bool ok = picker.create(lv_screen_active());
  assert(ok);
  assert(!picker.is_visible());
  assert(picker.category_tiles().size() == 17);

  lv_obj_update_layout(picker.container());
  // Each tile is >= 56px in height and width per UX requirements
  for (lv_obj_t* tile : picker.category_tiles()) {
    assert(lv_obj_get_width(tile) >= 56);
    assert(lv_obj_get_height(tile) >= 56);
  }

  std::cout << "[PASS] test_category_picker_initial_state\n";
}

void test_category_picker_selection_and_callbacks() {
  CategoryPicker picker;
  picker.create(lv_screen_active());

  picker.set_target_subtitle("Viola_Caipira.sfz");
  assert(picker.target_subtitle() == "Viola_Caipira.sfz");

  picker.set_selected_category(velora::format::Category::kStrings);
  assert(picker.selected_category() == velora::format::Category::kStrings);

  bool select_fired = false;
  velora::format::Category chosen_cat = velora::format::Category::kNone;
  bool chosen_apply_all = false;

  picker.set_on_select([&](velora::format::Category c, bool all) {
    select_fired = true;
    chosen_cat = c;
    chosen_apply_all = all;
  });

  picker.set_visible(true);
  assert(picker.is_visible());

  // Encoder turn down
  picker.on_panel_event(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, 1));
  // WRITE confirms
  picker.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));

  assert(select_fired);
  assert(chosen_cat == velora::format::Category::kBrass);
  assert(!picker.is_visible());

  std::cout << "[PASS] test_category_picker_selection_and_callbacks\n";
}

void test_category_picker_apply_all_toggle() {
  CategoryPicker picker;
  picker.create(lv_screen_active());

  assert(!picker.apply_to_all());
  picker.set_apply_to_all(true);
  assert(picker.apply_to_all());

  picker.set_visible(true);

  bool cancel_fired = false;
  picker.set_on_cancel([&]() {
    cancel_fired = true;
  });

  // EXIT button cancels
  picker.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  assert(cancel_fired);
  assert(!picker.is_visible());

  std::cout << "[PASS] test_category_picker_apply_all_toggle\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_category_picker_initial_state();
  test_category_picker_selection_and_callbacks();
  test_category_picker_apply_all_toggle();

  std::cout << "All CategoryPicker tests passed!\n";
  return 0;
}

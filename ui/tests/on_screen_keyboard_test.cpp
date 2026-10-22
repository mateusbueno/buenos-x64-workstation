#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/components/on_screen_keyboard.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_on_screen_keyboard() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  OnScreenKeyboard osk;
  assert(osk.create());
  lv_obj_update_layout(osk.root_container());

  // 1. Initial State: hidden
  assert(!osk.is_open());
  assert(lv_obj_is_hidden(osk.root_container()));

  // 2. Open keyboard with initial text "Hello"
  osk.open("Hello", KeyboardLayout::EN);
  lv_obj_update_layout(osk.root_container());

  assert(osk.is_open());
  assert(!lv_obj_is_hidden(osk.root_container()));
  assert(osk.text() == "Hello");

  // Verify positioning and dimensions: lower area (y >= 300), height >= 250
  assert(lv_obj_get_y(osk.root_container()) == 324);
  assert(lv_obj_get_height(osk.root_container()) == 276);
  assert(lv_obj_get_width(osk.root_container()) == 1024);

  // Verify key touch targets: all keys >= 56 px
  assert(osk.key_count() >= 30);
  for (size_t i = 0; i < osk.key_count(); ++i) {
    lv_obj_t* btn = osk.key_button(i);
    assert(btn != nullptr);
    assert(lv_obj_get_height(btn) >= 56);
  }

  // Done button is accent-filled
  assert(osk.done_button() != nullptr);
  assert(lv_color_to_u32(lv_obj_get_style_bg_color(osk.done_button(), LV_PART_MAIN)) ==
         lv_color_to_u32(Theme::current().accent));

  // 3. Typing in EN layout
  osk.press_key("Space");
  osk.press_key("w");
  osk.press_key("o");
  osk.press_key("r");
  osk.press_key("l");
  osk.press_key("d");
  assert(osk.text() == "Hello world");

  // Backspace
  osk.press_key("⌫");
  assert(osk.text() == "Hello worl");

  // 4. Test PT-BR Layout with Ç and accents
  osk.set_layout(KeyboardLayout::PT_BR);
  osk.set_text("Cora");
  osk.press_key("ç");
  osk.press_key("ã");
  osk.press_key("o");
  assert(osk.text() == "Coração");

  // Multi-byte UTF-8 backspace
  osk.press_key("⌫");
  assert(osk.text() == "Coraçã");
  osk.press_key("⌫");
  assert(osk.text() == "Coraç");
  osk.press_key("⌫");
  assert(osk.text() == "Cora");

  // 5. Test ES Layout with Ñ and accents
  osk.set_layout(KeyboardLayout::ES);
  osk.set_text("A");
  osk.press_key("ñ");
  osk.press_key("o");
  assert(osk.text() == "Año");

  osk.set_text("Canci");
  osk.press_key("ó");
  osk.press_key("n");
  assert(osk.text() == "Canción");

  // 6. Test WRITE confirms and closes
  std::string confirmed_text;
  osk.set_confirm_callback([&](const std::string& t) {
    confirmed_text = t;
  });

  PanelEvent write_ev = PanelEvent::button_press(MCU_BTN_WRITE);
  osk.on_panel_event(write_ev);

  assert(!osk.is_open());
  assert(lv_obj_is_hidden(osk.root_container()));
  assert(confirmed_text == "Canción");

  // 7. Test EXIT closes keeping text
  std::string canceled_text;
  osk.set_cancel_callback([&](const std::string& t) {
    canceled_text = t;
  });

  osk.open("Pending Song", KeyboardLayout::EN);
  assert(osk.is_open());

  PanelEvent exit_ev = PanelEvent::button_press(MCU_BTN_EXIT);
  osk.on_panel_event(exit_ev);

  assert(!osk.is_open());
  assert(lv_obj_is_hidden(osk.root_container()));
  assert(canceled_text == "Pending Song");
  assert(osk.text() == "Pending Song");

  // 8. Test Done key press
  confirmed_text.clear();
  osk.open("Final Live Set", KeyboardLayout::EN);
  osk.press_key("Done");
  assert(!osk.is_open());
  assert(confirmed_text == "Final Live Set");

  std::cout << "test_on_screen_keyboard passed" << std::endl;
}

int main() {
  test_on_screen_keyboard();
  return 0;
}

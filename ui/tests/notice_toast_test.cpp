#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/components/notice_toast.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_notice_toast_show_hide() {
  NoticeToast toast;
  bool ok = toast.create(lv_screen_active());
  assert(ok);
  assert(!toast.is_visible());

  toast.show("USB drive found", "Import?", 8000);
  assert(toast.is_visible());
  assert(toast.message() == "USB drive found");
  assert(toast.action_text() == "Import?");
  lv_obj_update_layout(toast.toast_pill());
  assert(lv_obj_get_height(toast.toast_pill()) >= 56);

  toast.hide();
  assert(!toast.is_visible());

  std::cout << "[PASS] test_notice_toast_show_hide\n";
}

void test_notice_toast_auto_dismiss_and_pause() {
  NoticeToast toast;
  toast.create(lv_screen_active());

  bool dismissed = false;
  toast.set_on_dismiss([&]() {
    dismissed = true;
  });

  toast.show("Test Message", "", 1000);
  assert(toast.is_visible());

  // Tick 500ms -> still visible
  toast.tick(500);
  assert(toast.is_visible());
  assert(!dismissed);

  // Tick another 500ms -> auto dismissed
  toast.tick(500);
  assert(!toast.is_visible());
  assert(dismissed);

  std::cout << "[PASS] test_notice_toast_auto_dismiss_and_pause\n";
}

void test_notice_toast_action_and_exit() {
  NoticeToast toast;
  toast.create(lv_screen_active());

  bool action_fired = false;
  toast.set_on_action([&]() {
    action_fired = true;
  });

  toast.show("USB drive found", "Import?", 8000);
  // EXIT button dismisses
  toast.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  assert(!toast.is_visible());

  // Re-show and trigger action
  toast.show("USB drive found", "Import?", 8000);
  // Send click event to action button
  lv_obj_send_event(toast.action_button(), LV_EVENT_CLICKED, nullptr);
  assert(action_fired);
  assert(!toast.is_visible());

  std::cout << "[PASS] test_notice_toast_action_and_exit\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_notice_toast_show_hide();
  test_notice_toast_auto_dismiss_and_pause();
  test_notice_toast_action_and_exit();

  std::cout << "All NoticeToast tests passed!\n";
  return 0;
}

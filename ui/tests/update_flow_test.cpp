#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

namespace {

struct FakeUpdateSysd : SysdConfigSender {
  std::deque<Event> events;
  std::vector<std::string> install_requests;

  void request_clock_source(ClockSourceMode) override {}
  void request_boot_into(uint8_t, const std::string&) override {}
  void request_start_pairing() override {}
  void request_cancel_pairing() override {}
  void request_pairing_status() override {}
  void request_list_paired_computers() override {}
  void request_forget_computer(const std::string&) override {}
  void request_install_update(const std::string& bundle_path) override {
    install_requests.push_back(bundle_path);
  }
  bool poll(Event& out) override {
    if (events.empty()) return false;
    out = std::move(events.front());
    events.pop_front();
    return true;
  }

  void push_available(const std::string& ver, const std::string& path) {
    Event e;
    e.kind = Event::Kind::UpdateAvailable;
    e.update_version = ver;
    e.update_bundle_path = path;
    events.push_back(std::move(e));
  }
  void push_progress(const std::string& phase, uint32_t pct, const std::string& target_ver, const std::string& fallback_ver) {
    Event e;
    e.kind = Event::Kind::UpdateProgress;
    e.update_phase = phase;
    e.update_percent = pct;
    e.update_version = target_ver;
    e.update_fallback_version = fallback_ver;
    events.push_back(std::move(e));
  }
  void push_finished() {
    Event e;
    e.kind = Event::Kind::UpdateFinished;
    events.push_back(std::move(e));
  }
  void push_failed(const std::string& why) {
    Event e;
    e.kind = Event::Kind::Failed;
    e.setting = Event::Setting::Update;
    e.error = why;
    events.push_back(std::move(e));
  }
};

// AC1 & AC2: System & Update section states, "Update n found · Open?" toast navigation, and Action Sheet confirmation.
void test_ac1_ac2_system_update_section_and_action_sheet() {
  NavigationController nav(nullptr);
  assert(nav.initialize(lv_screen_active()));
  FakeUpdateSysd sysd;
  nav.set_sysd_sender(&sysd);

  auto& sv = nav.settings_view();
  sv.set_firmware_info("1.2", "Development");

  // With no bundle: shows "No update found. Insert a USB drive or send one from Velora Studio." and hides Install row
  assert(sv.update_status_text() == "No update found. Insert a USB drive or send one from Velora Studio.");
  assert(!sv.is_install_row_visible());
  assert(!sv.action_sheet().is_open());

  // sysd announces a verified bundle "1.3": shows "Update 1.3 found · Open?" Notice Toast
  sysd.push_available("1.3", "/data/update/velora-1.3.raucb");
  nav.tick(10);
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "Update 1.3 found");
  assert(nav.notice_toast().action_text() == "Open?");
  assert(sv.update_status_text() == "1.3 ready to install");
  assert(sv.is_install_row_visible());
  // Action sheet never opens unprompted
  assert(!sv.action_sheet().is_open());

  // Triggering the Notice Toast action opens Settings › System & Update
  lv_obj_send_event(nav.notice_toast().action_button(), LV_EVENT_CLICKED, nullptr);
  assert(nav.current_surface() == SurfaceId::Settings);
  assert(sv.selected_section() == SettingsSection::SystemUpdate);

  // Choosing Install opens the Action Sheet with "Install update? Sound stops until the Velora restarts."
  sv.open_install_update_action_sheet();
  assert(sv.action_sheet().is_open());
  assert(sv.action_sheet().title() == "Install update? Sound stops until the Velora restarts.");
  assert(sv.action_sheet().action_count() == 2);

  // Pressing EXIT closes the Action Sheet and nothing changes
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(!sv.action_sheet().is_open());
  assert(!nav.is_installing_update());
  assert(sysd.install_requests.empty());

  std::cout << "[PASS] test_ac1_ac2_system_update_section_and_action_sheet\n";
}

// AC3 & AC4: Confirming with WRITE mutes sound, switches to Boot Screen layout with UPDATING TO 1.3 + progress + reassurance, ignores input, and shows "Updated to 1.3" after reboot.
void test_ac3_ac4_install_progress_and_post_update_toast() {
  NavigationController nav(nullptr);
  assert(nav.initialize(lv_screen_active()));
  FakeUpdateSysd sysd;
  nav.set_sysd_sender(&sysd);

  auto& sv = nav.settings_view();
  sv.set_firmware_info("1.2", "Development");
  nav.on_update_available("1.3", "/data/update/velora-1.3.raucb");
  lv_obj_send_event(nav.notice_toast().action_button(), LV_EVENT_CLICKED, nullptr);

  sv.open_install_update_action_sheet();
  assert(sv.action_sheet().is_open());

  // Confirm with WRITE (highlighted index 0 = WRITE · Install)
  sv.action_sheet().confirm_highlighted();
  assert(!sv.action_sheet().is_open());
  assert(nav.is_installing_update());
  assert(nav.is_audio_muted_for_update());
  assert(nav.current_surface() == SurfaceId::Boot);
  assert(nav.boot_view().status_text() == "UPDATING TO 1.3");
  assert(nav.boot_view().sub_status_text() == "If power is lost, the Velora starts on 1.2.");
  assert(sysd.install_requests.size() == 1);

  // Panel input is ignored during install (MENU, EXIT, LIVE SET next do nothing)
  const int prev_steps = nav.live_set_step_count();
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_LIVE_SET_NEXT));
  assert(nav.current_surface() == SurfaceId::Boot);
  assert(nav.live_set_step_count() == prev_steps);

  // Feed progress from sysd
  sysd.push_progress("writing_slot", 65, "1.3", "1.2");
  nav.tick(10);
  assert(std::abs(nav.boot_view().progress() - 0.65f) < 0.01f);

  // Finish install -> transitions to Performance and shows "Updated to 1.3" Notice Toast
  sysd.push_finished();
  nav.tick(10);
  assert(!nav.is_installing_update());
  assert(!nav.is_audio_muted_for_update());
  assert(nav.current_surface() == SurfaceId::Performance);
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "Updated to 1.3");
  assert(sv.firmware_version() == "1.3");

  std::cout << "[PASS] test_ac3_ac4_install_progress_and_post_update_toast\n";
}

// AC5 & AC6: Rejected bundle restores sound without restart; rollback state file at boot shows toast once and deletes file.
void test_ac5_ac6_rejected_bundle_and_rollback_toast() {
  NavigationController nav(nullptr);
  assert(nav.initialize(lv_screen_active()));
  FakeUpdateSysd sysd;
  nav.set_sysd_sender(&sysd);

  auto& sv = nav.settings_view();
  sv.set_firmware_info("1.2", "Development");
  nav.on_update_available("1.3", "/data/update/bad.raucb");
  lv_obj_send_event(nav.notice_toast().action_button(), LV_EVENT_CLICKED, nullptr);
  sv.open_install_update_action_sheet();
  sv.action_sheet().confirm_highlighted();
  assert(nav.is_installing_update());
  assert(nav.is_audio_muted_for_update());

  // sysd rejects the bundle before writing slot
  sysd.push_failed("ERROR_CODE_UPDATE_BAD_SIGNATURE: corrupt");
  nav.tick(10);
  assert(!nav.is_installing_update());
  assert(!nav.is_audio_muted_for_update());  // Sound restored without a restart!
  assert(nav.current_surface() == SurfaceId::Settings);
  assert(sv.selected_section() == SettingsSection::SystemUpdate);
  assert(sv.update_status_text() == "Update failed: the file is damaged or not signed. Still on 1.2.");

  // AC6: Rollback state file from Story 6.12 present at boot
  auto rb_path = std::filesystem::temp_directory_path() / "velora_test_rollback_8_3.json";
  {
    std::ofstream ofs(rb_path, std::ios::trunc);
    ofs << "{\n  \"schema\": 1,\n  \"from_version\": \"1.3\",\n  \"to_version\": \"1.2\",\n  \"reason\": \"boot-failed\"\n}\n";
  }
  assert(nav.check_and_consume_rollback_file(rb_path.string()));
  assert(nav.notice_toast().is_visible());
  assert(nav.notice_toast().message() == "Update did not finish. Still on 1.2 · Try again in Settings › System & Update.");
  assert(!std::filesystem::exists(rb_path));  // Cleared after shown once
  assert(!nav.check_and_consume_rollback_file(rb_path.string()));

  std::cout << "[PASS] test_ac5_ac6_rejected_bundle_and_rollback_toast\n";
}

// AC7: All strings exist in EN, PT-BR, ES and pass 1024x600 layout at +40% pseudo-localized length.
void test_ac7_locales_and_pseudo_layout() {
  static const char* kKeys[] = {
      "settings.update.version_variant",
      "settings.update.no_update_found",
      "settings.update.ready_to_install",
      "settings.update.install_action",
      "settings.update.failed",
      "action_sheet.eyebrow.update",
      "action_sheet.update.title",
      "action_sheet.update.install",
      "action_sheet.update.later",
      "boot.updating_to",
      "boot.power_loss_reassurance",
      "notice.update_found",
      "notice.action.open",
      "notice.updated_to",
      "notice.update_rollback",
  };

  for (const char* loc : {"en", "pt-BR", "es", "pseudo"}) {
    Locale::instance().set_locale(loc);
    assert(Locale::instance().current_locale() == loc);
    for (const char* k : kKeys) {
      std::string val = tr(k);
      assert(!val.empty() && val != k);
    }
  }

  // Render Settings › System & Update, Action Sheet, and Boot Screen under +40% pseudo-locale on 1024x600
  Locale::instance().set_locale("pseudo");
  NavigationController nav(nullptr);
  assert(nav.initialize(lv_screen_active()));
  nav.settings_view().set_firmware_info("1.2.0", "Development");
  nav.on_update_available("1.3.0", "/data/update/velora-1.3.0.raucb");
  nav.navigate_to(SurfaceId::Settings);
  nav.settings_view().select_section(SettingsSection::SystemUpdate);
  nav.settings_view().open_install_update_action_sheet();
  lv_refr_now(nullptr);

  assert(lv_obj_get_width(nav.settings_view().system_update_container()) <= 764);
  assert(lv_obj_get_width(nav.settings_view().action_sheet().card_container()) <= 1024);

  Locale::instance().set_locale("en");
  std::cout << "[PASS] test_ac7_locales_and_pseudo_layout\n";
}

}  // namespace

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to initialize headless adapter\n";
    return 1;
  }

  test_ac1_ac2_system_update_section_and_action_sheet();
  test_ac3_ac4_install_progress_and_post_update_toast();
  test_ac5_ac6_rejected_bundle_and_rollback_toast();
  test_ac7_locales_and_pseudo_layout();

  std::cout << "update_flow_test: ALL PASSED\n";
  return 0;
}

#pragma once

#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <optional>
#include <vector>

#include "velora/format/sysex_6op.hpp"
#include "velora/ui/panel_event.hpp"
#include "velora/ui/led_driver.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/screens/launcher_view.hpp"
#include "velora/ui/screens/effects_view.hpp"
#include "velora/ui/screens/setlist_view.hpp"
#include "velora/ui/screens/setlist_browser_view.hpp"
#include "velora/ui/screens/scenes_view.hpp"
#include "velora/ui/screens/part_view.hpp"
#include "velora/ui/screens/settings_view.hpp"
#include "velora/ui/screens/library_view.hpp"
#include "velora/ui/screens/import_view.hpp"
#include "velora/ui/screens/boot_view.hpp"
#include "velora/ui/components/led_view.hpp"
#include "velora/ui/components/notice_toast.hpp"
#include "velora/ui/components/write_dialog.hpp"

namespace velora::ui {

class WsClient;

enum class SurfaceId {
  Boot = 0,
  Performance,
  Launcher,
  Effects,
  Setlist,
  SetlistBrowser,
  Scenes,
  Part,
  Settings,
  Library,
  Import
};

class NavigationController {
 public:
  explicit NavigationController(WsClient* client = nullptr);
  ~NavigationController();

  bool initialize(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void navigate_to(SurfaceId surface);
  void navigate_back();

  SurfaceId current_surface() const noexcept { return current_surface_; }
  SurfaceId previous_surface() const noexcept { return previous_surface_; }

  PerformanceView& performance_view() noexcept { return performance_view_; }
  LauncherView& launcher_view() noexcept { return launcher_view_; }
  EffectsView& effects_view() noexcept { return effects_view_; }
  SetlistView& setlist_view() noexcept { return setlist_view_; }
  SetlistBrowserView& setlist_browser_view() noexcept { return setlist_browser_view_; }
  ScenesView& scenes_view() noexcept { return scenes_view_; }
  PartView& part_view() noexcept { return part_view_; }
  SettingsView& settings_view() noexcept { return settings_view_; }
  LibraryView& library_view() noexcept { return library_view_; }
  ImportView& import_view() noexcept { return import_view_; }
  BootView& boot_view() noexcept { return boot_view_; }
  NoticeToast& notice_toast() noexcept { return notice_toast_; }
  WriteDialog& write_dialog() noexcept { return write_dialog_; }

  void on_usb_drive_inserted(const std::string& drive_name, const std::vector<ImportItem>& items);
  void on_sysex_dump_received(const format::SysexParseResult& result);
  void on_sysex_damaged();
  void tick(uint32_t elapsed_ms);

  void on_midi_note(uint8_t note_number);

  void open_write_dialog();
  bool has_pending_edits() const;
  void show_nothing_to_write_hint();
  void handle_write_confirmed(ws::v1::WriteScope scope, const std::string& name);
  void set_pending_edits(bool pending, bool has_timbre = false, const std::string& summary = "");

  // Testing helpers
  int live_set_step_count() const noexcept { return live_set_step_count_; }
  int scene_step_count() const noexcept { return scene_step_count_; }

  void set_panel_guard(bool enabled) noexcept { panel_guard_ = enabled; }
  bool panel_guard() const noexcept { return panel_guard_; }

  void set_write_hold_ms(uint32_t ms) noexcept { write_hold_threshold_us_ = ms * 1000ULL; }
  uint32_t write_hold_ms() const noexcept { return static_cast<uint32_t>(write_hold_threshold_us_ / 1000ULL); }

  void handle_footswitch(uint64_t ts = 0);

  // Story 3.12: panel LEDs. The toggle state (PerformanceView::toggles()) is the single source; the LED driver turns it into ws.v1 SetLed commands, one lit LED
  // per toggle state, with the blink-twice / recall-dip patterns. A (re)connection to the engine, and a freshly started ui, send the full LED state.
  LedDriver& led_driver() noexcept { return led_driver_; }
  const LedDriver& led_driver() const noexcept { return led_driver_; }
  ToggleState& toggles() noexcept { return performance_view_.toggles(); }
  // Simulator only (the physical panel has real LEDs): an on-screen view of the LEDs on the top layer.
  bool enable_led_view();
  LedView& led_view() noexcept { return led_view_; }
  // Test seams (default: the WsClient): whether the engine connection is up, and where a built ws.v1 command goes.
  // Story 7.7: the sysd seam of the Settings Clock source control (not owned).
  void show_clock_source(ClockSourceMode m);
  void set_sysd_sender(SysdConfigSender* s) { settings_view_.set_sysd_sender(s); }
  // Story 8.9: the All Live Sets list the Boot into > Default Live Set picker offers (the Live Set that sounds is added from State).
  void set_live_set_choices(std::vector<LiveSetChoice> c) { settings_view_.set_live_set_choices(std::move(c)); }
  // Story 8.9: a SetlistsReply from the engine (GetSetlists{include_all_live_sets}) feeds the picker; request_live_set_choices() asks for it (when the picker opens and
  // when the setlist library revision changes).
  void on_setlists_reply(const ws::v1::SetlistsReply& r) { settings_view_.update_from_setlists_reply(r); }
  bool request_live_set_choices();
  void set_connected_probe(std::function<bool()> fn) { connected_fn_ = std::move(fn); }
  void set_command_sender(std::function<bool(const ws::v1::ClientMessage&)> fn) { command_fn_ = std::move(fn); }
  void resend_leds();  // the full LED state again (what the connection edge in tick() does)

  // Fired (from an LVGL event) when the UI language changed; the host should rebuild the UI
  // outside the event callback so every label is re-created with the new catalog.
  void set_on_language_changed(std::function<void()> cb) { on_language_changed_ = std::move(cb); }

 private:
  void on_panel_event_impl(const PanelEvent& ev);
  void sync_leds();
  void show_surface(SurfaceId surface);
  void hide_surface(SurfaceId surface);

  WsClient* client_{nullptr};

  PerformanceView performance_view_;
  LauncherView launcher_view_;
  EffectsView effects_view_;
  SetlistView setlist_view_;
  SetlistBrowserView setlist_browser_view_;
  ScenesView scenes_view_;
  PartView part_view_;
  SettingsView settings_view_;
  LibraryView library_view_;
  ImportView import_view_;
  BootView boot_view_;
  NoticeToast notice_toast_;
  WriteDialog write_dialog_;

  SurfaceId current_surface_{SurfaceId::Performance};
  SurfaceId previous_surface_{SurfaceId::Performance};
  SurfaceId surface_before_launcher_{SurfaceId::Performance};

  std::function<void()> on_language_changed_;
  std::string applied_language_;

  std::function<bool()> connected_fn_;
  std::function<bool(const ws::v1::ClientMessage&)> command_fn_;
  uint64_t live_set_choices_revision_{~0ull};  // library_revision the picker list was last requested for
  LedDriver led_driver_;
  LedView led_view_;
  bool led_view_enabled_{false};
  uint64_t now_ms_{0};
  bool was_connected_{false};
  bool prev_scene_dip_{false};
  uint32_t prev_drawbars_refused_{0};

  int live_set_step_count_{0};
  int scene_step_count_{0};

  uint64_t write_btn_down_time_us_{0};
  uint64_t write_hold_threshold_us_{500000};
  bool panel_guard_{false};
  bool exit_btn_down_{false};
  bool exit_turned_value_{false};
  uint64_t live_set_btn_down_time_us_{0};
  uint32_t live_set_btn_pending_id_{0};
  bool manual_pending_edits_{false};
  bool has_timbre_edit_{false};
  std::string change_summary_text_;
  std::optional<format::SysexParseResult> pending_sysex_;
};

}  // namespace velora::ui

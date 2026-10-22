#pragma once

#include <lvgl.h>
#include <memory>
#include <vector>

#include "velora/ui/panel_event.hpp"
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

 private:
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

  int live_set_step_count_{0};
  int scene_step_count_{0};

  uint64_t write_btn_down_time_us_{0};
  uint64_t write_hold_threshold_us_{500000};
  bool panel_guard_{false};
  uint64_t live_set_btn_down_time_us_{0};
  uint32_t live_set_btn_pending_id_{0};
  bool manual_pending_edits_{false};
  bool has_timbre_edit_{false};
  std::string change_summary_text_;
};

}  // namespace velora::ui

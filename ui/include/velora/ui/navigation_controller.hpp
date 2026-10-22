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

namespace velora::ui {

class WsClient;

enum class SurfaceId {
  Performance = 0,
  Launcher,
  Effects,
  Setlist,
  SetlistBrowser,
  Scenes,
  Settings
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

  // Testing helpers
  int live_set_step_count() const noexcept { return live_set_step_count_; }
  int scene_step_count() const noexcept { return scene_step_count_; }

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

  SurfaceId current_surface_{SurfaceId::Performance};
  SurfaceId previous_surface_{SurfaceId::Performance};
  SurfaceId surface_before_launcher_{SurfaceId::Performance};

  int live_set_step_count_{0};
  int scene_step_count_{0};
};

}  // namespace velora::ui

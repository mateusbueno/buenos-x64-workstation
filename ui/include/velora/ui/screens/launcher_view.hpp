#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

enum class LauncherTileId {
  SetlistBrowser = 0,
  Setlist,
  Library,
  Effects,
  Scenes,
  Import,
  Settings,
  Count
};

struct LauncherTileData {
  LauncherTileId id;
  std::string title_key;
  bool is_available{true};
};

class LauncherView {
 public:
  using TileSelectCallback = std::function<void(LauncherTileId id)>;
  using ExitCallback = std::function<void()>;

  LauncherView();
  ~LauncherView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);

  void set_highlight(int32_t index);
  int32_t highlight() const noexcept { return highlight_idx_; }

  void activate_highlighted();

  void set_tile_select_callback(TileSelectCallback cb) { on_tile_select_ = std::move(cb); }
  void set_exit_callback(ExitCallback cb) { on_exit_ = std::move(cb); }

  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }

  lv_obj_t* tile_obj(size_t index) const {
    return (index < tile_objs_.size()) ? tile_objs_[index] : nullptr;
  }

  bool is_tile_available(size_t index) const {
    return (index < tiles_.size()) ? tiles_[index].is_available : false;
  }

 private:
  static void tile_click_cb(lv_event_t* e);
  void update_tile_styles();

  SurfaceSkeleton skeleton_;
  std::vector<LauncherTileData> tiles_;
  std::vector<lv_obj_t*> tile_objs_;
  std::vector<lv_obj_t*> title_lbl_objs_;
  std::vector<lv_obj_t*> status_lbl_objs_;

  int32_t highlight_idx_{0};
  TileSelectCallback on_tile_select_;
  ExitCallback on_exit_;
};

}  // namespace velora::ui

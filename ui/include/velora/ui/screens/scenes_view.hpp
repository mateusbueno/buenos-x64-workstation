#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/screens/performance_view.hpp"
#include "velora/ui/components/on_screen_keyboard.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

class ScenesView {
 public:
  using CloseCallback = std::function<void()>;

  explicit ScenesView(WsClient* client = nullptr);
  ~ScenesView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }

  // Scene slots manipulation
  void set_scenes(const std::vector<SceneTileData>& scenes, int32_t current_idx = 0);
  void set_live_set_name(const std::string& name);

  // Selection & Recall
  void select_slot(size_t index);
  int32_t selected_slot() const noexcept { return selected_slot_idx_; }
  int32_t current_scene() const noexcept { return current_scene_idx_; }
  size_t used_slot_count() const;

  void recall_selected_scene();

  // Actions
  bool create_new_scene();
  bool rename_selected_scene();
  bool duplicate_selected_scene();
  bool move_scene_left();
  bool move_scene_right();
  bool clear_selected_scene();

  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }

  // Accessors
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }
  OnScreenKeyboard& keyboard() noexcept { return keyboard_; }

  const std::vector<SceneTileData>& scenes() const noexcept { return scenes_; }
  lv_obj_t* scene_card(size_t index) const {
    return (index < card_objs_.size()) ? card_objs_[index] : nullptr;
  }

 private:
  void rebuild_grid();
  void update_top_bar();
  void update_hints();
  void mark_edits();

  static void on_card_click(lv_event_t* e);
  static void on_new_scene_click(lv_event_t* e);
  static void on_rename_click(lv_event_t* e);
  static void on_duplicate_click(lv_event_t* e);
  static void on_move_click(lv_event_t* e);
  static void on_clear_click(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  OnScreenKeyboard keyboard_;
  CloseCallback on_close_;

  std::vector<SceneTileData> scenes_;
  int32_t current_scene_idx_{0};
  int32_t selected_slot_idx_{0};
  std::string live_set_name_{"Perfect"};
  bool has_unsaved_edits_{false};

  // UI objects
  lv_obj_t* grid_container_{nullptr};
  std::vector<lv_obj_t*> card_objs_;
  std::vector<lv_obj_t*> num_objs_;
  std::vector<lv_obj_t*> tag_objs_;
  std::vector<lv_obj_t*> name_objs_;
  std::vector<lv_obj_t*> sub_objs_;

  lv_obj_t* lbl_reassurance_{nullptr};
  lv_obj_t* action_strip_{nullptr};
  lv_obj_t* btn_new_{nullptr};
  lv_obj_t* btn_rename_{nullptr};
  lv_obj_t* btn_dup_{nullptr};
  lv_obj_t* btn_move_{nullptr};
  lv_obj_t* btn_clear_{nullptr};
};

}  // namespace velora::ui

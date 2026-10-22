#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ws_v1.pb.h"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

struct SceneTileData {
  std::string name;
  std::string sub_label;
  bool is_empty{false};
};

class PerformanceView {
 public:
  explicit PerformanceView(WsClient* client = nullptr);
  ~PerformanceView();

  bool create(lv_obj_t* parent = nullptr);

  void update_from_state(const ws::v1::State& state);
  void on_panel_event(const PanelEvent& ev);

  // Setters for direct UI control
  void set_tempo(float bpm);
  void set_setlist_context(const std::string& setlist_name, int32_t current_idx, int32_t total_count);
  void set_live_set(const std::string& name, int32_t index, bool dirty = false);
  void set_next_live_set(const std::string& title, const std::string& status_text, bool ready = true);
  void set_scenes(const std::vector<SceneTileData>& scenes, int32_t current_scene_idx);
  void set_modes(bool drawbars, bool scenes_mode, int32_t knob_mode, int32_t knob_part = 1);

  void recall_scene(int32_t scene_idx);

  // Accessors for testing
  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* top_bar() const noexcept { return top_bar_; }
  lv_obj_t* hero() const noexcept { return hero_; }
  lv_obj_t* scene_strip() const noexcept { return scene_strip_; }
  lv_obj_t* part_area() const noexcept { return part_area_; }
  lv_obj_t* status_bar() const noexcept { return status_bar_; }

  int32_t current_scene() const noexcept { return current_scene_idx_; }
  const std::string& live_set_name() const noexcept { return live_set_name_; }
  float tempo() const noexcept { return current_bpm_; }
  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }

 private:
  static void scene_click_cb(lv_event_t* e);
  void update_scene_tiles();
  void update_top_bar_modes();

  WsClient* client_{nullptr};

  lv_obj_t* root_{nullptr};
  lv_obj_t* top_bar_{nullptr};
  lv_obj_t* hero_{nullptr};
  lv_obj_t* scene_strip_{nullptr};
  lv_obj_t* part_area_{nullptr};
  lv_obj_t* status_bar_{nullptr};

  // Top Bar elements
  lv_obj_t* lbl_context_{nullptr};
  lv_obj_t* lbl_mode_mixer_{nullptr};
  lv_obj_t* lbl_mode_parts_{nullptr};
  lv_obj_t* lbl_mode_knobs_{nullptr};
  lv_obj_t* lbl_tempo_{nullptr};

  // Hero elements
  lv_obj_t* lbl_hero_meta_{nullptr};
  lv_obj_t* lbl_hero_name_{nullptr};
  lv_obj_t* lbl_hero_unsaved_{nullptr};
  lv_obj_t* lbl_next_header_{nullptr};
  lv_obj_t* lbl_next_title_{nullptr};
  lv_obj_t* lbl_next_status_{nullptr};

  // Scene Strip elements
  std::vector<SceneTileData> scenes_;
  std::vector<lv_obj_t*> scene_tile_objs_;
  std::vector<lv_obj_t*> scene_title_objs_;
  std::vector<lv_obj_t*> scene_sub_objs_;

  int32_t current_scene_idx_{-1};
  std::string live_set_name_;
  float current_bpm_{120.0f};
  bool has_unsaved_edits_{false};

  // Modes
  bool mode_drawbars_{false};
  bool mode_scenes_{false};
  int32_t mode_knobs_{0}; // 0 = PART, 1 = ALL, 2 = ASSIGN
  int32_t knob_part_{1};
};

}  // namespace velora::ui

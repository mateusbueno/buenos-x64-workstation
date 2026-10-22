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

enum class LedLevel {
  Off = 0,
  Weak = 1,
  Strong = 2
};

struct SceneTileData {
  std::string name;
  std::string sub_label;
  bool is_empty{false};
};

struct PartStripData {
  std::string timbre_name;
  std::string category;
  float volume{0.75f};
  bool enabled{true};
  bool is_empty{false};
  bool missing_library{false};
  bool pickup_pending{false};
  float physical_fader_pos{0.0f};
};

class PerformanceView {
 public:
  using PartToggleCallback = std::function<void(int32_t part_idx, bool enabled)>;
  using PartSelectCallback = std::function<void(int32_t part_idx)>;
  using RevClickCallback = std::function<void()>;

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

  // Story 4.21 Setters
  void set_parts(const std::vector<PartStripData>& parts);
  void set_part_enabled(int32_t part_idx, bool enabled);
  void set_part_volume(int32_t part_idx, float volume);
  void set_status(uint32_t voices_active, uint32_t voices_max, bool streaming_ok, int32_t rotary_state, float rev_send);
  void set_warning_takeover(const std::string& warning_text);
  void clear_warning_takeover();

  // Story 4.23 Features
  void toggle_parts_scenes_mode();
  void select_knob_part(int32_t part_idx);
  int32_t selected_knob_part() const noexcept { return selected_knob_part_idx_; }
  bool is_scenes_mode() const noexcept { return mode_scenes_; }
  LedLevel button_led_level(size_t index) const;
  bool is_scene_led_dipping() const noexcept { return scene_led_dipping_; }
  void handle_part_scene_press(int32_t btn_idx, bool pressed, uint64_t timestamp_us = 0);

  // Story 4.24 Features: Pickup Ghost & Status Takeover
  void set_pickup_pending(int32_t part_idx, float physical_pos);
  void handle_fader_move(int32_t part_idx, float physical_pos);
  bool is_part_pickup_pending(size_t idx) const {
    return (idx < parts_.size()) ? parts_[idx].pickup_pending : false;
  }
  int32_t active_pickup_fader() const noexcept { return active_pickup_fader_; }
  lv_obj_t* part_ghost(size_t idx) const { return (idx < part_ghost_objs_.size()) ? part_ghost_objs_[idx] : nullptr; }
  lv_obj_t* part_chevron(size_t idx) const { return (idx < part_chevron_objs_.size()) ? part_chevron_objs_[idx] : nullptr; }

  // Story 4.25 Features: Drawbars & Organ Card
  bool has_organ_part() const;
  void toggle_drawbars_mixer_mode();
  bool is_drawbars_mode() const noexcept { return mode_drawbars_; }
  void set_drawbar(size_t index, uint8_t value);
  uint8_t drawbar(size_t index) const { return (index < 9) ? drawbars_[index] : 0; }
  std::string registration_string() const;
  void set_rotary_speed(int32_t speed);
  int32_t rotary_speed() const noexcept { return organ_rotary_speed_; }
  bool drawbars_led_blinking() const noexcept { return drawbars_led_blinking_; }
  lv_obj_t* organ_card() const noexcept { return organ_card_obj_; }
  lv_obj_t* drawbar_col(size_t index) const { return (index < drawbar_col_objs_.size()) ? drawbar_col_objs_[index] : nullptr; }

  void set_part_toggle_callback(PartToggleCallback cb) { on_part_toggle_ = std::move(cb); }
  void set_part_select_callback(PartSelectCallback cb) { on_part_select_ = std::move(cb); }
  void set_rev_click_callback(RevClickCallback cb) { on_rev_click_ = std::move(cb); }

  void recall_scene(int32_t scene_idx);

  // Accessors for testing
  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* top_bar() const noexcept { return top_bar_; }
  lv_obj_t* hero() const noexcept { return hero_; }
  lv_obj_t* scene_strip() const noexcept { return scene_strip_; }
  lv_obj_t* part_area() const noexcept { return part_area_; }
  lv_obj_t* mixer_container() const noexcept { return mixer_container_; }
  lv_obj_t* drawbars_container() const noexcept { return drawbars_container_; }
  lv_obj_t* status_bar() const noexcept { return status_bar_; }

  int32_t current_scene() const noexcept { return current_scene_idx_; }
  const std::string& live_set_name() const noexcept { return live_set_name_; }
  float tempo() const noexcept { return current_bpm_; }
  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }

  size_t part_count() const noexcept { return parts_.size(); }
  const PartStripData& part_data(size_t idx) const { return parts_[idx]; }
  bool is_warning_active() const noexcept { return warning_active_; }
  const std::string& warning_text() const noexcept { return warning_text_; }

  lv_obj_t* part_card(size_t idx) const { return part_card_objs_[idx]; }
  lv_obj_t* part_power_btn(size_t idx) const { return part_power_objs_[idx]; }
  lv_obj_t* rev_button() const noexcept { return btn_status_rev_; }

 private:
  static void scene_click_cb(lv_event_t* e);
  static void part_card_click_cb(lv_event_t* e);
  static void part_power_click_cb(lv_event_t* e);
  static void rev_click_cb(lv_event_t* e);

  void update_scene_tiles();
  void update_top_bar_modes();
  void update_part_strips();
  void update_status_bar();
  void update_pickup_takeover();
  void update_drawbars_ui();

  WsClient* client_{nullptr};

  PartToggleCallback on_part_toggle_;
  PartSelectCallback on_part_select_;
  RevClickCallback on_rev_click_;

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

  // Mixer Container (8 Part Strips)
  lv_obj_t* mixer_container_{nullptr};
  std::vector<PartStripData> parts_;
  std::vector<lv_obj_t*> part_card_objs_;
  std::vector<lv_obj_t*> part_num_objs_;
  std::vector<lv_obj_t*> part_led_objs_;
  std::vector<lv_obj_t*> part_power_objs_;
  std::vector<lv_obj_t*> part_power_lbl_objs_;
  std::vector<lv_obj_t*> part_name_objs_;
  std::vector<lv_obj_t*> part_cat_objs_;
  std::vector<lv_obj_t*> part_vol_track_objs_;
  std::vector<lv_obj_t*> part_vol_fill_objs_;
  std::vector<lv_obj_t*> part_ghost_objs_;
  std::vector<lv_obj_t*> part_chevron_objs_;

  // Drawbars Container (Organ Card + 9 Drawbar Columns)
  lv_obj_t* drawbars_container_{nullptr};
  lv_obj_t* organ_card_obj_{nullptr};
  lv_obj_t* lbl_organ_meta_{nullptr};
  lv_obj_t* lbl_organ_title_{nullptr};
  lv_obj_t* lbl_organ_cat_{nullptr};
  lv_obj_t* lbl_organ_reg_{nullptr};
  lv_obj_t* lbl_organ_details_{nullptr};

  std::vector<lv_obj_t*> drawbar_col_objs_;
  std::vector<lv_obj_t*> drawbar_track_objs_;
  std::vector<lv_obj_t*> drawbar_head_objs_;
  std::vector<lv_obj_t*> drawbar_val_objs_;

  uint8_t drawbars_[9] = {8, 8, 8, 6, 0, 0, 0, 0, 0};
  int32_t organ_part_idx_{3};
  int32_t organ_rotary_speed_{1}; // 0 = STOP, 1 = SLOW, 2 = FAST
  bool drawbars_led_blinking_{false};

  // Status Bar elements
  lv_obj_t* status_normal_container_{nullptr};
  lv_obj_t* lbl_status_voices_{nullptr};
  lv_obj_t* lbl_status_streaming_{nullptr};
  lv_obj_t* lbl_status_rotary_{nullptr};
  lv_obj_t* btn_status_rev_{nullptr};
  lv_obj_t* lbl_status_rev_{nullptr};

  // Status Bar warning takeover
  lv_obj_t* lbl_status_takeover_{nullptr};
  bool warning_active_{false};
  std::string warning_text_;

  uint32_t voices_active_{0};
  uint32_t voices_max_{128};
  bool streaming_ok_{true};
  int32_t rotary_state_{-1}; // -1 = hide, 0 = STOP, 1 = SLOW, 2 = FAST
  float rev_send_{0.25f};

  int32_t current_scene_idx_{-1};
  std::string live_set_name_;
  float current_bpm_{120.0f};
  bool has_unsaved_edits_{false};

  // Modes
  bool mode_drawbars_{false};
  bool mode_scenes_{false};
  int32_t mode_knobs_{0}; // 0 = PART, 1 = ALL, 2 = ASSIGN
  int32_t knob_part_{1};
  int32_t selected_knob_part_idx_{0};

  // Pickup & Fader tracking
  int32_t active_pickup_fader_{-1};

  // Button tracking
  uint64_t part_btn_press_time_[8]{0};
  bool scene_led_dipping_{false};
  uint64_t scene_dip_start_us_{0};
};

}  // namespace velora::ui

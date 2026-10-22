#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class PartSection {
  Timbre = 0,
  ZoneKeyboard,
  LevelOutput,
  Count
};

struct TimbreItem {
  std::string id;
  std::string name;
  std::string category;
};

class PartView {
 public:
  using CloseCallback = std::function<void()>;
  using PartEditCallback = std::function<void(int32_t part_idx)>;

  explicit PartView(WsClient* client = nullptr);
  ~PartView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_edit(PartEditCallback cb) { on_edit_ = std::move(cb); }

  // Part data accessors & mutators
  void set_part_index(int32_t idx);
  int32_t part_index() const noexcept { return part_idx_; }

  void set_timbre(const std::string& name, const std::string& category);
  const std::string& timbre_name() const noexcept { return timbre_name_; }
  const std::string& category_name() const noexcept { return category_name_; }

  void set_zone(int32_t low_key, int32_t high_key, int32_t octave, int32_t transpose, bool kb_on);
  int32_t low_key() const noexcept { return low_key_; }
  int32_t high_key() const noexcept { return high_key_; }
  int32_t octave() const noexcept { return octave_; }
  int32_t transpose() const noexcept { return transpose_; }
  bool keyboard_on() const noexcept { return keyboard_on_; }

  void set_level_output(float volume, bool aux_output);
  float volume() const noexcept { return volume_; }
  bool is_aux_output() const noexcept { return aux_output_; }

  void set_power(bool on);
  bool is_powered_on() const noexcept { return power_on_; }

  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }

  void set_section(PartSection sec);
  PartSection current_section() const noexcept { return current_section_; }

  // Timbre picker modal
  void open_timbre_picker();
  void close_timbre_picker();
  bool is_timbre_picker_open() const noexcept { return timbre_picker_open_; }
  void select_timbre(size_t index);

  // Note event from keybed for setting low/high key
  void on_midi_note(uint8_t note_number);

  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }

  // Testing helpers
  int32_t highlighted_field() const noexcept { return field_focus_idx_; }
  lv_obj_t* change_timbre_button() const noexcept { return btn_change_timbre_; }
  lv_obj_t* timbre_picker_modal() const noexcept { return picker_modal_; }

 private:
  void rebuild_top_bar();
  void rebuild_panel();
  void update_nav_ui();
  void update_field_focus_ui();
  void mark_edits();

  static void on_nav_item_click(lv_event_t* e);
  static void on_field_click(lv_event_t* e);
  static void on_change_timbre_click(lv_event_t* e);
  static void on_picker_item_click(lv_event_t* e);
  static void on_power_click(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  CloseCallback on_close_;
  PartEditCallback on_edit_;

  int32_t part_idx_{0};
  std::string timbre_name_{"German Grand D-274"};
  std::string category_name_{"ACOUSTIC PIANO"};
  int32_t low_key_{21};   // A0
  int32_t high_key_{108}; // C8
  int32_t octave_{0};
  int32_t transpose_{0};
  bool keyboard_on_{true};
  float volume_{0.85f};
  bool aux_output_{false};
  bool power_on_{true};
  bool has_unsaved_edits_{false};

  PartSection current_section_{PartSection::Timbre};
  int32_t field_focus_idx_{0}; // Focused field inside current section

  // UI objects
  lv_obj_t* btn_power_{nullptr};
  lv_obj_t* lbl_power_{nullptr};
  lv_obj_t* nav_col_{nullptr};
  std::vector<lv_obj_t*> nav_item_objs_;

  lv_obj_t* panel_col_{nullptr};
  lv_obj_t* btn_change_timbre_{nullptr};
  std::vector<lv_obj_t*> field_objs_;
  std::vector<lv_obj_t*> field_val_objs_;

  // Timbre picker modal
  lv_obj_t* picker_modal_{nullptr};
  lv_obj_t* picker_card_{nullptr};
  lv_obj_t* picker_list_{nullptr};
  std::vector<TimbreItem> timbres_catalog_;
  int32_t picker_focus_idx_{0};
  bool timbre_picker_open_{false};
};

}  // namespace velora::ui

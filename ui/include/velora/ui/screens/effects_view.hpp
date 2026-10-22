#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/widgets/segmented_control.hpp"
#include "velora/ui/widgets/list_row.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class EffectsSection {
  Inserts,
  Sends,
  Master,
  Outputs
};

enum class EffectsViewMode {
  ChainList,
  AddPicker,
  ParamEditor
};

class EffectsView {
 public:
  using CloseCallback = std::function<void()>;

  explicit EffectsView(WsClient* client = nullptr);
  ~EffectsView();

  bool create(lv_obj_t* parent = nullptr);

  // Updates screen when engine State is received
  void update_from_state(const ws::v1::State& state);
  // Handles server errors (refused commands e.g. FX budget exceeded)
  void handle_error(const ws::v1::Error& error);

  // Panel event processing (from VirtualPanel / physical panel)
  void on_panel_event(const PanelEvent& ev);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }

  // Accessors for testing & verification
  EffectsSection current_section() const noexcept { return section_; }
  EffectsViewMode current_mode() const noexcept { return mode_; }
  uint32_t selected_part() const noexcept { return selected_part_; }
  int32_t selected_slot() const noexcept { return selected_slot_; }
  size_t slot_count() const;
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }

  void select_section(EffectsSection sec);
  void select_part(uint32_t part);
  void add_insert(ws::v1::InsertType type);
  void toggle_bypass(int32_t slot_idx);
  void remove_insert(int32_t slot_idx);
  void move_insert(int32_t slot_idx, int32_t new_idx);
  void open_param_editor(int32_t slot_idx);
  void close_subview();

  // Sends, Master, Outputs actions
  void set_part_send_levels(uint32_t part, float reverb, float delay);
  void set_part_output(uint32_t part, ws::v1::PartOutput out);
  void set_master_eq(float low_db, float mid_gain_db, float mid_freq_hz, float high_db, bool bypass = false);
  void set_master_comp(float thresh_db, float ratio, float attack_ms, float release_ms, float makeup_db, bool bypass = false);
  void set_line_in_level(float level);

 private:
  void render_current_mode();
  void render_inserts_section();
  void render_chain_list();
  void render_add_picker();
  void render_param_editor();

  void render_sends_section();
  void render_master_section();
  void render_outputs_section();

  void send_current_chain();
  const ws::v1::PartState* get_part_state(uint32_t part) const;

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;

  EffectsSection section_{EffectsSection::Inserts};
  EffectsViewMode mode_{EffectsViewMode::ChainList};
  uint32_t selected_part_{0}; // 0..7
  int32_t selected_slot_{-1};
  int32_t selected_param_idx_{0};

  ws::v1::State current_state_;
  CloseCallback on_close_;

  SegmentedControl section_selector_;
  lv_obj_t* view_container_{nullptr};
  SegmentedControl part_selector_;
  std::vector<std::unique_ptr<ListRow>> row_items_;
  std::vector<std::unique_ptr<SegmentedControl>> bypass_controls_;
  std::vector<std::unique_ptr<SegmentedControl>> output_controls_;
};

}  // namespace velora::ui

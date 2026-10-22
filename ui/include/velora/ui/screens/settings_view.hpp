#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class SettingsSection : uint8_t {
  DisplayLanguage = 0,
  KeyboardPedals = 1,
  MidiClock = 2,
  KnobsGlobalAssign = 3,
  Computer = 4,
  StorageLibrary = 5,
  SystemUpdate = 6,
};

enum class AssignScope : uint8_t {
  GlobalDefault = 0,
  ThisLiveSet = 1,
};

struct InstalledLibraryInfo {
  std::string id;
  std::string name;
  uint32_t timbre_count{0};
  uint64_t size_bytes{0};
  bool in_use{false};
};

struct AssignableParamItem {
  uint32_t param_id{0};
  std::string name;
  std::string category;
};

class SettingsView {
 public:
  using CloseCallback = std::function<void()>;
  using AssignSlotCallback = std::function<void(ws::v1::AssignTarget target, uint32_t slot_1_based, uint32_t param_id)>;
  using EditCallback = std::function<void()>;

  explicit SettingsView(WsClient* client = nullptr);
  ~SettingsView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_assign_changed(AssignSlotCallback cb) { on_assign_changed_ = std::move(cb); }
  void set_on_edit(EditCallback cb) { on_edit_ = std::move(cb); }

  // Navigation / Selection
  void select_section(SettingsSection section);
  SettingsSection selected_section() const noexcept { return current_section_; }

  void set_storage_info(uint64_t free_bytes, uint64_t total_bytes);
  void set_installed_libraries(std::vector<InstalledLibraryInfo> libs);
  const std::vector<InstalledLibraryInfo>& installed_libraries() const noexcept { return libraries_; }
  bool remove_library(const std::string& lib_id);
  const std::string& storage_error() const noexcept { return storage_error_; }

  void set_scope(AssignScope scope);
  AssignScope scope() const noexcept { return current_scope_; }

  void select_knob_slot(size_t index);
  int32_t selected_knob_slot() const noexcept { return selected_slot_idx_; }

  // Assign map data setters/getters
  void set_global_default_slots(const std::vector<uint32_t>& param_ids);
  void set_live_set_slots(const std::vector<uint32_t>& param_ids);

  const std::vector<uint32_t>& global_default_slots() const noexcept { return global_default_slots_; }
  const std::vector<uint32_t>& live_set_slots() const noexcept { return live_set_slots_; }

  uint32_t get_slot_param(AssignScope scope, size_t slot_idx) const;

  // Picker modal
  void open_param_picker(size_t slot_idx);
  void close_param_picker();
  bool is_picker_open() const noexcept { return picker_open_; }
  void select_param_in_picker(uint32_t param_id);
  int32_t picker_selected_row() const noexcept { return picker_selected_row_; }

  bool has_unsaved_edits() const noexcept { return has_unsaved_edits_; }
  void clear_unsaved_edits() noexcept { has_unsaved_edits_ = false; }

  // Accessors
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }

  static std::string param_name_for_id(uint32_t param_id);
  static const std::vector<AssignableParamItem>& get_assignable_parameters();

 private:
  void build_sidebar();
  void build_detail();
  void build_storage_detail();
  void rebuild_storage_list();
  void update_sidebar_ui();
  void update_knob_cards_ui();
  void update_reassurance_ui();

  void create_picker_modal();
  void update_picker_ui();

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;

  CloseCallback on_close_;
  AssignSlotCallback on_assign_changed_;
  EditCallback on_edit_;

  SettingsSection current_section_{SettingsSection::KnobsGlobalAssign};
  AssignScope current_scope_{AssignScope::GlobalDefault};
  int32_t selected_slot_idx_{0};

  // 8 slots for each scope (0 = unassigned)
  std::vector<uint32_t> global_default_slots_;
  std::vector<uint32_t> live_set_slots_;
  bool has_unsaved_edits_{false};

  // UI objects
  lv_obj_t* sidebar_container_{nullptr};
  std::vector<lv_obj_t*> section_row_objs_;
  std::vector<lv_obj_t*> section_label_objs_;
  std::vector<lv_obj_t*> section_sub_objs_;

  lv_obj_t* detail_container_{nullptr};
  lv_obj_t* scope_segmented_btn_{nullptr};
  lv_obj_t* btn_scope_global_{nullptr};
  lv_obj_t* lbl_scope_global_{nullptr};
  lv_obj_t* btn_scope_live_set_{nullptr};
  lv_obj_t* lbl_scope_live_set_{nullptr};
  lv_obj_t* lbl_reassurance_{nullptr};

  lv_obj_t* grid_container_{nullptr};
  std::vector<lv_obj_t*> slot_card_objs_;
  std::vector<lv_obj_t*> slot_k_badge_objs_;
  std::vector<lv_obj_t*> slot_name_objs_;
  std::vector<lv_obj_t*> slot_sub_objs_;

  // Picker modal objects
  bool picker_open_{false};
  int32_t picker_slot_idx_{0};
  int32_t picker_selected_row_{0};
  lv_obj_t* picker_backdrop_{nullptr};
  lv_obj_t* picker_modal_{nullptr};
  lv_obj_t* picker_title_{nullptr};
  lv_obj_t* picker_list_{nullptr};
  std::vector<lv_obj_t*> picker_row_objs_;

  // Storage & Library UI
  lv_obj_t* storage_container_{nullptr};
  lv_obj_t* lbl_storage_ssd_{nullptr};
  lv_obj_t* lbl_storage_error_{nullptr};
  lv_obj_t* storage_list_obj_{nullptr};
  std::vector<InstalledLibraryInfo> libraries_;
  uint64_t storage_free_bytes_{240ULL * 1024 * 1024 * 1024};
  uint64_t storage_total_bytes_{480ULL * 1024 * 1024 * 1024};
  std::string storage_error_;
  std::vector<uint32_t> picker_param_ids_;
};

}  // namespace velora::ui

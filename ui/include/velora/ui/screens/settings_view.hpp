#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/panel_event.hpp"
#include "velora/ui/widgets/segmented_control.hpp"
#include "velora/ui/system_config.hpp"
#include "velora/ui/sysd_config_sender.hpp"

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
  using ConfigChangeCallback = std::function<void(const SystemConfig& cfg)>;

  explicit SettingsView(WsClient* client = nullptr);
  ~SettingsView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }
  void set_on_assign_changed(AssignSlotCallback cb) { on_assign_changed_ = std::move(cb); }
  void set_on_edit(EditCallback cb) { on_edit_ = std::move(cb); }
  void set_on_config_changed(ConfigChangeCallback cb) { on_config_changed_ = std::move(cb); }
  // Story 7.7: the sysd seam (not owned; null = the clock source stays a local setting, as before sysd). With a sender the Clock source control asks sysd and
  // shows what sysd reports: SystemConfig::clock_source is only a cache of that value. poll_sysd() (UI thread, every tick) applies the reports.
  void set_sysd_sender(SysdConfigSender* sender) { sysd_ = sender; }
  void poll_sysd();
  using NoticeCallback = std::function<void(const std::string& message)>;
  void set_on_notice(NoticeCallback cb) { on_notice_ = std::move(cb); }

  SegmentedControl& seg_theme() noexcept { return seg_theme_; }
  SegmentedControl& seg_language() noexcept { return seg_language_; }
  SegmentedControl& seg_screen_brightness() noexcept { return seg_screen_brightness_; }
  SegmentedControl& seg_led_brightness() noexcept { return seg_led_brightness_; }
  SegmentedControl& seg_large_text() noexcept { return seg_large_text_; }
  SegmentedControl& seg_touch() noexcept { return seg_touch_; }
  SegmentedControl& seg_write_hold() noexcept { return seg_write_hold_; }
  SegmentedControl& seg_long_press() noexcept { return seg_long_press_; }
  SegmentedControl& seg_panel_guard() noexcept { return seg_panel_guard_; }
  SegmentedControl& seg_reduced_motion() noexcept { return seg_reduced_motion_; }
  SegmentedControl& seg_boot_into() noexcept { return seg_boot_into_; }
  lv_obj_t* display_container() const noexcept { return display_container_; }

  SegmentedControl& seg_velocity_curve() noexcept { return seg_velocity_curve_; }
  SegmentedControl& seg_aftertouch() noexcept { return seg_aftertouch_; }
  lv_obj_t* card_aftertouch() const noexcept { return card_aftertouch_; }
  SegmentedControl& seg_footswitch() noexcept { return seg_footswitch_; }
  lv_obj_t* btn_sustain_calib() const noexcept { return btn_sustain_calib_; }
  lv_obj_t* lbl_sustain_status() const noexcept { return lbl_sustain_status_; }
  lv_obj_t* btn_expression_calib() const noexcept { return btn_expression_calib_; }
  lv_obj_t* lbl_expression_status() const noexcept { return lbl_expression_status_; }
  lv_obj_t* keyboard_pedals_container() const noexcept { return keyboard_pedals_container_; }

  SegmentedControl& seg_clock_source() noexcept { return seg_clock_source_; }
  SegmentedControl& seg_midi_out_ch() noexcept { return seg_midi_out_ch_; }
  SegmentedControl& seg_keybed_out() noexcept { return seg_keybed_out_; }
  SegmentedControl& seg_midi_thru() noexcept { return seg_midi_thru_; }
  lv_obj_t* midi_clock_container() const noexcept { return midi_clock_container_; }

  lv_obj_t* detail_container() const noexcept { return detail_container_; }
  lv_obj_t* storage_container() const noexcept { return storage_container_; }
  lv_obj_t* computer_container() const noexcept { return computer_container_; }
  lv_obj_t* system_update_container() const noexcept { return system_update_container_; }

  void calibrate_sustain();
  void calibrate_expression();
  void update_conditional_rows();

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
  void build_display_detail();
  void build_keyboard_pedals_detail();
  void build_midi_clock_detail();
  void build_computer_detail();
  void build_system_update_detail();
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

  // Display & Language UI
  lv_obj_t* display_container_{nullptr};
  SegmentedControl seg_theme_;
  SegmentedControl seg_language_;
  SegmentedControl seg_screen_brightness_;
  SegmentedControl seg_led_brightness_;
  SegmentedControl seg_large_text_;
  SegmentedControl seg_touch_;
  SegmentedControl seg_write_hold_;
  SegmentedControl seg_long_press_;
  SegmentedControl seg_panel_guard_;
  SegmentedControl seg_reduced_motion_;
  SegmentedControl seg_boot_into_;

  // Keyboard & Pedals UI
  lv_obj_t* keyboard_pedals_container_{nullptr};
  SegmentedControl seg_velocity_curve_;
  lv_obj_t* card_aftertouch_{nullptr};
  SegmentedControl seg_aftertouch_;
  SegmentedControl seg_footswitch_;
  lv_obj_t* btn_sustain_calib_{nullptr};
  lv_obj_t* lbl_sustain_status_{nullptr};
  lv_obj_t* btn_expression_calib_{nullptr};
  lv_obj_t* lbl_expression_status_{nullptr};

  // MIDI & Clock UI
  lv_obj_t* midi_clock_container_{nullptr};
  SegmentedControl seg_clock_source_;
  SysdConfigSender* sysd_{nullptr};
  NoticeCallback on_notice_;
  bool sysd_reported_{false};  // sysd has reported at least once
  bool applying_sysd_{false};  // set while the control is moved by a sysd report / revert, so that is not taken for a user change
  SegmentedControl seg_midi_out_ch_;
  SegmentedControl seg_keybed_out_;
  SegmentedControl seg_midi_thru_;

  // Computer (Velora Studio) UI
  lv_obj_t* computer_container_{nullptr};
  lv_obj_t* lbl_computer_status_{nullptr};

  // System & Update UI
  lv_obj_t* system_update_container_{nullptr};
  lv_obj_t* lbl_update_version_{nullptr};
  lv_obj_t* lbl_update_status_{nullptr};

  ConfigChangeCallback on_config_changed_;
};

}  // namespace velora::ui

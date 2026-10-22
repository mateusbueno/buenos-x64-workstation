#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "ws_v1.pb.h"
#include "velora/ui/panel_event.hpp"
#include "velora/ui/components/on_screen_keyboard.hpp"

namespace velora::ui {

class WriteDialog {
 public:
  using WriteCallback = std::function<void(ws::v1::WriteScope scope, const std::string& name)>;
  using CancelCallback = std::function<void()>;

  WriteDialog();
  ~WriteDialog();

  bool create(lv_obj_t* parent = nullptr);

  void open(const std::string& current_name,
            bool has_timbre_edit = false,
            const std::string& change_summary = "",
            const std::string& suggested_free_name = "");
  void close();
  bool is_open() const noexcept { return is_open_; }

  void on_panel_event(const PanelEvent& ev);

  // Scope selection
  void set_scope(ws::v1::WriteScope scope);
  ws::v1::WriteScope current_scope() const noexcept { return current_scope_; }
  bool is_scene_scope_available() const noexcept { return scene_scope_available_; }

  // Errors / Banners
  void show_name_in_use_error(const std::string& name, const std::string& suggested_free_name);
  void show_storage_full_error();
  void clear_error();

  void confirm_write();
  void cancel();

  void set_on_write(WriteCallback cb) { on_write_ = std::move(cb); }
  void set_on_cancel(CancelCallback cb) { on_cancel_ = std::move(cb); }

  OnScreenKeyboard& keyboard() noexcept { return keyboard_; }

  // UI inspection helpers for tests
  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* card_container() const noexcept { return card_; }
  lv_obj_t* scene_option_obj() const noexcept { return opt_scene_; }
  lv_obj_t* whole_option_obj() const noexcept { return opt_whole_; }
  lv_obj_t* new_option_obj() const noexcept { return opt_new_; }
  lv_obj_t* name_field_obj() const noexcept { return name_field_; }
  lv_obj_t* error_label_obj() const noexcept { return lbl_error_; }
  const std::string& current_name_input() const noexcept { return new_name_input_; }
  int32_t focused_index() const noexcept { return focus_idx_; }

 private:
  void update_scope_ui();
  void update_name_field_ui();
  void update_focus_ui();

  static void on_scene_click(lv_event_t* e);
  static void on_whole_click(lv_event_t* e);
  static void on_new_click(lv_event_t* e);
  static void on_name_click(lv_event_t* e);
  static void on_btn_yes_click(lv_event_t* e);
  static void on_btn_cancel_click(lv_event_t* e);

  lv_obj_t* root_{nullptr};
  lv_obj_t* card_{nullptr};
  lv_obj_t* lbl_title_{nullptr};

  // Scope options
  lv_obj_t* opt_scene_{nullptr};
  lv_obj_t* opt_scene_radio_{nullptr};
  lv_obj_t* opt_scene_label_{nullptr};
  lv_obj_t* opt_scene_reason_{nullptr};

  lv_obj_t* opt_whole_{nullptr};
  lv_obj_t* opt_whole_radio_{nullptr};
  lv_obj_t* opt_whole_label_{nullptr};

  lv_obj_t* opt_new_{nullptr};
  lv_obj_t* opt_new_radio_{nullptr};
  lv_obj_t* opt_new_label_{nullptr};

  // Name field (for Save as new)
  lv_obj_t* name_field_{nullptr};
  lv_obj_t* lbl_name_input_{nullptr};

  // Changed summary line
  lv_obj_t* lbl_changed_{nullptr};

  // Error label
  lv_obj_t* lbl_error_{nullptr};

  // Action buttons
  lv_obj_t* actions_row_{nullptr};
  lv_obj_t* btn_cancel_{nullptr};
  lv_obj_t* btn_yes_{nullptr};

  OnScreenKeyboard keyboard_;
  WriteCallback on_write_;
  CancelCallback on_cancel_;

  ws::v1::WriteScope current_scope_{ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET};
  bool scene_scope_available_{true};
  std::string current_live_set_name_;
  std::string new_name_input_;
  std::string suggested_free_name_;
  std::string change_summary_text_;
  int32_t focus_idx_{1}; // 0 = Scene, 1 = Whole, 2 = New, 3 = Name Field, 4 = Cancel, 5 = Yes
  bool is_open_{false};
};

}  // namespace velora::ui

#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

enum class KeyboardLayout {
  EN,
  PT_BR,
  ES
};

class OnScreenKeyboard {
 public:
  using TextChangeCallback = std::function<void(const std::string& new_text)>;
  using ConfirmCallback = std::function<void(const std::string& final_text)>;
  using CancelCallback = std::function<void(const std::string& text)>;

  OnScreenKeyboard();
  ~OnScreenKeyboard();

  bool create(lv_obj_t* parent = nullptr);

  void open(const std::string& initial_text = "", KeyboardLayout layout = KeyboardLayout::EN);
  void close();
  bool is_open() const noexcept { return is_open_; }

  void set_layout(KeyboardLayout layout);
  KeyboardLayout layout() const noexcept { return layout_; }

  void set_text(const std::string& text);
  const std::string& text() const noexcept { return text_; }

  void on_panel_event(const PanelEvent& ev);

  void set_text_change_callback(TextChangeCallback cb) { on_text_change_ = std::move(cb); }
  void set_confirm_callback(ConfirmCallback cb) { on_confirm_ = std::move(cb); }
  void set_cancel_callback(CancelCallback cb) { on_cancel_ = std::move(cb); }

  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* done_button() const noexcept { return btn_done_; }
  size_t key_count() const noexcept { return key_btns_.size(); }
  lv_obj_t* key_button(size_t index) const { return (index < key_btns_.size()) ? key_btns_[index] : nullptr; }

  // Simulate key press for testing or UI event
  void press_key(const std::string& key_glyph);

 private:
  void build_keys();
  static void key_click_cb(lv_event_t* e);

  lv_obj_t* root_{nullptr};
  lv_obj_t* btn_done_{nullptr};
  lv_obj_t* lbl_done_{nullptr};
  lv_obj_t* btn_shift_{nullptr};
  lv_obj_t* btn_space_{nullptr};
  lv_obj_t* lbl_space_{nullptr};
  lv_obj_t* btn_backspace_{nullptr};

  std::vector<lv_obj_t*> row_objs_;
  std::vector<lv_obj_t*> key_btns_;
  std::vector<lv_obj_t*> key_lbls_;
  std::vector<std::string> key_glyphs_;

  std::string text_;
  bool is_open_{false};
  bool shift_active_{false};
  KeyboardLayout layout_{KeyboardLayout::EN};

  TextChangeCallback on_text_change_;
  ConfirmCallback on_confirm_;
  CancelCallback on_cancel_;
};

}  // namespace velora::ui

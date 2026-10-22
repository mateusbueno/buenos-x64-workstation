#pragma once

#include <lvgl.h>
#include <string>
#include <functional>

#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class NoticeToast {
 public:
  using ActionCallback = std::function<void()>;
  using DismissCallback = std::function<void()>;

  NoticeToast();
  ~NoticeToast();

  bool create(lv_obj_t* parent = nullptr);

  void show(const std::string& message, const std::string& action_text = "", uint32_t duration_ms = 8000);
  void hide();

  bool is_visible() const noexcept { return is_visible_; }
  const std::string& message() const noexcept { return message_; }
  const std::string& action_text() const noexcept { return action_text_; }

  void set_on_action(ActionCallback cb) { on_action_ = std::move(cb); }
  void set_on_dismiss(DismissCallback cb) { on_dismiss_ = std::move(cb); }

  void tick(uint32_t elapsed_ms);
  void on_panel_event(const PanelEvent& ev);

  lv_obj_t* toast_pill() const noexcept { return toast_pill_; }
  lv_obj_t* action_button() const noexcept { return btn_action_; }

 private:
  static void on_action_clicked(lv_event_t* e);
  static void on_toast_pressed(lv_event_t* e);
  static void on_toast_released(lv_event_t* e);

  ActionCallback on_action_;
  DismissCallback on_dismiss_;

  lv_obj_t* layer_container_{nullptr};
  lv_obj_t* toast_pill_{nullptr};
  lv_obj_t* lbl_msg_{nullptr};
  lv_obj_t* lbl_sep_{nullptr};
  lv_obj_t* btn_action_{nullptr};
  lv_obj_t* lbl_action_{nullptr};

  std::string message_;
  std::string action_text_;
  uint32_t remaining_ms_{0};
  bool is_visible_{false};
  bool is_pressed_{false};
};

}  // namespace velora::ui

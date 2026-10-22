#include "velora/ui/components/notice_toast.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

NoticeToast::NoticeToast() = default;

NoticeToast::~NoticeToast() = default;

bool NoticeToast::create(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // Toast layer container placed at bottom (above Hint/Status bar, bottom: 64px)
  layer_container_ = lv_obj_create(parent);
  lv_obj_set_size(layer_container_, 1024, 56);
  lv_obj_set_pos(layer_container_, 0, 480);
  lv_obj_set_style_bg_opa(layer_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(layer_container_, 0, 0);
  lv_obj_set_style_pad_all(layer_container_, 0, 0);
  lv_obj_set_flex_flow(layer_container_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(layer_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Pill
  toast_pill_ = lv_obj_create(layer_container_);
  lv_obj_set_height(toast_pill_, 56);
  lv_obj_set_style_bg_color(toast_pill_, p.raised, 0);
  lv_obj_set_style_bg_opa(toast_pill_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(toast_pill_, p.line_strong, 0);
  lv_obj_set_style_border_width(toast_pill_, 2, 0);
  lv_obj_set_style_radius(toast_pill_, r.full, 0);
  lv_obj_set_style_pad_ver(toast_pill_, 0, 0);
  lv_obj_set_style_pad_left(toast_pill_, sp.space_4, 0);
  lv_obj_set_style_pad_right(toast_pill_, sp.space_3, 0);
  lv_obj_set_flex_flow(toast_pill_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(toast_pill_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_add_event_cb(toast_pill_, on_toast_pressed, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(toast_pill_, on_toast_released, LV_EVENT_RELEASED, this);

  lbl_msg_ = lv_label_create(toast_pill_);
  lv_label_set_text(lbl_msg_, "");
  lv_obj_set_style_text_color(lbl_msg_, p.text, 0);

  lbl_sep_ = lv_label_create(toast_pill_);
  lv_label_set_text(lbl_sep_, tr("symbol.middle_dot").c_str());
  lv_obj_set_style_text_color(lbl_sep_, p.muted, 0);
  lv_obj_set_style_pad_hor(lbl_sep_, sp.space_2, 0);

  // Action button inside toast
  btn_action_ = lv_obj_create(toast_pill_);
  lv_obj_set_height(btn_action_, 56);
  lv_obj_set_style_bg_opa(btn_action_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_action_, 0, 0);
  lv_obj_set_style_pad_hor(btn_action_, sp.space_2, 0);
  lv_obj_set_flex_flow(btn_action_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_action_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(btn_action_, on_action_clicked, LV_EVENT_CLICKED, this);

  lbl_action_ = lv_label_create(btn_action_);
  lv_label_set_text(lbl_action_, "");
  lv_obj_set_style_text_color(lbl_action_, p.accent, 0);
  lv_obj_set_style_text_decor(lbl_action_, LV_TEXT_DECOR_UNDERLINE, 0);

  hide();
  return true;
}

void NoticeToast::show(const std::string& message, const std::string& action_text, uint32_t duration_ms) {
  message_ = message;
  action_text_ = action_text;
  remaining_ms_ = duration_ms;
  is_visible_ = true;
  is_pressed_ = false;

  if (lbl_msg_) {
    lv_label_set_text(lbl_msg_, message_.c_str());
  }

  if (action_text_.empty()) {
    if (lbl_sep_) lv_obj_set_hidden(lbl_sep_, true);
    if (btn_action_) lv_obj_set_hidden(btn_action_, true);
    if (toast_pill_) {
      const auto& sp = Theme::spacing();
      lv_obj_set_style_pad_right(toast_pill_, sp.space_4, 0);
    }
  } else {
    if (lbl_sep_) lv_obj_set_hidden(lbl_sep_, false);
    if (btn_action_) lv_obj_set_hidden(btn_action_, false);
    if (lbl_action_) lv_label_set_text(lbl_action_, action_text_.c_str());
    if (toast_pill_) {
      const auto& sp = Theme::spacing();
      lv_obj_set_style_pad_right(toast_pill_, sp.space_2, 0);
    }
  }

  if (layer_container_) {
    lv_obj_set_hidden(layer_container_, false);
    lv_obj_move_foreground(layer_container_);
  }
}

void NoticeToast::hide() {
  is_visible_ = false;
  remaining_ms_ = 0;
  if (layer_container_) {
    lv_obj_set_hidden(layer_container_, true);
  }
}

void NoticeToast::tick(uint32_t elapsed_ms) {
  if (!is_visible_ || is_pressed_) return;

  if (elapsed_ms >= remaining_ms_) {
    hide();
    if (on_dismiss_) {
      on_dismiss_();
    }
  } else {
    remaining_ms_ -= elapsed_ms;
  }
}

void NoticeToast::on_panel_event(const PanelEvent& ev) {
  if (!is_visible_) return;

  if (ev.type == PanelEventType::Button && ev.pressed && ev.id == MCU_BTN_EXIT) {
    hide();
    if (on_dismiss_) {
      on_dismiss_();
    }
  }
}

void NoticeToast::on_action_clicked(lv_event_t* e) {
  auto* self = static_cast<NoticeToast*>(lv_event_get_user_data(e));
  if (!self) return;

  self->hide();
  if (self->on_action_) {
    self->on_action_();
  }
}

void NoticeToast::on_toast_pressed(lv_event_t* e) {
  auto* self = static_cast<NoticeToast*>(lv_event_get_user_data(e));
  if (!self) return;
  self->is_pressed_ = true;
}

void NoticeToast::on_toast_released(lv_event_t* e) {
  auto* self = static_cast<NoticeToast*>(lv_event_get_user_data(e));
  if (!self) return;
  self->is_pressed_ = false;
}

}  // namespace velora::ui

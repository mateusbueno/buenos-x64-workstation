#include "velora/ui/components/action_sheet.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

#include <algorithm>

namespace velora::ui {

ActionSheet::ActionSheet() = default;

ActionSheet::~ActionSheet() {
  row_objs_.clear();
  marker_objs_.clear();
}

bool ActionSheet::create(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // Dim overlay covering the whole screen
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_set_style_bg_color(root_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
  lv_obj_set_clickable(root_, true);

  // Floating card
  card_ = lv_obj_create(root_);
  lv_obj_set_width(card_, 540);
  lv_obj_set_height(card_, LV_SIZE_CONTENT);
  lv_obj_align(card_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_scrollable(card_, false);
  lv_obj_set_style_bg_color(card_, p.raised, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(card_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(card_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(card_, r.lg, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card_, sp.space_5, LV_PART_MAIN);

  // Eyebrow
  lbl_eyebrow_ = lv_label_create(card_);
  lv_obj_add_style(lbl_eyebrow_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_eyebrow_, LV_ALIGN_TOP_LEFT, 0, 0);

  // Title
  lbl_title_ = lv_label_create(card_);
  lv_obj_add_style(lbl_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_title_, LV_ALIGN_TOP_LEFT, 0, 24);

  // Body (optional)
  lbl_body_ = lv_label_create(card_);
  lv_obj_add_style(lbl_body_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_body_, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_obj_set_hidden(lbl_body_, true);

  // Actions container
  container_actions_ = lv_obj_create(card_);
  lv_obj_set_width(container_actions_, 540 - 2 * sp.space_5);
  lv_obj_set_height(container_actions_, LV_SIZE_CONTENT);
  lv_obj_set_pos(container_actions_, 0, 80);
  lv_obj_set_scrollable(container_actions_, false);
  lv_obj_set_style_bg_opa(container_actions_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_actions_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container_actions_, 0, LV_PART_MAIN);

  // Footer text
  lbl_footer_ = lv_label_create(card_);
  std::string footer_txt = "VALUE " + tr("action_sheet.foot.value") + "   WRITE " + tr("action_sheet.foot.write") + "   EXIT " + tr("action_sheet.foot.exit");
  lv_label_set_text(lbl_footer_, footer_txt.c_str());
  lv_obj_add_style(lbl_footer_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_footer_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  close();
  return true;
}

void ActionSheet::open(const std::string& eyebrow,
                       const std::string& title,
                       const std::vector<ActionSheetOption>& options,
                       const std::string& body) {
  eyebrow_ = eyebrow;
  title_ = title;
  body_ = body;
  options_ = options;
  highlight_idx_ = 0;
  is_open_ = true;

  if (!root_) return;

  lv_label_set_text(lbl_eyebrow_, eyebrow_.c_str());
  lv_label_set_text(lbl_title_, title_.c_str());

  if (!body_.empty()) {
    lv_label_set_text(lbl_body_, body_.c_str());
    lv_obj_set_hidden(lbl_body_, false);
    lv_obj_set_pos(container_actions_, 0, 96);
  } else {
    lv_label_set_text(lbl_body_, "");
    lv_obj_set_hidden(lbl_body_, true);
    lv_obj_set_pos(container_actions_, 0, 64);
  }

  std::string footer_txt = "VALUE " + tr("action_sheet.foot.value") + "   WRITE " + tr("action_sheet.foot.write") + "   EXIT " + tr("action_sheet.foot.exit");
  lv_label_set_text(lbl_footer_, footer_txt.c_str());

  rebuild_actions();
  lv_obj_set_hidden(root_, false);
}

void ActionSheet::close() {
  is_open_ = false;
  if (root_) {
    lv_obj_set_hidden(root_, true);
  }
}

void ActionSheet::rebuild_actions() {
  row_objs_.clear();
  marker_objs_.clear();
  if (!container_actions_) return;
  lv_obj_clean(container_actions_);

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  int32_t y = 0;
  int32_t width = 540 - 2 * sp.space_5;

  for (size_t i = 0; i < options_.size(); ++i) {
    lv_obj_t* row = lv_obj_create(container_actions_);
    lv_obj_set_size(row, width, sp.touch_target_min);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    lv_obj_set_style_bg_color(row, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);

    // Marker ▶
    lv_obj_t* lbl_marker = lv_label_create(row);
    std::string marker_sym = "▶";
    lv_label_set_text(lbl_marker, marker_sym.c_str());
    lv_obj_set_style_text_color(lbl_marker, p.accent, LV_PART_MAIN);
    lv_obj_align(lbl_marker, LV_ALIGN_LEFT_MID, sp.space_3, 0);

    // Key badge [WRITE] or [EXIT]
    lv_obj_t* badge = lv_obj_create(row);
    lv_obj_set_size(badge, 64, 28);
    lv_obj_set_pos(badge, 36, (sp.touch_target_min - 28) / 2);
    lv_obj_set_scrollable(badge, false);
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);

    lv_obj_t* lbl_badge = lv_label_create(badge);
    lv_label_set_text(lbl_badge, options_[i].key_label.c_str());
    lv_obj_add_style(lbl_badge, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_badge, LV_ALIGN_CENTER, 0, 0);

    // Action text
    lv_obj_t* lbl_text = lv_label_create(row);
    lv_label_set_text(lbl_text, options_[i].text.c_str());
    lv_obj_add_style(lbl_text, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_text, LV_ALIGN_LEFT_MID, 114, 0);

    lv_obj_add_event_cb(row, on_action_click, LV_EVENT_CLICKED, this);

    row_objs_.push_back(row);
    marker_objs_.push_back(lbl_marker);

    y += sp.touch_target_min + sp.space_2;
  }

  // Adjust card height and footer position
  lv_obj_set_pos(lbl_footer_, 0, y + 16);
  lv_obj_set_height(card_, y + 64 + (body_.empty() ? 64 : 96));

  update_row_highlights();
}

void ActionSheet::update_row_highlights() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < row_objs_.size(); ++i) {
    bool is_hl = (static_cast<int32_t>(i) == highlight_idx_);
    lv_obj_set_style_border_color(row_objs_[i], is_hl ? p.accent : p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_objs_[i], is_hl ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_hidden(marker_objs_[i], !is_hl);
  }
}

void ActionSheet::set_highlight(int32_t index) {
  if (options_.empty()) return;
  highlight_idx_ = std::clamp(index, 0, static_cast<int32_t>(options_.size() - 1));
  update_row_highlights();
}

void ActionSheet::confirm_highlighted() {
  if (highlight_idx_ >= 0 && highlight_idx_ < static_cast<int32_t>(options_.size())) {
    auto cb = options_[highlight_idx_].on_select;
    close();
    if (cb) {
      cb();
    }
  }
}

void ActionSheet::on_action_click(lv_event_t* e) {
  auto* self = static_cast<ActionSheet*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (size_t i = 0; i < self->row_objs_.size(); ++i) {
    if (self->row_objs_[i] == target) {
      self->set_highlight(static_cast<int32_t>(i));
      self->confirm_highlighted();
      return;
    }
  }
}

void ActionSheet::on_panel_event(const PanelEvent& ev) {
  if (!is_open_) return;

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    set_highlight(highlight_idx_ + ev.delta);
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_WRITE) {
      confirm_highlighted();
      return;
    }
    if (ev.id == MCU_BTN_EXIT) {
      close();
      if (on_close_) {
        on_close_();
      }
      return;
    }
  }
}

}  // namespace velora::ui

#include "velora/ui/widgets/hint_bar.hpp"
#include "velora/ui/theme.hpp"

namespace velora::ui {

HintBar::~HintBar() {
  container_ = nullptr;
  left_box_ = nullptr;
  lbl_right_ = nullptr;
}

bool HintBar::create(lv_obj_t* parent) {
  if (container_) return true;
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& sp = Theme::spacing();

  container_ = lv_obj_create(parent);
  lv_obj_set_size(container_, 1024, sp.device_hint);
  lv_obj_set_scrollable(container_, false);
  lv_obj_add_style(container_, Theme::style_hintbar(), LV_PART_MAIN);

  // Left flex container for hint keycaps
  left_box_ = lv_obj_create(container_);
  lv_obj_set_size(left_box_, 750, sp.device_hint);
  lv_obj_set_pos(left_box_, sp.space_5, 0);
  lv_obj_set_scrollable(left_box_, false);
  lv_obj_set_style_bg_opa(left_box_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(left_box_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(left_box_, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(left_box_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(left_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(left_box_, sp.space_4, LV_PART_MAIN);

  // Right-aligned status/reassurance label
  lbl_right_ = lv_label_create(container_);
  lv_label_set_text(lbl_right_, "");
  lv_obj_add_style(lbl_right_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_right_, LV_ALIGN_RIGHT_MID, -sp.space_5, 0);

  rebuild_hints();
  return true;
}

void HintBar::set_hints(const std::vector<HintItem>& hints, const std::string& right_text) {
  current_hints_ = hints;
  current_right_text_ = right_text;
  if (container_) {
    rebuild_hints();
    set_right_text(right_text);
  }
}

void HintBar::set_right_text(const std::string& text) {
  current_right_text_ = text;
  if (lbl_right_) {
    lv_label_set_text(lbl_right_, text.c_str());
  }
}

void HintBar::rebuild_hints() {
  if (!left_box_) return;

  lv_obj_clean(left_box_);

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  for (const auto& item : current_hints_) {
    lv_obj_t* hint_group = lv_obj_create(left_box_);
    lv_obj_set_size(hint_group, LV_SIZE_CONTENT, sp.device_hint);
    lv_obj_set_scrollable(hint_group, false);
    lv_obj_set_style_bg_opa(hint_group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hint_group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hint_group, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(hint_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint_group, sp.space_2, LV_PART_MAIN);

    // Keycap badge
    lv_obj_t* keycap = lv_obj_create(hint_group);
    lv_obj_set_size(keycap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(keycap, false);
    lv_obj_set_style_bg_color(keycap, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(keycap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(keycap, p.line_strong, LV_PART_MAIN);
    lv_obj_set_style_border_width(keycap, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(keycap, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(keycap, r.sm, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(keycap, sp.space_1, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(keycap, 2, LV_PART_MAIN);

    lv_obj_t* lbl_ctrl = lv_label_create(keycap);
    lv_label_set_text(lbl_ctrl, item.control.c_str());
    lv_obj_add_style(lbl_ctrl, Theme::style_text(), LV_PART_MAIN);

    // Localized verb
    lv_obj_t* lbl_verb = lv_label_create(hint_group);
    lv_label_set_text(lbl_verb, item.verb.c_str());
    lv_obj_add_style(lbl_verb, Theme::style_text_muted(), LV_PART_MAIN);
  }
}

}  // namespace velora::ui

#include "velora/ui/widgets/segmented_control.hpp"
#include "velora/ui/theme.hpp"

namespace velora::ui {

SegmentedControl::~SegmentedControl() {
  container_ = nullptr;
  segment_objs_.clear();
  label_objs_.clear();
}

bool SegmentedControl::create(lv_obj_t* parent, const std::vector<std::string>& segments, int32_t min_segment_width) {
  if (container_) return true;
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  segments_ = segments;
  int32_t seg_w = (min_segment_width < sp.touch_target_min) ? sp.touch_target_min : min_segment_width;
  for (const auto& s : segments) {
    int32_t est_w = static_cast<int32_t>(s.length() * 9) + 24;
    if (est_w > seg_w) {
      seg_w = est_w;
    }
  }
  int32_t total_w = seg_w * static_cast<int32_t>(segments.size());
  int32_t height = sp.touch_target_min;  // 56px

  container_ = lv_obj_create(parent);
  lv_obj_set_size(container_, total_w, height);
  lv_obj_set_scrollable(container_, false);
  lv_obj_set_style_bg_color(container_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(container_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(container_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(container_, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  segment_objs_.resize(segments.size());
  label_objs_.resize(segments.size());

  for (size_t i = 0; i < segments.size(); ++i) {
    lv_obj_t* seg = lv_obj_create(container_);
    lv_obj_set_size(seg, seg_w, height);
    lv_obj_set_scrollable(seg, false);
    lv_obj_set_clickable(seg, true);
    lv_obj_set_style_pad_all(seg, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(seg, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(seg, segment_click_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl = lv_label_create(seg);
    lv_obj_set_width(lbl, seg_w - 8);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(lbl, segments[i].c_str());
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    segment_objs_[i] = seg;
    label_objs_[i] = lbl;
  }

  update_segment_styles();
  return true;
}

void SegmentedControl::set_selected_index(int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(segments_.size())) return;
  if (selected_index_ == index) return;
  selected_index_ = index;
  update_segment_styles();
  if (on_change_) {
    on_change_(selected_index_);
  }
}

void SegmentedControl::on_value_turn(int32_t delta) {
  int32_t new_idx = selected_index_ + delta;
  if (new_idx < 0) new_idx = 0;
  if (new_idx >= static_cast<int32_t>(segments_.size())) {
    new_idx = static_cast<int32_t>(segments_.size()) - 1;
  }
  set_selected_index(new_idx);
}

void SegmentedControl::on_write() {
  if (on_confirm_) {
    on_confirm_(selected_index_);
  }
}

void SegmentedControl::update_segment_styles() {
  if (!container_) return;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  for (size_t i = 0; i < segment_objs_.size(); ++i) {
    bool is_sel = (static_cast<int32_t>(i) == selected_index_);
    lv_obj_t* seg = segment_objs_[i];
    lv_obj_t* lbl = label_objs_[i];

    if (is_sel) {
      lv_obj_set_style_bg_color(seg, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(seg, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(seg, sp.selection_border, LV_PART_MAIN);
      lv_obj_set_style_radius(seg, r.sm, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl, p.text, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(seg, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(seg, p.line, LV_PART_MAIN);
      lv_obj_set_style_border_width(seg, 1, LV_PART_MAIN);
      lv_obj_set_style_border_side(seg, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
      lv_obj_set_style_radius(seg, 0, LV_PART_MAIN);
      lv_obj_set_style_text_color(lbl, p.muted, LV_PART_MAIN);
    }
  }
}

void SegmentedControl::segment_click_cb(lv_event_t* e) {
  auto* self = static_cast<SegmentedControl*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->segment_objs_.size(); ++i) {
    if (self->segment_objs_[i] == target) {
      self->set_selected_index(static_cast<int32_t>(i));
      break;
    }
  }
}

}  // namespace velora::ui

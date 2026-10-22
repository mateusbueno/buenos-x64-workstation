#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/theme.hpp"

namespace velora::ui {

SurfaceSkeleton::~SurfaceSkeleton() {
  root_ = nullptr;
  top_bar_ = nullptr;
  lbl_title_ = nullptr;
  content_area_ = nullptr;
}

bool SurfaceSkeleton::create(lv_obj_t* parent) {
  if (root_) return true;
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& sp = Theme::spacing();

  // Root container: 1024x600 fixed
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_add_style(root_, Theme::style_root(), LV_PART_MAIN);

  // Top Bar: 1024x56
  top_bar_ = lv_obj_create(root_);
  lv_obj_set_size(top_bar_, 1024, sp.device_topbar);
  lv_obj_set_pos(top_bar_, 0, 0);
  lv_obj_set_scrollable(top_bar_, false);
  lv_obj_add_style(top_bar_, Theme::style_topbar(), LV_PART_MAIN);

  lbl_title_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_title_, "");
  lv_obj_align(lbl_title_, LV_ALIGN_LEFT_MID, sp.space_5, 0);
  lv_obj_add_style(lbl_title_, Theme::style_text(), LV_PART_MAIN);

  // Content Area: 1024x488 (600 - 56 - 56)
  int32_t content_h = 600 - sp.device_topbar - sp.device_hint;
  content_area_ = lv_obj_create(root_);
  lv_obj_set_size(content_area_, 1024, content_h);
  lv_obj_set_pos(content_area_, 0, sp.device_topbar);
  lv_obj_set_scrollable(content_area_, false);
  lv_obj_add_style(content_area_, Theme::style_root(), LV_PART_MAIN);

  // Hint Bar: 1024x56 at bottom
  if (!hint_bar_.create(root_)) {
    return false;
  }
  lv_obj_set_pos(hint_bar_.container(), 0, 600 - sp.device_hint);

  return true;
}

void SurfaceSkeleton::set_title(const std::string& title) {
  if (lbl_title_) {
    lv_label_set_text(lbl_title_, title.c_str());
  }
}

void SurfaceSkeleton::set_hints(const std::vector<HintItem>& hints, const std::string& right_text) {
  hint_bar_.set_hints(hints, right_text);
}

}  // namespace velora::ui

#include "velora/ui/components/led_view.hpp"

#include "mcu/mcu_ids.h"
#include "velora/ui/theme.hpp"

namespace velora::ui {

namespace {
struct DotSpec {
  uint8_t id;
  int gap_before;  // extra space before this dot (group separation)
};
constexpr DotSpec kDots[] = {
    {MCU_LED_PART_SCENE_LED_1, 0},     {MCU_LED_PART_SCENE_LED_2, 0},     {MCU_LED_PART_SCENE_LED_3, 0},   {MCU_LED_PART_SCENE_LED_4, 0},
    {MCU_LED_PART_SCENE_LED_5, 0},     {MCU_LED_PART_SCENE_LED_6, 0},     {MCU_LED_PART_SCENE_LED_7, 0},   {MCU_LED_PART_SCENE_LED_8, 0},
    {MCU_LED_TOGGLE_LED_DRAWBARS, 16}, {MCU_LED_TOGGLE_LED_MIXER, 0},     {MCU_LED_TOGGLE_LED_PARTS, 10},  {MCU_LED_TOGGLE_LED_SCENES, 0},
    {MCU_LED_TOGGLE_LED_PART, 10},     {MCU_LED_TOGGLE_LED_ALL, 0},       {MCU_LED_TOGGLE_LED_ASSIGN, 0},
};
constexpr int kDotSize = 10;
constexpr int kDotGap = 6;
}  // namespace

bool LedView::create(lv_obj_t* parent) {
  if (!parent) parent = lv_layer_top();
  if (!parent) return false;
  const auto& p = Theme::current();

  container_ = lv_obj_create(parent);
  lv_obj_set_clickable(container_, false);
  lv_obj_set_scrollable(container_, false);
  lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(container_, 0, 0);
  lv_obj_set_style_pad_all(container_, 0, 0);
  int x = 0;
  for (const DotSpec& d : kDots) {
    x += d.gap_before;
    lv_obj_t* dot = lv_obj_create(container_);
    lv_obj_set_clickable(dot, false);
    lv_obj_set_scrollable(dot, false);
    lv_obj_set_size(dot, kDotSize, kDotSize);
    lv_obj_set_pos(dot, x, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, p.line_strong, 0);
    lv_obj_set_style_bg_color(dot, p.accent, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    dots_[d.id] = dot;
    x += kDotSize + kDotGap;
  }
  lv_obj_set_size(container_, x, kDotSize);
  lv_obj_set_pos(container_, 8, 8);
  return true;
}

void LedView::update(const LedDriver& driver) {
  if (!container_) return;
  for (const DotSpec& d : kDots) {
    const uint8_t b = driver.known(d.id) ? driver.output(d.id).brightness : 0;
    shown_[d.id] = b;
    lv_obj_set_style_bg_opa(dots_[d.id], b, 0);
  }
}

void LedView::set_visible(bool visible) {
  if (!container_) return;
  lv_obj_set_hidden(container_, !visible);
}

}  // namespace velora::ui

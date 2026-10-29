#include "velora/ui/screens/boot_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/system_config.hpp"

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace velora::ui {

BootView::BootView(WsClient* client)
    : client_(client),
      reduced_motion_(SystemConfig::instance().reduced_motion) {}

BootView::~BootView() {
  if (root_) {
    lv_obj_delete(root_);
    root_ = nullptr;
  }
}

bool BootView::create(lv_obj_t* parent) {
  if (root_) return true;
  if (!parent) parent = lv_screen_active();
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();

  // Root container: 1024x600 fixed, full bg
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_set_style_bg_color(root_, p.bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);

  // Symbol container: centers the 5 drawbars
  symbol_container_ = lv_obj_create(root_);
  lv_obj_set_size(symbol_container_, 1024, 300);
  lv_obj_set_pos(symbol_container_, 0, 0);
  lv_obj_set_scrollable(symbol_container_, false);
  lv_obj_set_style_bg_opa(symbol_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(symbol_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(symbol_container_, 0, LV_PART_MAIN);

  int start_x = kCenterX - (kTotalSymbolWidth / 2);
  drawbar_objs_.resize(kNumBars, nullptr);

  for (int i = 0; i < kNumBars; ++i) {
    lv_obj_t* bar = lv_obj_create(symbol_container_);
    int bar_x = start_x + i * (kBarWidth + kBarGap);
    int target_h = (i == 0) ? kTallBarHeight : kShortBarHeight;
    int initial_h = reduced_motion_ ? target_h : 4;
    int bar_y = kBaselineY - initial_h;

    lv_obj_set_size(bar, kBarWidth, initial_h);
    lv_obj_set_pos(bar, bar_x, bar_y);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_color(bar, p.text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, r.full, LV_PART_MAIN);
    drawbar_objs_[i] = bar;
  }

  // Wordmark VELORA
  lbl_wordmark_ = lv_label_create(root_);
  lv_label_set_text(lbl_wordmark_, tr("boot.wordmark").c_str());
  lv_obj_add_style(lbl_wordmark_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lbl_wordmark_, 8, LV_PART_MAIN);
  lv_obj_align(lbl_wordmark_, LV_ALIGN_TOP_MID, 0, 270);
  lv_obj_set_style_opa(lbl_wordmark_, reduced_motion_ ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);

  // Thin Progress Bar
  progress_bar_ = lv_bar_create(root_);
  lv_obj_set_size(progress_bar_, 320, 4);
  lv_obj_align(progress_bar_, LV_ALIGN_TOP_MID, 0, 350);
  lv_obj_set_style_bg_color(progress_bar_, p.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(progress_bar_, r.full, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_bar_, p.accent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progress_bar_, r.full, LV_PART_INDICATOR);
  lv_bar_set_range(progress_bar_, 0, 1000);
  lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);

  // Muted status line
  lbl_status_ = lv_label_create(root_);
  current_status_text_ = tr("boot.resolving_target");
  lv_label_set_text(lbl_status_, current_status_text_.c_str());
  lv_obj_add_style(lbl_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_status_, LV_ALIGN_TOP_MID, 0, 370);

  // Sub-status line (Story 8.3: "If power is lost, the Velora starts on <ver>.")
  lbl_sub_status_ = lv_label_create(root_);
  lv_label_set_text(lbl_sub_status_, "");
  lv_obj_add_style(lbl_sub_status_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_sub_status_, LV_ALIGN_TOP_MID, 0, 400);
  lv_obj_set_hidden(lbl_sub_status_, true);

  if (reduced_motion_) {
    animation_finished_ = true;
  }

  lv_obj_update_layout(root_);
  return true;
}

void BootView::set_visible(bool visible) {
  visible_ = visible;
  if (!root_) return;
  lv_obj_set_hidden(root_, !visible);
}

void BootView::set_reduced_motion(bool rm) {
  reduced_motion_ = rm;
  if (reduced_motion_) {
    anim_elapsed_ms_ = 2000;
    update_drawbars_layout();
    update_wordmark_opacity();
    animation_finished_ = true;
  }
}

uint32_t BootView::transition_duration_ms() const noexcept {
  return reduced_motion_ ? 50 : 120;  // <= 80 ms under reduced motion, <= 150 ms normal
}

int32_t BootView::bar_height(size_t idx) const {
  if (idx >= drawbar_objs_.size() || !drawbar_objs_[idx]) return 0;
  return lv_obj_get_height(drawbar_objs_[idx]);
}

lv_opa_t BootView::wordmark_opacity() const {
  if (!lbl_wordmark_) return LV_OPA_TRANSP;
  return lv_obj_get_style_opa(lbl_wordmark_, LV_PART_MAIN);
}

void BootView::set_progress(float fraction) {
  progress_ = std::clamp(fraction, 0.0f, 1.0f);
  if (progress_bar_) {
    int val = static_cast<int>(progress_ * 1000.0f);
    lv_bar_set_value(progress_bar_, val, LV_ANIM_OFF);
  }
}

void BootView::set_status(const std::string& live_set_name, int scene_number) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), tr("boot.status_restoring").c_str(), live_set_name.c_str(), scene_number);
  set_status_text(buf);
}

void BootView::set_status_text(const std::string& text) {
  current_status_text_ = text;
  if (lbl_status_) {
    lv_label_set_text(lbl_status_, current_status_text_.c_str());
    lv_obj_align(lbl_status_, LV_ALIGN_TOP_MID, 0, 370);
  }
}

void BootView::set_sub_status_text(const std::string& text) {
  current_sub_status_text_ = text;
  if (lbl_sub_status_) {
    lv_label_set_text(lbl_sub_status_, current_sub_status_text_.c_str());
    lv_obj_set_hidden(lbl_sub_status_, current_sub_status_text_.empty());
    lv_obj_align(lbl_sub_status_, LV_ALIGN_TOP_MID, 0, 400);
  }
}

void BootView::set_ready(bool ready) {
  is_ready_ = ready;
  if (is_ready_ && !transition_triggered_) {
    trigger_transition_to_performance();
  }
}

void BootView::trigger_transition_to_performance() {
  transition_triggered_ = true;
  if (on_ready_) {
    on_ready_();
  }
}

void BootView::tick(uint32_t elapsed_ms) {
  if (reduced_motion_ || animation_finished_) return;

  anim_elapsed_ms_ += elapsed_ms;
  update_drawbars_layout();
  update_wordmark_opacity();

  // Animation completes when all drawbars (last finishes at 600 + 700 = 1300 ms)
  // and wordmark fade (finishes at 1250 ms) are done.
  if (anim_elapsed_ms_ >= 1350) {
    animation_finished_ = true;
  }
}

void BootView::update_drawbars_layout() {
  if (drawbar_objs_.size() < kNumBars) return;

  constexpr uint32_t kDuration = 700;  // 0.7 s
  constexpr uint32_t kStagger = 150;   // 0.15 s

  int start_x = kCenterX - (kTotalSymbolWidth / 2);

  for (int i = 0; i < kNumBars; ++i) {
    lv_obj_t* bar = drawbar_objs_[i];
    if (!bar) continue;

    int target_h = (i == 0) ? kTallBarHeight : kShortBarHeight;
    int current_h = target_h;

    if (!reduced_motion_) {
      uint32_t bar_start = i * kStagger;
      if (anim_elapsed_ms_ <= bar_start) {
        current_h = 4;
      } else if (anim_elapsed_ms_ >= bar_start + kDuration) {
        current_h = target_h;
      } else {
        float u = static_cast<float>(anim_elapsed_ms_ - bar_start) / static_cast<float>(kDuration);
        // Ease-out cubic: 1 - (1 - u)^3
        float ease = 1.0f - std::pow(1.0f - u, 3.0f);
        current_h = 4 + static_cast<int>((target_h - 4) * ease);
      }
    }

    int bar_x = start_x + i * (kBarWidth + kBarGap);
    int bar_y = kBaselineY - current_h;

    lv_obj_set_size(bar, kBarWidth, current_h);
    lv_obj_set_pos(bar, bar_x, bar_y);
  }
  if (symbol_container_) {
    lv_obj_update_layout(symbol_container_);
  }
}

void BootView::update_wordmark_opacity() {
  if (!lbl_wordmark_) return;

  if (reduced_motion_) {
    lv_obj_set_style_opa(lbl_wordmark_, LV_OPA_COVER, LV_PART_MAIN);
    return;
  }

  constexpr uint32_t kFadeStart = 750;
  constexpr uint32_t kFadeDuration = 500;

  if (anim_elapsed_ms_ <= kFadeStart) {
    lv_obj_set_style_opa(lbl_wordmark_, LV_OPA_TRANSP, LV_PART_MAIN);
  } else if (anim_elapsed_ms_ >= kFadeStart + kFadeDuration) {
    lv_obj_set_style_opa(lbl_wordmark_, LV_OPA_COVER, LV_PART_MAIN);
  } else {
    float u = static_cast<float>(anim_elapsed_ms_ - kFadeStart) / static_cast<float>(kFadeDuration);
    lv_opa_t opa = static_cast<lv_opa_t>(static_cast<int>(LV_OPA_COVER) * u);
    lv_obj_set_style_opa(lbl_wordmark_, opa, LV_PART_MAIN);
  }
}

}  // namespace velora::ui

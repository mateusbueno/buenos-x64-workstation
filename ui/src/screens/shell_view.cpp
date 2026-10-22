#include "velora/ui/screens/shell_view.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

#include <cstdio>

namespace velora::ui {

ShellView::~ShellView() {
  root_ = nullptr;
}

bool ShellView::create(lv_obj_t* parent) {
  if (root_) return true;

  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  // Root container: 1024x600 fixed
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 1024, 600);
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_scrollable(root_, false);
  lv_obj_add_style(root_, Theme::style_root(), LV_PART_MAIN);

  // Top Bar: 1024x56
  top_bar_ = lv_obj_create(root_);
  lv_obj_set_size(top_bar_, 1024, 56);
  lv_obj_set_pos(top_bar_, 0, 0);
  lv_obj_set_scrollable(top_bar_, false);
  lv_obj_add_style(top_bar_, Theme::style_topbar(), LV_PART_MAIN);

  // Title: "Velora 73"
  lbl_title_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_title_, tr("shell.top_bar.title").c_str());
  lv_obj_align(lbl_title_, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_add_style(lbl_title_, Theme::style_text(), LV_PART_MAIN);

  // Live Set: center
  lbl_liveset_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_liveset_, tr("shell.hint.live_set").c_str());
  lv_obj_align(lbl_liveset_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_style(lbl_liveset_, Theme::style_text(), LV_PART_MAIN);

  // Clock source: right
  lbl_clock_source_ = lv_label_create(top_bar_);
  lv_label_set_text(lbl_clock_source_, tr("shell.top_bar.clock_internal").c_str());
  lv_obj_align(lbl_clock_source_, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_add_style(lbl_clock_source_, Theme::style_text(), LV_PART_MAIN);

  // Tempo: next to clock source
  lbl_tempo_ = lv_label_create(top_bar_);
  char tempo_buf[32];
  std::snprintf(tempo_buf, sizeof(tempo_buf), "%.1f BPM", current_bpm_);
  lv_label_set_text(lbl_tempo_, tempo_buf);
  lv_obj_align(lbl_tempo_, LV_ALIGN_RIGHT_MID, -80, 0);
  lv_obj_add_style(lbl_tempo_, Theme::style_text(), LV_PART_MAIN);

  // Main area: 1024x488
  main_area_ = lv_obj_create(root_);
  lv_obj_set_size(main_area_, 1024, 488);
  lv_obj_set_pos(main_area_, 0, 56);
  lv_obj_set_scrollable(main_area_, false);
  lv_obj_add_style(main_area_, Theme::style_root(), LV_PART_MAIN);

  // Hint Bar: 1024x56
  hint_bar_ = lv_obj_create(root_);
  lv_obj_set_size(hint_bar_, 1024, 56);
  lv_obj_set_pos(hint_bar_, 0, 544);
  lv_obj_set_scrollable(hint_bar_, false);
  lv_obj_add_style(hint_bar_, Theme::style_hintbar(), LV_PART_MAIN);

  // Status (left)
  lbl_status_ = lv_label_create(hint_bar_);
  lv_label_set_text(lbl_status_, tr("shell.hint.ready").c_str());
  lv_obj_align(lbl_status_, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_add_style(lbl_status_, Theme::style_text(), LV_PART_MAIN);

  // Hint (right)
  lbl_hint_ = lv_label_create(hint_bar_);
  lv_label_set_text(lbl_hint_, tr("shell.hint.write_to_save").c_str());
  lv_obj_align(lbl_hint_, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_add_style(lbl_hint_, Theme::style_text_muted(), LV_PART_MAIN);

  return true;
}

void ShellView::set_tempo(float bpm) {
  current_bpm_ = bpm;
  if (lbl_tempo_) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f BPM", bpm);
    lv_label_set_text(lbl_tempo_, buf);
  }
}

void ShellView::set_live_set_name(const std::string& name) {
  current_live_set_ = name;
  if (lbl_liveset_) {
    lv_label_set_text(lbl_liveset_, name.c_str());
  }
}

void ShellView::set_clock_lost(bool lost) {
  if (lbl_clock_source_) {
    if (lost) {
      lv_label_set_text(lbl_clock_source_, tr("shell.top_bar.clock_lost").c_str());
    } else {
      lv_label_set_text(lbl_clock_source_, tr("shell.top_bar.clock_internal").c_str());
    }
  }
}

void ShellView::update_from_state(const ws::v1::State& state) {
  for (const auto& pv : state.performance_init()) {
    if (pv.param_id() == ws::v1::PARAM_TEMPO_BPM) {
      set_tempo(pv.value());
      break;
    }
  }

  if (!state.live_set_name().empty()) {
    set_live_set_name(state.live_set_name());
  }
}

}  // namespace velora::ui

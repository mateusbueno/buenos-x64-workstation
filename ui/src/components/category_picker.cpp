#include "velora/ui/components/category_picker.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

CategoryPicker::CategoryPicker() = default;

CategoryPicker::~CategoryPicker() {
  tile_objs_.clear();
  tile_label_objs_.clear();
}

bool CategoryPicker::create(lv_obj_t* parent) {
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // Scrim overlay
  modal_root_ = lv_obj_create(parent);
  lv_obj_set_size(modal_root_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(modal_root_, 0, 0);
  lv_obj_set_style_bg_color(modal_root_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(modal_root_, LV_OPA_30, 0);
  lv_obj_set_style_border_width(modal_root_, 0, 0);
  lv_obj_set_style_pad_all(modal_root_, 0, 0);
  lv_obj_add_event_cb(modal_root_, on_scrim_clicked, LV_EVENT_CLICKED, this);

  // Raised panel card
  panel_card_ = lv_obj_create(modal_root_);
  lv_obj_set_size(panel_card_, 880, 500);
  lv_obj_center(panel_card_);
  lv_obj_set_style_bg_color(panel_card_, p.raised, 0);
  lv_obj_set_style_bg_opa(panel_card_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel_card_, p.line_strong, 0);
  lv_obj_set_style_border_width(panel_card_, 2, 0);
  lv_obj_set_style_radius(panel_card_, r.lg, 0);
  lv_obj_set_style_pad_all(panel_card_, sp.space_3, 0);
  lv_obj_set_flex_flow(panel_card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel_card_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Header row (Title + Subtitle)
  lv_obj_t* header_row = lv_obj_create(panel_card_);
  lv_obj_set_size(header_row, LV_PCT(100), 40);
  lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(header_row, 0, 0);
  lv_obj_set_style_pad_all(header_row, 0, 0);
  lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_title_ = lv_label_create(header_row);
  lv_label_set_text(lbl_title_, tr("category_picker.title").c_str());
  lv_obj_set_style_text_color(lbl_title_, p.text, 0);

  lbl_subtitle_ = lv_label_create(header_row);
  lv_label_set_text(lbl_subtitle_, "");
  lv_obj_set_style_text_color(lbl_subtitle_, p.muted, 0);

  // Grid container: 5 columns
  grid_container_ = lv_obj_create(panel_card_);
  lv_obj_set_size(grid_container_, LV_PCT(100), 340);
  lv_obj_set_style_bg_opa(grid_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid_container_, 0, 0);
  lv_obj_set_style_pad_all(grid_container_, 0, 0);
  lv_obj_set_flex_flow(grid_container_, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid_container_, 8, 0);
  lv_obj_set_style_pad_column(grid_container_, 8, 0);

  tile_objs_.resize(format::kCategoryCount);
  tile_label_objs_.resize(format::kCategoryCount);

  for (unsigned i = 0; i < format::kCategoryCount; ++i) {
    format::Category cat = static_cast<format::Category>(i + 1);
    std::string key = "category." + std::string(format::slug(cat)) + ".full";

    lv_obj_t* tile = lv_obj_create(grid_container_);
    // 5 columns across ~840px with 8px gaps -> ~160px width
    lv_obj_set_size(tile, 161, 56);
    lv_obj_set_style_bg_color(tile, p.surface, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tile, p.line, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_radius(tile, r.md, 0);
    lv_obj_set_style_pad_hor(tile, 4, 0);
    lv_obj_set_style_pad_ver(tile, 4, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, tr(key).c_str());
    lv_obj_set_style_text_color(lbl, p.text, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(95));

    lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED, this);

    tile_objs_[i] = tile;
    tile_label_objs_[i] = lbl;
  }

  // Apply one category to all row
  apply_row_obj_ = lv_obj_create(panel_card_);
  lv_obj_set_size(apply_row_obj_, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(apply_row_obj_, p.surface, 0);
  lv_obj_set_style_bg_opa(apply_row_obj_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(apply_row_obj_, p.line_strong, 0);
  lv_obj_set_style_border_width(apply_row_obj_, 2, 0);
  lv_obj_set_style_radius(apply_row_obj_, r.md, 0);
  lv_obj_set_style_pad_hor(apply_row_obj_, sp.space_3, 0);
  lv_obj_set_flex_flow(apply_row_obj_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(apply_row_obj_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(apply_row_obj_, on_apply_row_clicked, LV_EVENT_CLICKED, this);

  apply_check_lbl_ = lv_label_create(apply_row_obj_);
  lv_label_set_text(apply_check_lbl_, tr("symbol.check_unchecked").c_str());
  lv_obj_set_style_text_color(apply_check_lbl_, p.accent, 0);

  apply_text_lbl_ = lv_label_create(apply_row_obj_);
  lv_label_set_text(apply_text_lbl_, tr("category_picker.apply_all").c_str());
  lv_obj_set_style_text_color(apply_text_lbl_, p.text, 0);
  lv_obj_set_flex_grow(apply_text_lbl_, 1);

  apply_state_lbl_ = lv_label_create(apply_row_obj_);
  lv_label_set_text(apply_state_lbl_, tr("state.off").c_str());
  lv_obj_set_style_text_color(apply_state_lbl_, p.muted, 0);

  update_selection_ui();
  update_apply_ui();
  set_visible(false);

  return true;
}

void CategoryPicker::set_visible(bool visible) {
  is_visible_ = visible;
  if (!modal_root_) return;
  if (visible) {
    lv_obj_set_hidden(modal_root_, false);
    update_selection_ui();
    update_apply_ui();
  } else {
    lv_obj_set_hidden(modal_root_, true);
  }
}

void CategoryPicker::set_target_subtitle(const std::string& subtitle) {
  subtitle_ = subtitle;
  if (lbl_subtitle_) {
    lv_label_set_text(lbl_subtitle_, subtitle_.c_str());
  }
}

void CategoryPicker::set_selected_category(format::Category cat) {
  if (format::is_assignable(cat)) {
    selected_cat_ = cat;
    focus_idx_ = static_cast<int32_t>(cat) - 1;
    update_selection_ui();
  }
}

void CategoryPicker::set_apply_to_all(bool apply_all) {
  apply_to_all_ = apply_all;
  update_apply_ui();
}

void CategoryPicker::set_show_apply_all(bool show) {
  show_apply_all_ = show;
  if (apply_row_obj_) {
    lv_obj_set_hidden(apply_row_obj_, !show);
  }
}

void CategoryPicker::update_selection_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < tile_objs_.size(); ++i) {
    if (!tile_objs_[i]) continue;
    bool is_focused = (focus_idx_ == static_cast<int32_t>(i));
    bool is_sel = (selected_cat_ == static_cast<format::Category>(i + 1));

    if (is_focused || is_sel) {
      lv_obj_set_style_border_color(tile_objs_[i], p.accent, 0);
      lv_obj_set_style_border_width(tile_objs_[i], 3, 0);
      lv_obj_set_style_bg_color(tile_objs_[i], p.raised, 0);
    } else {
      lv_obj_set_style_border_color(tile_objs_[i], p.line, 0);
      lv_obj_set_style_border_width(tile_objs_[i], 2, 0);
      lv_obj_set_style_bg_color(tile_objs_[i], p.surface, 0);
    }
  }
}

void CategoryPicker::update_apply_ui() {
  const auto& p = Theme::current();
  if (!apply_row_obj_ || !apply_check_lbl_ || !apply_state_lbl_) return;

  bool is_row_focused = (focus_idx_ == static_cast<int32_t>(format::kCategoryCount));
  if (is_row_focused) {
    lv_obj_set_style_border_color(apply_row_obj_, p.accent, 0);
    lv_obj_set_style_border_width(apply_row_obj_, 3, 0);
  } else {
    lv_obj_set_style_border_color(apply_row_obj_, apply_to_all_ ? p.accent : p.line_strong, 0);
    lv_obj_set_style_border_width(apply_row_obj_, 2, 0);
  }

  if (apply_to_all_) {
    lv_label_set_text(apply_check_lbl_, tr("symbol.check_checked").c_str());
    lv_label_set_text(apply_state_lbl_, tr("state.on").c_str());
    lv_obj_set_style_text_color(apply_state_lbl_, p.accent, 0);
  } else {
    lv_label_set_text(apply_check_lbl_, tr("symbol.check_unchecked").c_str());
    lv_label_set_text(apply_state_lbl_, tr("state.off").c_str());
    lv_obj_set_style_text_color(apply_state_lbl_, p.muted, 0);
  }
}

void CategoryPicker::confirm_selection() {
  if (focus_idx_ >= 0 && focus_idx_ < static_cast<int32_t>(format::kCategoryCount)) {
    selected_cat_ = static_cast<format::Category>(focus_idx_ + 1);
  }
  set_visible(false);
  if (on_select_) {
    on_select_(selected_cat_, apply_to_all_);
  }
}

void CategoryPicker::cancel() {
  set_visible(false);
  if (on_cancel_) {
    on_cancel_();
  }
}

void CategoryPicker::on_panel_event(const PanelEvent& ev) {
  if (!is_visible_) return;

  int32_t max_items = show_apply_all_ ? static_cast<int32_t>(format::kCategoryCount) + 1 : static_cast<int32_t>(format::kCategoryCount);

  if (ev.type == PanelEventType::Encoder) {
    if (ev.id == MCU_CTRL_VALUE_ENCODER) {
      focus_idx_ += ev.delta;
      if (focus_idx_ < 0) focus_idx_ = 0;
      if (focus_idx_ >= max_items) focus_idx_ = max_items - 1;
      if (focus_idx_ < static_cast<int32_t>(format::kCategoryCount)) {
        selected_cat_ = static_cast<format::Category>(focus_idx_ + 1);
      }
      update_selection_ui();
      update_apply_ui();
    }
  } else if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_WRITE || ev.id == MCU_BTN_VALUE_PUSH) {
      if (focus_idx_ == static_cast<int32_t>(format::kCategoryCount)) {
        // Toggled apply row
        apply_to_all_ = !apply_to_all_;
        update_apply_ui();
      } else {
        confirm_selection();
      }
    } else if (ev.id == MCU_BTN_EXIT) {
      cancel();
    }
  }
}

void CategoryPicker::on_tile_clicked(lv_event_t* e) {
  auto* self = static_cast<CategoryPicker*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->tile_objs_.size(); ++i) {
    if (self->tile_objs_[i] == target) {
      self->focus_idx_ = static_cast<int32_t>(i);
      self->selected_cat_ = static_cast<format::Category>(i + 1);
      self->update_selection_ui();
      self->confirm_selection();
      break;
    }
  }
}

void CategoryPicker::on_apply_row_clicked(lv_event_t* e) {
  auto* self = static_cast<CategoryPicker*>(lv_event_get_user_data(e));
  if (!self) return;

  self->focus_idx_ = static_cast<int32_t>(format::kCategoryCount);
  self->apply_to_all_ = !self->apply_to_all_;
  self->update_apply_ui();
}

void CategoryPicker::on_scrim_clicked(lv_event_t* e) {
  auto* self = static_cast<CategoryPicker*>(lv_event_get_user_data(e));
  if (!self) return;
  if (static_cast<lv_obj_t*>(lv_event_get_target(e)) == self->modal_root_) {
    self->cancel();
  }
}

}  // namespace velora::ui

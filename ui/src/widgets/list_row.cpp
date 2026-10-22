#include "velora/ui/widgets/list_row.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

ListRow::~ListRow() {
  container_ = nullptr;
  lbl_title_ = nullptr;
  lbl_now_ = nullptr;
  lbl_warn_ = nullptr;
  lbl_meta_ = nullptr;
}

bool ListRow::create(lv_obj_t* parent, int32_t width, int32_t height) {
  if (container_) return true;
  if (!parent) {
    parent = lv_screen_active();
  }
  if (!parent) return false;

  const auto& sp = Theme::spacing();
  int32_t actual_height = (height < sp.touch_target_min) ? sp.touch_target_min : height;

  container_ = lv_obj_create(parent);
  lv_obj_set_size(container_, width, actual_height);
  lv_obj_set_scrollable(container_, false);

  // Clickable for touch target
  lv_obj_set_clickable(container_, true);
  lv_obj_add_event_cb(container_, event_cb, LV_EVENT_CLICKED, this);

  // Title label
  lbl_title_ = lv_label_create(container_);
  lv_label_set_text(lbl_title_, title_.c_str());
  lv_obj_add_style(lbl_title_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_title_, LV_ALIGN_LEFT_MID, sp.space_5, 0);

  // Now playing indicator
  lbl_now_ = lv_label_create(container_);
  lv_label_set_text(lbl_now_, "");
  lv_obj_set_style_text_color(lbl_now_, Theme::current().accent, LV_PART_MAIN);
  if (!now_playing_) {
    lv_obj_set_hidden(lbl_now_, true);
  }

  // Pending edits indicator
  lbl_warn_ = lv_label_create(container_);
  lv_label_set_text(lbl_warn_, "");
  lv_obj_set_style_text_color(lbl_warn_, Theme::current().warn, LV_PART_MAIN);
  if (!pending_edits_) {
    lv_obj_set_hidden(lbl_warn_, true);
  }

  // Meta / count label
  lbl_meta_ = lv_label_create(container_);
  lv_label_set_text(lbl_meta_, meta_.c_str());
  lv_obj_add_style(lbl_meta_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_meta_, LV_ALIGN_RIGHT_MID, -sp.space_5, 0);

  update_styles();
  return true;
}

void ListRow::set_title(const std::string& title) {
  title_ = title;
  if (lbl_title_) {
    lv_label_set_text(lbl_title_, title.c_str());
  }
}

void ListRow::set_meta(const std::string& meta) {
  meta_ = meta;
  if (lbl_meta_) {
    lv_label_set_text(lbl_meta_, meta.c_str());
  }
}

void ListRow::set_now_playing(bool now_playing) {
  now_playing_ = now_playing;
  if (lbl_now_) {
    if (now_playing) {
      lv_label_set_text(lbl_now_, tr("list_row.tag.now").c_str());
      lv_obj_set_hidden(lbl_now_, false);
      lv_obj_align_to(lbl_now_, lbl_title_, LV_ALIGN_OUT_RIGHT_MID, Theme::spacing().space_2, 0);
    } else {
      lv_label_set_text(lbl_now_, "");
      lv_obj_set_hidden(lbl_now_, true);
    }
  }
}

void ListRow::set_has_pending_edits(bool edits) {
  pending_edits_ = edits;
  if (lbl_warn_) {
    if (edits) {
      lv_label_set_text(lbl_warn_, tr("list_row.indicator.unsaved").c_str());
      lv_obj_set_hidden(lbl_warn_, false);
      lv_obj_t* anchor = (now_playing_ && lbl_now_) ? lbl_now_ : lbl_title_;
      lv_obj_align_to(lbl_warn_, anchor, LV_ALIGN_OUT_RIGHT_MID, Theme::spacing().space_2, 0);
    } else {
      lv_label_set_text(lbl_warn_, "");
      lv_obj_set_hidden(lbl_warn_, true);
    }
  }
}

void ListRow::set_selected(bool selected) {
  if (is_selected_ == selected) return;
  is_selected_ = selected;
  update_styles();
}

void ListRow::on_write() {
  if (is_selected_ && on_confirm_) {
    on_confirm_(this);
  }
}

void ListRow::update_styles() {
  if (!container_) return;

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  if (is_selected_) {
    lv_obj_set_style_bg_color(container_, p.raised, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(container_, p.accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(container_, sp.selection_border, LV_PART_MAIN);
    lv_obj_set_style_border_side(container_, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(container_, r.md, LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(container_, p.surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(container_, p.line, LV_PART_MAIN);
    lv_obj_set_style_border_width(container_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(container_, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(container_, 0, LV_PART_MAIN);
  }
}

void ListRow::event_cb(lv_event_t* e) {
  auto* self = static_cast<ListRow*>(lv_event_get_user_data(e));
  if (!self) return;

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    self->set_selected(true);
    if (self->on_select_) {
      self->on_select_(self);
    }
  }
}

}  // namespace velora::ui

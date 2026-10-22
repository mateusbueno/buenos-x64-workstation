#include "velora/ui/screens/import_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

#include <cstdio>

namespace velora::ui {

namespace {

std::string format_size(uint64_t bytes, const std::string& locale_code) {
  double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  char buf[32];
  if (locale_code == "pt-BR" || locale_code == "es") {
    snprintf(buf, sizeof(buf), "%.1f GB", gb);
    for (char* p = buf; *p; ++p) {
      if (*p == '.') *p = ',';
    }
  } else {
    snprintf(buf, sizeof(buf), "%.1f GB", gb);
  }
  return std::string(buf);
}

}  // namespace

ImportView::ImportView(WsClient* client) : client_(client) {}

ImportView::~ImportView() {
  row_objs_.clear();
  chip_objs_.clear();
  chip_lbl_objs_.clear();
  status_lbl_objs_.clear();
}

bool ImportView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) {
    return false;
  }

  skeleton_.set_title(tr("import.title"));

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();

  content_container_ = lv_obj_create(content);
  lv_obj_set_size(content_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(content_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content_container_, 0, 0);
  lv_obj_set_style_pad_all(content_container_, 0, 0);
  lv_obj_remove_flag(content_container_, LV_OBJ_FLAG_SCROLLABLE);

  // Rows container
  rows_container_ = lv_obj_create(content_container_);
  lv_obj_set_size(rows_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(rows_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rows_container_, 0, 0);
  lv_obj_set_style_pad_all(rows_container_, 0, 0);
  lv_obj_set_flex_flow(rows_container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rows_container_, sp.space_2, 0);

  // Progress container (hidden initially)
  progress_container_ = lv_obj_create(content_container_);
  lv_obj_set_size(progress_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(progress_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(progress_container_, 0, 0);
  lv_obj_set_style_pad_all(progress_container_, sp.space_5, 0);
  lv_obj_set_flex_flow(progress_container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(progress_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_hidden(progress_container_, true);

  progress_lbl_obj_ = lv_label_create(progress_container_);
  lv_label_set_text(progress_lbl_obj_, "");
  lv_obj_set_style_text_color(progress_lbl_obj_, p.text, 0);
  lv_obj_set_style_pad_bottom(progress_lbl_obj_, sp.space_4, 0);

  progress_bar_obj_ = lv_bar_create(progress_container_);
  lv_obj_set_size(progress_bar_obj_, 600, 16);
  lv_bar_set_range(progress_bar_obj_, 0, 100);
  lv_bar_set_value(progress_bar_obj_, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_bar_obj_, p.line, 0);
  lv_obj_set_style_bg_color(progress_bar_obj_, p.accent, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progress_bar_obj_, r.full, 0);
  lv_obj_set_style_radius(progress_bar_obj_, r.full, LV_PART_INDICATOR);

  // Message container for Insufficient Storage / Drive Removed
  message_container_ = lv_obj_create(content_container_);
  lv_obj_set_size(message_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(message_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(message_container_, 0, 0);
  lv_obj_set_style_pad_all(message_container_, sp.space_5, 0);
  lv_obj_set_flex_flow(message_container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(message_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_hidden(message_container_, true);

  message_lbl_obj_ = lv_label_create(message_container_);
  lv_label_set_text(message_lbl_obj_, "");
  lv_obj_set_style_text_color(message_lbl_obj_, p.warn, 0);
  lv_obj_set_style_text_align(message_lbl_obj_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(message_lbl_obj_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(message_lbl_obj_, 750);

  // Initialize embedded CategoryPicker
  category_picker_.create(parent);
  category_picker_.set_on_select([this](format::Category cat, bool apply_all) {
    if (apply_all) {
      for (auto& item : items_) {
        if (item.status != ImportItemStatus::SkippedUnsupported && item.status != ImportItemStatus::SkippedDamaged) {
          item.category = cat;
          item.status = ImportItemStatus::Ready;
          item.is_suggested = false;
        }
      }
    } else {
      if (focused_row_idx_ >= 0 && focused_row_idx_ < static_cast<int32_t>(items_.size())) {
        items_[focused_row_idx_].category = cat;
        items_[focused_row_idx_].status = ImportItemStatus::Ready;
        items_[focused_row_idx_].is_suggested = false;
      }
    }
    warning_msg_.clear();
    rebuild_rows_ui();
    update_hints_ui();
  });

  category_picker_.set_on_cancel([this]() {
    update_hints_ui();
  });

  update_header_ui();
  update_hints_ui();

  return true;
}

void ImportView::set_drive_info(const std::string& drive_name, uint64_t required_bytes, uint64_t internal_free_bytes) {
  drive_name_ = drive_name;
  required_bytes_ = required_bytes;
  internal_free_bytes_ = internal_free_bytes;
  update_header_ui();
}

void ImportView::set_items(std::vector<ImportItem> items) {
  items_ = std::move(items);
  focused_row_idx_ = 0;
  state_ = ImportViewState::Rows;
  warning_msg_.clear();
  rebuild_rows_ui();
  update_header_ui();
  update_hints_ui();
}

void ImportView::update_header_ui() {
  const std::string& loc = Locale::instance().current_locale();
  std::string size_str = format_size(required_bytes_, loc);

  char title_buf[128];
  snprintf(title_buf, sizeof(title_buf), tr("import.header").c_str(), drive_name_.c_str());
  skeleton_.set_title(title_buf);

  char sub_buf[64];
  snprintf(sub_buf, sizeof(sub_buf), tr("import.summary").c_str(), static_cast<int>(items_.size()), size_str.c_str());
  skeleton_.set_right_text(sub_buf);
}

void ImportView::rebuild_rows_ui() {
  if (!rows_container_) return;
  lv_obj_clean(rows_container_);
  row_objs_.clear();
  chip_objs_.clear();
  chip_lbl_objs_.clear();
  status_lbl_objs_.clear();

  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();
  const std::string& loc = Locale::instance().current_locale();

  row_objs_.resize(items_.size());
  chip_objs_.resize(items_.size());
  chip_lbl_objs_.resize(items_.size());
  status_lbl_objs_.resize(items_.size());

  for (size_t i = 0; i < items_.size(); ++i) {
    const auto& it = items_[i];

    lv_obj_t* row = lv_obj_create(rows_container_);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(row, p.surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, p.line, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_radius(row, r.md, 0);
    lv_obj_set_style_pad_hor(row, sp.space_3, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);

    // File name and size column
    lv_obj_t* col_info = lv_obj_create(row);
    lv_obj_set_size(col_info, 380, LV_PCT(100));
    lv_obj_set_style_bg_opa(col_info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_info, 0, 0);
    lv_obj_set_style_pad_all(col_info, 0, 0);
    lv_obj_set_flex_flow(col_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* lbl_fname = lv_label_create(col_info);
    lv_label_set_text(lbl_fname, it.file_name.c_str());
    lv_obj_set_style_text_color(lbl_fname, p.text, 0);

    lv_obj_t* lbl_fmeta = lv_label_create(col_info);
    std::string sz_str = format_size(it.size_bytes, loc) + " " + tr("symbol.middle_dot") + " " + it.format_str;
    lv_label_set_text(lbl_fmeta, sz_str.c_str());
    lv_obj_set_style_text_color(lbl_fmeta, p.muted, 0);

    // Category Chip (height >= 44px, min-width >= 56px)
    lv_obj_t* chip = lv_obj_create(row);
    lv_obj_set_height(chip, 44);
    lv_obj_set_style_bg_color(chip, p.raised, 0);
    lv_obj_set_style_radius(chip, r.full, 0);
    lv_obj_set_style_pad_hor(chip, sp.space_3, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(chip, on_category_chip_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_chip = lv_label_create(chip);

    if (it.category == format::Category::kNone) {
      lv_label_set_text(lbl_chip, tr("import.choose_category").c_str());
      lv_obj_set_style_border_color(chip, p.warn, 0);
      lv_obj_set_style_border_width(chip, 2, 0);
      lv_obj_set_style_text_color(lbl_chip, p.warn, 0);
    } else {
      std::string cat_key = "category." + std::string(format::slug(it.category)) + ".full";
      std::string chip_txt = tr(cat_key);
      if (it.is_suggested) {
        chip_txt += " " + tr("import.suggested");
      }
      lv_label_set_text(lbl_chip, chip_txt.c_str());
      lv_obj_set_style_border_color(chip, it.is_suggested ? p.accent : p.line_strong, 0);
      lv_obj_set_style_border_width(chip, 2, 0);
      lv_obj_set_style_text_color(lbl_chip, it.is_suggested ? p.accent : p.text, 0);
    }

    // Status label
    lv_obj_t* lbl_status = lv_label_create(row);
    lv_obj_set_flex_grow(lbl_status, 1);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_RIGHT, 0);

    switch (it.status) {
      case ImportItemStatus::Ready:
        lv_label_set_text(lbl_status, tr("import.status.ready").c_str());
        lv_obj_set_style_text_color(lbl_status, p.ok, 0);
        break;
      case ImportItemStatus::NeedsCategory:
        lv_label_set_text(lbl_status, tr("import.status.needs_category").c_str());
        lv_obj_set_style_text_color(lbl_status, p.warn, 0);
        break;
      case ImportItemStatus::SkippedUnsupported:
        lv_label_set_text(lbl_status, tr("import.status.unsupported").c_str());
        lv_obj_set_style_text_color(lbl_status, p.warn, 0);
        break;
      case ImportItemStatus::SkippedDamaged:
        lv_label_set_text(lbl_status, tr("import.status.damaged").c_str());
        lv_obj_set_style_text_color(lbl_status, p.warn, 0);
        break;
      case ImportItemStatus::AlreadyImported: {
        std::string txt = tr("import.status.already_imported") + " (" +
                          (it.import_again ? tr("import.action.import_again") : tr("import.action.skip")) + ")";
        lv_label_set_text(lbl_status, txt.c_str());
        lv_obj_set_style_text_color(lbl_status, p.warn, 0);
        break;
      }
    }

    row_objs_[i] = row;
    chip_objs_[i] = chip;
    chip_lbl_objs_[i] = lbl_chip;
    status_lbl_objs_[i] = lbl_status;
  }

  update_focus_ui();
}

void ImportView::update_focus_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < row_objs_.size(); ++i) {
    if (!row_objs_[i]) continue;
    bool is_focused = (focused_row_idx_ == static_cast<int32_t>(i));
    if (is_focused) {
      lv_obj_set_style_border_color(row_objs_[i], p.accent, 0);
      lv_obj_set_style_border_width(row_objs_[i], 3, 0);
      lv_obj_set_style_bg_color(row_objs_[i], p.raised, 0);
    } else {
      lv_obj_set_style_border_color(row_objs_[i], p.line, 0);
      lv_obj_set_style_border_width(row_objs_[i], 2, 0);
      lv_obj_set_style_bg_color(row_objs_[i], p.surface, 0);
    }
  }
}

void ImportView::update_hints_ui() {
  std::vector<HintItem> hints;
  if (state_ == ImportViewState::Rows) {
    hints = {
      {"VALUE", tr("hint.verb.scroll")},
      {"WRITE", tr("import.hint.start_import")},
      {"EXIT", tr("hint.verb.back")}
    };
    std::string reassurance = warning_msg_.empty() ? tr("import.hint.reassurance") : warning_msg_;
    skeleton_.set_hints(hints, reassurance);
  } else if (state_ == ImportViewState::Progress) {
    hints = {
      {"EXIT", tr("hint.verb.back")}
    };
    skeleton_.set_hints(hints, tr("import.hint.reassurance"));
  } else {
    hints = {
      {"EXIT", tr("hint.verb.back")}
    };
    skeleton_.set_hints(hints, "");
  }
}

void ImportView::check_and_start_import() {
  // Check for any non-skipped rows without category
  uint32_t needed_count = 0;
  for (const auto& it : items_) {
    if (it.status == ImportItemStatus::SkippedUnsupported || it.status == ImportItemStatus::SkippedDamaged) {
      continue;
    }
    if (it.category == format::Category::kNone) {
      ++needed_count;
    }
  }

  if (needed_count > 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), tr("import.warn.needs_category").c_str(), needed_count);
    warning_msg_ = buf;
    update_hints_ui();
    return;
  }

  // Check storage capacity
  if (required_bytes_ > internal_free_bytes_) {
    state_ = ImportViewState::InsufficientStorage;
    lv_obj_set_hidden(rows_container_, true);
    lv_obj_set_hidden(progress_container_, true);
    lv_obj_set_hidden(message_container_, false);

    const std::string& loc = Locale::instance().current_locale();
    std::string req_str = format_size(required_bytes_, loc);
    std::string free_str = format_size(internal_free_bytes_, loc);
    char err_buf[256];
    snprintf(err_buf, sizeof(err_buf), tr("import.storage_insufficient").c_str(), req_str.c_str(), free_str.c_str());
    lv_label_set_text(message_lbl_obj_, err_buf);
    update_hints_ui();
    return;
  }

  // Valid to start import!
  start_import();
}

void ImportView::start_import() {
  state_ = ImportViewState::Progress;
  current_importing_idx_ = 0;
  total_imported_ = 0;
  total_skipped_ = 0;

  for (const auto& it : items_) {
    if (it.status == ImportItemStatus::SkippedUnsupported || it.status == ImportItemStatus::SkippedDamaged) {
      ++total_skipped_;
    } else if (it.status == ImportItemStatus::AlreadyImported && !it.import_again) {
      ++total_skipped_;
    } else {
      ++total_imported_;
    }
  }

  lv_obj_set_hidden(rows_container_, true);
  lv_obj_set_hidden(message_container_, true);
  lv_obj_set_hidden(progress_container_, false);

  update_progress_ui();
  update_hints_ui();
}

void ImportView::update_progress_ui() {
  if (items_.empty() || !progress_lbl_obj_ || !progress_bar_obj_) return;

  uint32_t display_idx = current_importing_idx_ + 1;
  if (display_idx > items_.size()) display_idx = items_.size();

  char buf[256];
  snprintf(buf, sizeof(buf), tr("import.progress").c_str(),
           display_idx, static_cast<int>(items_.size()), items_[display_idx - 1].file_name.c_str());
  lv_label_set_text(progress_lbl_obj_, buf);

  int32_t pct = (items_.size() > 0) ? static_cast<int32_t>((display_idx * 100) / items_.size()) : 100;
  lv_bar_set_value(progress_bar_obj_, pct, LV_ANIM_OFF);
}

void ImportView::tick(uint32_t elapsed_ms) {
  if (category_picker_.is_visible()) return;

  if (state_ == ImportViewState::Progress) {
    if (current_importing_idx_ < items_.size()) {
      ++current_importing_idx_;
      update_progress_ui();
    } else {
      state_ = ImportViewState::Completed;
      if (on_import_finished_) {
        on_import_finished_(total_imported_, total_skipped_);
      }
    }
  }
}

void ImportView::simulate_drive_removed() {
  state_ = ImportViewState::DriveRemoved;
  lv_obj_set_hidden(rows_container_, true);
  lv_obj_set_hidden(progress_container_, true);
  lv_obj_set_hidden(message_container_, false);

  char buf[256];
  snprintf(buf, sizeof(buf), tr("import.drive_removed").c_str(),
           current_importing_idx_, static_cast<int>(items_.size()));
  lv_label_set_text(message_lbl_obj_, buf);
  update_hints_ui();
}

void ImportView::on_panel_event(const PanelEvent& ev) {
  if (category_picker_.is_visible()) {
    category_picker_.on_panel_event(ev);
    return;
  }

  if (state_ == ImportViewState::Rows) {
    if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
      focused_row_idx_ += ev.delta;
      if (focused_row_idx_ < 0) focused_row_idx_ = 0;
      if (focused_row_idx_ >= static_cast<int32_t>(items_.size())) {
        focused_row_idx_ = static_cast<int32_t>(items_.size()) - 1;
      }
      update_focus_ui();
    } else if (ev.type == PanelEventType::Button && ev.pressed) {
      if (ev.id == MCU_BTN_WRITE) {
        check_and_start_import();
      } else if (ev.id == MCU_BTN_VALUE_PUSH) {
        if (focused_row_idx_ >= 0 && focused_row_idx_ < static_cast<int32_t>(items_.size())) {
          auto& it = items_[focused_row_idx_];
          if (it.status == ImportItemStatus::AlreadyImported) {
            it.import_again = !it.import_again;
            rebuild_rows_ui();
          } else if (it.status != ImportItemStatus::SkippedUnsupported && it.status != ImportItemStatus::SkippedDamaged) {
            category_picker_.set_target_subtitle(it.file_name);
            category_picker_.set_selected_category(it.category == format::Category::kNone ? format::Category::kPiano : it.category);
            category_picker_.set_visible(true);
          }
        }
      } else if (ev.id == MCU_BTN_EXIT) {
        if (on_close_) on_close_();
      }
    }
  } else if (state_ == ImportViewState::Progress) {
    if (ev.type == PanelEventType::Button && ev.pressed && ev.id == MCU_BTN_EXIT) {
      // EXIT sends import to background; returns to previous surface
      if (on_close_) on_close_();
    }
  } else {
    if (ev.type == PanelEventType::Button && ev.pressed && ev.id == MCU_BTN_EXIT) {
      if (on_close_) on_close_();
    }
  }
}

void ImportView::on_row_clicked(lv_event_t* e) {
  auto* self = static_cast<ImportView*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->row_objs_.size(); ++i) {
    if (self->row_objs_[i] == target) {
      self->focused_row_idx_ = static_cast<int32_t>(i);
      self->update_focus_ui();
      break;
    }
  }
}

void ImportView::on_category_chip_clicked(lv_event_t* e) {
  auto* self = static_cast<ImportView*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->chip_objs_.size(); ++i) {
    if (self->chip_objs_[i] == target) {
      self->focused_row_idx_ = static_cast<int32_t>(i);
      self->update_focus_ui();
      auto& it = self->items_[i];
      if (it.status != ImportItemStatus::SkippedUnsupported && it.status != ImportItemStatus::SkippedDamaged) {
        self->category_picker_.set_target_subtitle(it.file_name);
        self->category_picker_.set_selected_category(it.category == format::Category::kNone ? format::Category::kPiano : it.category);
        self->category_picker_.set_visible(true);
      }
      break;
    }
  }
}

}  // namespace velora::ui

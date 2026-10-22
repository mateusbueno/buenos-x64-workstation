#include "velora/ui/screens/library_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"

#include <cstdio>
#include <algorithm>

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

LibraryView::LibraryView(WsClient* client) : client_(client) {}

LibraryView::~LibraryView() {
  card_objs_.clear();
  card_name_objs_.clear();
  card_meta_objs_.clear();
  card_star_objs_.clear();
  category_chip_objs_.clear();
}

bool LibraryView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) {
    return false;
  }

  skeleton_.set_title(tr("library.title"));

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();

  content_container_ = lv_obj_create(content);
  lv_obj_set_size(content_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(content_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content_container_, 0, 0);
  lv_obj_set_style_pad_all(content_container_, sp.space_3, 0);
  lv_obj_set_flex_flow(content_container_, LV_FLEX_FLOW_COLUMN);

  // 1. Search bar
  search_bar_obj_ = lv_obj_create(content_container_);
  lv_obj_set_size(search_bar_obj_, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(search_bar_obj_, p.surface, 0);
  lv_obj_set_style_bg_opa(search_bar_obj_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(search_bar_obj_, p.line_strong, 0);
  lv_obj_set_style_border_width(search_bar_obj_, 2, 0);
  lv_obj_set_style_radius(search_bar_obj_, r.md, 0);
  lv_obj_set_style_pad_hor(search_bar_obj_, sp.space_3, 0);
  lv_obj_set_flex_flow(search_bar_obj_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(search_bar_obj_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_search_icon_ = lv_label_create(search_bar_obj_);
  lv_label_set_text(lbl_search_icon_, tr("symbol.search").c_str());
  lv_obj_set_style_text_color(lbl_search_icon_, p.muted, 0);

  lbl_search_text_ = lv_label_create(search_bar_obj_);
  lv_label_set_text(lbl_search_text_, tr("library.search_placeholder").c_str());
  lv_obj_set_style_text_color(lbl_search_text_, p.muted, 0);
  lv_obj_set_flex_grow(lbl_search_text_, 1);

  // 2. Filter chips container (horizontally scrollable)
  chips_container_ = lv_obj_create(content_container_);
  lv_obj_set_size(chips_container_, LV_PCT(100), 48);
  lv_obj_set_style_bg_opa(chips_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chips_container_, 0, 0);
  lv_obj_set_style_pad_all(chips_container_, 0, 0);
  lv_obj_set_flex_flow(chips_container_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chips_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(chips_container_, sp.space_2, 0);

  // All chip
  chip_all_ = lv_obj_create(chips_container_);
  lv_obj_set_height(chip_all_, 36);
  lv_obj_set_style_radius(chip_all_, r.full, 0);
  lv_obj_set_style_pad_hor(chip_all_, sp.space_3, 0);
  lv_obj_set_flex_flow(chip_all_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip_all_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(chip_all_, on_chip_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* lbl_all = lv_label_create(chip_all_);
  lv_label_set_text(lbl_all, tr("library.filter.all").c_str());

  // Favorites chip
  chip_fav_ = lv_obj_create(chips_container_);
  lv_obj_set_height(chip_fav_, 36);
  lv_obj_set_style_radius(chip_fav_, r.full, 0);
  lv_obj_set_style_pad_hor(chip_fav_, sp.space_3, 0);
  lv_obj_set_flex_flow(chip_fav_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip_fav_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(chip_fav_, on_chip_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* lbl_fav = lv_label_create(chip_fav_);
  lv_label_set_text(lbl_fav, tr("library.filter.favorites").c_str());

  // Recent chip
  chip_rec_ = lv_obj_create(chips_container_);
  lv_obj_set_height(chip_rec_, 36);
  lv_obj_set_style_radius(chip_rec_, r.full, 0);
  lv_obj_set_style_pad_hor(chip_rec_, sp.space_3, 0);
  lv_obj_set_flex_flow(chip_rec_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip_rec_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(chip_rec_, on_chip_clicked, LV_EVENT_CLICKED, this);
  lv_obj_t* lbl_rec = lv_label_create(chip_rec_);
  lv_label_set_text(lbl_rec, tr("library.filter.recent").c_str());

  // 17 Category chips
  category_chip_objs_.resize(format::kCategoryCount);
  for (unsigned i = 0; i < format::kCategoryCount; ++i) {
    format::Category cat = static_cast<format::Category>(i + 1);
    std::string cat_key = "category." + std::string(format::slug(cat)) + ".short";

    lv_obj_t* chip = lv_obj_create(chips_container_);
    lv_obj_set_height(chip, 36);
    lv_obj_set_style_radius(chip, r.full, 0);
    lv_obj_set_style_pad_hor(chip, sp.space_3, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(chip, on_chip_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl = lv_label_create(chip);
    lv_label_set_text(lbl, tr(cat_key).c_str());

    category_chip_objs_[i] = chip;
  }

  // 3. Results Grid container (3 columns)
  grid_container_ = lv_obj_create(content_container_);
  lv_obj_set_size(grid_container_, LV_PCT(100), 340);
  lv_obj_set_style_bg_opa(grid_container_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid_container_, 0, 0);
  lv_obj_set_style_pad_all(grid_container_, 0, 0);
  lv_obj_set_flex_flow(grid_container_, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid_container_, sp.space_2, 0);
  lv_obj_set_style_pad_column(grid_container_, sp.space_2, 0);

  empty_label_obj_ = lv_label_create(grid_container_);
  lv_label_set_text(empty_label_obj_, tr("library.empty").c_str());
  lv_obj_set_style_text_color(empty_label_obj_, p.muted, 0);
  lv_obj_set_hidden(empty_label_obj_, true);

  ensure_builtin_timbres();
  refilter();
  update_chips_ui();
  update_header_ui();
  update_hints_ui();

  return true;
}

void LibraryView::ensure_builtin_timbres() {
  bool has_init_fm = false, has_init_va = false;
  for (const auto& it : catalog_) {
    if (it.name == "Init FM" || it.id == "builtin:init-fm") has_init_fm = true;
    if (it.name == "Init VA" || it.id == "builtin:init-va") has_init_va = true;
  }
  if (!has_init_fm) {
    catalog_.push_back({"builtin:init-fm", "Init FM", format::Category::kSynthPoly, true, false, 0, 0});
  }
  if (!has_init_va) {
    catalog_.push_back({"builtin:init-va", "Init VA", format::Category::kSynthPoly, true, false, 0, 0});
  }
}

void LibraryView::set_mode(LibraryMode mode, int32_t target_part_idx) {
  mode_ = mode;
  target_part_idx_ = target_part_idx;
  audition_active_ = false;
  last_auditioned_note_ = 0;
  update_header_ui();
  update_hints_ui();
}

void LibraryView::set_catalog(std::vector<TimbreCatalogEntry> entries) {
  catalog_ = std::move(entries);
  ensure_builtin_timbres();
  refilter();
  update_header_ui();
}

void LibraryView::set_search_query(const std::string& query) {
  search_query_ = query;
  if (lbl_search_text_) {
    if (search_query_.empty()) {
      lv_label_set_text(lbl_search_text_, tr("library.search_placeholder").c_str());
      const auto& p = Theme::current();
      lv_obj_set_style_text_color(lbl_search_text_, p.muted, 0);
    } else {
      lv_label_set_text(lbl_search_text_, search_query_.c_str());
      const auto& p = Theme::current();
      lv_obj_set_style_text_color(lbl_search_text_, p.text, 0);
    }
  }
  refilter();
}

void LibraryView::set_filter_category(format::Category cat) {
  filter_cat_ = cat;
  filter_favorites_only_ = false;
  filter_recent_only_ = false;
  refilter();
  update_chips_ui();
}

void LibraryView::set_filter_favorites_only(bool fav) {
  filter_favorites_only_ = fav;
  if (fav) {
    filter_cat_ = format::Category::kNone;
    filter_recent_only_ = false;
  }
  refilter();
  update_chips_ui();
}

void LibraryView::set_filter_recent_only(bool rec) {
  filter_recent_only_ = rec;
  if (rec) {
    filter_cat_ = format::Category::kNone;
    filter_favorites_only_ = false;
  }
  refilter();
  update_chips_ui();
}

void LibraryView::refilter() {
  filtered_indices_.clear();

  std::string lower_query = search_query_;
  std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

  for (size_t i = 0; i < catalog_.size(); ++i) {
    const auto& it = catalog_[i];

    if (filter_favorites_only_ && !it.is_favorite) continue;
    if (filter_recent_only_ && it.last_used_timestamp == 0) continue;
    if (filter_cat_ != format::Category::kNone && it.category != filter_cat_) continue;

    if (!lower_query.empty()) {
      std::string lower_name = it.name;
      std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
      if (lower_name.find(lower_query) == std::string::npos) continue;
    }

    filtered_indices_.push_back(i);
  }

  focused_grid_idx_ = 0;
  rebuild_grid_ui();
}

void LibraryView::update_header_ui() {
  if (mode_ == LibraryMode::ChooseForPart) {
    char buf[128];
    snprintf(buf, sizeof(buf), tr("library.choose_title").c_str(), target_part_idx_ + 1);
    skeleton_.set_title(buf);
  } else {
    skeleton_.set_title(tr("library.title"));
  }

  uint64_t total_bytes = 0;
  for (const auto& it : catalog_) {
    total_bytes += it.size_bytes;
  }
  const std::string& loc = Locale::instance().current_locale();
  std::string sz_str = format_size(total_bytes, loc);

  char sub_buf[64];
  char count_buf[32];
  snprintf(count_buf, sizeof(count_buf), "%zu", catalog_.size());
  snprintf(sub_buf, sizeof(sub_buf), tr("library.summary").c_str(), count_buf, sz_str.c_str());
  skeleton_.set_right_text(sub_buf);
}

void LibraryView::update_chips_ui() {
  const auto& p = Theme::current();

  auto apply_chip_style = [&](lv_obj_t* chip, bool active) {
    if (!chip) return;
    if (active) {
      lv_obj_set_style_bg_color(chip, p.accent, 0);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(chip, p.accent, 0);
      lv_obj_set_style_border_width(chip, 2, 0);
      lv_obj_t* child = lv_obj_get_child(chip, 0);
      if (child) lv_obj_set_style_text_color(child, p.on_accent, 0);
    } else {
      lv_obj_set_style_bg_color(chip, p.surface, 0);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(chip, p.line_strong, 0);
      lv_obj_set_style_border_width(chip, 2, 0);
      lv_obj_t* child = lv_obj_get_child(chip, 0);
      if (child) lv_obj_set_style_text_color(child, p.text, 0);
    }
  };

  bool is_all_active = (!filter_favorites_only_ && !filter_recent_only_ && filter_cat_ == format::Category::kNone);
  apply_chip_style(chip_all_, is_all_active);
  apply_chip_style(chip_fav_, filter_favorites_only_);
  apply_chip_style(chip_rec_, filter_recent_only_);

  for (size_t i = 0; i < category_chip_objs_.size(); ++i) {
    bool is_cat_active = (filter_cat_ == static_cast<format::Category>(i + 1));
    apply_chip_style(category_chip_objs_[i], is_cat_active);
  }
}

void LibraryView::rebuild_grid_ui() {
  if (!grid_container_) return;
  lv_obj_clean(grid_container_);
  card_objs_.clear();
  card_name_objs_.clear();
  card_meta_objs_.clear();
  card_star_objs_.clear();

  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();
  const auto& r = Theme::radius();

  if (filtered_indices_.empty()) {
    empty_label_obj_ = lv_label_create(grid_container_);
    lv_label_set_text(empty_label_obj_, tr("library.empty").c_str());
    lv_obj_set_style_text_color(empty_label_obj_, p.muted, 0);
    return;
  }

  card_objs_.resize(filtered_indices_.size());
  card_name_objs_.resize(filtered_indices_.size());
  card_meta_objs_.resize(filtered_indices_.size());
  card_star_objs_.resize(filtered_indices_.size());

  for (size_t i = 0; i < filtered_indices_.size(); ++i) {
    size_t cat_idx = filtered_indices_[i];
    const auto& it = catalog_[cat_idx];

    // Card: 3 columns across 1024 - ~32px = 992px -> ~316px per card
    lv_obj_t* card = lv_obj_create(grid_container_);
    lv_obj_set_size(card, 318, 72);
    lv_obj_set_style_bg_color(card, p.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, p.line, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, r.md, 0);
    lv_obj_set_style_pad_all(card, sp.space_3, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(card, on_card_long_pressed, LV_EVENT_LONG_PRESSED, this);

    // Title row with star
    lv_obj_t* top_row = lv_obj_create(card);
    lv_obj_set_size(top_row, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lbl_name = lv_label_create(top_row);
    lv_label_set_text(lbl_name, it.name.c_str());
    lv_obj_set_style_text_color(lbl_name, p.text, 0);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_name, 260);

    lv_obj_t* lbl_star = lv_label_create(top_row);
    lv_label_set_text(lbl_star, it.is_favorite ? tr("symbol.star").c_str() : "");
    lv_obj_set_style_text_color(lbl_star, p.accent, 0);

    // Meta row (Category · Factory / Imported)
    std::string cat_key = "category." + std::string(format::slug(it.category)) + ".full";
    std::string origin_str = it.is_factory ? tr("library.origin.factory") : tr("library.origin.imported");
    std::string meta_str = tr(cat_key) + " " + tr("symbol.middle_dot") + " " + origin_str;

    lv_obj_t* lbl_meta = lv_label_create(card);
    lv_label_set_text(lbl_meta, meta_str.c_str());
    lv_obj_set_style_text_color(lbl_meta, p.muted, 0);

    card_objs_[i] = card;
    card_name_objs_[i] = lbl_name;
    card_meta_objs_[i] = lbl_meta;
    card_star_objs_[i] = lbl_star;
  }

  update_focus_ui();
}

void LibraryView::update_focus_ui() {
  const auto& p = Theme::current();
  for (size_t i = 0; i < card_objs_.size(); ++i) {
    if (!card_objs_[i]) continue;
    bool is_focused = (focused_grid_idx_ == static_cast<int32_t>(i));
    if (is_focused) {
      lv_obj_set_style_border_color(card_objs_[i], p.accent, 0);
      lv_obj_set_style_border_width(card_objs_[i], 3, 0);
      lv_obj_set_style_bg_color(card_objs_[i], p.raised, 0);
    } else {
      lv_obj_set_style_border_color(card_objs_[i], p.line, 0);
      lv_obj_set_style_border_width(card_objs_[i], 2, 0);
      lv_obj_set_style_bg_color(card_objs_[i], p.surface, 0);
    }
  }
}

void LibraryView::update_hints_ui() {
  std::vector<HintItem> hints = {
    {"VALUE", tr("hint.verb.scroll")},
    {"WRITE", tr("hint.verb.confirm")},
    {"EXIT", tr("hint.verb.back")}
  };
  std::string reassurance = (mode_ == LibraryMode::ChooseForPart) ? tr("library.hint.audition") : "";
  skeleton_.set_hints(hints, reassurance);
}

void LibraryView::assign_focused_timbre() {
  if (focused_grid_idx_ < 0 || focused_grid_idx_ >= static_cast<int32_t>(filtered_indices_.size())) return;

  size_t cat_idx = filtered_indices_[focused_grid_idx_];
  const auto& it = catalog_[cat_idx];

  if (mode_ == LibraryMode::ChooseForPart) {
    if (on_timbre_selected_) {
      on_timbre_selected_(target_part_idx_, it.id);
    }
    if (on_close_) {
      on_close_();
    }
  }
}

void LibraryView::toggle_favorite_at_focused() {
  if (focused_grid_idx_ < 0 || focused_grid_idx_ >= static_cast<int32_t>(filtered_indices_.size())) return;
  size_t cat_idx = filtered_indices_[focused_grid_idx_];
  toggle_favorite(cat_idx);
}

void LibraryView::toggle_favorite(size_t catalog_idx) {
  if (catalog_idx >= catalog_.size()) return;
  catalog_[catalog_idx].is_favorite = !catalog_[catalog_idx].is_favorite;
  if (on_favorite_toggled_) {
    on_favorite_toggled_(catalog_[catalog_idx].id, catalog_[catalog_idx].is_favorite);
  }
  refilter();
}

void LibraryView::on_midi_note(uint8_t note_number) {
  audition_active_ = true;
  last_auditioned_note_ = note_number;
}

void LibraryView::on_panel_event(const PanelEvent& ev) {
  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    focused_grid_idx_ += ev.delta;
    if (focused_grid_idx_ < 0) focused_grid_idx_ = 0;
    if (focused_grid_idx_ >= static_cast<int32_t>(filtered_indices_.size())) {
      focused_grid_idx_ = static_cast<int32_t>(filtered_indices_.size()) - 1;
    }
    update_focus_ui();
  } else if (ev.type == PanelEventType::Button && ev.pressed) {
    if (ev.id == MCU_BTN_WRITE || ev.id == MCU_BTN_VALUE_PUSH) {
      assign_focused_timbre();
    } else if (ev.id == MCU_BTN_EXIT) {
      if (on_close_) on_close_();
    }
  }
}

void LibraryView::on_card_clicked(lv_event_t* e) {
  auto* self = static_cast<LibraryView*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->card_objs_.size(); ++i) {
    if (self->card_objs_[i] == target) {
      self->focused_grid_idx_ = static_cast<int32_t>(i);
      self->update_focus_ui();
      if (self->mode_ == LibraryMode::ChooseForPart) {
        self->assign_focused_timbre();
      }
      break;
    }
  }
}

void LibraryView::on_card_long_pressed(lv_event_t* e) {
  auto* self = static_cast<LibraryView*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (size_t i = 0; i < self->card_objs_.size(); ++i) {
    if (self->card_objs_[i] == target) {
      self->focused_grid_idx_ = static_cast<int32_t>(i);
      self->toggle_favorite_at_focused();
      break;
    }
  }
}

void LibraryView::on_chip_clicked(lv_event_t* e) {
  auto* self = static_cast<LibraryView*>(lv_event_get_user_data(e));
  if (!self) return;

  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  if (target == self->chip_all_) {
    self->set_filter_category(format::Category::kNone);
  } else if (target == self->chip_fav_) {
    self->set_filter_favorites_only(!self->filter_favorites_only_);
  } else if (target == self->chip_rec_) {
    self->set_filter_recent_only(!self->filter_recent_only_);
  } else {
    for (size_t i = 0; i < self->category_chip_objs_.size(); ++i) {
      if (self->category_chip_objs_[i] == target) {
        format::Category chosen = static_cast<format::Category>(i + 1);
        if (self->filter_cat_ == chosen) {
          self->set_filter_category(format::Category::kNone);
        } else {
          self->set_filter_category(chosen);
        }
        break;
      }
    }
  }
}

}  // namespace velora::ui

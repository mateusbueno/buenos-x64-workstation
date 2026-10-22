#include "velora/ui/screens/setlist_browser_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/ws_client.hpp"

#include <algorithm>
#include <cstdio>

namespace velora::ui {

static std::string to_lower(const std::string& s) {
  std::string res = s;
  std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  return res;
}

SetlistBrowserView::SetlistBrowserView(WsClient* client)
    : client_(client) {
  // Sample data conforming to mockups and test requirements
  user_tags_ = {"Wedding", "Church", "Pop", "Jazz"};

  all_items_ = {
      {"song-1", "A Thousand Years", BrowserItemType::LiveSet, "LIVE SET · IN WEDDING ANA & PEDRO +2", true, false, true, {"Wedding", "Pop"}},
      {"song-2", "Thousand Miles", BrowserItemType::LiveSet, "LIVE SET · IN ROCK COVERS", false, false, false, {"Pop"}},
      {"song-3", "Thou Art Worthy", BrowserItemType::LiveSet, "LIVE SET · IN SUNDAY SERVICE", false, false, false, {"Church"}},
      {"setlist-1", "Thousand Lights Tour", BrowserItemType::Setlist, "SETLIST · 16 SONGS · LAST MONTH", false, false, false, {"Pop"}},
      {"song-4", "Perfect", BrowserItemType::LiveSet, "LIVE SET · IN WEDDING ANA & PEDRO", false, true, true, {"Wedding"}},
      {"song-5", "Canon in D", BrowserItemType::LiveSet, "LIVE SET · IN WEDDING ANA & PEDRO", true, false, false, {"Wedding"}},
      {"setlist-2", "Wedding Ana & Pedro", BrowserItemType::Setlist, "SETLIST · 12 SONGS", true, false, false, {"Wedding"}},
      {"setlist-3", "Sunday Service", BrowserItemType::Setlist, "SETLIST · 9 SONGS", false, false, false, {"Church"}},
  };
}

SetlistBrowserView::~SetlistBrowserView() {
  chip_objs_.clear();
  tile_objs_.clear();
}

bool SetlistBrowserView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;

  keyboard_.create(skeleton_.root_container());
  keyboard_.close();

  action_sheet_.create(skeleton_.root_container());
  action_sheet_.close();

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // 1. Search Field Container (y = 8, h = 50, w = 992)
  container_search_ = lv_obj_create(content);
  lv_obj_set_size(container_search_, 992, 50);
  lv_obj_set_pos(container_search_, 16, 8);
  lv_obj_set_scrollable(container_search_, false);
  lv_obj_set_clickable(container_search_, true);
  lv_obj_set_style_bg_color(container_search_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container_search_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(container_search_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_search_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(container_search_, r.md, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container_search_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(container_search_, on_search_click, LV_EVENT_CLICKED, this);

  lbl_search_icon_ = lv_label_create(container_search_);
  std::string icon_txt = "⌕";
  lv_label_set_text(lbl_search_icon_, icon_txt.c_str());
  lv_obj_add_style(lbl_search_icon_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_search_icon_, LV_ALIGN_LEFT_MID, 16, 0);

  lbl_search_text_ = lv_label_create(container_search_);
  lv_label_set_text(lbl_search_text_, "");
  lv_obj_add_style(lbl_search_text_, Theme::style_text(), LV_PART_MAIN);
  lv_obj_align(lbl_search_text_, LV_ALIGN_LEFT_MID, 44, 0);

  caret_search_ = lv_obj_create(container_search_);
  lv_obj_set_size(caret_search_, 2, 24);
  lv_obj_set_style_bg_color(caret_search_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(caret_search_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(caret_search_, 0, LV_PART_MAIN);
  lv_obj_align(caret_search_, LV_ALIGN_LEFT_MID, 46, 0);

  lbl_search_hint_ = lv_label_create(container_search_);
  lv_label_set_text(lbl_search_hint_, tr("setlist_browser.search_hint").c_str());
  lv_obj_add_style(lbl_search_hint_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_search_hint_, LV_ALIGN_RIGHT_MID, -16, 0);

  // 2. Chips Container (y = 66, h = 42, w = 992)
  container_chips_ = lv_obj_create(content);
  lv_obj_set_size(container_chips_, 992, 42);
  lv_obj_set_pos(container_chips_, 16, 66);
  lv_obj_set_scrollable(container_chips_, true);
  lv_obj_set_style_bg_opa(container_chips_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_chips_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container_chips_, 0, LV_PART_MAIN);

  // 3. Result Grid Container (y = 116, h = 360, w = 992)
  container_grid_ = lv_obj_create(content);
  lv_obj_set_size(container_grid_, 992, 360);
  lv_obj_set_pos(container_grid_, 16, 116);
  lv_obj_set_scrollable(container_grid_, true);
  lv_obj_set_style_bg_opa(container_grid_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_grid_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container_grid_, 0, LV_PART_MAIN);

  // Empty state message (centered in grid area)
  lbl_empty_msg_ = lv_label_create(content);
  lv_obj_add_style(lbl_empty_msg_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_empty_msg_, LV_ALIGN_CENTER, 0, 40);
  lv_obj_set_hidden(lbl_empty_msg_, true);

  rebuild_chips();
  filter_items();
  update_hints();
  return true;
}

void SetlistBrowserView::rebuild_chips() {
  chip_objs_.clear();
  if (!container_chips_) return;
  lv_obj_clean(container_chips_);

  const auto& p = Theme::current();
  const auto& sp = Theme::spacing();

  chip_names_ = {
      tr("setlist_browser.chip.favorites"),
      tr("setlist_browser.chip.recent"),
      tr("setlist_browser.chip.setlists"),
      tr("setlist_browser.chip.all_live_sets")
  };
  for (const auto& tag : user_tags_) {
    chip_names_.push_back(tag);
  }

  int32_t x = 0;
  for (size_t i = 0; i < chip_names_.size(); ++i) {
    bool is_on = (i == selected_chip_idx_);
    lv_obj_t* chip = lv_obj_create(container_chips_);
    lv_obj_set_height(chip, 36);
    lv_obj_set_scrollable(chip, false);
    lv_obj_set_clickable(chip, true);
    lv_obj_set_style_radius(chip, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(chip, sp.space_3, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 6, LV_PART_MAIN);

    if (is_on) {
      lv_obj_set_style_bg_color(chip, p.accent, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(chip, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(chip, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(chip, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    }

    lv_obj_t* lbl = lv_label_create(chip);
    lv_label_set_text(lbl, chip_names_[i].c_str());
    if (is_on) {
      lv_obj_set_style_text_color(lbl, p.on_accent, LV_PART_MAIN);
    } else {
      lv_obj_add_style(lbl, Theme::style_text(), LV_PART_MAIN);
    }
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_pos(chip, x, 0);
    lv_obj_add_event_cb(chip, on_chip_click, LV_EVENT_CLICKED, this);

    chip_objs_.push_back(chip);
    x += 120 + sp.space_2; // approximate dynamic chip stride
  }
}

void SetlistBrowserView::filter_items() {
  filtered_indices_.clear();
  std::string q_lower = to_lower(search_query_);

  for (size_t i = 0; i < all_items_.size(); ++i) {
    const auto& item = all_items_[i];

    // 1. Text Search Filter
    if (!q_lower.empty()) {
      std::string name_lower = to_lower(item.name);
      std::string meta_lower = to_lower(item.meta);
      if (name_lower.find(q_lower) == std::string::npos &&
          meta_lower.find(q_lower) == std::string::npos) {
        continue;
      }
    }

    // 2. Chip Filter
    // 0: Favorites, 1: Recent, 2: Setlists, 3: All Live Sets, >=4: User tags
    if (selected_chip_idx_ == 0) {
      if (!item.is_favorite) continue;
    } else if (selected_chip_idx_ == 1) {
      if (!item.is_recent) continue;
    } else if (selected_chip_idx_ == 2) {
      if (item.type != BrowserItemType::Setlist) continue;
    } else if (selected_chip_idx_ == 3) {
      if (item.type != BrowserItemType::LiveSet) continue;
    } else if (selected_chip_idx_ >= 4) {
      size_t tag_idx = selected_chip_idx_ - 4;
      if (tag_idx < user_tags_.size()) {
        const std::string& required_tag = user_tags_[tag_idx];
        if (std::find(item.tags.begin(), item.tags.end(), required_tag) == item.tags.end()) {
          continue;
        }
      }
    }

    filtered_indices_.push_back(i);
  }

  // Determine empty message if filtered list is empty
  current_empty_message_.clear();
  if (filtered_indices_.empty()) {
    if (!search_query_.empty()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), tr("setlist_browser.empty.no_matches").c_str(), search_query_.c_str());
      current_empty_message_ = buf;
    } else if (selected_chip_idx_ == 0) {
      current_empty_message_ = tr("setlist_browser.empty.no_favorites");
    } else if (selected_chip_idx_ == 1) {
      current_empty_message_ = tr("setlist_browser.empty.no_recent");
    } else {
      current_empty_message_ = tr("setlist.empty.no_setlists_yet");
    }
  }

  if (selected_result_idx_ >= static_cast<int32_t>(filtered_indices_.size())) {
    selected_result_idx_ = filtered_indices_.empty() ? 0 : static_cast<int32_t>(filtered_indices_.size() - 1);
  }

  rebuild_grid();
  update_top_bar();
}

void SetlistBrowserView::rebuild_grid() {
  tile_objs_.clear();
  if (!container_grid_) return;
  lv_obj_clean(container_grid_);

  if (filtered_indices_.empty()) {
    lv_label_set_text(lbl_empty_msg_, current_empty_message_.c_str());
    lv_obj_set_hidden(lbl_empty_msg_, false);
    return;
  }
  lv_obj_set_hidden(lbl_empty_msg_, true);

  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  int32_t col_w = 318;
  int32_t row_h = 76;
  int32_t gap_x = 18;
  int32_t gap_y = 10;

  for (size_t grid_i = 0; grid_i < filtered_indices_.size(); ++grid_i) {
    size_t item_idx = filtered_indices_[grid_i];
    const auto& item = all_items_[item_idx];
    bool is_sel = (static_cast<int32_t>(grid_i) == selected_result_idx_);

    int32_t col = grid_i % 3;
    int32_t row = grid_i / 3;
    int32_t x = col * (col_w + gap_x);
    int32_t y = row * (row_h + gap_y);

    lv_obj_t* tile = lv_obj_create(container_grid_);
    lv_obj_set_size(tile, col_w, row_h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_style_radius(tile, r.md, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, sp.space_3, LV_PART_MAIN);

    if (is_sel) {
      lv_obj_set_style_bg_color(tile, p.raised, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.accent, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 3, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(tile, p.surface, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(tile, p.line_strong, LV_PART_MAIN);
      lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    }

    // Title label
    lv_obj_t* lbl_title = lv_label_create(tile);
    lv_label_set_text(lbl_title, item.name.c_str());
    lv_obj_add_style(lbl_title, Theme::style_text(), LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Star icon ★ if favorite
    if (item.is_favorite) {
      lv_obj_t* lbl_star = lv_label_create(tile);
      std::string star_sym = "★";
      lv_label_set_text(lbl_star, star_sym.c_str());
      lv_obj_set_style_text_color(lbl_star, p.accent, LV_PART_MAIN);
      lv_obj_align(lbl_star, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    // Warn icon ◆ if has edits
    if (item.has_edits ||
        (std::find(dirty_live_set_ids_.begin(), dirty_live_set_ids_.end(), item.id) != dirty_live_set_ids_.end())) {
      lv_obj_t* lbl_warn = lv_label_create(tile);
      std::string warn_sym = "◆";
      lv_label_set_text(lbl_warn, warn_sym.c_str());
      lv_obj_set_style_text_color(lbl_warn, p.warn, LV_PART_MAIN);
      if (item.is_favorite) {
        lv_obj_align(lbl_warn, LV_ALIGN_TOP_RIGHT, -20, 0);
      } else {
        lv_obj_align(lbl_warn, LV_ALIGN_TOP_RIGHT, 0, 0);
      }
    }

    // Meta line below title
    lv_obj_t* lbl_meta = lv_label_create(tile);
    lv_label_set_text(lbl_meta, item.meta.c_str());
    lv_obj_add_style(lbl_meta, Theme::style_text_muted(), LV_PART_MAIN);
    lv_obj_align(lbl_meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_event_cb(tile, on_tile_click, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(tile, on_tile_long_press, LV_EVENT_LONG_PRESSED, this);

    tile_objs_.push_back(tile);
  }
}

void SetlistBrowserView::update_top_bar() {
  std::string title = tr("setlist_browser.title");
  char buf[64];
  if (!search_query_.empty()) {
    std::snprintf(buf, sizeof(buf), tr("setlist_browser.results_for_query").c_str(),
                  static_cast<int>(filtered_indices_.size()), search_query_.c_str());
  } else {
    std::snprintf(buf, sizeof(buf), tr("setlist_browser.results_count").c_str(),
                  static_cast<int>(filtered_indices_.size()));
  }
  title += " · ";
  title += buf;
  skeleton_.set_title(title);
}

void SetlistBrowserView::update_hints() {
  std::vector<HintItem> hints = {
      {"VALUE", tr("hint.verb.scroll")},
      {"WRITE", tr("setlist_browser.hint.load_or_jump")},
      {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, tr("setlist_browser.hint.reassurance"));
}

void SetlistBrowserView::set_search_query(const std::string& query) {
  search_query_ = query;
  if (lbl_search_text_) {
    lv_label_set_text(lbl_search_text_, search_query_.c_str());
    if (caret_search_) {
      int32_t offset = static_cast<int32_t>(search_query_.length() * 10);
      lv_obj_align(caret_search_, LV_ALIGN_LEFT_MID, 46 + offset, 0);
    }
  }
  filter_items();
}

void SetlistBrowserView::select_chip(size_t chip_index) {
  if (chip_index >= chip_names_.size()) return;
  selected_chip_idx_ = chip_index;
  rebuild_chips();
  filter_items();
}

void SetlistBrowserView::set_items(const std::vector<BrowserItem>& items) {
  all_items_ = items;
  filter_items();
}

void SetlistBrowserView::set_user_tags(const std::vector<std::string>& tags) {
  user_tags_ = tags;
  rebuild_chips();
  filter_items();
}

void SetlistBrowserView::select_result(size_t index) {
  if (index >= filtered_indices_.size()) return;
  selected_result_idx_ = static_cast<int32_t>(index);
  rebuild_grid();
}

void SetlistBrowserView::activate_selected() {
  if (selected_result_idx_ < 0 || selected_result_idx_ >= static_cast<int32_t>(filtered_indices_.size())) return;
  size_t item_idx = filtered_indices_[selected_result_idx_];
  const auto& item = all_items_[item_idx];

  last_activated_id_ = item.id;
  last_activated_type_ = item.type;

  if (item.type == BrowserItemType::LiveSet) {
    if (client_) {
      ws::v1::ClientMessage msg;
      msg.set_request_id(301);
      auto* cmd = msg.mutable_jump_to_live_set();
      cmd->set_id(item.id);
      client_->send_command(msg);
    }
  } else {
    if (client_) {
      ws::v1::ClientMessage msg;
      msg.set_request_id(302);
      auto* cmd = msg.mutable_load_setlist();
      cmd->set_id(item.id);
      client_->send_command(msg);
    }
    loaded_setlist_id_ = item.id;
  }
}

void SetlistBrowserView::toggle_favorite_on_selected() {
  if (selected_result_idx_ < 0 || selected_result_idx_ >= static_cast<int32_t>(filtered_indices_.size())) return;
  size_t item_idx = filtered_indices_[selected_result_idx_];
  all_items_[item_idx].is_favorite = !all_items_[item_idx].is_favorite;
  filter_items();
}

void SetlistBrowserView::open_action_sheet_for_selected() {
  if (selected_result_idx_ < 0 || selected_result_idx_ >= static_cast<int32_t>(filtered_indices_.size())) return;
  size_t item_idx = filtered_indices_[selected_result_idx_];
  const auto& item = all_items_[item_idx];

  std::string eyebrow = (item.type == BrowserItemType::LiveSet)
      ? tr("action_sheet.eyebrow.live_set")
      : tr("action_sheet.eyebrow.setlist");

  std::vector<ActionSheetOption> options;
  if (!item.is_favorite) {
    options.push_back({"WRITE", tr("action_sheet.add_to_favorites"), [this]() {
      toggle_favorite_on_selected();
    }, false});
  } else {
    options.push_back({"WRITE", tr("action_sheet.remove_from_favorites"), [this]() {
      toggle_favorite_on_selected();
    }, false});
  }

  options.push_back({"EXIT", tr("action_sheet.cancel"), nullptr, true});

  action_sheet_.open(eyebrow, item.name, options);
}

void SetlistBrowserView::on_panel_event(const PanelEvent& ev) {
  if (action_sheet_.is_open()) {
    action_sheet_.on_panel_event(ev);
    return;
  }

  if (keyboard_.is_open()) {
    keyboard_.on_panel_event(ev);
    return;
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    if (!filtered_indices_.empty()) {
      int32_t next = selected_result_idx_ + ev.delta;
      next = std::clamp(next, 0, static_cast<int32_t>(filtered_indices_.size() - 1));
      select_result(next);
    }
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    switch (ev.id) {
      case MCU_BTN_WRITE:
        activate_selected();
        break;
      case MCU_BTN_EXIT:
        if (on_close_) {
          on_close_();
        }
        break;
      default:
        break;
    }
  }
}

void SetlistBrowserView::update_from_state(const ws::v1::State& state) {
  if (state.has_setlists()) {
    loaded_setlist_id_ = state.setlists().loaded_setlist_id();
  }
  dirty_live_set_ids_.clear();
  if (state.has_edits()) {
    for (int i = 0; i < state.edits().dirty_live_set_ids_size(); ++i) {
      dirty_live_set_ids_.push_back(state.edits().dirty_live_set_ids(i));
    }
  }
  filter_items();
}

void SetlistBrowserView::update_from_setlists_reply(const ws::v1::SetlistsReply& reply) {
  std::vector<BrowserItem> new_items;
  for (int i = 0; i < reply.setlists_size(); ++i) {
    const auto& s = reply.setlists(i);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SETLIST · %u SONGS", s.live_set_count());
    new_items.push_back({s.id(), s.name(), BrowserItemType::Setlist, buf, false, false, false, {}});
  }
  for (int i = 0; i < reply.entries_size(); ++i) {
    const auto& e = reply.entries(i);
    new_items.push_back({e.live_set_id(), e.name(), BrowserItemType::LiveSet, "LIVE SET", false, false, false, {}});
  }
  set_items(new_items);
}

void SetlistBrowserView::on_search_click(lv_event_t* e) {
  auto* self = static_cast<SetlistBrowserView*>(lv_event_get_user_data(e));
  if (!self) return;

  self->keyboard_.open(self->search_query_, [self](const std::string& query) {
    self->set_search_query(query);
  });
}

void SetlistBrowserView::on_chip_click(lv_event_t* e) {
  auto* self = static_cast<SetlistBrowserView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (size_t i = 0; i < self->chip_objs_.size(); ++i) {
    if (self->chip_objs_[i] == target) {
      self->select_chip(i);
      return;
    }
  }
}

void SetlistBrowserView::on_tile_click(lv_event_t* e) {
  auto* self = static_cast<SetlistBrowserView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (size_t i = 0; i < self->tile_objs_.size(); ++i) {
    if (self->tile_objs_[i] == target) {
      self->select_result(i);
      return;
    }
  }
}

void SetlistBrowserView::on_tile_long_press(lv_event_t* e) {
  auto* self = static_cast<SetlistBrowserView*>(lv_event_get_user_data(e));
  if (!self) return;

  auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (size_t i = 0; i < self->tile_objs_.size(); ++i) {
    if (self->tile_objs_[i] == target) {
      self->select_result(i);
      self->open_action_sheet_for_selected();
      return;
    }
  }
}

}  // namespace velora::ui

#include "velora/ui/screens/setlist_view.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/ws_client.hpp"

#include <cstdio>
#include <algorithm>

namespace velora::ui {

SetlistView::SetlistView(WsClient* client)
    : client_(client) {
  // Populate realistic sample data if running standalone
  setlists_ = {
      {"setlist-1", "Wedding Ana & Pedro", 5, true},
      {"setlist-2", "Sunday Service", 9, false},
      {"setlist-3", "Rock Covers", 18, false},
      {"setlist-4", "Jazz Trio", 14, false}
  };
  loaded_setlist_id_ = "setlist-1";
  loaded_setlist_name_ = "Wedding Ana & Pedro";

  songs_ = {
      {"song-1", "01 Canon in D", 4, false, false},
      {"song-2", "02 A Thousand Years", 3, false, false},
      {"song-3", "03 Perfect", 5, true, false},
      {"song-4", "04 Thinking Out Loud", 2, false, true},
      {"song-5", "05 All of Me", 4, false, false}
  };
  current_live_set_id_ = "song-3";
}

SetlistView::~SetlistView() {
  setlist_rows_.clear();
  song_rows_.clear();
}

bool SetlistView::create(lv_obj_t* parent) {
  if (!skeleton_.create(parent)) return false;
  keyboard_.create(skeleton_.root_container());
  keyboard_.close();

  lv_obj_t* content = skeleton_.content_area();
  const auto& p = Theme::current();
  const auto& r = Theme::radius();
  const auto& sp = Theme::spacing();

  // 1. Empty state container
  container_empty_ = lv_obj_create(content);
  lv_obj_set_size(container_empty_, 1024, 488);
  lv_obj_set_pos(container_empty_, 0, 0);
  lv_obj_set_scrollable(container_empty_, false);
  lv_obj_set_style_bg_opa(container_empty_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_empty_, 0, LV_PART_MAIN);

  lbl_empty_ = lv_label_create(container_empty_);
  lv_label_set_text(lbl_empty_, tr("setlist.empty.no_setlists_yet").c_str());
  lv_obj_add_style(lbl_empty_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_empty_, LV_ALIGN_CENTER, 0, -40);

  btn_empty_new_ = lv_obj_create(container_empty_);
  lv_obj_set_size(btn_empty_new_, 240, sp.touch_target_min);
  lv_obj_align(btn_empty_new_, LV_ALIGN_CENTER, 0, 24);
  lv_obj_set_scrollable(btn_empty_new_, false);
  lv_obj_set_clickable(btn_empty_new_, true);
  lv_obj_set_style_bg_color(btn_empty_new_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_empty_new_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_empty_new_, p.accent, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_empty_new_, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_empty_new_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_empty_new_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_empty_new_, on_empty_new_click, LV_EVENT_CLICKED, this);

  lbl_empty_btn_ = lv_label_create(btn_empty_new_);
  lv_label_set_text(lbl_empty_btn_, tr("setlist.action.new_setlist").c_str());
  lv_obj_set_style_text_color(lbl_empty_btn_, p.accent, LV_PART_MAIN);
  lv_obj_align(lbl_empty_btn_, LV_ALIGN_CENTER, 0, 0);

  // 2. Two-column container
  container_cols_ = lv_obj_create(content);
  lv_obj_set_size(container_cols_, 1024, 488);
  lv_obj_set_pos(container_cols_, 0, 0);
  lv_obj_set_scrollable(container_cols_, false);
  lv_obj_set_style_bg_opa(container_cols_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container_cols_, 0, LV_PART_MAIN);

  // Left column: Setlists (~310px)
  col_left_ = lv_obj_create(container_cols_);
  lv_obj_set_size(col_left_, 310, 472);
  lv_obj_set_pos(col_left_, 16, 8);
  lv_obj_set_scrollable(col_left_, false);
  lv_obj_set_style_bg_opa(col_left_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(col_left_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(col_left_, 0, LV_PART_MAIN);

  lbl_left_tag_ = lv_label_create(col_left_);
  lv_label_set_text(lbl_left_tag_, tr("setlist.header.your_setlists").c_str());
  lv_obj_add_style(lbl_left_tag_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_left_tag_, LV_ALIGN_TOP_LEFT, 0, 0);

  list_setlists_ = lv_obj_create(col_left_);
  lv_obj_set_size(list_setlists_, 310, 380);
  lv_obj_set_pos(list_setlists_, 0, 24);
  lv_obj_set_scrollable(list_setlists_, true);
  lv_obj_set_style_bg_opa(list_setlists_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list_setlists_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list_setlists_, 0, LV_PART_MAIN);

  btn_new_setlist_ = lv_obj_create(col_left_);
  lv_obj_set_size(btn_new_setlist_, 310, sp.touch_target_min);
  lv_obj_set_pos(btn_new_setlist_, 0, 412);
  lv_obj_set_scrollable(btn_new_setlist_, false);
  lv_obj_set_clickable(btn_new_setlist_, true);
  lv_obj_set_style_bg_color(btn_new_setlist_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_new_setlist_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_new_setlist_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_new_setlist_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_new_setlist_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_new_setlist_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_new_setlist_, on_new_setlist_click, LV_EVENT_CLICKED, this);

  lbl_new_setlist_ = lv_label_create(btn_new_setlist_);
  lv_label_set_text(lbl_new_setlist_, tr("setlist.action.new_setlist").c_str());
  lv_obj_add_style(lbl_new_setlist_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_new_setlist_, LV_ALIGN_CENTER, 0, 0);

  // Right column: Songs (~666px)
  col_right_ = lv_obj_create(container_cols_);
  lv_obj_set_size(col_right_, 666, 472);
  lv_obj_set_pos(col_right_, 342, 8);
  lv_obj_set_scrollable(col_right_, false);
  lv_obj_set_style_bg_opa(col_right_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(col_right_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(col_right_, 0, LV_PART_MAIN);

  lbl_right_tag_ = lv_label_create(col_right_);
  lv_label_set_text(lbl_right_tag_, tr("setlist.header.songs_drag_or_hold").c_str());
  lv_obj_add_style(lbl_right_tag_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_right_tag_, LV_ALIGN_TOP_LEFT, 0, 0);

  list_songs_ = lv_obj_create(col_right_);
  lv_obj_set_size(list_songs_, 666, 380);
  lv_obj_set_pos(list_songs_, 0, 24);
  lv_obj_set_scrollable(list_songs_, true);
  lv_obj_set_style_bg_opa(list_songs_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list_songs_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list_songs_, 0, LV_PART_MAIN);

  btn_add_liveset_ = lv_obj_create(col_right_);
  lv_obj_set_size(btn_add_liveset_, 666, sp.touch_target_min);
  lv_obj_set_pos(btn_add_liveset_, 0, 412);
  lv_obj_set_scrollable(btn_add_liveset_, false);
  lv_obj_set_clickable(btn_add_liveset_, true);
  lv_obj_set_style_bg_color(btn_add_liveset_, p.surface, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn_add_liveset_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_add_liveset_, p.line_strong, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_add_liveset_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn_add_liveset_, r.sm, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_add_liveset_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_add_liveset_, on_add_liveset_click, LV_EVENT_CLICKED, this);

  lbl_add_liveset_ = lv_label_create(btn_add_liveset_);
  lv_label_set_text(lbl_add_liveset_, tr("setlist.action.add_live_set").c_str());
  lv_obj_add_style(lbl_add_liveset_, Theme::style_text_muted(), LV_PART_MAIN);
  lv_obj_align(lbl_add_liveset_, LV_ALIGN_CENTER, 0, 0);

  rebuild_ui();
  return true;
}

void SetlistView::rebuild_ui() {
  if (setlists_.empty()) {
    if (container_empty_) lv_obj_set_hidden(container_empty_, false);
    if (container_cols_) lv_obj_set_hidden(container_cols_, true);
  } else {
    if (container_empty_) lv_obj_set_hidden(container_empty_, true);
    if (container_cols_) lv_obj_set_hidden(container_cols_, false);
    rebuild_setlists_list();
    rebuild_songs_list();
  }
  update_top_bar();
  update_hints();
}

void SetlistView::rebuild_setlists_list() {
  setlist_rows_.clear();
  if (!list_setlists_) return;
  lv_obj_clean(list_setlists_);

  const auto& sp = Theme::spacing();
  int32_t y = 0;
  for (size_t i = 0; i < setlists_.size(); ++i) {
    auto row = std::make_unique<ListRow>();
    row->create(list_setlists_, 310, sp.touch_target_min);
    row->set_title(setlists_[i].name);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u", setlists_[i].song_count);
    row->set_meta(buf);
    row->set_selected(focus_col_ == SetlistFocusColumn::Setlists && static_cast<int32_t>(i) == selected_setlist_idx_);

    size_t idx = i;
    row->set_on_select([this, idx](ListRow*) {
      select_setlist(idx);
    });
    row->set_on_confirm([this, idx](ListRow*) {
      select_setlist(idx);
      load_selected_setlist();
    });

    if (row->container()) {
      lv_obj_set_pos(row->container(), 0, y);
      y += sp.touch_target_min + sp.space_2;
    }
    setlist_rows_.push_back(std::move(row));
  }
}

void SetlistView::rebuild_songs_list() {
  song_rows_.clear();
  if (!list_songs_) return;
  lv_obj_clean(list_songs_);

  const auto& sp = Theme::spacing();
  int32_t y = 0;
  for (size_t i = 0; i < songs_.size(); ++i) {
    auto row = std::make_unique<ListRow>();
    row->create(list_songs_, 666, sp.touch_target_min);
    row->set_title(songs_[i].name);

    char meta_buf[64];
    std::snprintf(meta_buf, sizeof(meta_buf), tr("setlist.meta.scenes_count").c_str(), songs_[i].scene_count);
    row->set_meta(meta_buf);

    bool is_now = (songs_[i].id == current_live_set_id_) || songs_[i].now_playing;
    row->set_now_playing(is_now);

    bool has_edits = songs_[i].has_edits ||
        (std::find(dirty_live_set_ids_.begin(), dirty_live_set_ids_.end(), songs_[i].id) != dirty_live_set_ids_.end());
    row->set_has_pending_edits(has_edits);

    row->set_selected(focus_col_ == SetlistFocusColumn::Songs && static_cast<int32_t>(i) == selected_song_idx_);

    size_t idx = i;
    row->set_on_select([this, idx](ListRow*) {
      select_song(idx);
    });
    row->set_on_confirm([this, idx](ListRow*) {
      select_song(idx);
      jump_to_selected_song();
    });

    if (row->container()) {
      lv_obj_set_pos(row->container(), 0, y);
      y += sp.touch_target_min + sp.space_2;
    }
    song_rows_.push_back(std::move(row));
  }
}

void SetlistView::update_top_bar() {
  if (setlists_.empty()) {
    skeleton_.set_title(tr("setlist.title"));
    return;
  }

  std::string title = tr("setlist.title");
  if (selected_setlist_idx_ >= 0 && selected_setlist_idx_ < static_cast<int32_t>(setlists_.size())) {
    title += " · " + setlists_[selected_setlist_idx_].name;
    char buf[64];
    std::snprintf(buf, sizeof(buf), tr("setlist.meta.songs_count").c_str(), static_cast<int>(songs_.size()));
    title += " · ";
    title += buf;
  }
  skeleton_.set_title(title);
}

void SetlistView::update_hints() {
  std::vector<HintItem> hints = {
      {"VALUE", tr("hint.verb.scroll")},
      {"WRITE", (focus_col_ == SetlistFocusColumn::Setlists) ? tr("setlist.hint.load_setlist") : tr("setlist.hint.jump_to_song")},
      {"EXIT", tr("hint.verb.back")}
  };
  skeleton_.set_hints(hints, tr("setlist.hint.reassurance_keeps_playing"));
}

void SetlistView::set_items(const std::vector<SetlistItem>& setlists, const std::vector<SongItem>& songs) {
  setlists_ = setlists;
  songs_ = songs;
  if (selected_setlist_idx_ >= static_cast<int32_t>(setlists_.size())) {
    selected_setlist_idx_ = setlists_.empty() ? 0 : static_cast<int32_t>(setlists_.size() - 1);
  }
  if (selected_song_idx_ >= static_cast<int32_t>(songs_.size())) {
    selected_song_idx_ = songs_.empty() ? 0 : static_cast<int32_t>(songs_.size() - 1);
  }
  rebuild_ui();
}

void SetlistView::clear_setlists() {
  setlists_.clear();
  songs_.clear();
  rebuild_ui();
}

void SetlistView::select_setlist(size_t index) {
  if (index >= setlists_.size()) return;
  selected_setlist_idx_ = static_cast<int32_t>(index);
  focus_col_ = SetlistFocusColumn::Setlists;
  for (size_t i = 0; i < setlist_rows_.size(); ++i) {
    setlist_rows_[i]->set_selected(i == index);
  }
  for (auto& row : song_rows_) {
    row->set_selected(false);
  }
  update_top_bar();
  update_hints();
}

void SetlistView::select_song(size_t index) {
  if (index >= songs_.size()) return;
  selected_song_idx_ = static_cast<int32_t>(index);
  focus_col_ = SetlistFocusColumn::Songs;
  for (size_t i = 0; i < song_rows_.size(); ++i) {
    song_rows_[i]->set_selected(i == index);
  }
  for (auto& row : setlist_rows_) {
    row->set_selected(false);
  }
  update_hints();
}

void SetlistView::toggle_focus_column() {
  if (focus_col_ == SetlistFocusColumn::Setlists) {
    set_focus_column(SetlistFocusColumn::Songs);
  } else {
    set_focus_column(SetlistFocusColumn::Setlists);
  }
}

void SetlistView::set_focus_column(SetlistFocusColumn col) {
  focus_col_ = col;
  if (focus_col_ == SetlistFocusColumn::Setlists) {
    select_setlist(selected_setlist_idx_);
  } else {
    select_song(selected_song_idx_);
  }
}

void SetlistView::load_selected_setlist() {
  if (selected_setlist_idx_ < 0 || selected_setlist_idx_ >= static_cast<int32_t>(setlists_.size())) return;
  const auto& sel = setlists_[selected_setlist_idx_];
  loaded_setlist_id_ = sel.id;
  loaded_setlist_name_ = sel.name;

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(201);
    auto* cmd = msg.mutable_load_setlist();
    cmd->set_id(sel.id);
    client_->send_command(msg);
  }
  update_top_bar();
  update_hints();
}

void SetlistView::jump_to_selected_song() {
  if (selected_song_idx_ < 0 || selected_song_idx_ >= static_cast<int32_t>(songs_.size())) return;
  const auto& sel = songs_[selected_song_idx_];
  current_live_set_id_ = sel.id;

  if (client_) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(202);
    auto* cmd = msg.mutable_jump_to_live_set();
    cmd->set_id(sel.id);
    client_->send_command(msg);
  }

  // Update now-playing indicator on rows
  for (size_t i = 0; i < song_rows_.size(); ++i) {
    song_rows_[i]->set_now_playing(static_cast<int32_t>(i) == selected_song_idx_);
  }
}

void SetlistView::prompt_new_setlist() {
  keyboard_.open("", [this](const std::string& name) {
    if (name.empty()) return;
    if (client_) {
      ws::v1::ClientMessage msg;
      msg.set_request_id(203);
      auto* cmd = msg.mutable_create_setlist();
      cmd->set_name(name);
      client_->send_command(msg);
    }
    // Optimistic / local update
    std::string new_id = "setlist-" + std::to_string(setlists_.size() + 1);
    setlists_.push_back({new_id, name, 0, false});
    select_setlist(setlists_.size() - 1);
    rebuild_ui();
  });
}

void SetlistView::prompt_new_live_set() {
  keyboard_.open("", [this](const std::string& name) {
    if (name.empty()) return;
    std::string new_id = "song-" + std::to_string(songs_.size() + 1);
    if (client_) {
      // 1. Create Live Set
      ws::v1::ClientMessage msg1;
      msg1.set_request_id(204);
      auto* cmd1 = msg1.mutable_create_live_set();
      cmd1->set_name(name);
      client_->send_command(msg1);

      // 2. Add to setlist
      if (selected_setlist_idx_ >= 0 && selected_setlist_idx_ < static_cast<int32_t>(setlists_.size())) {
        ws::v1::ClientMessage msg2;
        msg2.set_request_id(205);
        auto* cmd2 = msg2.mutable_add_live_set_to_setlist();
        cmd2->set_setlist_id(setlists_[selected_setlist_idx_].id);
        cmd2->set_live_set_id(new_id);
        client_->send_command(msg2);
      }
    }
    // Optimistically add and jump to it
    songs_.push_back({new_id, name, 1, true, false});
    current_live_set_id_ = new_id;
    if (selected_setlist_idx_ >= 0 && selected_setlist_idx_ < static_cast<int32_t>(setlists_.size())) {
      setlists_[selected_setlist_idx_].song_count = static_cast<uint32_t>(songs_.size());
    }
    select_song(songs_.size() - 1);
    rebuild_ui();
  });
}

void SetlistView::reorder_song_up() {
  if (selected_song_idx_ <= 0 || songs_.empty()) return;
  std::swap(songs_[selected_song_idx_], songs_[selected_song_idx_ - 1]);
  selected_song_idx_--;
  if (client_ && selected_setlist_idx_ >= 0 && selected_setlist_idx_ < static_cast<int32_t>(setlists_.size())) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(206);
    auto* cmd = msg.mutable_reorder_live_sets_in_setlist();
    cmd->set_setlist_id(setlists_[selected_setlist_idx_].id);
    for (const auto& s : songs_) {
      cmd->add_live_set_ids(s.id);
    }
    client_->send_command(msg);
  }
  rebuild_songs_list();
}

void SetlistView::reorder_song_down() {
  if (selected_song_idx_ < 0 || selected_song_idx_ >= static_cast<int32_t>(songs_.size() - 1)) return;
  std::swap(songs_[selected_song_idx_], songs_[selected_song_idx_ + 1]);
  selected_song_idx_++;
  if (client_ && selected_setlist_idx_ >= 0 && selected_setlist_idx_ < static_cast<int32_t>(setlists_.size())) {
    ws::v1::ClientMessage msg;
    msg.set_request_id(207);
    auto* cmd = msg.mutable_reorder_live_sets_in_setlist();
    cmd->set_setlist_id(setlists_[selected_setlist_idx_].id);
    for (const auto& s : songs_) {
      cmd->add_live_set_ids(s.id);
    }
    client_->send_command(msg);
  }
  rebuild_songs_list();
}

void SetlistView::on_panel_event(const PanelEvent& ev) {
  if (keyboard_.is_open()) {
    keyboard_.on_panel_event(ev);
    return;
  }

  if (ev.type == PanelEventType::Encoder && ev.id == MCU_CTRL_VALUE_ENCODER) {
    // VALUE Turn
    if (focus_col_ == SetlistFocusColumn::Setlists) {
      if (!setlists_.empty()) {
        int32_t next = selected_setlist_idx_ + ev.delta;
        next = std::clamp(next, 0, static_cast<int32_t>(setlists_.size() - 1));
        select_setlist(next);
      }
    } else {
      if (!songs_.empty()) {
        int32_t next = selected_song_idx_ + ev.delta;
        next = std::clamp(next, 0, static_cast<int32_t>(songs_.size() - 1));
        select_song(next);
      }
    }
    return;
  }

  if (ev.type == PanelEventType::Button && ev.pressed) {
    switch (ev.id) {
      case MCU_BTN_VALUE_PUSH:
        toggle_focus_column();
        break;
      case MCU_BTN_WRITE:
        if (setlists_.empty()) {
          prompt_new_setlist();
        } else if (focus_col_ == SetlistFocusColumn::Setlists) {
          load_selected_setlist();
        } else {
          jump_to_selected_song();
        }
        break;
      case MCU_BTN_EXIT:
        if (on_close_) {
          on_close_();
        }
        break;
      case MCU_BTN_SCENE_PREV:
        reorder_song_up();
        break;
      case MCU_BTN_SCENE_NEXT:
        reorder_song_down();
        break;
      default:
        break;
    }
  }
}

void SetlistView::update_from_state(const ws::v1::State& state) {
  if (state.has_setlists()) {
    loaded_setlist_id_ = state.setlists().loaded_setlist_id();
    loaded_setlist_name_ = state.setlists().loaded_setlist_name();
  }
  if (!state.live_set_id().empty()) {
    current_live_set_id_ = state.live_set_id();
  }
  dirty_live_set_ids_.clear();
  if (state.has_edits()) {
    for (int i = 0; i < state.edits().dirty_live_set_ids_size(); ++i) {
      dirty_live_set_ids_.push_back(state.edits().dirty_live_set_ids(i));
    }
  }
  rebuild_ui();
}

void SetlistView::update_from_setlists_reply(const ws::v1::SetlistsReply& reply) {
  std::vector<SetlistItem> new_setlists;
  for (int i = 0; i < reply.setlists_size(); ++i) {
    const auto& s = reply.setlists(i);
    new_setlists.push_back({s.id(), s.name(), s.live_set_count(), s.loaded()});
  }
  std::vector<SongItem> new_songs;
  for (int i = 0; i < reply.entries_size(); ++i) {
    const auto& e = reply.entries(i);
    new_songs.push_back({e.live_set_id(), e.name(), 1, (e.live_set_id() == current_live_set_id_), false});
  }
  set_items(new_setlists, new_songs);
}

void SetlistView::on_empty_new_click(lv_event_t* e) {
  auto* self = static_cast<SetlistView*>(lv_event_get_user_data(e));
  if (self) self->prompt_new_setlist();
}

void SetlistView::on_new_setlist_click(lv_event_t* e) {
  auto* self = static_cast<SetlistView*>(lv_event_get_user_data(e));
  if (self) self->prompt_new_setlist();
}

void SetlistView::on_add_liveset_click(lv_event_t* e) {
  auto* self = static_cast<SetlistView*>(lv_event_get_user_data(e));
  if (self) self->prompt_new_live_set();
}

}  // namespace velora::ui

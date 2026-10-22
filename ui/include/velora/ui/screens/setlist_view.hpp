#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/widgets/list_row.hpp"
#include "velora/ui/components/on_screen_keyboard.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

struct SetlistItem {
  std::string id;
  std::string name;
  uint32_t song_count{0};
  bool loaded{false};
};

struct SongItem {
  std::string id;
  std::string name;
  uint32_t scene_count{1};
  bool now_playing{false};
  bool has_edits{false};
};

enum class SetlistFocusColumn {
  Setlists = 0,
  Songs
};

class SetlistView {
 public:
  using CloseCallback = std::function<void()>;

  explicit SetlistView(WsClient* client = nullptr);
  ~SetlistView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);
  void update_from_setlists_reply(const ws::v1::SetlistsReply& reply);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }

  // Setlist / Song data manipulation
  void set_items(const std::vector<SetlistItem>& setlists, const std::vector<SongItem>& songs);
  void clear_setlists();

  // Selection & navigation
  void select_setlist(size_t index);
  void select_song(size_t index);
  void toggle_focus_column();
  void set_focus_column(SetlistFocusColumn col);

  // Actions
  void load_selected_setlist();
  void jump_to_selected_song();
  void prompt_new_setlist();
  void prompt_new_live_set();
  void reorder_song_up();
  void reorder_song_down();

  // Accessors
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }
  OnScreenKeyboard& keyboard() noexcept { return keyboard_; }

  size_t setlist_count() const noexcept { return setlists_.size(); }
  size_t song_count() const noexcept { return songs_.size(); }
  int32_t selected_setlist_index() const noexcept { return selected_setlist_idx_; }
  int32_t selected_song_index() const noexcept { return selected_song_idx_; }
  SetlistFocusColumn focus_column() const noexcept { return focus_col_; }
  bool is_empty_state() const noexcept { return setlists_.empty(); }

  const std::string& loaded_setlist_id() const noexcept { return loaded_setlist_id_; }
  const std::string& now_playing_live_set_id() const noexcept { return current_live_set_id_; }

  const std::vector<SetlistItem>& setlists() const noexcept { return setlists_; }
  const std::vector<SongItem>& songs() const noexcept { return songs_; }

  lv_obj_t* empty_container() const noexcept { return container_empty_; }
  lv_obj_t* empty_button() const noexcept { return btn_empty_new_; }

 private:
  void rebuild_ui();
  void rebuild_setlists_list();
  void rebuild_songs_list();
  void update_top_bar();
  void update_hints();

  static void on_empty_new_click(lv_event_t* e);
  static void on_new_setlist_click(lv_event_t* e);
  static void on_add_liveset_click(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  OnScreenKeyboard keyboard_;
  CloseCallback on_close_;

  SetlistFocusColumn focus_col_{SetlistFocusColumn::Setlists};
  int32_t selected_setlist_idx_{0};
  int32_t selected_song_idx_{0};

  std::vector<SetlistItem> setlists_;
  std::vector<SongItem> songs_;

  std::string loaded_setlist_id_;
  std::string loaded_setlist_name_;
  std::string current_live_set_id_;
  std::vector<std::string> dirty_live_set_ids_;

  // LVGL UI elements
  lv_obj_t* container_empty_{nullptr};
  lv_obj_t* lbl_empty_{nullptr};
  lv_obj_t* btn_empty_new_{nullptr};
  lv_obj_t* lbl_empty_btn_{nullptr};

  lv_obj_t* container_cols_{nullptr};
  lv_obj_t* col_left_{nullptr};
  lv_obj_t* col_right_{nullptr};

  lv_obj_t* lbl_left_tag_{nullptr};
  lv_obj_t* list_setlists_{nullptr};
  lv_obj_t* btn_new_setlist_{nullptr};
  lv_obj_t* lbl_new_setlist_{nullptr};

  lv_obj_t* lbl_right_tag_{nullptr};
  lv_obj_t* list_songs_{nullptr};
  lv_obj_t* btn_add_liveset_{nullptr};
  lv_obj_t* lbl_add_liveset_{nullptr};

  std::vector<std::unique_ptr<ListRow>> setlist_rows_;
  std::vector<std::unique_ptr<ListRow>> song_rows_;
};

}  // namespace velora::ui

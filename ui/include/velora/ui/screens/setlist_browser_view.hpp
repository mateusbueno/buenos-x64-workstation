#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ws_v1.pb.h"
#include "velora/ui/screens/surface_skeleton.hpp"
#include "velora/ui/components/on_screen_keyboard.hpp"
#include "velora/ui/components/action_sheet.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class WsClient;

enum class BrowserItemType {
  LiveSet = 0,
  Setlist
};

struct BrowserItem {
  std::string id;
  std::string name;
  BrowserItemType type{BrowserItemType::LiveSet};
  std::string meta;
  bool is_favorite{false};
  bool has_edits{false};
  bool is_recent{false};
  std::vector<std::string> tags;
};

class SetlistBrowserView {
 public:
  using CloseCallback = std::function<void()>;

  explicit SetlistBrowserView(WsClient* client = nullptr);
  ~SetlistBrowserView();

  bool create(lv_obj_t* parent = nullptr);

  void on_panel_event(const PanelEvent& ev);
  void update_from_state(const ws::v1::State& state);
  void update_from_setlists_reply(const ws::v1::SetlistsReply& reply);

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }

  // Search & filter
  void set_search_query(const std::string& query);
  const std::string& search_query() const noexcept { return search_query_; }

  void select_chip(size_t chip_index);
  size_t selected_chip_index() const noexcept { return selected_chip_idx_; }

  void set_items(const std::vector<BrowserItem>& items);
  void set_user_tags(const std::vector<std::string>& tags);

  // Selection & actions
  void select_result(size_t index);
  int32_t selected_result_index() const noexcept { return selected_result_idx_; }
  size_t filtered_count() const noexcept { return filtered_indices_.size(); }

  void activate_selected();
  void open_action_sheet_for_selected();
  void toggle_favorite_on_selected();

  // Accessors
  SurfaceSkeleton& skeleton() noexcept { return skeleton_; }
  const SurfaceSkeleton& skeleton() const noexcept { return skeleton_; }
  OnScreenKeyboard& keyboard() noexcept { return keyboard_; }
  ActionSheet& action_sheet() noexcept { return action_sheet_; }

  bool is_empty_state() const noexcept { return filtered_indices_.empty(); }
  const std::string& empty_message() const noexcept { return current_empty_message_; }

  const std::string& last_activated_id() const noexcept { return last_activated_id_; }
  BrowserItemType last_activated_type() const noexcept { return last_activated_type_; }

 private:
  void filter_items();
  void rebuild_chips();
  void rebuild_grid();
  void update_top_bar();
  void update_hints();

  static void on_search_click(lv_event_t* e);
  static void on_chip_click(lv_event_t* e);
  static void on_tile_click(lv_event_t* e);
  static void on_tile_long_press(lv_event_t* e);

  WsClient* client_{nullptr};
  SurfaceSkeleton skeleton_;
  OnScreenKeyboard keyboard_;
  ActionSheet action_sheet_;
  CloseCallback on_close_;

  std::vector<BrowserItem> all_items_;
  std::vector<size_t> filtered_indices_;
  std::vector<std::string> user_tags_;
  std::vector<std::string> chip_names_;

  std::string search_query_;
  size_t selected_chip_idx_{3}; // Default to "All Live Sets"
  int32_t selected_result_idx_{0};

  std::string current_empty_message_;
  std::string last_activated_id_;
  BrowserItemType last_activated_type_{BrowserItemType::LiveSet};

  std::string loaded_setlist_id_;
  std::vector<std::string> dirty_live_set_ids_;

  // UI objects
  lv_obj_t* container_search_{nullptr};
  lv_obj_t* lbl_search_icon_{nullptr};
  lv_obj_t* lbl_search_text_{nullptr};
  lv_obj_t* caret_search_{nullptr};
  lv_obj_t* lbl_search_hint_{nullptr};

  lv_obj_t* container_chips_{nullptr};
  std::vector<lv_obj_t*> chip_objs_;

  lv_obj_t* container_grid_{nullptr};
  std::vector<lv_obj_t*> tile_objs_;

  lv_obj_t* lbl_empty_msg_{nullptr};
};

}  // namespace velora::ui

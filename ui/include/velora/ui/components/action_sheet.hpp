#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "velora/ui/panel_event.hpp"

namespace velora::ui {

struct ActionSheetOption {
  std::string key_label;  // e.g. "WRITE", "EXIT"
  std::string text;       // e.g. "★ Add to Favorites", "Cancel"
  std::function<void()> on_select;
  bool is_cancel{false};
};

class ActionSheet {
 public:
  using CloseCallback = std::function<void()>;

  ActionSheet();
  ~ActionSheet();

  bool create(lv_obj_t* parent = nullptr);

  void open(const std::string& eyebrow,
            const std::string& title,
            const std::vector<ActionSheetOption>& options,
            const std::string& body = "");
  void close();
  bool is_open() const noexcept { return is_open_; }

  void on_panel_event(const PanelEvent& ev);

  void set_highlight(int32_t index);
  int32_t highlight() const noexcept { return highlight_idx_; }
  void confirm_highlighted();

  void set_on_close(CloseCallback cb) { on_close_ = std::move(cb); }

  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* card_container() const noexcept { return card_; }
  size_t action_count() const noexcept { return options_.size(); }
  const std::string& title() const noexcept { return title_; }

 private:
  void rebuild_actions();
  void update_row_highlights();

  static void on_action_click(lv_event_t* e);

  lv_obj_t* root_{nullptr};
  lv_obj_t* card_{nullptr};
  lv_obj_t* lbl_eyebrow_{nullptr};
  lv_obj_t* lbl_title_{nullptr};
  lv_obj_t* lbl_body_{nullptr};
  lv_obj_t* container_actions_{nullptr};
  lv_obj_t* lbl_footer_{nullptr};

  std::string eyebrow_;
  std::string title_;
  std::string body_;
  std::vector<ActionSheetOption> options_;
  int32_t highlight_idx_{0};
  bool is_open_{false};

  std::vector<lv_obj_t*> row_objs_;
  std::vector<lv_obj_t*> marker_objs_;
  CloseCallback on_close_;
};

}  // namespace velora::ui

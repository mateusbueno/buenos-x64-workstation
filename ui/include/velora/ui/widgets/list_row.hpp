#pragma once

#include <lvgl.h>
#include <string>
#include <functional>

namespace velora::ui {

class ListRow {
 public:
  using SelectCallback = std::function<void(ListRow*)>;
  using ConfirmCallback = std::function<void(ListRow*)>;

  ListRow() = default;
  ~ListRow();

  bool create(lv_obj_t* parent, int32_t width = 980, int32_t height = 56);

  void set_title(const std::string& title);
  void set_meta(const std::string& meta);
  void set_now_playing(bool now_playing);
  void set_has_pending_edits(bool edits);

  void set_selected(bool selected);
  bool is_selected() const noexcept { return is_selected_; }

  void set_on_select(SelectCallback cb) { on_select_ = std::move(cb); }
  void set_on_confirm(ConfirmCallback cb) { on_confirm_ = std::move(cb); }

  void on_write();

  lv_obj_t* container() const noexcept { return container_; }
  const std::string& title() const noexcept { return title_; }
  const std::string& meta() const noexcept { return meta_; }
  bool now_playing() const noexcept { return now_playing_; }
  bool has_pending_edits() const noexcept { return pending_edits_; }

 private:
  static void event_cb(lv_event_t* e);
  void update_styles();

  lv_obj_t* container_{nullptr};
  lv_obj_t* lbl_title_{nullptr};
  lv_obj_t* lbl_now_{nullptr};
  lv_obj_t* lbl_warn_{nullptr};
  lv_obj_t* lbl_meta_{nullptr};

  std::string title_;
  std::string meta_;
  bool is_selected_{false};
  bool now_playing_{false};
  bool pending_edits_{false};

  SelectCallback on_select_;
  ConfirmCallback on_confirm_;
};

}  // namespace velora::ui

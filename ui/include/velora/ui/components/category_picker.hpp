#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

#include "velora/format/category.hpp"
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class CategoryPicker {
 public:
  using SelectCallback = std::function<void(format::Category category, bool apply_to_all)>;
  using CancelCallback = std::function<void()>;

  CategoryPicker();
  ~CategoryPicker();

  bool create(lv_obj_t* parent = nullptr);

  void set_visible(bool visible);
  bool is_visible() const noexcept { return is_visible_; }

  void set_target_subtitle(const std::string& subtitle);
  const std::string& target_subtitle() const noexcept { return subtitle_; }

  void set_selected_category(format::Category cat);
  format::Category selected_category() const noexcept { return selected_cat_; }

  void set_apply_to_all(bool apply_all);
  bool apply_to_all() const noexcept { return apply_to_all_; }

  void set_show_apply_all(bool show);
  bool show_apply_all() const noexcept { return show_apply_all_; }

  void set_on_select(SelectCallback cb) { on_select_ = std::move(cb); }
  void set_on_cancel(CancelCallback cb) { on_cancel_ = std::move(cb); }

  void on_panel_event(const PanelEvent& ev);

  lv_obj_t* container() const noexcept { return modal_root_; }
  lv_obj_t* apply_row() const noexcept { return apply_row_obj_; }
  const std::vector<lv_obj_t*>& category_tiles() const noexcept { return tile_objs_; }

 private:
  void update_selection_ui();
  void update_apply_ui();
  void confirm_selection();
  void cancel();

  static void on_tile_clicked(lv_event_t* e);
  static void on_apply_row_clicked(lv_event_t* e);
  static void on_scrim_clicked(lv_event_t* e);

  SelectCallback on_select_;
  CancelCallback on_cancel_;

  lv_obj_t* modal_root_{nullptr};
  lv_obj_t* panel_card_{nullptr};
  lv_obj_t* lbl_title_{nullptr};
  lv_obj_t* lbl_subtitle_{nullptr};
  lv_obj_t* grid_container_{nullptr};
  lv_obj_t* apply_row_obj_{nullptr};
  lv_obj_t* apply_check_lbl_{nullptr};
  lv_obj_t* apply_text_lbl_{nullptr};
  lv_obj_t* apply_state_lbl_{nullptr};

  std::vector<lv_obj_t*> tile_objs_;
  std::vector<lv_obj_t*> tile_label_objs_;

  format::Category selected_cat_{format::Category::kPiano};
  int32_t focus_idx_{0}; // 0..16 for categories, 17 for apply row (if enabled)
  bool apply_to_all_{false};
  bool show_apply_all_{true};
  bool is_visible_{false};
  std::string subtitle_;
};

}  // namespace velora::ui

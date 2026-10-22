#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

namespace velora::ui {

class SegmentedControl {
 public:
  using ChangeCallback = std::function<void(int32_t index)>;
  using ConfirmCallback = std::function<void(int32_t index)>;

  SegmentedControl() = default;
  ~SegmentedControl();

  bool create(lv_obj_t* parent, const std::vector<std::string>& segments, int32_t min_segment_width = 80);

  void set_selected_index(int32_t index);
  int32_t selected_index() const noexcept { return selected_index_; }

  void set_on_change(ChangeCallback cb) { on_change_ = std::move(cb); }
  void set_on_confirm(ConfirmCallback cb) { on_confirm_ = std::move(cb); }

  void on_value_turn(int32_t delta);
  void on_write();

  lv_obj_t* container() const noexcept { return container_; }
  size_t segment_count() const noexcept { return segments_.size(); }
  lv_obj_t* segment_obj(size_t index) const {
    return (index < segment_objs_.size()) ? segment_objs_[index] : nullptr;
  }

 private:
  static void segment_click_cb(lv_event_t* e);
  void update_segment_styles();

  lv_obj_t* container_{nullptr};
  std::vector<std::string> segments_;
  std::vector<lv_obj_t*> segment_objs_;
  std::vector<lv_obj_t*> label_objs_;
  int32_t selected_index_{0};

  ChangeCallback on_change_;
  ConfirmCallback on_confirm_;
};

}  // namespace velora::ui

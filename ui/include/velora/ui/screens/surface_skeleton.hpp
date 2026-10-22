#pragma once

#include <lvgl.h>
#include <string>
#include "velora/ui/widgets/hint_bar.hpp"

namespace velora::ui {

class SurfaceSkeleton {
 public:
  SurfaceSkeleton() = default;
  ~SurfaceSkeleton();

  bool create(lv_obj_t* parent = nullptr);

  void set_title(const std::string& title);
  void set_hints(const std::vector<HintItem>& hints, const std::string& right_text = "");

  lv_obj_t* root_container() const noexcept { return root_; }
  lv_obj_t* top_bar() const noexcept { return top_bar_; }
  lv_obj_t* title_label() const noexcept { return lbl_title_; }
  lv_obj_t* content_area() const noexcept { return content_area_; }
  HintBar& hint_bar() noexcept { return hint_bar_; }
  const HintBar& hint_bar() const noexcept { return hint_bar_; }

 private:
  lv_obj_t* root_{nullptr};
  lv_obj_t* top_bar_{nullptr};
  lv_obj_t* lbl_title_{nullptr};
  lv_obj_t* content_area_{nullptr};
  HintBar hint_bar_;
};

}  // namespace velora::ui

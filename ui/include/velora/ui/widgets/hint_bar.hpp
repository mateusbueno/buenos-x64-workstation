#pragma once

#include <lvgl.h>
#include <string>
#include <vector>

namespace velora::ui {

struct HintItem {
  std::string control;  // English physical control name, e.g. "VALUE", "WRITE", "EXIT"
  std::string verb;     // Localized verb, e.g. "scroll", "select", "back"
};

class HintBar {
 public:
  HintBar() = default;
  ~HintBar();

  bool create(lv_obj_t* parent);
  void set_hints(const std::vector<HintItem>& hints, const std::string& right_text = "");
  void set_right_text(const std::string& text);

  lv_obj_t* container() const noexcept { return container_; }
  const std::vector<HintItem>& hints() const noexcept { return current_hints_; }
  const std::string& right_text() const noexcept { return current_right_text_; }

 private:
  void rebuild_hints();

  lv_obj_t* container_{nullptr};
  lv_obj_t* left_box_{nullptr};
  lv_obj_t* lbl_right_{nullptr};

  std::vector<HintItem> current_hints_;
  std::string current_right_text_;
};

}  // namespace velora::ui

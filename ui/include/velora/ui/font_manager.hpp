#pragma once

#include <lvgl.h>

namespace velora::ui {

class FontManager {
 public:
  static FontManager& instance() {
    static FontManager s_instance;
    return s_instance;
  }

  bool initialize();

  const lv_font_t* font_14() const noexcept { return font_14_; }
  const lv_font_t* font_18() const noexcept { return font_18_; }
  const lv_font_t* font_24() const noexcept { return font_24_; }

  void apply_to_screen(lv_obj_t* screen);

 private:
  FontManager() = default;
  ~FontManager();

  const lv_font_t* font_14_{&lv_font_montserrat_14};
  const lv_font_t* font_18_{&lv_font_montserrat_18};
  const lv_font_t* font_24_{&lv_font_montserrat_24};
  bool initialized_{false};
};

}  // namespace velora::ui

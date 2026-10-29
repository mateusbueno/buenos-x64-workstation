#include "velora/ui/font_manager.hpp"
#include "velora/ui/fonts/lato_data.hpp"

#include <iostream>

namespace velora::ui {

FontManager::~FontManager() {
#if LV_USE_TINY_TTF
  if (initialized_) {
    if (font_14_ && font_14_ != &lv_font_montserrat_14) {
      lv_tiny_ttf_destroy(const_cast<lv_font_t*>(font_14_));
    }
    if (font_18_ && font_18_ != &lv_font_montserrat_18) {
      lv_tiny_ttf_destroy(const_cast<lv_font_t*>(font_18_));
    }
    if (font_24_ && font_24_ != &lv_font_montserrat_24) {
      lv_tiny_ttf_destroy(const_cast<lv_font_t*>(font_24_));
    }
  }
#endif
}

bool FontManager::initialize() {
  if (initialized_) return true;

#if LV_USE_TINY_TTF
  lv_font_t* f14 = lv_tiny_ttf_create_data(kLatoRegularData, kLatoRegularSize, 14);
  lv_font_t* f18 = lv_tiny_ttf_create_data(kLatoRegularData, kLatoRegularSize, 18);
  lv_font_t* f24 = lv_tiny_ttf_create_data(kLatoRegularData, kLatoRegularSize, 24);

  if (f14) font_14_ = f14;
  if (f18) font_18_ = f18;
  if (f24) font_24_ = f24;

  initialized_ = true;

  if (lv_is_initialized() && lv_screen_active()) {
    apply_to_screen(lv_screen_active());
  }

  return true;
#else
  font_14_ = &lv_font_montserrat_14;
  font_18_ = &lv_font_montserrat_18;
  font_24_ = &lv_font_montserrat_24;
  initialized_ = true;
  return true;
#endif
}

void FontManager::apply_to_screen(lv_obj_t* screen) {
  if (!screen) return;
  lv_obj_set_style_text_font(screen, font_14_, LV_PART_MAIN);
}

}  // namespace velora::ui

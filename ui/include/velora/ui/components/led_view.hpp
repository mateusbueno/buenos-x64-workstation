#pragma once

// Story 3.12 (UX-DR50, simulator): an on-screen view of the panel LEDs the ui drives (the 8 part/scene LEDs, then the 2 + 2 + 3 toggle-state LEDs), so the Mac
// simulator shows what the physical panel LEDs would show. Pure display: dots on the top layer, no input, no text. Created on demand (simulator only).

#include <lvgl.h>

#include <array>
#include <cstdint>

#include "velora/ui/led_driver.hpp"

namespace velora::ui {

class LedView {
 public:
  bool create(lv_obj_t* parent = nullptr);
  // Shows the driver's current outputs: brightness 0 = dark dot, 255 = fully lit.
  void update(const LedDriver& driver);
  void set_visible(bool visible);
  bool created() const noexcept { return container_ != nullptr; }
  // The shown brightness of an LED's dot (0 for an LED the view does not draw).
  uint8_t shown_brightness(uint8_t led_id) const noexcept { return shown_[led_id]; }
  lv_obj_t* dot(uint8_t led_id) const noexcept { return dots_[led_id]; }

 private:
  lv_obj_t* container_{nullptr};
  std::array<lv_obj_t*, 256> dots_{};
  std::array<uint8_t, 256> shown_{};
};

}  // namespace velora::ui

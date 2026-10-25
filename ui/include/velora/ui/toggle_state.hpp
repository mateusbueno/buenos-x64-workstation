#pragma once

// Story 3.12 (FR111, UX-DR50/51): the ONE owner of the three panel toggles' state. The SoC owns the state; the panel MCU only reports presses.
//   DRAWBARS/MIXER   2 states (0 DRAWBARS, 1 MIXER)       LEDs TOGGLE_LED_DRAWBARS, TOGGLE_LED_MIXER
//   PARTS/SCENES     2 states (0 PARTS, 1 SCENES)         LEDs TOGGLE_LED_PARTS, TOGGLE_LED_SCENES
//   PART/ALL/ASSIGN  3 states (0 PART, 1 ALL, 2 ASSIGN)   LEDs TOGGLE_LED_PART, TOGGLE_LED_ALL, TOGGLE_LED_ASSIGN; presses cycle PART -> ALL -> ASSIGN -> PART
// Exactly one LED of a toggle is lit: the active state's. Every change is published from here (the listener), the Top Bar mirror and the mode
// behaviours (PerformanceView) only read it, and the LED driver turns it into LED commands.

#include <cstdint>
#include <functional>

#include "mcu/mcu_ids.h"

namespace velora::ui {

class ToggleState {
 public:
  static constexpr int32_t kKnobStates = 3;  // PART, ALL, ASSIGN

  bool drawbars() const noexcept { return drawbars_; }  // false = MIXER
  bool scenes() const noexcept { return scenes_; }      // false = PARTS
  int32_t knob_mode() const noexcept { return knob_; }  // 0 PART, 1 ALL, 2 ASSIGN

  void set_drawbars(bool v) { set(drawbars_, v); }
  void set_scenes(bool v) { set(scenes_, v); }
  void set_knob_mode(int32_t m) { set(knob_, (m < 0 || m >= kKnobStates) ? 0 : m); }
  void toggle_drawbars() { set(drawbars_, !drawbars_); }
  void toggle_scenes() { set(scenes_, !scenes_); }
  void cycle_knob_mode() { set(knob_, (knob_ + 1) % kKnobStates); }
  void set_all(bool drawbars, bool scenes, int32_t knob_mode) {
    const bool ch = drawbars != drawbars_ || scenes != scenes_ || knob_mode != knob_;
    drawbars_ = drawbars;
    scenes_ = scenes;
    knob_ = (knob_mode < 0 || knob_mode >= kKnobStates) ? 0 : knob_mode;
    if (ch) publish();
  }

  // True when the toggle-state LED `led_id` (MCU_LED_TOGGLE_LED_*) is the active state's. False for any other id.
  bool led_lit(uint8_t led_id) const noexcept {
    switch (led_id) {
      case MCU_LED_TOGGLE_LED_DRAWBARS: return drawbars_;
      case MCU_LED_TOGGLE_LED_MIXER: return !drawbars_;
      case MCU_LED_TOGGLE_LED_PARTS: return !scenes_;
      case MCU_LED_TOGGLE_LED_SCENES: return scenes_;
      case MCU_LED_TOGGLE_LED_PART: return knob_ == 0;
      case MCU_LED_TOGGLE_LED_ALL: return knob_ == 1;
      case MCU_LED_TOGGLE_LED_ASSIGN: return knob_ == 2;
      default: return false;
    }
  }

  // The single publication point: called after every actual change (not for a set to the same value).
  void set_listener(std::function<void()> cb) { listener_ = std::move(cb); }

 private:
  template <typename T>
  void set(T& field, T v) {
    if (field == v) return;
    field = v;
    publish();
  }
  void publish() {
    if (listener_) listener_();
  }

  bool drawbars_{false};
  bool scenes_{false};
  int32_t knob_{0};
  std::function<void()> listener_;
};

}  // namespace velora::ui

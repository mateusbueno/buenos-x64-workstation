#pragma once

#include <cstdint>
#include <functional>
#include "mcu/mcu_ids.h"

namespace velora::ui {

enum class PanelEventType {
  Button,
  Encoder,
  Fader
};

struct PanelEvent {
  PanelEventType type{PanelEventType::Button};
  uint16_t id{0};          // MCU_BTN_* or MCU_CTRL_*
  bool pressed{true};      // For buttons: true = pressed, false = released
  int32_t delta{0};        // For encoders / faders: relative steps or absolute raw value (0..4095)
  uint64_t timestamp_us{0};

  static PanelEvent button_press(uint16_t btn_id, uint64_t ts = 0) {
    return {PanelEventType::Button, btn_id, true, 0, ts};
  }

  static PanelEvent button_release(uint16_t btn_id, uint64_t ts = 0) {
    return {PanelEventType::Button, btn_id, false, 0, ts};
  }

  static PanelEvent encoder_turn(uint16_t ctrl_id, int32_t delta, uint64_t ts = 0) {
    return {PanelEventType::Encoder, ctrl_id, false, delta, ts};
  }

  static PanelEvent fader_move(uint16_t ctrl_id, int32_t raw_val, uint64_t ts = 0) {
    return {PanelEventType::Fader, ctrl_id, false, raw_val, ts};
  }
};

using PanelEventListener = std::function<void(const PanelEvent&)>;

}  // namespace velora::ui

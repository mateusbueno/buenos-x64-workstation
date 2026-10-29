/* Plain C11 smoke test for mcu/panel inputs (Story 3.8). */
#include "mcu/panel_inputs.h"

#include <string.h>

typedef struct frame_capture {
  mcu_frame last;
  uint32_t count;
} frame_capture;

static void capture_tx(void* user, const mcu_frame* frame) {
  frame_capture* c = (frame_capture*)user;
  c->last = *frame;
  c->count += 1u;
}

int main(void) {
  mcu_panel_inputs panel;
  mcu_panel_inputs_config cfg;
  frame_capture cap;
  mcu_hello h;

  memset(&cap, 0, sizeof(cap));
  cfg.fw_version = 0x0104u;
  cfg.adc_bits = 12u;
  cfg.steps_per_detent = 4u;
  cfg.deadband_12bit = 8u;
  cfg.report_interval_us = 1000u;
  cfg.debounce_us = 5000u;

  if (mcu_panel_inputs_init(&panel, &cfg, capture_tx, &cap) != MCU_OK) return 1;
  if (mcu_panel_inputs_boot(&panel, 100u, 3800u, 1u, 100u) != MCU_OK) return 2;
  if (cap.count != 1u || mcu_unpack_hello(&cap.last, &h) != MCU_OK) return 3;
  if (h.board_id != MCU_BOARD_PANEL) return 4;
  if (h.keybed_contacts != (MCU_PANEL_PEDAL_EXPRESSION_INVERTED | MCU_PANEL_PEDAL_SWITCH_INVERTED)) {
    return 5;
  }

  return 0;
}

/* Plain C11 smoke test for mcu/panel LEDs (Story 3.9). */
#include "mcu/panel_leds.h"

int main(void) {
  mcu_panel_leds leds;
  mcu_frame f;
  mcu_led_set cmd;
  const mcu_panel_led_entry* e;

  if (mcu_panel_leds_init(&leds, 24000u, MCU_PANEL_BRIGHTNESS_HIGH, NULL, NULL) != MCU_OK) {
    return 1;
  }

  /* Boot state: all LEDs off. */
  e = mcu_panel_leds_get(&leds, MCU_LED_PART_SCENE_LED_1);
  if (!e || e->pwm_duty != 0u) return 2;

  /* Command strong brightness (255) on PART_SCENE_LED_1. */
  mcu_frame_init(&f, MCU_TYPE_LED_SET, 1u, 100u);
  cmd.id = MCU_LED_PART_SCENE_LED_1;
  cmd.brightness = MCU_LED_BRIGHTNESS_STRONG;
  cmd.arg = 0u;
  if (mcu_pack_led_set(&f, &cmd) != MCU_OK) return 3;
  if (mcu_panel_leds_on_frame(&leds, &f) != MCU_OK) return 4;
  if (e->pwm_duty != MCU_PANEL_PWM_MAX_DUTY) return 5;

  return 0;
}

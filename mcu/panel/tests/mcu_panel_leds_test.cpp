// Story 3.9: Panel MCU firmware — SoC-driven LEDs unit tests:
// - Boot state: all 23 LEDs (toggle state LEDs, part/scene LEDs, encoder rings) strictly off (0 PWM) until commanded
// - Strong (255), weak (~15% PWM = 38) and off (0) levels across Low, Mid, High global brightness with PWM >= 20 kHz
// - Encoder ring value arcs: lit segments show the arc, unlit segments are strictly 0 (never weak-lit)
// - 50 klx contrast evaluation against >= 10:1 target
#include "mcu/panel_leds.h"

#include <cassert>
#include <cstdint>

namespace {

void test_boot_all_off_and_pwm_frequency_gate() {
  mcu_panel_leds leds{};
  // Below 20 kHz PWM is rejected (flicker guard)
  assert(mcu_panel_leds_init(&leds, 19999u, MCU_PANEL_BRIGHTNESS_HIGH, nullptr, nullptr) == MCU_ERR_BAD_VALUE);
  assert(mcu_panel_leds_init(&leds, 20000u, MCU_PANEL_BRIGHTNESS_HIGH, nullptr, nullptr) == MCU_OK);

  // Every single LED and encoder ring segment is 0 at boot
  for (unsigned i = 0; i < MCU_LED_COUNT; ++i) {
    const auto& e = leds.entries[i];
    assert(e.brightness == 0u);
    assert(e.pwm_duty == 0u);
    for (unsigned s = 0; s < MCU_PANEL_RING_SEGMENTS; ++s) {
      assert(e.ring_segment_duty[s] == 0u);
    }
  }
}

void test_strong_weak_off_and_global_brightness() {
  mcu_panel_leds leds{};
  assert(mcu_panel_leds_init(&leds, 25000u, MCU_PANEL_BRIGHTNESS_HIGH, nullptr, nullptr) == MCU_OK);

  auto send_led = [&](std::uint8_t id, std::uint8_t br, std::uint8_t arg) {
    mcu_frame f{};
    mcu_frame_init(&f, MCU_TYPE_LED_SET, 1, 100);
    mcu_led_set cmd{id, br, arg};
    assert(mcu_pack_led_set(&f, &cmd) == MCU_OK);
    assert(mcu_panel_leds_on_frame(&leds, &f) == MCU_OK);
  };

  // Toggle LEDs & Part/Scene LEDs at High global brightness
  send_led(MCU_LED_TOGGLE_LED_DRAWBARS, MCU_LED_BRIGHTNESS_STRONG, 0);
  send_led(MCU_LED_PART_SCENE_LED_2, MCU_LED_BRIGHTNESS_WEAK, 0);

  const auto* strong_e = mcu_panel_leds_get(&leds, MCU_LED_TOGGLE_LED_DRAWBARS);
  const auto* weak_e = mcu_panel_leds_get(&leds, MCU_LED_PART_SCENE_LED_2);
  assert(strong_e && weak_e);
  assert(strong_e->pwm_duty == 65535u);
  // Weak (~15% of 65535 = ~9766)
  assert(weak_e->pwm_duty > 9500u && weak_e->pwm_duty < 10000u);

  // Switch global brightness to Mid (68%) and Low (35%)
  assert(mcu_panel_leds_set_global_brightness(&leds, MCU_PANEL_BRIGHTNESS_MID) == MCU_OK);
  const std::uint16_t mid_strong = strong_e->pwm_duty;
  const std::uint16_t mid_weak = weak_e->pwm_duty;
  assert(mid_strong < 65535u && mid_strong > 40000u);
  assert(mid_weak < 9500u && mid_weak > 6000u);

  assert(mcu_panel_leds_set_global_brightness(&leds, MCU_PANEL_BRIGHTNESS_LOW) == MCU_OK);
  assert(strong_e->pwm_duty < mid_strong && strong_e->pwm_duty > 20000u);
  assert(weak_e->pwm_duty < mid_weak && weak_e->pwm_duty > 3000u);

  // Command off (0)
  send_led(MCU_LED_TOGGLE_LED_DRAWBARS, MCU_LED_BRIGHTNESS_OFF, 0);
  assert(strong_e->pwm_duty == 0u);
}

void test_encoder_ring_value_arc_unlit_fully_off() {
  // Story 3.9 AC3: Given an encoder ring, when the SoC sends a value arc,
  // the lit segments show the arc and the rest are fully off, never weak-lit.
  mcu_panel_leds leds{};
  assert(mcu_panel_leds_init(&leds, 24000u, MCU_PANEL_BRIGHTNESS_HIGH, nullptr, nullptr) == MCU_OK);

  mcu_frame f{};
  mcu_frame_init(&f, MCU_TYPE_LED_SET, 1, 200);
  // ~50% arc (arg = 128 -> 6 of 11 segments lit)
  mcu_led_set cmd{MCU_LED_ENCODER_RING_3, MCU_LED_BRIGHTNESS_STRONG, 128};
  assert(mcu_pack_led_set(&f, &cmd) == MCU_OK);
  assert(mcu_panel_leds_on_frame(&leds, &f) == MCU_OK);

  const auto* ring = mcu_panel_leds_get(&leds, MCU_LED_ENCODER_RING_3);
  assert(ring && ring->is_ring == 1u);
  for (unsigned s = 0; s < 6; ++s) {
    assert(ring->ring_segment_duty[s] == 65535u);
  }
  for (unsigned s = 6; s < MCU_PANEL_RING_SEGMENTS; ++s) {
    assert(ring->ring_segment_duty[s] == 0u);  // strictly 0, never weak-lit
  }
}

void test_50klx_contrast_target() {
  std::uint32_t ratio_x100 = 0;
  // 12.5:1 passes
  assert(mcu_panel_leds_evaluate_contrast_50klx(125000, 10000, &ratio_x100) == 1);
  assert(ratio_x100 == 1250u);
  // 8.5:1 fails (< 10:1)
  assert(mcu_panel_leds_evaluate_contrast_50klx(85000, 10000, &ratio_x100) == 0);
  assert(ratio_x100 == 850u);
}

}  // namespace

int main() {
  test_boot_all_off_and_pwm_frequency_gate();
  test_strong_weak_off_and_global_brightness();
  test_encoder_ring_value_arc_unlit_fully_off();
  test_50klx_contrast_target();
  return 0;
}

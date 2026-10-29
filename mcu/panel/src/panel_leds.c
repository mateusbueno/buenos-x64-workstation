/*
 * Panel MCU firmware — SoC-driven LEDs implementation (Story 3.9).
 * Portable C11: no OS, no heap, no stdio, no floating point.
 */
#include "mcu/panel_leds.h"

#include <string.h>

static const uint8_t kLedIds[MCU_LED_COUNT] = {
    MCU_LED_PART_SCENE_LED_1,    MCU_LED_PART_SCENE_LED_2,
    MCU_LED_PART_SCENE_LED_3,    MCU_LED_PART_SCENE_LED_4,
    MCU_LED_PART_SCENE_LED_5,    MCU_LED_PART_SCENE_LED_6,
    MCU_LED_PART_SCENE_LED_7,    MCU_LED_PART_SCENE_LED_8,
    MCU_LED_ENCODER_RING_1,      MCU_LED_ENCODER_RING_2,
    MCU_LED_ENCODER_RING_3,      MCU_LED_ENCODER_RING_4,
    MCU_LED_ENCODER_RING_5,      MCU_LED_ENCODER_RING_6,
    MCU_LED_ENCODER_RING_7,      MCU_LED_ENCODER_RING_8,
    MCU_LED_TOGGLE_LED_DRAWBARS, MCU_LED_TOGGLE_LED_MIXER,
    MCU_LED_TOGGLE_LED_PARTS,    MCU_LED_TOGGLE_LED_SCENES,
    MCU_LED_TOGGLE_LED_PART,     MCU_LED_TOGGLE_LED_ALL,
    MCU_LED_TOGGLE_LED_ASSIGN};

static int is_ring_id(uint8_t id) {
  return id >= MCU_LED_ENCODER_RING_1 && id <= MCU_LED_ENCODER_RING_8;
}

static uint32_t global_scale_per_mille(mcu_panel_global_brightness gb) {
  switch (gb) {
    case MCU_PANEL_BRIGHTNESS_LOW:
      return 350u; /* 35.0% */
    case MCU_PANEL_BRIGHTNESS_MID:
      return 680u; /* 68.0% */
    case MCU_PANEL_BRIGHTNESS_HIGH:
    default:
      return 1000u; /* 100.0% */
  }
}

static uint16_t compute_pwm_duty(uint8_t brightness, mcu_panel_global_brightness gb) {
  uint64_t scale;
  uint64_t num;
  if (brightness == 0u) return 0u;
  scale = (uint64_t)global_scale_per_mille(gb);
  /* Map brightness (1..255) * scale (350..1000) into 0..65535 with rounding. */
  num = (uint64_t)brightness * (uint64_t)MCU_PANEL_PWM_MAX_DUTY * scale + 127500u;
  return (uint16_t)(num / 255000u);
}

static void update_entry_pwm(mcu_panel_led_entry* e, mcu_panel_global_brightness gb) {
  unsigned seg;
  e->pwm_duty = compute_pwm_duty(e->brightness, gb);

  if (!e->is_ring) {
    memset(e->ring_segment_duty, 0, sizeof(e->ring_segment_duty));
    return;
  }

  /* Encoder ring (Story 3.9 AC3):
   * When brightness == 0 or arg == 0, all segments are 0 (off).
   * When arg in 1..255 and brightness > 0, lit_count = (arg * 11 + 254) / 255 (1..11 segments).
   * Lit segments show the arc at `e->pwm_duty`, and the remaining segments are strictly 0 (fully off, never weak-lit).
   */
  if (e->brightness == 0u || e->arg == 0u) {
    for (seg = 0u; seg < MCU_PANEL_RING_SEGMENTS; ++seg) {
      e->ring_segment_duty[seg] = 0u;
    }
    return;
  }

  {
    unsigned lit_count = ((unsigned)e->arg * MCU_PANEL_RING_SEGMENTS + 254u) / 255u;
    if (lit_count > MCU_PANEL_RING_SEGMENTS) lit_count = MCU_PANEL_RING_SEGMENTS;
    for (seg = 0u; seg < MCU_PANEL_RING_SEGMENTS; ++seg) {
      e->ring_segment_duty[seg] = (seg < lit_count) ? e->pwm_duty : 0u;
    }
  }
}

int mcu_panel_leds_init(mcu_panel_leds* leds,
                        uint32_t pwm_freq_hz,
                        mcu_panel_global_brightness global_brightness,
                        mcu_panel_pwm_apply_fn apply_cb,
                        void* apply_user) {
  unsigned i;
  if (!leds) return MCU_ERR_NULL;
  if (pwm_freq_hz < MCU_PANEL_MIN_PWM_HZ) return MCU_ERR_BAD_VALUE;
  if (global_brightness != MCU_PANEL_BRIGHTNESS_LOW &&
      global_brightness != MCU_PANEL_BRIGHTNESS_MID &&
      global_brightness != MCU_PANEL_BRIGHTNESS_HIGH) {
    return MCU_ERR_BAD_VALUE;
  }

  memset(leds, 0, sizeof(*leds));
  leds->pwm_freq_hz = pwm_freq_hz;
  leds->global_brightness = global_brightness;
  leds->apply_cb = apply_cb;
  leds->apply_user = apply_user;

  /* Story 3.9 AC1: all LEDs off at boot until the first SoC command. */
  for (i = 0u; i < MCU_LED_COUNT; ++i) {
    leds->entries[i].led_id = kLedIds[i];
    leds->entries[i].is_ring = (uint8_t)(is_ring_id(kLedIds[i]) ? 1u : 0u);
    leds->entries[i].brightness = 0u;
    leds->entries[i].arg = 0u;
    leds->entries[i].pwm_duty = 0u;
  }
  return MCU_OK;
}

int mcu_panel_leds_set_global_brightness(mcu_panel_leds* leds,
                                         mcu_panel_global_brightness global_brightness) {
  unsigned i;
  if (!leds) return MCU_ERR_NULL;
  if (global_brightness != MCU_PANEL_BRIGHTNESS_LOW &&
      global_brightness != MCU_PANEL_BRIGHTNESS_MID &&
      global_brightness != MCU_PANEL_BRIGHTNESS_HIGH) {
    return MCU_ERR_BAD_VALUE;
  }

  leds->global_brightness = global_brightness;
  for (i = 0u; i < MCU_LED_COUNT; ++i) {
    update_entry_pwm(&leds->entries[i], global_brightness);
    if (leds->apply_cb) {
      leds->apply_cb(leds->apply_user, &leds->entries[i]);
    }
  }
  return MCU_OK;
}

int mcu_panel_leds_on_frame(mcu_panel_leds* leds, const mcu_frame* frame) {
  mcu_led_set cmd;
  unsigned i;

  if (!leds || !frame) return MCU_ERR_NULL;
  if (mcu_unpack_led_set(frame, &cmd) != MCU_OK) {
    leds->frames_rejected += 1u;
    return MCU_ERR_BAD_VALUE;
  }

  for (i = 0u; i < MCU_LED_COUNT; ++i) {
    if (leds->entries[i].led_id == cmd.id) {
      leds->entries[i].brightness = cmd.brightness;
      leds->entries[i].arg = cmd.arg;
      update_entry_pwm(&leds->entries[i], leds->global_brightness);
      leds->frames_applied += 1u;
      if (leds->apply_cb) {
        leds->apply_cb(leds->apply_user, &leds->entries[i]);
      }
      return MCU_OK;
    }
  }

  leds->frames_rejected += 1u;
  return MCU_ERR_BAD_VALUE;
}

const mcu_panel_led_entry* mcu_panel_leds_get(const mcu_panel_leds* leds, uint8_t led_id) {
  unsigned i;
  if (!leds) return NULL;
  for (i = 0u; i < MCU_LED_COUNT; ++i) {
    if (leds->entries[i].led_id == led_id) {
      return &leds->entries[i];
    }
  }
  return NULL;
}

int mcu_panel_leds_evaluate_contrast_50klx(uint32_t luminance_on_mcd_m2,
                                           uint32_t luminance_off_mcd_m2,
                                           uint32_t* out_ratio_x100) {
  uint32_t ratio_x100;
  if (!out_ratio_x100 || luminance_off_mcd_m2 == 0u) return 0;
  ratio_x100 = (uint32_t)(((uint64_t)luminance_on_mcd_m2 * 100u) / (uint64_t)luminance_off_mcd_m2);
  *out_ratio_x100 = ratio_x100;
  return ratio_x100 >= 1000u ? 1 : 0; /* >= 10.00:1 target under 50 klx */
}

/*
 * Panel MCU firmware — SoC-driven LEDs (Story 3.9: AR41, FR111, UX-DR50..52, NFR36).
 *
 * Portable C11: no OS, no heap, no stdio, no floating point.
 * Rules:
 *   - All LEDs start completely off (0 PWM duty) at boot and stay off until the SoC sends LED_SET frames.
 *   - The MCU never modifies any LED state autonomously (no local button/encoder-driven LED changes).
 *   - PWM carrier frequency must be >= 20 kHz (20000 Hz) for flicker-free operation.
 *   - Supports strong (255), weak (~15% PWM = 38/255) and off (0) per-LED levels scaled by global brightness
 *     (Low = 35%, Mid = 68%, High = 100%) into 16-bit PWM duty (0..65535).
 *   - Encoder rings (ENCODER_RING_1..8, 11 segments per ring): when the SoC sends a value arc (`arg` = fill
 *     position 0..255, `brightness` = arc brightness), the lit segments in the arc receive the scaled strong
 *     PWM duty and all unlit segments are strictly 0 (fully off, never weak-lit).
 */
#ifndef MCU_PANEL_LEDS_H
#define MCU_PANEL_LEDS_H

#include <stddef.h>
#include <stdint.h>

#include "mcu/mcu_frame.h"
#include "mcu/mcu_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_PANEL_MIN_PWM_HZ 20000u
#define MCU_PANEL_PWM_MAX_DUTY 65535u
#define MCU_PANEL_RING_COUNT 8u
#define MCU_PANEL_RING_SEGMENTS 11u

/* Logical per-LED brightness tiers sent by the SoC (or arbitrary 0..255). */
#define MCU_LED_BRIGHTNESS_OFF 0u
#define MCU_LED_BRIGHTNESS_WEAK 38u    /* ~15% of 255 (38 / 255 = 14.9%) */
#define MCU_LED_BRIGHTNESS_STRONG 255u

typedef enum mcu_panel_global_brightness {
  MCU_PANEL_BRIGHTNESS_LOW = 0,  /* 35% global scale */
  MCU_PANEL_BRIGHTNESS_MID = 1,  /* 68% global scale */
  MCU_PANEL_BRIGHTNESS_HIGH = 2  /* 100% global scale */
} mcu_panel_global_brightness;

typedef struct mcu_panel_led_entry {
  uint8_t led_id;
  uint8_t is_ring;
  uint8_t brightness; /* 0..255 commanded by SoC */
  uint8_t arg;        /* colour index for discrete LEDs, or 0..255 arc fill for encoder rings */
  uint16_t pwm_duty;  /* 0..65535 computed PWM duty for discrete LED (or arc segment duty for rings) */
  uint16_t ring_segment_duty[MCU_PANEL_RING_SEGMENTS]; /* per-segment PWM duty (unlit segments always 0) */
} mcu_panel_led_entry;

/* Optional hardware PWM sink invoked when an LED_SET updates a discrete LED or ring. */
typedef void (*mcu_panel_pwm_apply_fn)(void* user, const mcu_panel_led_entry* entry);

typedef struct mcu_panel_leds {
  uint32_t pwm_freq_hz;
  mcu_panel_global_brightness global_brightness;
  mcu_panel_pwm_apply_fn apply_cb;
  void* apply_user;
  uint32_t frames_applied;
  uint32_t frames_rejected;
  mcu_panel_led_entry entries[MCU_LED_COUNT];
} mcu_panel_leds;

/* Initializes the LED driver: validates pwm_freq_hz >= 20000 Hz and sets every LED and ring segment to 0 (off). */
int mcu_panel_leds_init(mcu_panel_leds* leds,
                        uint32_t pwm_freq_hz,
                        mcu_panel_global_brightness global_brightness,
                        mcu_panel_pwm_apply_fn apply_cb,
                        void* apply_user);

/* Changes the global user brightness (Low, Mid, High) and recomputes PWM duties for all currently lit LEDs. */
int mcu_panel_leds_set_global_brightness(mcu_panel_leds* leds,
                                         mcu_panel_global_brightness global_brightness);

/* Processes an incoming LED_SET frame from the SoC. Rejects any non-LED_SET frame or unknown LED ID. */
int mcu_panel_leds_on_frame(mcu_panel_leds* leds, const mcu_frame* frame);

/* Looks up the current state of an LED by its ID (from mcu_ids.h), or NULL if unknown. */
const mcu_panel_led_entry* mcu_panel_leds_get(const mcu_panel_leds* leds, uint8_t led_id);

/* Evaluates a 50 klx ambient contrast measurement (lux_on_cd_m2 vs lux_off_cd_m2 in millicandelas/m^2)
 * against the >= 10:1 target (NFR36 / Story 3.9 AC4). Returns 1 if contrast_ratio_x100 >= 1000 (10.00:1). */
int mcu_panel_leds_evaluate_contrast_50klx(uint32_t luminance_on_mcd_m2,
                                           uint32_t luminance_off_mcd_m2,
                                           uint32_t* out_ratio_x100);

#ifdef __cplusplus
}
#endif

#endif /* MCU_PANEL_LEDS_H */

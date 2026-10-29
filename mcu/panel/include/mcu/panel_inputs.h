/*
 * Panel MCU firmware — inputs (Story 3.8: AR41, FR42, FR43).
 *
 * Portable C11: no OS, no heap, no stdio, no floating point.
 * Covers:
 *   - 9 quadrature encoders (ENCODER_1..8, VALUE_ENCODER) with Gray-code decoding and relative step deltas
 *   - 13 analog 12-bit channels (9 faders, 2 wheels, 2 continuous pedals; ADC >= 10 bits normalized to 12 bits,
 *     IIR filtered, deadband jitter suppression, reported on change at >= 500 Hz)
 *   - 1 switch pedal (PEDAL_SWITCH) + 21 buttons (including VALUE_PUSH and 3 toggles) with time-based debounce
 *   - Boot-time pedal polarity detection (sustain, expression, switch) reported in HELLO
 */
#ifndef MCU_PANEL_INPUTS_H
#define MCU_PANEL_INPUTS_H

#include <stddef.h>
#include <stdint.h>

#include "mcu/mcu_frame.h"
#include "mcu/mcu_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_PANEL_ENCODER_COUNT 9u
#define MCU_PANEL_ANALOG_COUNT 13u  /* 9 faders + 2 wheels + 2 continuous pedals */
#define MCU_PANEL_BUTTON_COUNT MCU_BTN_COUNT /* 21 buttons */

#define MCU_PANEL_ADC_MIN_BITS 10u
#define MCU_PANEL_ADC_TARGET_BITS 12u
#define MCU_PANEL_ADC_MAX_VALUE 4095u
#define MCU_PANEL_MAX_REPORT_INTERVAL_US 2000u /* >= 500 Hz */
#define MCU_PANEL_DEFAULT_REPORT_INTERVAL_US 1000u /* 1000 Hz */
#define MCU_PANEL_DEFAULT_DEADBAND 8u
#define MCU_PANEL_DEFAULT_DEBOUNCE_US 5000u

typedef void (*mcu_panel_frame_tx_fn)(void* user, const mcu_frame* frame);

typedef struct mcu_panel_inputs_config {
  uint16_t fw_version;
  uint8_t adc_bits;            /* 10..16 (rejected if < 10) */
  uint8_t steps_per_detent;    /* 1, 2 or 4 quadrature transitions per detent */
  uint16_t deadband_12bit;     /* deadband in 12-bit counts (default 8) */
  uint32_t report_interval_us; /* <= 2000 us (>= 500 Hz) */
  uint32_t debounce_us;        /* button & switch debounce window in ticks/us */
} mcu_panel_inputs_config;

typedef struct mcu_panel_encoder_state {
  uint8_t control_id;
  uint8_t prev_ab;       /* 2-bit Gray state (A << 1) | B */
  uint8_t initialized;
  int16_t sub_accum;     /* transitions accumulated toward a detent */
  int16_t pending_delta; /* detents accumulated between frame flushes */
  uint32_t glitches;     /* invalid 2-bit transitions ignored */
} mcu_panel_encoder_state;

typedef struct mcu_panel_analog_state {
  uint8_t control_id;
  uint8_t initialized;
  uint16_t filtered_12bit;
  uint16_t last_reported_12bit;
  uint32_t last_report_tick;
  uint8_t ever_reported;
} mcu_panel_analog_state;

typedef struct mcu_panel_button_state {
  uint8_t id;
  uint8_t stable_state;    /* 0 released, 1 pressed */
  uint8_t candidate_state; /* 0 or 1 */
  uint32_t candidate_since_tick;
  uint8_t initialized;
} mcu_panel_button_state;

typedef struct mcu_panel_inputs {
  mcu_panel_inputs_config cfg;
  mcu_panel_frame_tx_fn tx;
  void* tx_user;
  uint16_t next_seq;
  uint8_t pedal_polarity_flags; /* MCU_PANEL_PEDAL_*_INVERTED bits reported in HELLO */
  mcu_panel_encoder_state encoders[MCU_PANEL_ENCODER_COUNT];
  mcu_panel_analog_state analogs[MCU_PANEL_ANALOG_COUNT];
  mcu_panel_button_state pedal_switch;
  mcu_panel_button_state buttons[MCU_PANEL_BUTTON_COUNT];
} mcu_panel_inputs;

/* Initializes panel input state and validates config (adc_bits >= 10, report_interval_us <= 2000). */
int mcu_panel_inputs_init(mcu_panel_inputs* p,
                          const mcu_panel_inputs_config* cfg,
                          mcu_panel_frame_tx_fn tx,
                          void* tx_user);

/* Detects boot-time pedal polarity from initial raw ADC / GPIO readings and sends the HELLO frame. */
int mcu_panel_inputs_boot(mcu_panel_inputs* p,
                          uint16_t sustain_raw,
                          uint16_t expression_raw,
                          uint8_t switch_raw,
                          uint32_t tick_us);

/* Feeds a quadrature encoder AB sample (bit 1 = A, bit 0 = B) for encoder index 0..8 (or by control_id). */
int mcu_panel_inputs_sample_encoder(mcu_panel_inputs* p,
                                    uint8_t control_id,
                                    uint8_t ab_bits,
                                    uint32_t tick_us);

/* Feeds a raw ADC reading (at cfg.adc_bits resolution) for a fader, wheel, or continuous pedal. */
int mcu_panel_inputs_sample_analog(mcu_panel_inputs* p,
                                   uint8_t control_id,
                                   uint16_t raw_adc,
                                   uint32_t tick_us);

/* Feeds a raw switch pedal reading (0 or 1 before polarity). */
int mcu_panel_inputs_sample_pedal_switch(mcu_panel_inputs* p,
                                         uint8_t raw_level,
                                         uint32_t tick_us);

/* Feeds a raw button GPIO reading (0 released, 1 pressed) for a button ID in MCU-ID-MAP. */
int mcu_panel_inputs_sample_button(mcu_panel_inputs* p,
                                   uint8_t button_id,
                                   uint8_t raw_pressed,
                                   uint32_t tick_us);

/* Handles an incoming frame from the SoC (e.g., TIME_SYNC request -> replies with current tick_us). */
int mcu_panel_inputs_on_soc_frame(mcu_panel_inputs* p,
                                  const mcu_frame* frame,
                                  uint32_t tick_us);

#ifdef __cplusplus
}
#endif

#endif /* MCU_PANEL_INPUTS_H */

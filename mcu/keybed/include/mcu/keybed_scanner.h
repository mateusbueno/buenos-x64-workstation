/*
 * Keybed MCU firmware with raw per-contact timestamps (Story 3.3: AR40, NFR42, NFR1).
 *
 * Portable C11: no OS, no heap, no stdio, no floating point.
 * Rules:
 *   - Scans 73 keys (2 or 3 contacts per key) at >= 10 kHz (scan period <= 100 us).
 *   - Free-running tick clock at >= 1 MHz (1 tick <= 1 us).
 *   - Sends one KEY_CONTACT frame per contact edge (key 0..72, contact 0..2, closed 0/1) stamped with the scan tick.
 *   - Computes NO velocity and holds NO calibration table on the MCU.
 *   - At power-up sends HELLO (fw_version, protocol_version, MCU_BOARD_KEYBED, keybed_contacts + optional aftertouch flag)
 *     and answers every TIME_SYNC request with its current tick.
 */
#ifndef MCU_KEYBED_SCANNER_H
#define MCU_KEYBED_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#include "mcu/mcu_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_KEYBED_KEY_COUNT 73u
#define MCU_KEYBED_MAX_CONTACTS 3u
#define MCU_KEYBED_MIN_SCAN_HZ 10000u   /* >= 10 kHz */
#define MCU_KEYBED_MIN_TICK_HZ 1000000u /* >= 1 MHz */
#define MCU_KEYBED_MAX_LATENCY_US 200u  /* <= 200 us contact-to-frame complete */

typedef void (*mcu_keybed_frame_tx_fn)(void* user, const mcu_frame* frame);

typedef struct mcu_keybed_config {
  uint16_t fw_version;
  uint8_t contact_count;       /* 2 or 3 contacts per key */
  uint8_t has_aftertouch;      /* 0 or 1 (sets MCU_KEYBED_AFTERTOUCH_FLAG in HELLO) */
  uint32_t scan_rate_hz;       /* >= 10000 Hz */
  uint32_t tick_rate_hz;       /* >= 1000000 Hz */
  uint32_t link_baud_rate;     /* e.g. 3000000 baud (3 Mbaud) */
} mcu_keybed_config;

typedef struct mcu_keybed_scanner {
  mcu_keybed_config cfg;
  mcu_keybed_frame_tx_fn tx;
  void* tx_user;
  uint16_t next_seq;
  uint32_t events_emitted;
  uint32_t time_sync_replies;
  /* Bitmask of closed contacts per key: bit c (0..2) = contact c closed. */
  uint8_t contact_state[MCU_KEYBED_KEY_COUNT];
} mcu_keybed_scanner;

/* Initializes the keybed scanner and validates scan_rate_hz >= 10 kHz, tick_rate_hz >= 1 MHz, contact_count in {2,3}. */
int mcu_keybed_init(mcu_keybed_scanner* kb,
                    const mcu_keybed_config* cfg,
                    mcu_keybed_frame_tx_fn tx,
                    void* tx_user);

/* Sends the power-up HELLO frame at `tick`. */
int mcu_keybed_boot(mcu_keybed_scanner* kb, uint32_t tick);

/* Scans the full 73-key matrix (`matrix_masks[k]` bits 0..contact_count-1 hold the current closed state of key `k`)
 * at `tick` and emits one KEY_CONTACT frame per changed contact in closing/opening order. */
int mcu_keybed_scan_matrix(mcu_keybed_scanner* kb,
                           const uint8_t matrix_masks[MCU_KEYBED_KEY_COUNT],
                           uint32_t tick);

/* Handles an incoming frame from the SoC (answers TIME_SYNC requests with current `tick`). */
int mcu_keybed_on_soc_frame(mcu_keybed_scanner* kb,
                            const mcu_frame* frame,
                            uint32_t tick);

/* Computes worst-case contact-to-frame-complete latency in microseconds for `simultaneous_contacts` changes
 * given the scanner's scan_rate_hz and link_baud_rate (10 bits/byte UART framing, 15 bytes per KEY_CONTACT frame). */
uint32_t mcu_keybed_wire_latency_us(const mcu_keybed_scanner* kb, uint32_t simultaneous_contacts);

#ifdef __cplusplus
}
#endif

#endif /* MCU_KEYBED_SCANNER_H */

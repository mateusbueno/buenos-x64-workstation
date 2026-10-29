/* Plain C11 smoke test for mcu/keybed scanner (Story 3.3). */
#include "mcu/keybed_scanner.h"

#include <string.h>

typedef struct rx_cap {
  mcu_frame last;
  uint32_t count;
} rx_cap;

static void on_tx(void* user, const mcu_frame* f) {
  rx_cap* c = (rx_cap*)user;
  c->last = *f;
  c->count += 1u;
}

int main(void) {
  mcu_keybed_scanner kb;
  mcu_keybed_config cfg;
  rx_cap cap;
  uint8_t matrix[MCU_KEYBED_KEY_COUNT];
  mcu_key_contact kc;

  memset(&cap, 0, sizeof(cap));
  memset(matrix, 0, sizeof(matrix));
  cfg.fw_version = 0x0102u;
  cfg.contact_count = 3u;
  cfg.has_aftertouch = 1u;
  cfg.scan_rate_hz = 20000u;    /* 20 kHz (50 us scan period) */
  cfg.tick_rate_hz = 1000000u;  /* 1 MHz */
  cfg.link_baud_rate = 3000000u; /* 3 Mbaud */

  if (mcu_keybed_init(&kb, &cfg, on_tx, &cap) != MCU_OK) return 1;
  if (mcu_keybed_boot(&kb, 100u) != MCU_OK) return 2;
  if (cap.count != 1u || cap.last.type != MCU_TYPE_HELLO) return 3;

  matrix[36] = 0x01u; /* Key 36, contact 0 closes */
  if (mcu_keybed_scan_matrix(&kb, matrix, 150u) != MCU_OK) return 4;
  if (cap.count != 2u || mcu_unpack_key_contact(&cap.last, &kc) != MCU_OK) return 5;
  if (kc.key != 36u || kc.contact != 0u || kc.closed != 1u) return 6;

  /* Single contact latency at 20 kHz scan + 3 Mbaud UART is 50 us + 50 us = 100 us <= 200 us. */
  if (mcu_keybed_wire_latency_us(&kb, 1u) > MCU_KEYBED_MAX_LATENCY_US) return 7;

  return 0;
}

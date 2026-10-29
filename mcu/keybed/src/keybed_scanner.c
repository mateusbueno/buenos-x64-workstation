/*
 * Keybed MCU firmware implementation (Story 3.3).
 * Portable C11: no OS, no heap, no stdio, no floating point.
 */
#include "mcu/keybed_scanner.h"

#include <string.h>

static void emit_frame(mcu_keybed_scanner* kb, mcu_frame* f, uint32_t tick) {
  f->version = MCU_PROTOCOL_VERSION;
  f->seq = kb->next_seq++;
  f->tick = tick;
  if (kb->tx) {
    kb->tx(kb->tx_user, f);
  }
}

int mcu_keybed_init(mcu_keybed_scanner* kb,
                    const mcu_keybed_config* cfg,
                    mcu_keybed_frame_tx_fn tx,
                    void* tx_user) {
  if (!kb || !cfg) return MCU_ERR_NULL;
  if (cfg->contact_count != 2u && cfg->contact_count != 3u) {
    return MCU_ERR_BAD_VALUE;
  }
  if (cfg->scan_rate_hz < MCU_KEYBED_MIN_SCAN_HZ) {
    return MCU_ERR_BAD_VALUE;
  }
  if (cfg->tick_rate_hz < MCU_KEYBED_MIN_TICK_HZ) {
    return MCU_ERR_BAD_VALUE;
  }
  if (cfg->link_baud_rate == 0u) {
    return MCU_ERR_BAD_VALUE;
  }

  memset(kb, 0, sizeof(*kb));
  kb->cfg = *cfg;
  kb->tx = tx;
  kb->tx_user = tx_user;
  return MCU_OK;
}

int mcu_keybed_boot(mcu_keybed_scanner* kb, uint32_t tick) {
  mcu_frame f;
  mcu_hello h;
  uint8_t contacts_byte;

  if (!kb) return MCU_ERR_NULL;
  kb->next_seq = 0u;
  memset(kb->contact_state, 0, sizeof(kb->contact_state));

  contacts_byte = kb->cfg.contact_count;
  if (kb->cfg.has_aftertouch) {
    contacts_byte = (uint8_t)(contacts_byte | MCU_KEYBED_AFTERTOUCH_FLAG);
  }

  mcu_frame_init(&f, MCU_TYPE_HELLO, 0u, tick);
  h.fw_version = kb->cfg.fw_version;
  h.protocol_version = MCU_PROTOCOL_VERSION;
  h.board_id = MCU_BOARD_KEYBED;
  h.keybed_contacts = contacts_byte;
  if (mcu_pack_hello(&f, &h) != MCU_OK) return MCU_ERR_BAD_VALUE;
  emit_frame(kb, &f, tick);
  return MCU_OK;
}

int mcu_keybed_scan_matrix(mcu_keybed_scanner* kb,
                           const uint8_t matrix_masks[MCU_KEYBED_KEY_COUNT],
                           uint32_t tick) {
  uint8_t k;
  uint8_t c;
  uint8_t valid_mask;

  if (!kb || !matrix_masks) return MCU_ERR_NULL;
  valid_mask = (uint8_t)((1u << kb->cfg.contact_count) - 1u);

  for (k = 0u; k < MCU_KEYBED_KEY_COUNT; ++k) {
    uint8_t prev = kb->contact_state[k];
    uint8_t curr = (uint8_t)(matrix_masks[k] & valid_mask);
    uint8_t diff = (uint8_t)(prev ^ curr);
    if (diff == 0u) continue;

    kb->contact_state[k] = curr;
    for (c = 0u; c < kb->cfg.contact_count; ++c) {
      if ((diff & (uint8_t)(1u << c)) != 0u) {
        mcu_frame f;
        mcu_key_contact kc;
        kc.key = k;
        kc.contact = c;
        kc.closed = (uint8_t)((curr >> c) & 1u);
        mcu_frame_init(&f, MCU_TYPE_KEY_CONTACT, 0u, tick);
        if (mcu_pack_key_contact(&f, &kc) == MCU_OK) {
          emit_frame(kb, &f, tick);
          kb->events_emitted += 1u;
        }
      }
    }
  }
  return MCU_OK;
}

int mcu_keybed_on_soc_frame(mcu_keybed_scanner* kb,
                            const mcu_frame* frame,
                            uint32_t tick) {
  if (!kb || !frame) return MCU_ERR_NULL;
  if (frame->type == MCU_TYPE_TIME_SYNC) {
    mcu_time_sync req;
    if (mcu_unpack_time_sync(frame, &req) != MCU_OK) return MCU_ERR_BAD_VALUE;
    if (req.reply == MCU_TIME_SYNC_REQUEST) {
      mcu_frame rep_frame;
      mcu_time_sync rep;
      rep.reply = MCU_TIME_SYNC_REPLY;
      rep.token = req.token;
      rep.tick = tick;
      mcu_frame_init(&rep_frame, MCU_TYPE_TIME_SYNC, 0u, tick);
      if (mcu_pack_time_sync(&rep_frame, &rep) == MCU_OK) {
        emit_frame(kb, &rep_frame, tick);
        kb->time_sync_replies += 1u;
      }
    }
  }
  return MCU_OK;
}

uint32_t mcu_keybed_wire_latency_us(const mcu_keybed_scanner* kb,
                                    uint32_t simultaneous_contacts) {
  uint32_t scan_period_us;
  uint32_t wire_bits;
  uint32_t wire_us;
  if (!kb || kb->cfg.scan_rate_hz == 0u || kb->cfg.link_baud_rate == 0u) return 0xFFFFFFFFu;
  /* 1 scan period + wire serialization time (15 bytes = 150 UART 8N1 bits per KEY_CONTACT frame). */
  scan_period_us = (1000000u + kb->cfg.scan_rate_hz - 1u) / kb->cfg.scan_rate_hz;
  wire_bits = simultaneous_contacts * 150u;
  wire_us = (uint32_t)(((uint64_t)wire_bits * 1000000ull + kb->cfg.link_baud_rate - 1u) /
                       (uint64_t)kb->cfg.link_baud_rate);
  return scan_period_us + wire_us;
}

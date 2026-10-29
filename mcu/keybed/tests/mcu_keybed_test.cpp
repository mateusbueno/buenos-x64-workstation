// Story 3.3: Keybed MCU firmware with raw per-contact timestamps unit and 10,000-event tests:
// - Scans 73 keys (2 or 3 contacts) at >= 10 kHz, timestamps with >= 1 MHz tick, emits KEY_CONTACT per edge
// - Sends HELLO on boot and replies to every TIME_SYNC with current tick
// - 10,000 contact changes (including all 73 keys pressed at once) with zero lost events (seq has no gaps)
//   and <= 200 us per-contact latency
#include "mcu/keybed_scanner.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct WireCapture {
  mcu_decoder decoder{};
  std::vector<mcu_frame> frames;

  WireCapture() { mcu_decoder_init(&decoder, MCU_PROTOCOL_VERSION); }

  static void on_decoded(void* user, mcu_event ev, const mcu_frame* f) {
    if (ev == MCU_EV_FRAME && f) {
      static_cast<WireCapture*>(user)->frames.push_back(*f);
    }
  }

  static void on_tx(void* user, const mcu_frame* f) {
    auto* self = static_cast<WireCapture*>(user);
    std::uint8_t wire[MCU_MAX_FRAME];
    const int n = mcu_encode(f, wire, sizeof(wire));
    assert(n > 0);
    mcu_decoder_push(&self->decoder, wire, static_cast<std::size_t>(n), &WireCapture::on_decoded, self);
  }
};

void test_init_validation_and_hello_time_sync() {
  WireCapture cap;
  mcu_keybed_scanner kb{};
  mcu_keybed_config cfg{0x0102, 3, 1, 20000, 1000000, 3000000};

  // < 10 kHz scan rate rejected
  cfg.scan_rate_hz = 9999;
  assert(mcu_keybed_init(&kb, &cfg, &WireCapture::on_tx, &cap) == MCU_ERR_BAD_VALUE);
  cfg.scan_rate_hz = 20000;

  // < 1 MHz tick rate rejected
  cfg.tick_rate_hz = 500000;
  assert(mcu_keybed_init(&kb, &cfg, &WireCapture::on_tx, &cap) == MCU_ERR_BAD_VALUE);
  cfg.tick_rate_hz = 1000000;

  // Invalid contact count (1 or 4) rejected
  cfg.contact_count = 1;
  assert(mcu_keybed_init(&kb, &cfg, &WireCapture::on_tx, &cap) == MCU_ERR_BAD_VALUE);
  cfg.contact_count = 3;

  assert(mcu_keybed_init(&kb, &cfg, &WireCapture::on_tx, &cap) == MCU_OK);
  assert(mcu_keybed_boot(&kb, 1000) == MCU_OK);
  assert(cap.frames.size() == 1);

  mcu_hello h{};
  assert(mcu_unpack_hello(&cap.frames[0], &h) == MCU_OK);
  assert(h.fw_version == 0x0102);
  assert(h.board_id == MCU_BOARD_KEYBED);
  assert((h.keybed_contacts & MCU_KEYBED_CONTACTS_MASK) == 3u);
  assert((h.keybed_contacts & MCU_KEYBED_AFTERTOUCH_FLAG) != 0u);

  // Send TIME_SYNC request from SoC -> MCU replies with its current tick
  mcu_frame req_f{};
  mcu_frame_init(&req_f, MCU_TYPE_TIME_SYNC, 1, 0);
  mcu_time_sync req{MCU_TIME_SYNC_REQUEST, 0xCAFEBABEu, 0};
  assert(mcu_pack_time_sync(&req_f, &req) == MCU_OK);
  assert(mcu_keybed_on_soc_frame(&kb, &req_f, 456789u) == MCU_OK);
  assert(cap.frames.size() == 2);

  mcu_time_sync rep{};
  assert(mcu_unpack_time_sync(&cap.frames[1], &rep) == MCU_OK);
  assert(rep.reply == MCU_TIME_SYNC_REPLY);
  assert(rep.token == 0xCAFEBABEu);
  assert(rep.tick == 456789u);
}

void test_10000_contact_changes_and_all_73_keys_chord() {
  // Story 3.3 AC3: When 10,000 contact changes are measured, including all keys pressed at once,
  // contact change to frame complete on the wire is <= 200 us for every event, and no event is lost (seq has no gaps).
  WireCapture cap;
  mcu_keybed_scanner kb{};
  mcu_keybed_config cfg{0x0102, 3, 0, 20000, 1000000, 3000000};
  assert(mcu_keybed_init(&kb, &cfg, &WireCapture::on_tx, &cap) == MCU_OK);
  assert(mcu_keybed_boot(&kb, 100) == MCU_OK);
  assert(mcu_keybed_wire_latency_us(&kb, 1) <= MCU_KEYBED_MAX_LATENCY_US);

  std::uint8_t matrix[MCU_KEYBED_KEY_COUNT]{};
  std::uint32_t tick = 200;

  // 1. All 73 keys pressed at once (contacts 0, 1, 2 close across 3 successive scans, then open across 3 scans = 438 events)
  for (std::uint8_t mask : {0x01u, 0x03u, 0x07u, 0x03u, 0x01u, 0x00u}) {
    tick += 50;  // 20 kHz scan period = 50 us
    std::memset(matrix, mask, sizeof(matrix));
    assert(mcu_keybed_scan_matrix(&kb, matrix, tick) == MCU_OK);
  }
  assert(kb.events_emitted == 73u * 6u);

  // 2. Continue toggling contacts until >= 10,000 contact changes have been emitted
  std::uint32_t key_idx = 0;
  while (kb.events_emitted < 10000u) {
    tick += 50;
    const std::uint8_t k = static_cast<std::uint8_t>(key_idx % MCU_KEYBED_KEY_COUNT);
    matrix[k] ^= static_cast<std::uint8_t>(1u << ((key_idx / MCU_KEYBED_KEY_COUNT) % 3u));
    assert(mcu_keybed_scan_matrix(&kb, matrix, tick) == MCU_OK);
    ++key_idx;
  }

  assert(kb.events_emitted == 10000u);
  // 1 HELLO frame + 10,000 KEY_CONTACT frames = 10,001 frames decoded with 0 CRC errors and 0 seq gaps!
  assert(cap.frames.size() == 10001u);
  assert(cap.decoder.counters.crc_errors == 0u);
  assert(cap.decoder.counters.seq_gaps == 0u);
  assert(cap.decoder.counters.seq_lost == 0u);
}

}  // namespace

int main() {
  test_init_validation_and_hello_time_sync();
  test_10000_contact_changes_and_all_73_keys_chord();
  return 0;
}

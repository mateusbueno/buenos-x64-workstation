/* Plain-C11 smoke test of the MCU frame library: proves the header and library work without C++. */
#include <stdio.h>
#include <string.h>

#include "mcu/mcu_frame.h"
#include "mcu/mcu_ids.h"

static int g_fail = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
      ++g_fail;                                               \
    }                                                         \
  } while (0)

int main(void) {
  CHECK(mcu_crc16((const uint8_t*)"123456789", 9) == 0x29B1u);
  CHECK(MCU_ID_MAP_PROTOCOL_VERSION == MCU_PROTOCOL_VERSION);
  CHECK(MCU_CTRL_COUNT == 23u && MCU_BTN_COUNT == 21u && MCU_LED_COUNT == 23u);

  mcu_frame f;
  mcu_frame_init(&f, 0, 0, 0);
  mcu_button b = {MCU_BTN_VALUE_PUSH, 1};
  CHECK(mcu_pack_button(&f, &b) == MCU_OK);
  f.seq = 7;
  f.tick = 8;
  uint8_t wire[MCU_MAX_FRAME];
  const int n = mcu_encode(&f, wire, sizeof wire);
  static const uint8_t golden[] = {0xA5, 0x10, 0x05, 0x02, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x17, 0x01, 0xBA, 0xC5};
  CHECK(n == (int)sizeof golden && memcmp(wire, golden, sizeof golden) == 0);

  mcu_decoder d;
  mcu_decoder_init(&d, MCU_PROTOCOL_VERSION);
  mcu_frame out;
  mcu_event ev = MCU_EV_NONE;
  for (size_t i = 0; i < sizeof golden; ++i) ev = mcu_decoder_feed(&d, golden[i], &out);
  CHECK(ev == MCU_EV_FRAME && out.type == MCU_TYPE_BUTTON && out.seq == 7 && out.tick == 8);
  mcu_button back;
  CHECK(mcu_unpack_button(&out, &back) == MCU_OK && back.id == MCU_BTN_VALUE_PUSH && back.pressed == 1);
  CHECK(d.counters.frames_ok == 1 && d.counters.crc_errors == 0);

  if (g_fail == 0) printf("mcu_frame_c_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}

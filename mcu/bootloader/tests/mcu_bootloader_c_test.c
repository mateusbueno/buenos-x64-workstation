/* Plain C11 smoke test for mcu/bootloader (Story 3.4). */
#include "mcu/bootloader.h"

#include <string.h>

typedef struct sim_flash {
  mcu_bank_header headers[MCU_BOOT_BANK_COUNT];
  uint8_t data[MCU_BOOT_BANK_COUNT][1024];
  uint32_t resets;
} sim_flash;

static int sim_read_header(void* user, uint8_t bank, mcu_bank_header* out) {
  sim_flash* f = (sim_flash*)user;
  if (bank >= MCU_BOOT_BANK_COUNT) return -1;
  *out = f->headers[bank];
  return 0;
}

static int sim_write_header(void* user, uint8_t bank, const mcu_bank_header* hdr) {
  sim_flash* f = (sim_flash*)user;
  if (bank >= MCU_BOOT_BANK_COUNT) return -1;
  f->headers[bank] = *hdr;
  return 0;
}

static int sim_erase_bank(void* user, uint8_t bank) {
  sim_flash* f = (sim_flash*)user;
  if (bank >= MCU_BOOT_BANK_COUNT) return -1;
  memset(&f->headers[bank], 0xFF, sizeof(mcu_bank_header));
  memset(f->data[bank], 0xFF, sizeof(f->data[bank]));
  return 0;
}

static int sim_read_data(void* user, uint8_t bank, uint32_t offset, uint8_t* dst, size_t len) {
  sim_flash* f = (sim_flash*)user;
  if (bank >= MCU_BOOT_BANK_COUNT || offset + len > sizeof(f->data[bank])) return -1;
  memcpy(dst, &f->data[bank][offset], len);
  return 0;
}

static int sim_write_data(void* user, uint8_t bank, uint32_t offset, const uint8_t* src, size_t len) {
  sim_flash* f = (sim_flash*)user;
  if (bank >= MCU_BOOT_BANK_COUNT || offset + len > sizeof(f->data[bank])) return -1;
  memcpy(&f->data[bank][offset], src, len);
  return 0;
}

static void sim_reset(void* user) {
  sim_flash* f = (sim_flash*)user;
  f->resets += 1u;
}

int main(void) {
  sim_flash flash;
  mcu_flash_hal hal;
  mcu_bootloader_ctx ctx;
  uint8_t img_v1[64];
  uint8_t img_v2[64];
  mcu_hello hello;
  int reflashed = 0;
  size_t i;

  memset(&flash, 0xFF, sizeof(flash));
  flash.resets = 0u;
  for (i = 0; i < sizeof(img_v1); ++i) {
    img_v1[i] = (uint8_t)(i + 1u);
    img_v2[i] = (uint8_t)(i + 101u);
  }

  /* Seed Bank A with factory v1.0 (0x0100). */
  memcpy(flash.data[MCU_BOOT_BANK_A], img_v1, sizeof(img_v1));
  flash.headers[MCU_BOOT_BANK_A].magic = MCU_BANK_MAGIC;
  flash.headers[MCU_BOOT_BANK_A].generation = 1u;
  flash.headers[MCU_BOOT_BANK_A].image_size = (uint32_t)sizeof(img_v1);
  flash.headers[MCU_BOOT_BANK_A].fw_version = 0x0100u;
  flash.headers[MCU_BOOT_BANK_A].board_id = MCU_BOARD_KEYBED;
  flash.headers[MCU_BOOT_BANK_A].committed = MCU_BANK_COMMIT_MAGIC;
  flash.headers[MCU_BOOT_BANK_A].image_crc16 = mcu_crc16(img_v1, sizeof(img_v1));
  flash.headers[MCU_BOOT_BANK_A].header_crc16 = mcu_bank_header_crc(&flash.headers[MCU_BOOT_BANK_A]);

  hal.user = &flash;
  hal.bank_capacity = sizeof(flash.data[0]);
  hal.bootloader_write_protected = 1u;
  hal.read_header = sim_read_header;
  hal.write_header = sim_write_header;
  hal.erase_bank = sim_erase_bank;
  hal.read_data = sim_read_data;
  hal.write_data = sim_write_data;
  hal.reset_mcu = sim_reset;

  if (mcu_bootloader_boot(&ctx, &hal, MCU_BOARD_KEYBED) != MCU_BOOT_OK) return 1;
  if (ctx.active_bank != MCU_BOOT_BANK_A || ctx.active_fw_version != 0x0100u) return 2;

  hello.fw_version = ctx.active_fw_version;
  hello.protocol_version = MCU_PROTOCOL_VERSION;
  hello.board_id = MCU_BOARD_KEYBED;
  hello.keybed_contacts = 3u;

  /* Reflash to v1.1 (0x0101) into Bank B. */
  if (mcu_soc_reflash_if_needed(&ctx, &hello, 0x0101u, img_v2, sizeof(img_v2), 0, &reflashed) != MCU_BOOT_OK) {
    return 3;
  }
  if (!reflashed || ctx.active_bank != MCU_BOOT_BANK_B || ctx.active_fw_version != 0x0101u) return 4;

  return 0;
}

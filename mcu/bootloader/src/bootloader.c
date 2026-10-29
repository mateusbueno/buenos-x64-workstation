/*
 * MCU dual-bank bootloader and SoC reflash implementation (Story 3.4).
 * Portable C11: no OS, no heap, no stdio, no floating point.
 */
#include "mcu/bootloader.h"

#include <string.h>

static void put_u16_le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

uint16_t mcu_bank_header_crc(const mcu_bank_header* hdr) {
  uint8_t raw[18];
  if (!hdr) return 0u;
  put_u32_le(&raw[0], hdr->magic);
  put_u32_le(&raw[4], hdr->generation);
  put_u32_le(&raw[8], hdr->image_size);
  put_u16_le(&raw[12], hdr->fw_version);
  raw[14] = hdr->board_id;
  raw[15] = hdr->committed;
  put_u16_le(&raw[16], hdr->image_crc16);
  return mcu_crc16(raw, sizeof(raw));
}

int mcu_boot_verify_bank(const mcu_flash_hal* hal,
                         uint8_t bank,
                         uint8_t expected_board_id,
                         mcu_bank_header* out_hdr) {
  mcu_bank_header hdr;
  uint16_t crc = 0xFFFFu;
  uint32_t offset = 0u;
  uint8_t buf[64];

  if (!hal || !hal->read_header || !hal->read_data) return MCU_BOOT_ERR_NULL;
  if (bank >= MCU_BOOT_BANK_COUNT) return MCU_BOOT_ERR_OUT_OF_BOUNDS;
  if (hal->read_header(hal->user, bank, &hdr) != 0) return MCU_BOOT_ERR_FLASH_IO;

  if (hdr.magic != MCU_BANK_MAGIC) return MCU_BOOT_ERR_CRC_MISMATCH;
  if (hdr.committed != MCU_BANK_COMMIT_MAGIC) return MCU_BOOT_ERR_CRC_MISMATCH;
  if (hdr.board_id != expected_board_id) return MCU_BOOT_ERR_BOARD_MISMATCH;
  if (hdr.image_size == 0u || hdr.image_size > hal->bank_capacity ||
      hdr.image_size > MCU_BOOT_MAX_BANK_SIZE) {
    return MCU_BOOT_ERR_OUT_OF_BOUNDS;
  }
  if (mcu_bank_header_crc(&hdr) != hdr.header_crc16) {
    return MCU_BOOT_ERR_CRC_MISMATCH;
  }

  while (offset < hdr.image_size) {
    uint32_t rem = hdr.image_size - offset;
    size_t chunk = rem < sizeof(buf) ? (size_t)rem : sizeof(buf);
    size_t i;
    if (hal->read_data(hal->user, bank, offset, buf, chunk) != 0) {
      return MCU_BOOT_ERR_FLASH_IO;
    }
    for (i = 0; i < chunk; ++i) {
      crc = mcu_crc16_update(crc, buf[i]);
    }
    offset += (uint32_t)chunk;
  }

  if (crc != hdr.image_crc16) return MCU_BOOT_ERR_CRC_MISMATCH;
  if (out_hdr) *out_hdr = hdr;
  return MCU_BOOT_OK;
}

static int generation_is_newer(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) > 0;
}

int mcu_boot_select_bank(const mcu_flash_hal* hal,
                         uint8_t expected_board_id,
                         mcu_boot_selection* out) {
  mcu_bank_header hdr_a;
  mcu_bank_header hdr_b;
  int ok_a;
  int ok_b;

  if (!hal || !out) return MCU_BOOT_ERR_NULL;
  if (!hal->bootloader_write_protected) return MCU_BOOT_ERR_NOT_PROTECTED;

  memset(out, 0, sizeof(*out));
  out->active_bank = MCU_BOOT_BANK_NONE;

  ok_a = (mcu_boot_verify_bank(hal, MCU_BOOT_BANK_A, expected_board_id, &hdr_a) == MCU_BOOT_OK);
  ok_b = (mcu_boot_verify_bank(hal, MCU_BOOT_BANK_B, expected_board_id, &hdr_b) == MCU_BOOT_OK);
  out->bank_valid[MCU_BOOT_BANK_A] = (uint8_t)(ok_a ? 1u : 0u);
  out->bank_valid[MCU_BOOT_BANK_B] = (uint8_t)(ok_b ? 1u : 0u);

  if (ok_a && ok_b) {
    if (generation_is_newer(hdr_b.generation, hdr_a.generation)) {
      out->active_bank = MCU_BOOT_BANK_B;
      out->fw_version = hdr_b.fw_version;
      out->generation = hdr_b.generation;
    } else {
      out->active_bank = MCU_BOOT_BANK_A;
      out->fw_version = hdr_a.fw_version;
      out->generation = hdr_a.generation;
    }
    out->fallback_used = 0u;
    return MCU_BOOT_OK;
  }

  if (ok_a) {
    mcu_bank_header raw_b;
    out->active_bank = MCU_BOOT_BANK_A;
    out->fw_version = hdr_a.fw_version;
    out->generation = hdr_a.generation;
    if (hal->read_header(hal->user, MCU_BOOT_BANK_B, &raw_b) == 0 &&
        raw_b.magic == MCU_BANK_MAGIC) {
      out->fallback_used = 1u;
    }
    return MCU_BOOT_OK;
  }

  if (ok_b) {
    mcu_bank_header raw_a;
    out->active_bank = MCU_BOOT_BANK_B;
    out->fw_version = hdr_b.fw_version;
    out->generation = hdr_b.generation;
    if (hal->read_header(hal->user, MCU_BOOT_BANK_A, &raw_a) == 0 &&
        raw_a.magic == MCU_BANK_MAGIC) {
      out->fallback_used = 1u;
    }
    return MCU_BOOT_OK;
  }

  return MCU_BOOT_ERR_NO_VALID_BANK;
}

int mcu_bootloader_boot(mcu_bootloader_ctx* ctx,
                        const mcu_flash_hal* hal,
                        uint8_t board_id) {
  mcu_boot_selection sel;
  int rc;

  if (!ctx || !hal) return MCU_BOOT_ERR_NULL;
  if (board_id != MCU_BOARD_KEYBED && board_id != MCU_BOARD_PANEL) {
    return MCU_BOOT_ERR_BOARD_MISMATCH;
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->hal = hal;
  ctx->board_id = board_id;
  ctx->active_bank = MCU_BOOT_BANK_NONE;
  ctx->target_bank = MCU_BOOT_BANK_A;

  rc = mcu_boot_select_bank(hal, board_id, &sel);
  if (rc != MCU_BOOT_OK) {
    ctx->in_bootloader = 1u;
    return rc;
  }

  ctx->active_bank = sel.active_bank;
  ctx->target_bank = (uint8_t)(sel.active_bank == MCU_BOOT_BANK_A ? MCU_BOOT_BANK_B : MCU_BOOT_BANK_A);
  ctx->active_generation = sel.generation;
  ctx->active_fw_version = sel.fw_version;
  ctx->in_bootloader = 0u;
  return MCU_BOOT_OK;
}

int mcu_bootloader_enter_from_frame(mcu_bootloader_ctx* ctx, const mcu_frame* frame) {
  if (!ctx || !frame) return MCU_BOOT_ERR_NULL;
  if (mcu_unpack_enter_bootloader(frame) != MCU_OK) return MCU_BOOT_ERR_BAD_STATE;
  ctx->in_bootloader = 1u;
  ctx->write_in_progress = 0u;
  return MCU_BOOT_OK;
}

int mcu_bootloader_enter_via_gpio(mcu_bootloader_ctx* ctx) {
  if (!ctx) return MCU_BOOT_ERR_NULL;
  ctx->in_bootloader = 1u;
  ctx->write_in_progress = 0u;
  return MCU_BOOT_OK;
}

int mcu_bootloader_begin_update(mcu_bootloader_ctx* ctx,
                                uint16_t new_fw_version,
                                uint32_t image_size,
                                uint16_t image_crc16) {
  uint8_t inactive;
  if (!ctx || !ctx->hal) return MCU_BOOT_ERR_NULL;
  if (!ctx->hal->bootloader_write_protected) return MCU_BOOT_ERR_NOT_PROTECTED;
  if (!ctx->in_bootloader) return MCU_BOOT_ERR_BAD_STATE;
  if (image_size == 0u || image_size > ctx->hal->bank_capacity ||
      image_size > MCU_BOOT_MAX_BANK_SIZE) {
    return MCU_BOOT_ERR_OUT_OF_BOUNDS;
  }

  inactive = (uint8_t)(ctx->active_bank == MCU_BOOT_BANK_A ? MCU_BOOT_BANK_B : MCU_BOOT_BANK_A);
  ctx->target_bank = inactive;

  if (ctx->hal->erase_bank(ctx->hal->user, inactive) != 0) {
    ctx->write_in_progress = 0u;
    return MCU_BOOT_ERR_FLASH_IO;
  }

  ctx->expected_fw_version = new_fw_version;
  ctx->expected_size = image_size;
  ctx->expected_crc16 = image_crc16;
  ctx->bytes_written = 0u;
  ctx->write_in_progress = 1u;
  return MCU_BOOT_OK;
}

int mcu_bootloader_write_chunk(mcu_bootloader_ctx* ctx,
                               uint32_t offset,
                               const uint8_t* data,
                               size_t len) {
  if (!ctx || !ctx->hal || (!data && len > 0u)) return MCU_BOOT_ERR_NULL;
  if (!ctx->in_bootloader || !ctx->write_in_progress) return MCU_BOOT_ERR_BAD_STATE;
  if (len == 0u) return MCU_BOOT_OK;
  if (offset != ctx->bytes_written) return MCU_BOOT_ERR_OUT_OF_BOUNDS;
  if (offset + (uint32_t)len > ctx->expected_size) return MCU_BOOT_ERR_OUT_OF_BOUNDS;

  if (ctx->hal->write_data(ctx->hal->user, ctx->target_bank, offset, data, len) != 0) {
    ctx->write_in_progress = 0u;
    return MCU_BOOT_ERR_FLASH_IO;
  }

  ctx->bytes_written += (uint32_t)len;
  return MCU_BOOT_OK;
}

int mcu_bootloader_finalize_and_reset(mcu_bootloader_ctx* ctx) {
  mcu_bank_header hdr;
  mcu_bank_header verified;
  uint16_t crc = 0xFFFFu;
  uint32_t offset = 0u;
  uint8_t buf[64];

  if (!ctx || !ctx->hal) return MCU_BOOT_ERR_NULL;
  if (!ctx->in_bootloader || !ctx->write_in_progress) return MCU_BOOT_ERR_BAD_STATE;
  if (ctx->bytes_written != ctx->expected_size) return MCU_BOOT_ERR_BAD_STATE;

  /* Verify written flash contents before committing header. */
  while (offset < ctx->expected_size) {
    uint32_t rem = ctx->expected_size - offset;
    size_t chunk = rem < sizeof(buf) ? (size_t)rem : sizeof(buf);
    size_t i;
    if (ctx->hal->read_data(ctx->hal->user, ctx->target_bank, offset, buf, chunk) != 0) {
      ctx->write_in_progress = 0u;
      return MCU_BOOT_ERR_FLASH_IO;
    }
    for (i = 0; i < chunk; ++i) {
      crc = mcu_crc16_update(crc, buf[i]);
    }
    offset += (uint32_t)chunk;
  }

  if (crc != ctx->expected_crc16) {
    ctx->write_in_progress = 0u;
    return MCU_BOOT_ERR_CRC_MISMATCH;
  }

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MCU_BANK_MAGIC;
  hdr.generation = ctx->active_generation + 1u;
  hdr.image_size = ctx->expected_size;
  hdr.fw_version = ctx->expected_fw_version;
  hdr.board_id = ctx->board_id;
  hdr.committed = MCU_BANK_COMMIT_MAGIC;
  hdr.image_crc16 = ctx->expected_crc16;
  hdr.header_crc16 = mcu_bank_header_crc(&hdr);

  if (ctx->hal->write_header(ctx->hal->user, ctx->target_bank, &hdr) != 0) {
    ctx->write_in_progress = 0u;
    return MCU_BOOT_ERR_FLASH_IO;
  }

  if (mcu_boot_verify_bank(ctx->hal, ctx->target_bank, ctx->board_id, &verified) != MCU_BOOT_OK) {
    ctx->write_in_progress = 0u;
    return MCU_BOOT_ERR_CRC_MISMATCH;
  }

  ctx->write_in_progress = 0u;
  if (ctx->hal->reset_mcu) {
    ctx->hal->reset_mcu(ctx->hal->user);
  }
  return mcu_bootloader_boot(ctx, ctx->hal, ctx->board_id);
}

int mcu_soc_reflash_if_needed(mcu_bootloader_ctx* target_mcu,
                              const mcu_hello* reported_hello,
                              uint16_t packaged_fw_version,
                              const uint8_t* image_data,
                              size_t image_size,
                              int use_gpio_entry,
                              int* out_reflashed) {
  uint16_t crc;
  size_t offset = 0u;
  int rc;

  if (!target_mcu || !reported_hello || !image_data || !out_reflashed) {
    return MCU_BOOT_ERR_NULL;
  }
  *out_reflashed = 0;

  if (reported_hello->board_id != target_mcu->board_id) {
    return MCU_BOOT_ERR_BOARD_MISMATCH;
  }

  /* Story 3.4 AC2: when versions match, do nothing. */
  if (reported_hello->fw_version == packaged_fw_version) {
    return MCU_BOOT_OK;
  }

  if (use_gpio_entry) {
    rc = mcu_bootloader_enter_via_gpio(target_mcu);
  } else {
    mcu_frame enter_frame;
    mcu_frame_init(&enter_frame, MCU_TYPE_ENTER_BOOTLOADER, 1u, 0u);
    if (mcu_pack_enter_bootloader(&enter_frame) != MCU_OK) {
      return MCU_BOOT_ERR_BAD_STATE;
    }
    rc = mcu_bootloader_enter_from_frame(target_mcu, &enter_frame);
  }
  if (rc != MCU_BOOT_OK) return rc;

  crc = mcu_crc16(image_data, image_size);
  rc = mcu_bootloader_begin_update(target_mcu, packaged_fw_version, (uint32_t)image_size, crc);
  if (rc != MCU_BOOT_OK) return rc;

  while (offset < image_size) {
    size_t rem = image_size - offset;
    size_t chunk = rem < MCU_BOOT_CHUNK_SIZE ? rem : MCU_BOOT_CHUNK_SIZE;
    rc = mcu_bootloader_write_chunk(target_mcu, (uint32_t)offset, &image_data[offset], chunk);
    if (rc != MCU_BOOT_OK) return rc;
    offset += chunk;
  }

  rc = mcu_bootloader_finalize_and_reset(target_mcu);
  if (rc != MCU_BOOT_OK) return rc;

  *out_reflashed = 1;
  return MCU_BOOT_OK;
}

int mcu_boot_verify_production_options(const mcu_option_bytes* opts) {
  if (!opts) return MCU_BOOT_ERR_NULL;
  if (!opts->bootloader_write_protected) return MCU_BOOT_ERR_NOT_PROTECTED;
  if (!opts->swd_jtag_disabled) return MCU_BOOT_ERR_SECURITY_LOCK;
  if (!opts->external_boot_pin_disabled) return MCU_BOOT_ERR_SECURITY_LOCK;
  if (opts->rdp_level < 2u) return MCU_BOOT_ERR_SECURITY_LOCK;
  return MCU_BOOT_OK;
}

/*
 * MCU dual-bank bootloader and SoC reflash protocol (Story 3.4: AR43, FR48, NFR24).
 *
 * Portable C11: no OS, no heap, no stdio, no floating point.
 * Shared by the MCU bootloader target and the SoC host reflash driver / tests.
 */
#ifndef MCU_BOOTLOADER_H
#define MCU_BOOTLOADER_H

#include <stddef.h>
#include <stdint.h>

#include "mcu/mcu_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_BANK_MAGIC 0x56424E4Bu       /* "VBNK" */
#define MCU_BANK_COMMIT_MAGIC 0xA5u      /* set only after image + CRC verification */
#define MCU_BOOT_MAX_BANK_SIZE 65536u    /* 64 KiB max per application bank */
#define MCU_BOOT_CHUNK_SIZE 48u          /* fits inside MCU_MAX_PAYLOAD (64 bytes) */

typedef enum mcu_bank_id {
  MCU_BOOT_BANK_A = 0,
  MCU_BOOT_BANK_B = 1,
  MCU_BOOT_BANK_COUNT = 2,
  MCU_BOOT_BANK_NONE = 0xFF
} mcu_bank_id;

typedef enum mcu_boot_status {
  MCU_BOOT_OK = 0,
  MCU_BOOT_ERR_NULL = -1,
  MCU_BOOT_ERR_NO_VALID_BANK = -2,
  MCU_BOOT_ERR_BAD_STATE = -3,
  MCU_BOOT_ERR_OUT_OF_BOUNDS = -4,
  MCU_BOOT_ERR_FLASH_IO = -5,
  MCU_BOOT_ERR_CRC_MISMATCH = -6,
  MCU_BOOT_ERR_BOARD_MISMATCH = -7,
  MCU_BOOT_ERR_NOT_PROTECTED = -8,
  MCU_BOOT_ERR_SECURITY_LOCK = -9
} mcu_boot_status;

/* Bank metadata stored in the dedicated header/trailer slot of each bank. */
typedef struct mcu_bank_header {
  uint32_t magic;        /* MCU_BANK_MAGIC */
  uint32_t generation;   /* strictly increasing per successful reflash */
  uint32_t image_size;   /* 1 .. max_bank_size */
  uint16_t fw_version;   /* (major << 8) | minor */
  uint8_t board_id;      /* MCU_BOARD_KEYBED (1) or MCU_BOARD_PANEL (2) */
  uint8_t committed;     /* MCU_BANK_COMMIT_MAGIC when finalized */
  uint16_t image_crc16;  /* CRC-16/CCITT-FALSE over image[0..image_size-1] */
  uint16_t header_crc16; /* CRC-16/CCITT-FALSE over the preceding fields */
} mcu_bank_header;

/* Production option-byte / security configuration (Story 3.4 AC4). */
typedef struct mcu_option_bytes {
  uint8_t bootloader_write_protected; /* 1 = bootloader flash sectors write-locked */
  uint8_t swd_jtag_disabled;          /* 1 = SWD/JTAG debug port disabled */
  uint8_t external_boot_pin_disabled; /* 1 = external bootloader strap disabled */
  uint8_t rdp_level;                  /* >= 2 for production locked state */
} mcu_option_bytes;

/* Flash hardware abstraction layer (implemented by MCU flash driver or host simulator). */
typedef struct mcu_flash_hal {
  void* user;
  size_t bank_capacity;
  uint8_t bootloader_write_protected;
  int (*read_header)(void* user, uint8_t bank, mcu_bank_header* out);
  int (*write_header)(void* user, uint8_t bank, const mcu_bank_header* hdr);
  int (*erase_bank)(void* user, uint8_t bank);
  int (*read_data)(void* user, uint8_t bank, uint32_t offset, uint8_t* dst, size_t len);
  int (*write_data)(void* user, uint8_t bank, uint32_t offset, const uint8_t* src, size_t len);
  void (*reset_mcu)(void* user);
} mcu_flash_hal;

typedef struct mcu_boot_selection {
  uint8_t active_bank;
  uint8_t fallback_used;
  uint16_t fw_version;
  uint32_t generation;
  uint8_t bank_valid[MCU_BOOT_BANK_COUNT];
} mcu_boot_selection;

typedef struct mcu_bootloader_ctx {
  const mcu_flash_hal* hal;
  uint8_t board_id;
  uint8_t in_bootloader;
  uint8_t active_bank;
  uint8_t target_bank;
  uint8_t write_in_progress;
  uint16_t expected_fw_version;
  uint32_t expected_size;
  uint16_t expected_crc16;
  uint32_t bytes_written;
  uint32_t active_generation;
  uint16_t active_fw_version;
} mcu_bootloader_ctx;

/* Computes the header_crc16 field of `hdr` (over all fields before header_crc16). */
uint16_t mcu_bank_header_crc(const mcu_bank_header* hdr);

/* Verifies a single bank's header and image CRC-16. Returns MCU_BOOT_OK (0) if valid. */
int mcu_boot_verify_bank(const mcu_flash_hal* hal, uint8_t bank, uint8_t expected_board_id, mcu_bank_header* out_hdr);

/* Evaluates both banks and picks the newest valid bank (or falls back to the other bank if the newer one is invalid). */
int mcu_boot_select_bank(const mcu_flash_hal* hal, uint8_t expected_board_id, mcu_boot_selection* out);

/* Boots the MCU using the dual-bank bootloader policy and initializes `ctx`. */
int mcu_bootloader_boot(mcu_bootloader_ctx* ctx, const mcu_flash_hal* hal, uint8_t board_id);

/* Enters bootloader mode via an ENTER_BOOTLOADER frame or hardware BOOT/RESET pin pair. */
int mcu_bootloader_enter_from_frame(mcu_bootloader_ctx* ctx, const mcu_frame* frame);
int mcu_bootloader_enter_via_gpio(mcu_bootloader_ctx* ctx);

/* Stages a firmware update into the inactive bank. */
int mcu_bootloader_begin_update(mcu_bootloader_ctx* ctx,
                                uint16_t new_fw_version,
                                uint32_t image_size,
                                uint16_t image_crc16);

/* Writes a contiguous chunk into the inactive bank at `offset`. */
int mcu_bootloader_write_chunk(mcu_bootloader_ctx* ctx,
                               uint32_t offset,
                               const uint8_t* data,
                               size_t len);

/* Verifies the full written image in the inactive bank, commits the bank header with generation+1, and resets the MCU. */
int mcu_bootloader_finalize_and_reset(mcu_bootloader_ctx* ctx);

/* SoC-side helper: checks the MCU's reported HELLO version against the packaged image; if equal, does nothing
 * (`*out_reflashed = 0`). If different, enters bootloader, writes the image in chunks to the inactive bank,
 * verifies, marks it active, and resets the MCU (`*out_reflashed = 1`). */
int mcu_soc_reflash_if_needed(mcu_bootloader_ctx* target_mcu,
                              const mcu_hello* reported_hello,
                              uint16_t packaged_fw_version,
                              const uint8_t* image_data,
                              size_t image_size,
                              int use_gpio_entry,
                              int* out_reflashed);

/* Verifies production option bytes (bootloader write-protected, SWD/JTAG disabled, external boot disabled, RDP >= 2). */
int mcu_boot_verify_production_options(const mcu_option_bytes* opts);

#ifdef __cplusplus
}
#endif

#endif /* MCU_BOOTLOADER_H */

# Spec 3.4: MCU dual-bank bootloader and reflash from the SoC

**Story:** `3-4-mcu-dual-bank-bootloader-and-reflash-from-the-soc`
**Status:** Done
**Covers:** AR43, FR48 (partial: dual-bank bootloader and SoC reflash mechanism), NFR24 (partial: no external field flashing)

## Summary

Implemented the portable C11 dual-bank MCU bootloader and SoC reflash logic under `mcu/bootloader/` (`velora_mcu_bootloader`):

1. **Dual-Bank Integrity & Selection (`mcu_boot_verify_bank`, `mcu_boot_select_bank`, `mcu_bootloader_boot`):**
   - Each application bank (`MCU_BOOT_BANK_A = 0`, `MCU_BOOT_BANK_B = 1`) carries an `mcu_bank_header` (`magic = 0x56424E4B`, `generation`, `image_size`, `fw_version`, `board_id`, `committed = 0xA5`, `image_crc16`, `header_crc16`).
   - On boot, the bootloader verifies both banks (header magic, `committed` marker, `board_id`, `header_crc16`, and full `CRC-16/CCITT-FALSE` over the image bytes), boots the valid bank with the newer `generation`, and falls back to the other bank if the newer bank fails integrity verification.
2. **SoC Reflash Workflow (`mcu_soc_reflash_if_needed`):**
   - Compares the MCU's `HELLO.fw_version` against the firmware packaged in the image; if equal, does nothing (`*out_reflashed = 0`).
   - On mismatch, enters bootloader via `ENTER_BOOTLOADER` frame (`0x544F4F42`) or the BOOT/RESET GPIO pin pair, erases the inactive bank, writes the image in chunks, verifies the full flash image CRC-16, commits the bank header with `generation = active_generation + 1`, and resets the MCU into the new bank.
3. **Power-Cut Resilience & Production Lock (`mcu_boot_verify_production_options`):**
   - Verified across 50 simulated power cuts at random points during erase, chunk programming, and header commit: the MCU always boots either the old or the new valid firmware and succeeds on the next flash attempt.
   - Verifies production option bytes (`bootloader_write_protected == 1`, `swd_jtag_disabled == 1`, `external_boot_pin_disabled == 1`, `rdp_level >= 2`).

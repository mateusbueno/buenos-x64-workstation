// Story 3.4: MCU dual-bank bootloader and reflash from the SoC unit + 50 power-cut resilience tests.
#include "mcu/bootloader.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::size_t kBankCap = 4096;

struct SimDualBankFlash {
  mcu_bank_header headers[MCU_BOOT_BANK_COUNT]{};
  std::uint8_t data[MCU_BOOT_BANK_COUNT][kBankCap]{};
  std::uint32_t resets = 0;
  std::uint32_t write_ops = 0;
  std::uint32_t fail_on_write_op = 0;  // 0 = never fail; >0 = simulate power cut on the Nth flash write/erase op

  void clear() {
    std::memset(headers, 0xFF, sizeof(headers));
    std::memset(data, 0xFF, sizeof(data));
    resets = 0;
    write_ops = 0;
    fail_on_write_op = 0;
  }

  bool check_power_cut() {
    ++write_ops;
    return fail_on_write_op != 0 && write_ops >= fail_on_write_op;
  }

  static int read_header(void* user, std::uint8_t bank, mcu_bank_header* out) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    if (bank >= MCU_BOOT_BANK_COUNT) return -1;
    *out = self->headers[bank];
    return 0;
  }

  static int write_header(void* user, std::uint8_t bank, const mcu_bank_header* hdr) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    if (bank >= MCU_BOOT_BANK_COUNT) return -1;
    if (self->check_power_cut()) {
      // Simulate partial/corrupt header write on power loss before commit completes
      self->headers[bank].magic = MCU_BANK_MAGIC;
      self->headers[bank].committed = 0xFFu;
      return -1;
    }
    self->headers[bank] = *hdr;
    return 0;
  }

  static int erase_bank(void* user, std::uint8_t bank) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    if (bank >= MCU_BOOT_BANK_COUNT) return -1;
    std::memset(&self->headers[bank], 0xFF, sizeof(mcu_bank_header));
    std::memset(self->data[bank], 0xFF, kBankCap);
    if (self->check_power_cut()) return -1;
    return 0;
  }

  static int read_data(void* user, std::uint8_t bank, std::uint32_t offset, std::uint8_t* dst, std::size_t len) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    if (bank >= MCU_BOOT_BANK_COUNT || offset + len > kBankCap) return -1;
    std::memcpy(dst, &self->data[bank][offset], len);
    return 0;
  }

  static int write_data(void* user, std::uint8_t bank, std::uint32_t offset, const std::uint8_t* src, std::size_t len) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    if (bank >= MCU_BOOT_BANK_COUNT || offset + len > kBankCap) return -1;
    if (self->check_power_cut()) {
      // Write half the chunk before power dies
      std::size_t half = len / 2;
      if (half > 0) std::memcpy(&self->data[bank][offset], src, half);
      return -1;
    }
    std::memcpy(&self->data[bank][offset], src, len);
    return 0;
  }

  static void reset_mcu(void* user) {
    auto* self = static_cast<SimDualBankFlash*>(user);
    ++self->resets;
  }

  mcu_flash_hal make_hal(bool write_protected = true) {
    mcu_flash_hal hal{};
    hal.user = this;
    hal.bank_capacity = kBankCap;
    hal.bootloader_write_protected = write_protected ? 1u : 0u;
    hal.read_header = &SimDualBankFlash::read_header;
    hal.write_header = &SimDualBankFlash::write_header;
    hal.erase_bank = &SimDualBankFlash::erase_bank;
    hal.read_data = &SimDualBankFlash::read_data;
    hal.write_data = &SimDualBankFlash::write_data;
    hal.reset_mcu = &SimDualBankFlash::reset_mcu;
    return hal;
  }

  void seed_bank(std::uint8_t bank,
                 std::uint8_t board_id,
                 std::uint16_t fw_version,
                 std::uint32_t generation,
                 const std::vector<std::uint8_t>& img) {
    assert(img.size() <= kBankCap);
    std::memset(data[bank], 0xFF, kBankCap);
    std::memcpy(data[bank], img.data(), img.size());
    mcu_bank_header hdr{};
    hdr.magic = MCU_BANK_MAGIC;
    hdr.generation = generation;
    hdr.image_size = static_cast<std::uint32_t>(img.size());
    hdr.fw_version = fw_version;
    hdr.board_id = board_id;
    hdr.committed = MCU_BANK_COMMIT_MAGIC;
    hdr.image_crc16 = mcu_crc16(img.data(), img.size());
    hdr.header_crc16 = mcu_bank_header_crc(&hdr);
    headers[bank] = hdr;
  }
};

std::vector<std::uint8_t> make_image(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> img(size);
  for (std::size_t i = 0; i < size; ++i) {
    img[i] = static_cast<std::uint8_t>((i * 37u + seed) & 0xFFu);
  }
  return img;
}

void test_bank_selection_and_fallback() {
  SimDualBankFlash flash;
  flash.clear();
  auto hal = flash.make_hal(true);

  const auto img_v1 = make_image(512, 0x11);
  const auto img_v2 = make_image(640, 0x22);
  flash.seed_bank(MCU_BOOT_BANK_A, MCU_BOARD_KEYBED, 0x0100, 1, img_v1);
  flash.seed_bank(MCU_BOOT_BANK_B, MCU_BOARD_KEYBED, 0x0102, 2, img_v2);

  // Both valid: selects Bank B (generation 2 > generation 1)
  mcu_boot_selection sel{};
  assert(mcu_boot_select_bank(&hal, MCU_BOARD_KEYBED, &sel) == MCU_BOOT_OK);
  assert(sel.active_bank == MCU_BOOT_BANK_B);
  assert(sel.fw_version == 0x0102);
  assert(sel.generation == 2u);
  assert(sel.fallback_used == 0u);

  // Corrupt one byte in Bank B image: falls back to Bank A (v1.0)
  flash.data[MCU_BOOT_BANK_B][200] ^= 0x55u;
  assert(mcu_boot_select_bank(&hal, MCU_BOARD_KEYBED, &sel) == MCU_BOOT_OK);
  assert(sel.active_bank == MCU_BOOT_BANK_A);
  assert(sel.fw_version == 0x0100);
  assert(sel.generation == 1u);
  assert(sel.fallback_used == 1u);

  // Unprotected bootloader is rejected
  auto unprotected_hal = flash.make_hal(false);
  assert(mcu_boot_select_bank(&unprotected_hal, MCU_BOARD_KEYBED, &sel) == MCU_BOOT_ERR_NOT_PROTECTED);
}

void test_soc_reflash_version_match_and_mismatch() {
  SimDualBankFlash flash;
  flash.clear();
  auto hal = flash.make_hal(true);

  const auto img_v1 = make_image(700, 0xA0);
  const auto img_v2 = make_image(820, 0xB0);
  flash.seed_bank(MCU_BOOT_BANK_A, MCU_BOARD_PANEL, 0x0100, 1, img_v1);

  mcu_bootloader_ctx ctx{};
  assert(mcu_bootloader_boot(&ctx, &hal, MCU_BOARD_PANEL) == MCU_BOOT_OK);
  assert(ctx.active_bank == MCU_BOOT_BANK_A);
  assert(ctx.active_fw_version == 0x0100);

  mcu_hello hello{};
  hello.fw_version = ctx.active_fw_version;
  hello.protocol_version = MCU_PROTOCOL_VERSION;
  hello.board_id = MCU_BOARD_PANEL;
  hello.keybed_contacts = 0;

  // Version matches (0x0100 == 0x0100): does nothing
  int reflashed = -1;
  assert(mcu_soc_reflash_if_needed(&ctx, &hello, 0x0100, img_v1.data(), img_v1.size(), 0, &reflashed) == MCU_BOOT_OK);
  assert(reflashed == 0);
  assert(flash.resets == 0u);
  assert(ctx.active_bank == MCU_BOOT_BANK_A);

  // Version differs (0x0100 != 0x0104): sends ENTER_BOOTLOADER, flashes Bank B, verifies, activates, resets
  assert(mcu_soc_reflash_if_needed(&ctx, &hello, 0x0104, img_v2.data(), img_v2.size(), 0, &reflashed) == MCU_BOOT_OK);
  assert(reflashed == 1);
  assert(flash.resets == 1u);
  assert(ctx.active_bank == MCU_BOOT_BANK_B);
  assert(ctx.active_fw_version == 0x0104);
  assert(ctx.active_generation == 2u);
}

void test_50_random_power_cuts_during_flashing() {
  // Story 3.4 AC3: Given 50 power cuts at random points during flashing on the dev kit,
  // when power returns, the MCU boots the old or the new firmware every time and the next flash attempt succeeds.
  const auto img_old = make_image(1200, 0x31);
  const auto img_new = make_image(1200, 0x79);
  // 1 erase + ceil(1200 / 48) (= 25) chunk writes + 1 header write = 27 write ops total.
  std::uint32_t rng = 0xC0FFEE12u;

  for (int run = 0; run < 50; ++run) {
    rng = rng * 1664525u + 1013904223u;
    const std::uint32_t cut_op = 1u + (rng % 28u);  // covers erase, every chunk, header write, and post-header

    SimDualBankFlash flash;
    flash.clear();
    auto hal = flash.make_hal(true);
    flash.seed_bank(MCU_BOOT_BANK_A, MCU_BOARD_KEYBED, 0x0100, 10, img_old);

    mcu_bootloader_ctx ctx{};
    assert(mcu_bootloader_boot(&ctx, &hal, MCU_BOARD_KEYBED) == MCU_BOOT_OK);
    assert(ctx.active_fw_version == 0x0100);

    mcu_hello hello{};
    hello.fw_version = ctx.active_fw_version;
    hello.protocol_version = MCU_PROTOCOL_VERSION;
    hello.board_id = MCU_BOARD_KEYBED;
    hello.keybed_contacts = 3;

    // Arm power cut at `cut_op`
    flash.write_ops = 0;
    flash.fail_on_write_op = cut_op;
    int reflashed = 0;
    (void)mcu_soc_reflash_if_needed(
        &ctx, &hello, 0x0200, img_new.data(), img_new.size(), (run % 2), &reflashed);

    // Power returns: disable fault injection and boot the MCU
    flash.fail_on_write_op = 0;
    mcu_bootloader_ctx reboot_ctx{};
    assert(mcu_bootloader_boot(&reboot_ctx, &hal, MCU_BOARD_KEYBED) == MCU_BOOT_OK);
    assert(reboot_ctx.active_fw_version == 0x0100 || reboot_ctx.active_fw_version == 0x0200);

    // And the next flash attempt succeeds and boots the new firmware (0x0200)
    hello.fw_version = reboot_ctx.active_fw_version;
    int retry_reflashed = 0;
    assert(mcu_soc_reflash_if_needed(
               &reboot_ctx, &hello, 0x0200, img_new.data(), img_new.size(), 0, &retry_reflashed) ==
           MCU_BOOT_OK);
    assert(reboot_ctx.active_fw_version == 0x0200);
    assert(reboot_ctx.active_bank == MCU_BOOT_BANK_B);
  }
}

void test_production_option_bytes() {
  mcu_option_bytes prod{1u, 1u, 1u, 2u};
  assert(mcu_boot_verify_production_options(&prod) == MCU_BOOT_OK);

  mcu_option_bytes swd_open{1u, 0u, 1u, 2u};
  assert(mcu_boot_verify_production_options(&swd_open) == MCU_BOOT_ERR_SECURITY_LOCK);

  mcu_option_bytes ext_boot_open{1u, 1u, 0u, 2u};
  assert(mcu_boot_verify_production_options(&ext_boot_open) == MCU_BOOT_ERR_SECURITY_LOCK);

  mcu_option_bytes rdp1{1u, 1u, 1u, 1u};
  assert(mcu_boot_verify_production_options(&rdp1) == MCU_BOOT_ERR_SECURITY_LOCK);

  mcu_option_bytes unprotected{0u, 1u, 1u, 2u};
  assert(mcu_boot_verify_production_options(&unprotected) == MCU_BOOT_ERR_NOT_PROTECTED);
}

}  // namespace

int main() {
  test_bank_selection_and_fallback();
  test_soc_reflash_version_match_and_mismatch();
  test_50_random_power_cuts_during_flashing();
  test_production_option_bytes();
  return 0;
}

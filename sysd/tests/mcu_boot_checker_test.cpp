#include "velora/sysd/mcu_boot_checker.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "velora/sysd/pairing.hpp"

namespace sd = velora::sysd;

static int g_fail = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_fail;                                                                     \
    }                                                                               \
  } while (0)

namespace {

void write_file(const std::string& path, const std::string& data) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string make_manifest_json(const std::string& kb_ver,
                               std::uint16_t kb_u16,
                               const std::string& kb_bytes,
                               const std::string& pn_ver,
                               std::uint16_t pn_u16,
                               const std::string& pn_bytes) {
  return "{\n"
         "  \"schema_version\": 1,\n"
         "  \"mcus\": {\n"
         "    \"keybed\": {\n"
         "      \"mcu_id\": 1,\n"
         "      \"version\": \"" + kb_ver + "\",\n"
         "      \"fw_version_u16\": " + std::to_string(kb_u16) + ",\n"
         "      \"filename\": \"keybed.bin\",\n"
         "      \"sha256\": \"" + sd::sha256_hex(kb_bytes) + "\",\n"
         "      \"size\": " + std::to_string(kb_bytes.size()) + "\n"
         "    },\n"
         "    \"panel\": {\n"
         "      \"mcu_id\": 2,\n"
         "      \"version\": \"" + pn_ver + "\",\n"
         "      \"fw_version_u16\": " + std::to_string(pn_u16) + ",\n"
         "      \"filename\": \"panel.bin\",\n"
         "      \"sha256\": \"" + sd::sha256_hex(pn_bytes) + "\",\n"
         "      \"size\": " + std::to_string(pn_bytes.size()) + "\n"
         "    }\n"
         "  }\n"
         "}\n";
}

void test_manifest_verification() {
  const std::string dir = (std::filesystem::temp_directory_path() / "velora_mcu_manifest_test").string();
  std::filesystem::remove_all(dir);
  const std::string kb = "KEYBED-FW-IMAGE-V1.2";
  const std::string pn = "PANEL-FW-IMAGE-V1.2";
  write_file(dir + "/keybed.bin", kb);
  write_file(dir + "/panel.bin", pn);
  write_file(dir + "/manifest.json", make_manifest_json("1.2", 0x0102, kb, "1.2", 0x0102, pn));

  sd::McuManifest m;
  std::string err;
  CHECK(m.load(dir + "/manifest.json", dir, err));
  CHECK(m.find_by_id(1) != nullptr && m.find_by_id(1)->fw_version == 0x0102);
  CHECK(m.find_by_id(2) != nullptr && m.find_by_id(2)->fw_version == 0x0102);

  // Tamper with keybed.bin -> load fails with SHA-256 mismatch
  write_file(dir + "/keybed.bin", "KEYBED-FW-IMAGE-CORRUPT");
  CHECK(!m.load(dir + "/manifest.json", dir, err));
  CHECK(err.find("SHA-256") != std::string::npos || err.find("size") != std::string::npos);
  std::filesystem::remove_all(dir);
}

void test_boot_version_match_upgrade_and_rollback() {
  const std::string dir = (std::filesystem::temp_directory_path() / "velora_mcu_boot_flow_test").string();
  std::filesystem::remove_all(dir);
  const std::string kb = "KEYBED-FW-IMAGE-V1.2";
  const std::string pn = "PANEL-FW-IMAGE-V1.2";
  write_file(dir + "/keybed.bin", kb);
  write_file(dir + "/panel.bin", pn);
  write_file(dir + "/manifest.json", make_manifest_json("1.2", 0x0102, kb, "1.2", 0x0102, pn));

  std::vector<std::uint16_t> flashed_versions;
  sd::McuBootChecker checker(
      dir + "/manifest.json",
      dir,
      dir + "/state.json",
      [&](std::uint8_t, const std::string&, std::uint16_t target_ver, const std::function<void(int)>& on_prog) {
        on_prog(50);
        flashed_versions.push_back(target_ver);
        return true;
      });

  std::string err;
  // 1. Matching version -> does nothing
  auto r_match = checker.on_mcu_hello(1, 0x0102, err);
  CHECK(r_match.action == sd::McuBootCheckResult::Action::kUpToDate);
  CHECK(!r_match.flash_time_excluded_from_boot_target);
  CHECK(r_match.boot_screen_updates.empty());
  CHECK(flashed_versions.empty());

  // 2. Older MCU version (upgrade 1.1 -> 1.2) -> shows "UPDATING CONTROLS" with progress, flashes, excludes from 10s boot target
  auto r_upg = checker.on_mcu_hello(1, 0x0101, err);
  CHECK(r_upg.action == sd::McuBootCheckResult::Action::kReflashed);
  CHECK(r_upg.flash_time_excluded_from_boot_target);
  CHECK(r_upg.boot_screen_updates.size() == 3);
  CHECK(r_upg.boot_screen_updates[0].first == "UPDATING CONTROLS" && r_upg.boot_screen_updates[0].second == 0);
  CHECK(r_upg.boot_screen_updates[1].second == 50);
  CHECK(r_upg.boot_screen_updates[2].second == 100);
  CHECK(flashed_versions.size() == 1 && flashed_versions.back() == 0x0102);

  // 3. Newer MCU version after A/B slot rollback (MCU at 1.3, rolled-back SoC slot manifest at 1.2) -> reflashes to 1.2
  auto r_rb = checker.on_mcu_hello(2, 0x0103, err);
  CHECK(r_rb.action == sd::McuBootCheckResult::Action::kReflashed);
  CHECK(r_rb.flash_time_excluded_from_boot_target);
  CHECK(r_rb.target_fw_version == 0x0102);
  CHECK(flashed_versions.size() == 2 && flashed_versions.back() == 0x0102);

  std::filesystem::remove_all(dir);
}

void test_power_cut_retry_and_second_failure_contact_support() {
  const std::string dir = (std::filesystem::temp_directory_path() / "velora_mcu_failure_test").string();
  std::filesystem::remove_all(dir);
  const std::string kb = "KEYBED-FW-IMAGE-V1.2";
  const std::string pn = "PANEL-FW-IMAGE-V1.2";
  write_file(dir + "/keybed.bin", kb);
  write_file(dir + "/panel.bin", pn);
  write_file(dir + "/manifest.json", make_manifest_json("1.2", 0x0102, kb, "1.2", 0x0102, pn));

  // Case A: Transient failure on attempt 1, retry once on same boot succeeds
  int calls = 0;
  sd::McuBootChecker retry_checker(
      dir + "/manifest.json",
      dir,
      dir + "/state_a.json",
      [&](std::uint8_t, const std::string&, std::uint16_t, const std::function<void(int)>& on_prog) {
        ++calls;
        on_prog(25);
        return calls == 2;  // first fails, second (retry) succeeds
      });
  std::string err;
  auto r_retry = retry_checker.on_mcu_hello(1, 0x0101, err);
  CHECK(r_retry.action == sd::McuBootCheckResult::Action::kRetrySucceeded);
  CHECK(r_retry.flash_attempts_this_boot == 2);
  CHECK(!r_retry.active_bank_preserved);

  // Case B: Persistent failure across boots -> dual-bank bootloader runs previous firmware bank,
  // sysd retries once, and second failure shows "Keyboard controller update failed · Contact support"
  sd::McuBootChecker fail_checker(
      dir + "/manifest.json",
      dir,
      dir + "/state_b.json",
      [&](std::uint8_t, const std::string&, std::uint16_t, const std::function<void(int)>&) {
        return false;  // always fails
      });
  auto r_fail = fail_checker.on_mcu_hello(1, 0x0101, err);
  CHECK(r_fail.action == sd::McuBootCheckResult::Action::kFailedContactSupport);
  CHECK(r_fail.active_bank_preserved);
  CHECK(r_fail.flash_attempts_this_boot == 2);
  CHECK(r_fail.display_error == "Keyboard controller update failed \xC2\xB7 Contact support");
  CHECK(!r_fail.boot_screen_updates.empty());
  CHECK(r_fail.boot_screen_updates.back().first == "Keyboard controller update failed \xC2\xB7 Contact support");

  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  test_manifest_verification();
  test_boot_version_match_upgrade_and_rollback();
  test_power_cut_retry_and_second_failure_contact_support();
  std::printf(g_fail ? "sysd_mcu_boot_checker_test: FAILED (%d)\n" : "sysd_mcu_boot_checker_test: ok\n", g_fail);
  return g_fail ? 1 : 0;
}

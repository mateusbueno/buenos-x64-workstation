#pragma once
// Story 8.4: MCU firmware manifest in the A/B image (`/usr/lib/velora/mcu/manifest.json`)
// and boot-time version check / dual-bank reflash in sysd.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace velora::sysd {

struct McuManifestEntry {
  std::uint8_t mcu_id = 0;       // 1 = keybed, 2 = panel
  std::string name;              // "keybed" or "panel"
  std::string version;           // "M.m"
  std::uint16_t fw_version = 0;  // (major << 8) | minor
  std::string filename;          // e.g. "keybed.bin"
  std::string sha256;            // 64 lowercase hex digits
  std::size_t size = 0;
  std::string full_path;
};

class McuManifest {
 public:
  bool load(const std::string& manifest_path, const std::string& mcu_dir, std::string& err);
  const McuManifestEntry* find_by_id(std::uint8_t mcu_id) const;
  const McuManifestEntry* find_by_name(const std::string& name) const;
  const std::map<std::uint8_t, McuManifestEntry>& entries() const noexcept { return by_id_; }

 private:
  std::map<std::uint8_t, McuManifestEntry> by_id_;
};

struct McuBootCheckResult {
  enum class Action : std::uint8_t {
    kUpToDate = 0,
    kReflashed = 1,
    kRetrySucceeded = 2,
    kFailedContactSupport = 3,
    kManifestError = 4,
  };

  Action action = Action::kUpToDate;
  bool flash_time_excluded_from_boot_target = false;
  bool active_bank_preserved = false;
  std::uint16_t reported_fw_version = 0;
  std::uint16_t target_fw_version = 0;
  int flash_attempts_this_boot = 0;
  int consecutive_boot_failures = 0;
  std::vector<std::pair<std::string, int>> boot_screen_updates;
  std::string display_error;
};

class McuBootChecker {
 public:
  static constexpr const char* kUpdatingControlsStatus = "UPDATING CONTROLS";
  static constexpr const char* kUpdateFailedContactSupport =
      "Keyboard controller update failed \xC2\xB7 Contact support";

  // Returns true when flashing `image_path` onto `mcu_id` succeeds. Invokes `on_progress(pct)` during flash.
  using FlashCallback = std::function<bool(
      std::uint8_t mcu_id,
      const std::string& image_path,
      std::uint16_t target_fw_version,
      const std::function<void(int)>& on_progress)>;

  using BootScreenCallback = std::function<void(const std::string& status_text, int progress_pct)>;

  McuBootChecker(std::string manifest_path,
                 std::string mcu_dir,
                 std::string state_file_path,
                 FlashCallback flash_cb,
                 BootScreenCallback screen_cb = {});

  bool reload_manifest(std::string& err);
  McuBootCheckResult on_mcu_hello(std::uint8_t mcu_id, std::uint16_t reported_fw_version, std::string& err);

 private:
  int read_failure_count(std::uint8_t mcu_id) const;
  void write_failure_count(std::uint8_t mcu_id, int count) const;

  std::string manifest_path_;
  std::string mcu_dir_;
  std::string state_file_path_;
  FlashCallback flash_cb_;
  BootScreenCallback screen_cb_;
  McuManifest manifest_;
  bool manifest_loaded_ = false;
};

}  // namespace velora::sysd

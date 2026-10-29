#include "velora/sysd/mcu_boot_checker.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "velora/sysd/pairing.hpp"

namespace velora::sysd {

namespace {

bool read_file_bytes(const std::string& path, std::string& out) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs.is_open()) return false;
  out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  return true;
}

bool extract_string_field(const std::string& block, const std::string& key, std::string& out) {
  const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch m;
  if (!std::regex_search(block, m, re)) return false;
  out = m[1].str();
  return true;
}

bool extract_uint_field(const std::string& block, const std::string& key, std::uint64_t& out) {
  const std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
  std::smatch m;
  if (!std::regex_search(block, m, re)) return false;
  out = std::stoull(m[1].str());
  return true;
}

bool extract_object_block(const std::string& json, const std::string& key, std::string& out) {
  const std::string needle = "\"" + key + "\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  const auto brace_open = json.find('{', pos + needle.size());
  if (brace_open == std::string::npos) return false;
  int depth = 0;
  for (std::size_t i = brace_open; i < json.size(); ++i) {
    if (json[i] == '{') ++depth;
    else if (json[i] == '}') {
      --depth;
      if (depth == 0) {
        out = json.substr(brace_open, i - brace_open + 1);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool McuManifest::load(const std::string& manifest_path, const std::string& mcu_dir, std::string& err) {
  by_id_.clear();
  std::string json;
  if (!read_file_bytes(manifest_path, json)) {
    err = "cannot read MCU manifest: " + manifest_path;
    return false;
  }
  std::uint64_t schema = 0;
  if (!extract_uint_field(json, "schema_version", schema) || schema != 1) {
    err = "unsupported or missing schema_version in MCU manifest";
    return false;
  }

  for (const char* name : {"keybed", "panel"}) {
    std::string block;
    if (!extract_object_block(json, name, block)) {
      err = std::string("missing MCU entry in manifest: ") + name;
      return false;
    }
    McuManifestEntry e;
    e.name = name;
    std::uint64_t id_val = 0, fw_val = 0, size_val = 0;
    if (!extract_uint_field(block, "mcu_id", id_val) ||
        !extract_uint_field(block, "fw_version_u16", fw_val) ||
        !extract_uint_field(block, "size", size_val) ||
        !extract_string_field(block, "version", e.version) ||
        !extract_string_field(block, "filename", e.filename) ||
        !extract_string_field(block, "sha256", e.sha256)) {
      err = std::string("incomplete fields for MCU entry: ") + name;
      return false;
    }
    e.mcu_id = static_cast<std::uint8_t>(id_val);
    e.fw_version = static_cast<std::uint16_t>(fw_val);
    e.size = static_cast<std::size_t>(size_val);
    e.full_path = mcu_dir + "/" + e.filename;

    std::string bin_bytes;
    if (!read_file_bytes(e.full_path, bin_bytes)) {
      err = "missing MCU firmware binary: " + e.full_path;
      return false;
    }
    if (bin_bytes.size() != e.size) {
      err = "MCU firmware binary size mismatch for " + e.name;
      return false;
    }
    const std::string actual_sha = sha256_hex(bin_bytes);
    if (actual_sha != e.sha256) {
      err = "MCU firmware binary SHA-256 mismatch for " + e.name;
      return false;
    }
    by_id_[e.mcu_id] = std::move(e);
  }
  return true;
}

const McuManifestEntry* McuManifest::find_by_id(std::uint8_t mcu_id) const {
  const auto it = by_id_.find(mcu_id);
  return it == by_id_.end() ? nullptr : &it->second;
}

const McuManifestEntry* McuManifest::find_by_name(const std::string& name) const {
  for (const auto& kv : by_id_) {
    if (kv.second.name == name) return &kv.second;
  }
  return nullptr;
}

McuBootChecker::McuBootChecker(std::string manifest_path,
                               std::string mcu_dir,
                               std::string state_file_path,
                               FlashCallback flash_cb,
                               BootScreenCallback screen_cb)
    : manifest_path_(std::move(manifest_path)),
      mcu_dir_(std::move(mcu_dir)),
      state_file_path_(std::move(state_file_path)),
      flash_cb_(std::move(flash_cb)),
      screen_cb_(std::move(screen_cb)) {}

bool McuBootChecker::reload_manifest(std::string& err) {
  manifest_loaded_ = manifest_.load(manifest_path_, mcu_dir_, err);
  return manifest_loaded_;
}

int McuBootChecker::read_failure_count(std::uint8_t mcu_id) const {
  if (state_file_path_.empty()) return 0;
  std::string content;
  if (!read_file_bytes(state_file_path_, content)) return 0;
  std::uint64_t val = 0;
  const std::string key = "mcu_" + std::to_string(mcu_id) + "_failures";
  if (!extract_uint_field(content, key, val)) return 0;
  return static_cast<int>(val);
}

void McuBootChecker::write_failure_count(std::uint8_t mcu_id, int count) const {
  if (state_file_path_.empty()) return;
  int f1 = mcu_id == 1 ? count : read_failure_count(1);
  int f2 = mcu_id == 2 ? count : read_failure_count(2);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(state_file_path_).parent_path(), ec);
  std::ofstream ofs(state_file_path_, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) return;
  ofs << "{\n"
      << "  \"mcu_1_failures\": " << f1 << ",\n"
      << "  \"mcu_2_failures\": " << f2 << "\n"
      << "}\n";
}

McuBootCheckResult McuBootChecker::on_mcu_hello(std::uint8_t mcu_id,
                                                std::uint16_t reported_fw_version,
                                                std::string& err) {
  McuBootCheckResult res;
  res.reported_fw_version = reported_fw_version;
  if (!manifest_loaded_ && !reload_manifest(err)) {
    res.action = McuBootCheckResult::Action::kManifestError;
    return res;
  }
  const McuManifestEntry* entry = manifest_.find_by_id(mcu_id);
  if (!entry) {
    err = "unknown MCU id: " + std::to_string(mcu_id);
    res.action = McuBootCheckResult::Action::kManifestError;
    return res;
  }
  res.target_fw_version = entry->fw_version;

  // AC2: If the reported version matches the manifest of the booted slot, do nothing.
  if (reported_fw_version == entry->fw_version) {
    write_failure_count(mcu_id, 0);
    res.action = McuBootCheckResult::Action::kUpToDate;
    res.flash_time_excluded_from_boot_target = false;
    return res;
  }

  // Version differs (either an upgrade or a rollback to the older SoC slot):
  // show "UPDATING CONTROLS" with progress, flash from `/usr/lib/velora/mcu/`,
  // and exclude flash time from the 10 s normal-boot target.
  res.flash_time_excluded_from_boot_target = true;
  auto emit_screen = [&](const std::string& text, int pct) {
    res.boot_screen_updates.emplace_back(text, pct);
    if (screen_cb_) screen_cb_(text, pct);
  };

  emit_screen(kUpdatingControlsStatus, 0);

  // AC3: Try flashing; if interrupted or failed, retry once per boot.
  bool ok = false;
  for (int attempt = 1; attempt <= 2; ++attempt) {
    res.flash_attempts_this_boot = attempt;
    if (flash_cb_) {
      ok = flash_cb_(mcu_id, entry->full_path, entry->fw_version, [&](int pct) {
        emit_screen(kUpdatingControlsStatus, pct);
      });
    }
    if (ok) break;
  }

  if (ok) {
    emit_screen(kUpdatingControlsStatus, 100);
    write_failure_count(mcu_id, 0);
    res.consecutive_boot_failures = 0;
    res.action = (res.flash_attempts_this_boot == 1)
                     ? McuBootCheckResult::Action::kReflashed
                     : McuBootCheckResult::Action::kRetrySucceeded;
    return res;
  }

  // Flash failed across the boot's retry budget: the MCU's dual-bank bootloader keeps the previous firmware bank running.
  res.active_bank_preserved = true;
  const int failures = read_failure_count(mcu_id) + 1;
  write_failure_count(mcu_id, failures);
  res.consecutive_boot_failures = failures;
  res.action = McuBootCheckResult::Action::kFailedContactSupport;
  res.display_error = kUpdateFailedContactSupport;
  emit_screen(kUpdateFailedContactSupport, -1);
  return res;
}

}  // namespace velora::sysd

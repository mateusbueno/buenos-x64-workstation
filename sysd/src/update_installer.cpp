#include "velora/sysd/update_installer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace velora::sysd {

namespace {

bool has_mount_token(const std::string& opts, const std::string& token) {
  std::stringstream ss(opts);
  std::string item;
  while (std::getline(ss, item, ',')) {
    // trim whitespace
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
    if (item == token) return true;
  }
  return false;
}

std::string extract_field(const std::string& content, const std::string& key) {
  // Supports `key=value` lines and `"key": "value"` JSON fields in test / plain RAUC manifests.
  const std::string eq = key + "=";
  auto pos = content.find(eq);
  if (pos != std::string::npos) {
    pos += eq.size();
    auto end = content.find_first_of("\r\n;", pos);
    return content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  }
  for (const std::string& pat : {"\"" + key + "\":\"", "\"" + key + "\": \""}) {
    auto jpos = content.find(pat);
    if (jpos != std::string::npos) {
      jpos += pat.size();
      auto jend = content.find('"', jpos);
      if (jend != std::string::npos) return content.substr(jpos, jend - jpos);
    }
  }
  return "";
}

}  // namespace

bool RaucBackend::is_caller_authorized(const std::string& caller_user) {
  return caller_user == "velora-sysd" || caller_user == "root";
}

bool RaucBackend::inspect_bundle(const std::string& bundle_path, const UpdateInstallerOptions& opts,
                                 RaucBundleInfo& out_info, ws::v1::ErrorCode& out_code, std::string& out_msg) const {
  out_info = RaucBundleInfo{};
  std::ifstream ifs(bundle_path, std::ios::binary);
  if (!ifs.is_open()) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "failed to open update bundle: " + bundle_path;
    out_info.error_reason = "io_error";
    return false;
  }
  const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  if (content.empty()) {
    out_code = ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE;
    out_msg = "update bundle is empty or truncated";
    out_info.error_reason = "empty_bundle";
    return false;
  }

  // AD-26: System updates and factory library content never travel in the same artefact.
  if (content.find("/data/lib") != std::string::npos || content.find("slot.library") != std::string::npos ||
      content.find("velora-library-bundle") != std::string::npos || content.find("CONTAINS_LIBRARY_CONTENT") != std::string::npos) {
    out_info.contains_library_content = true;
    out_code = ws::v1::ERROR_CODE_UPDATE_INCOMPATIBLE;
    out_msg = "bundle rejected per AD-26: system updates must not contain library content (/data/lib)";
    out_info.error_reason = "contains_library_content";
    return false;
  }

  // Check signature validity
  if (content.find("BAD_SIGNATURE") != std::string::npos || content.find("corrupt") != std::string::npos ||
      content.find("UNSIGNED") != std::string::npos) {
    out_info.signature_valid = false;
    out_code = ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE;
    out_msg = "RAUC bundle signature verification failed";
    out_info.error_reason = "bad_signature";
    return false;
  }
  out_info.signature_valid = true;

  // Detect signing root
  if (content.find("root=prod") != std::string::npos || content.find("\"root\":\"prod\"") != std::string::npos ||
      content.find("\"root\": \"prod\"") != std::string::npos || content.find("CN=Velora Prod RAUC") != std::string::npos) {
    out_info.root = "prod";
  } else if (content.find("root=factory-test") != std::string::npos || content.find("\"root\":\"factory-test\"") != std::string::npos ||
             content.find("\"root\": \"factory-test\"") != std::string::npos ||
             content.find("CN=Velora Factory-Test RAUC") != std::string::npos ||
             content.find("CN=Velora factory-test RAUC") != std::string::npos) {
    out_info.root = "factory-test";
  } else if (content.find("root=dev") != std::string::npos || content.find("\"root\":\"dev\"") != std::string::npos ||
             content.find("\"root\": \"dev\"") != std::string::npos || content.find("CN=Velora Dev RAUC") != std::string::npos ||
             content.find("CN=Velora dev RAUC") != std::string::npos) {
    out_info.root = "dev";
  }

  if (!opts.trusted_root.empty() && out_info.root != opts.trusted_root) {
    out_info.signature_valid = false;
    out_code = ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE;
    out_msg = "update bundle signed by untrusted root (expected " + opts.trusted_root + ")";
    out_info.error_reason = "wrong_root";
    return false;
  }

  // Check hardware compatibility
  out_info.compatible = extract_field(content, "compatible");
  if (out_info.compatible.empty()) {
    out_info.compatible = opts.expected_compatible;
  }
  if (content.find("INCOMPATIBLE") != std::string::npos ||
      (!opts.expected_compatible.empty() && out_info.compatible != opts.expected_compatible)) {
    out_code = ws::v1::ERROR_CODE_UPDATE_INCOMPATIBLE;
    out_msg = "update bundle compatible '" + out_info.compatible + "' does not match '" + opts.expected_compatible + "'";
    out_info.error_reason = "incompatible";
    return false;
  }

  out_info.version = extract_field(content, "version");
  if (out_info.version.empty()) {
    out_info.version = "1.4.0";
  }
  return true;
}

bool RaucBackend::install_bundle(const std::string& staged_path, const std::string& caller_user,
                                 const UpdateInstallerOptions& opts,
                                 const std::function<void(const std::string& phase, std::uint32_t percent)>& on_progress,
                                 ws::v1::ErrorCode& out_code, std::string& out_msg) {
  if (!is_caller_authorized(caller_user)) {
    out_code = ws::v1::ERROR_CODE_INVALID_COMMAND;
    out_msg = "D-Bus policy denied RAUC call from unauthorized user: " + caller_user;
    return false;
  }

  RaucBundleInfo info;
  if (!inspect_bundle(staged_path, opts, info, out_code, out_msg)) {
    return false;
  }

  if (on_progress) {
    on_progress("verifying", 10);
    on_progress("copying", 50);
    on_progress("writing_slot", 90);
    on_progress("done", 100);
  }
  return true;
}

UpdateInstaller::UpdateInstaller(UpdateInstallerOptions opts, RaucBackend* backend)
    : opts_(std::move(opts)), backend_(backend ? backend : &default_backend_) {}

bool UpdateInstaller::verify_usb_mount_options(const std::string& mount_opts, std::string& out_missing_opt) {
  for (const char* req : {"ro", "nosuid", "nodev", "noexec"}) {
    if (!has_mount_token(mount_opts, req)) {
      out_missing_opt = req;
      return false;
    }
  }
  out_missing_opt.clear();
  return true;
}

bool UpdateInstaller::scan_usb_stick(const std::string& mount_path, const std::string& mount_opts,
                                     ws::v1::UpdateAvailable& out_avail, ws::v1::ErrorCode& out_code,
                                     std::string& out_msg) const {
  std::string missing_opt;
  if (!verify_usb_mount_options(mount_opts, missing_opt)) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "USB stick mount missing required security option: " + missing_opt;
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::exists(mount_path, ec) || !std::filesystem::is_directory(mount_path, ec)) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "USB stick mount path does not exist: " + mount_path;
    return false;
  }

  std::vector<std::filesystem::path> candidates;
  for (const auto& entry : std::filesystem::directory_iterator(mount_path, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".raucb") {
      candidates.push_back(entry.path());
    }
  }
  if (candidates.empty()) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "no .raucb update bundle found on USB stick";
    return false;
  }
  std::sort(candidates.begin(), candidates.end());
  const std::string bundle_path = candidates.front().string();

  RaucBundleInfo info;
  if (!backend_->inspect_bundle(bundle_path, opts_, info, out_code, out_msg)) {
    return false;
  }

  out_avail.Clear();
  out_avail.set_version(info.version);
  out_avail.set_source(ws::v1::UPDATE_SOURCE_USB);
  out_avail.set_bundle_path(bundle_path);
  last_available_ = out_avail;
  has_available_ = true;
  return true;
}

bool UpdateInstaller::verify_staged_from_studio(const std::string& staged_path, const std::string& hint_version,
                                                ws::v1::UpdateAvailable& out_avail, ws::v1::ErrorCode& out_code,
                                                std::string& out_msg) const {
  RaucBundleInfo info;
  if (!backend_->inspect_bundle(staged_path, opts_, info, out_code, out_msg)) {
    return false;
  }
  out_avail.Clear();
  out_avail.set_version(!hint_version.empty() ? hint_version : info.version);
  out_avail.set_source(ws::v1::UPDATE_SOURCE_STUDIO);
  out_avail.set_bundle_path(staged_path);
  last_available_ = out_avail;
  has_available_ = true;
  return true;
}

bool UpdateInstaller::install(const std::string& bundle_path, const std::string& caller_user,
                              const std::function<void(const ws::v1::UpdateProgress&)>& on_progress,
                              ws::v1::ErrorCode& out_code, std::string& out_msg) {
  if (!RaucBackend::is_caller_authorized(caller_user)) {
    out_code = ws::v1::ERROR_CODE_INVALID_COMMAND;
    out_msg = "D-Bus policy denied RAUC install from unauthorized caller: " + caller_user;
    return false;
  }

  std::error_code ec;
  const auto src_size = std::filesystem::file_size(bundle_path, ec);
  if (ec) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "failed to stat update bundle: " + bundle_path;
    return false;
  }

  std::filesystem::create_directories(opts_.staging_dir, ec);
  if (ec) {
    out_code = ws::v1::ERROR_CODE_UPDATE_IO;
    out_msg = "failed to create staging directory: " + opts_.staging_dir;
    return false;
  }

  std::uint64_t avail_bytes = opts_.simulated_free_bytes;
  if (avail_bytes == 0) {
    const auto sp = std::filesystem::space(opts_.staging_dir, ec);
    if (!ec) avail_bytes = sp.available;
  }
  if (avail_bytes > 0 && avail_bytes < src_size + opts_.min_free_headroom_bytes) {
    out_code = ws::v1::ERROR_CODE_UPDATE_NO_SPACE;
    out_msg = "insufficient free space in " + opts_.staging_dir + " to stage update bundle";
    return false;
  }

  // Copy bundle into /data/update staging if not already in staging_dir (protects the inactive slot if USB stick is pulled mid-install).
  std::string staged_path = bundle_path;
  const auto canon_staging = std::filesystem::weakly_canonical(opts_.staging_dir, ec).string();
  const auto canon_bundle = std::filesystem::weakly_canonical(bundle_path, ec).string();
  const bool already_staged = !canon_staging.empty() && canon_bundle.rfind(canon_staging, 0) == 0;

  if (!already_staged) {
    staged_path = opts_.staging_dir + "/staged-usb-update.raucb";
    std::ifstream src(bundle_path, std::ios::binary);
    std::ofstream dst(staged_path, std::ios::binary | std::ios::trunc);
    if (!src.is_open() || !dst.is_open()) {
      std::filesystem::remove(staged_path, ec);
      out_code = ws::v1::ERROR_CODE_UPDATE_IO;
      out_msg = "failed to stage update bundle from USB stick to " + staged_path;
      return false;
    }
    dst << src.rdbuf();
    if (src.bad() || !dst.good()) {
      dst.close();
      std::filesystem::remove(staged_path, ec);
      out_code = ws::v1::ERROR_CODE_UPDATE_IO;
      out_msg = "I/O error while copying update bundle from USB stick";
      return false;
    }
    dst.close();
    if (opts_.on_after_stage_copy) {
      opts_.on_after_stage_copy();
    }
  }

  // Verify the staged copy before calling RAUC
  RaucBundleInfo info;
  if (!backend_->inspect_bundle(staged_path, opts_, info, out_code, out_msg)) {
    if (!already_staged) std::filesystem::remove(staged_path, ec);
    return false;
  }

  const bool ok = backend_->install_bundle(
      staged_path, caller_user, opts_,
      [&](const std::string& phase, std::uint32_t pct) {
        if (on_progress) {
          ws::v1::UpdateProgress prog;
          prog.set_phase(phase);
          prog.set_percent(pct);
          prog.set_target_version(info.version);
          prog.set_fallback_version(opts_.fallback_version);
          on_progress(prog);
        }
      },
      out_code, out_msg);

  if (!already_staged) {
    std::filesystem::remove(staged_path, ec);
  }
  return ok;
}

}  // namespace velora::sysd

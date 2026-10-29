#pragma once
// Story 8.1: sysd is the sole installer from a USB-A stick (and the shared verification + install pipeline for Studio-staged bundles).
//
// Responsibilities:
// 1. D-Bus caller policy check (`RaucBackend::is_caller_authorized`): only `velora-sysd` and `root` are allowed to invoke `de.pengutronix.rauc.Installer`.
// 2. Removable USB-A mount verification (`UpdateInstaller::verify_usb_mount_options`): requires `ro`, `nosuid`, `nodev`, `noexec`.
// 3. Bundle inspection (`RaucBackend::inspect_bundle`): checks signature, compatible string ("Velora RK3588"), trusted root if configured,
//    and rejects bundles containing library content (`/data/lib` payload or library bundle format) per AD-26.
// 4. Staging before install (`UpdateInstaller::install`): copies USB bundles to `/data/update` staging first so unplugging the USB stick
//    mid-install cannot corrupt the inactive root slot, verifies the staged bundle, invokes `RaucBackend::install_bundle`, and emits
//    `ws::v1::UpdateProgress` messages.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ws_v1.pb.h"

namespace velora::sysd {

struct RaucBundleInfo {
  std::string version;
  std::string compatible;
  std::string root;
  bool signature_valid = false;
  bool contains_library_content = false;
  std::string error_reason;
};

struct UpdateInstallerOptions {
  std::string staging_dir = "/data/update";
  std::string expected_compatible = "Velora RK3588";
  std::string trusted_root;       // When non-empty ("prod", "factory-test", "dev"), enforces root match
  std::string fallback_version = "0.1.0-dev";
  std::uint64_t min_free_headroom_bytes = 64ull << 20;  // 64 MiB headroom on /data/update
  std::uint64_t simulated_free_bytes = 0;               // 0 = query std::filesystem::space(staging_dir)
  // Optional hook called during staging copy (e.g., to simulate USB stick removal mid-copy or mid-install in tests).
  std::function<void()> on_after_stage_copy;
};

class RaucBackend {
 public:
  virtual ~RaucBackend() = default;

  // Story 8.1 AC1: D-Bus policy `de.pengutronix.rauc.velora.conf` only permits `velora-sysd` (and `root`) to call `de.pengutronix.rauc.Installer`.
  static bool is_caller_authorized(const std::string& caller_user);

  // Inspects a `.raucb` bundle without installing it.
  virtual bool inspect_bundle(const std::string& bundle_path, const UpdateInstallerOptions& opts,
                              RaucBundleInfo& out_info, ws::v1::ErrorCode& out_code, std::string& out_msg) const;

  // Installs a verified, staged `.raucb` bundle into the inactive slot.
  virtual bool install_bundle(const std::string& staged_path, const std::string& caller_user,
                              const UpdateInstallerOptions& opts,
                              const std::function<void(const std::string& phase, std::uint32_t percent)>& on_progress,
                              ws::v1::ErrorCode& out_code, std::string& out_msg);
};

class UpdateInstaller {
 public:
  explicit UpdateInstaller(UpdateInstallerOptions opts = {}, RaucBackend* backend = nullptr);

  const UpdateInstallerOptions& options() const noexcept { return opts_; }
  void set_options(UpdateInstallerOptions opts) { opts_ = std::move(opts); }

  // Story 8.1 AC2: Removable media mount options must include `ro`, `nosuid`, `nodev`, `noexec`.
  static bool verify_usb_mount_options(const std::string& mount_opts, std::string& out_missing_opt);

  // Scans the root of a mounted USB-A stick for a `.raucb` bundle, inspects its header/signature without installing,
  // and populates `UpdateAvailable {version, UPDATE_SOURCE_USB, bundle_path}`.
  bool scan_usb_stick(const std::string& mount_path, const std::string& mount_opts,
                      ws::v1::UpdateAvailable& out_avail, ws::v1::ErrorCode& out_code, std::string& out_msg) const;

  // Story 8.1 AC5: Shared verification path for a bundle staged in `/data/update` by Velora Studio (Story 9.20).
  bool verify_staged_from_studio(const std::string& staged_path, const std::string& hint_version,
                                 ws::v1::UpdateAvailable& out_avail, ws::v1::ErrorCode& out_code, std::string& out_msg) const;

  // Story 8.1 AC3 & AC4: Copies a USB bundle to `/data/update` staging (if not already inside `staging_dir`),
  // verifies signature/compatible/root/AD-26, runs RAUC install into the inactive slot, and emits `UpdateProgress`.
  bool install(const std::string& bundle_path, const std::string& caller_user,
               const std::function<void(const ws::v1::UpdateProgress&)>& on_progress,
               ws::v1::ErrorCode& out_code, std::string& out_msg);

  const ws::v1::UpdateAvailable& last_available() const noexcept { return last_available_; }
  bool has_available() const noexcept { return has_available_; }
  void set_available(const ws::v1::UpdateAvailable& avail) {
    last_available_ = avail;
    has_available_ = !avail.bundle_path().empty();
  }

 private:
  UpdateInstallerOptions opts_;
  RaucBackend default_backend_;
  RaucBackend* backend_;
  mutable ws::v1::UpdateAvailable last_available_;
  mutable bool has_available_ = false;
};

}  // namespace velora::sysd

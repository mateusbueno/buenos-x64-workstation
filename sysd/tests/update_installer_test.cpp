#include "velora/sysd/update_installer.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "velora/sysd/config_service.hpp"
#include "velora/sysd/config_store.hpp"

using namespace velora::sysd;

namespace {

std::filesystem::path make_temp_dir(const char* tag) {
  auto base = std::filesystem::temp_directory_path() / (std::string("velora_s81_") + tag);
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base;
}

void write_file(const std::filesystem::path& p, const std::string& content) {
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  assert(ofs.is_open());
  ofs << content;
}

// AC1: Only velora-sysd (and root) is permitted by D-Bus policy; any other process is denied.
void test_ac1_sole_caller_policy() {
  assert(RaucBackend::is_caller_authorized("velora-sysd"));
  assert(RaucBackend::is_caller_authorized("root"));
  assert(!RaucBackend::is_caller_authorized("velora-ui"));
  assert(!RaucBackend::is_caller_authorized("velora-engine"));
  assert(!RaucBackend::is_caller_authorized("velora-librarian"));
  assert(!RaucBackend::is_caller_authorized("unprivileged"));

  auto tmp = make_temp_dir("ac1");
  auto bundle = tmp / "velora-1.5.0.raucb";
  write_file(bundle, "version=1.5.0\ncompatible=Velora RK3588\nroot=dev\n");

  UpdateInstallerOptions opts;
  opts.staging_dir = (tmp / "staging").string();
  opts.trusted_root = "dev";
  UpdateInstaller installer(opts);

  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string msg;
  bool ok = installer.install(bundle.string(), "velora-ui", nullptr, code, msg);
  assert(!ok);
  assert(code == ws::v1::ERROR_CODE_INVALID_COMMAND);
  assert(msg.find("unauthorized") != std::string::npos);

  std::filesystem::remove_all(tmp);
}

// AC2: Removable USB-A mount verification and non-installing discovery scan.
void test_ac2_usb_mount_and_discovery() {
  std::string missing;
  assert(UpdateInstaller::verify_usb_mount_options("ro,nosuid,nodev,noexec,relatime", missing));
  assert(missing.empty());
  assert(!UpdateInstaller::verify_usb_mount_options("rw,nosuid,nodev,noexec", missing));
  assert(missing == "ro");
  assert(!UpdateInstaller::verify_usb_mount_options("ro,nodev,noexec", missing));
  assert(missing == "nosuid");
  assert(!UpdateInstaller::verify_usb_mount_options("ro,nosuid,noexec", missing));
  assert(missing == "nodev");
  assert(!UpdateInstaller::verify_usb_mount_options("ro,nosuid,nodev", missing));
  assert(missing == "noexec");

  auto tmp = make_temp_dir("ac2");
  auto usb_dir = tmp / "usb";
  std::filesystem::create_directories(usb_dir);
  write_file(usb_dir / "velora-update-1.6.0.raucb", "version=1.6.0\ncompatible=Velora RK3588\nroot=dev\n");

  UpdateInstallerOptions opts;
  opts.staging_dir = (tmp / "staging").string();
  opts.trusted_root = "dev";
  UpdateInstaller installer(opts);

  ws::v1::UpdateAvailable avail;
  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string msg;
  // Rejects insecure mount options
  assert(!installer.scan_usb_stick(usb_dir.string(), "rw,nosuid,nodev,noexec", avail, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_IO);

  // Accepts ro,nosuid,nodev,noexec and reports UpdateAvailable without installing
  assert(installer.scan_usb_stick(usb_dir.string(), "ro,nosuid,nodev,noexec", avail, code, msg));
  assert(avail.version() == "1.6.0");
  assert(avail.source() == ws::v1::UPDATE_SOURCE_USB);
  assert(avail.bundle_path() == (usb_dir / "velora-update-1.6.0.raucb").string());
  assert(!std::filesystem::exists(tmp / "staging"));  // Not staged or installed during discovery!

  std::filesystem::remove_all(tmp);
}

// AC3: Verification and rejection (bad signature, wrong compatible, AD-26 library content, no space, USB removal mid-install).
void test_ac3_verification_and_rejection() {
  auto tmp = make_temp_dir("ac3");
  auto usb_dir = tmp / "usb";
  std::filesystem::create_directories(usb_dir);

  UpdateInstallerOptions opts;
  opts.staging_dir = (tmp / "staging").string();
  opts.trusted_root = "dev";
  UpdateInstaller installer(opts);

  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string msg;

  // 1. Bad signature
  auto bad_sig = usb_dir / "bad_sig.raucb";
  write_file(bad_sig, "version=1.6.0\ncompatible=Velora RK3588\nBAD_SIGNATURE\n");
  assert(!installer.install(bad_sig.string(), "velora-sysd", nullptr, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_BAD_SIGNATURE);

  // 2. Wrong compatible string
  auto bad_compat = usb_dir / "bad_compat.raucb";
  write_file(bad_compat, "version=1.6.0\ncompatible=Other Synth Board\nroot=dev\n");
  assert(!installer.install(bad_compat.string(), "velora-sysd", nullptr, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_INCOMPATIBLE);

  // 3. AD-26 violation: bundle containing library content (/data/lib)
  auto lib_bundle = usb_dir / "with_lib.raucb";
  write_file(lib_bundle, "version=1.6.0\ncompatible=Velora RK3588\nroot=dev\npayload=/data/lib/factory.vlib\n");
  assert(!installer.install(lib_bundle.string(), "velora-sysd", nullptr, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_INCOMPATIBLE);
  assert(msg.find("AD-26") != std::string::npos);

  // 4. Insufficient free space in /data/update
  auto good_bundle = usb_dir / "good.raucb";
  write_file(good_bundle, "version=1.6.0\ncompatible=Velora RK3588\nroot=dev\n");
  UpdateInstallerOptions low_space_opts = opts;
  low_space_opts.simulated_free_bytes = 1024;  // less than 64 MiB headroom
  UpdateInstaller low_space_installer(low_space_opts);
  assert(!low_space_installer.install(good_bundle.string(), "velora-sysd", nullptr, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_NO_SPACE);

  // 5. Removing USB stick after staging copy completes does NOT corrupt install (install runs from /data/update)
  UpdateInstallerOptions pull_usb_opts = opts;
  pull_usb_opts.on_after_stage_copy = [&]() {
    std::filesystem::remove(good_bundle);  // Simulate user yanking the USB drive right after staging copy
  };
  UpdateInstaller pull_usb_installer(pull_usb_opts);
  assert(pull_usb_installer.install(good_bundle.string(), "velora-sysd", nullptr, code, msg));
  assert(!std::filesystem::exists(good_bundle));

  // 6. Removing USB stick BEFORE staging copy starts returns clean ERROR_CODE_UPDATE_IO without writing slot
  assert(!installer.install((usb_dir / "missing.raucb").string(), "velora-sysd", nullptr, code, msg));
  assert(code == ws::v1::ERROR_CODE_UPDATE_IO);

  std::filesystem::remove_all(tmp);
}

// AC4 & AC5: Install progress over ws.v1 ConfigService and shared Studio verification path.
void test_ac4_ac5_ws_v1_progress_and_studio_shared_path() {
  auto tmp = make_temp_dir("ac4_ac5");
  StoreOptions sopt;
  sopt.dir = tmp.string();
  ConfigStore store(sopt);
  assert(store.writer_locked());
  ConfigService svc(store);

  UpdateInstallerOptions opts;
  opts.staging_dir = (tmp / "staging").string();
  opts.trusted_root = "dev";
  opts.fallback_version = "1.4.0";
  UpdateInstaller installer(opts);
  svc.set_update_installer(&installer);

  // Connect and subscribe a UI client on ConnId 1
  svc.open(1);
  std::vector<Outbound> out;
  {
    ws::v1::ClientMessage h;
    h.set_request_id(1);
    h.mutable_hello()->set_version(1);
    h.mutable_hello()->set_client_name("velora-ui");
    svc.handle(1, h.SerializeAsString(), out);
    assert(out.size() == 1 && out[0].msg.has_hello_ack());
    out.clear();

    ws::v1::ClientMessage sub;
    sub.set_request_id(2);
    sub.mutable_subscribe_config()->set_enable(true);
    svc.handle(1, sub.SerializeAsString(), out);
    out.clear();
  }

  // AC5: Stage a bundle in /data/update as Velora Studio does, verify via shared path, and announce UpdateAvailable
  std::filesystem::create_directories(opts.staging_dir);
  auto studio_staged = (tmp / "staging" / "studio-1.7.0.raucb").string();
  write_file(studio_staged, "version=1.7.0\ncompatible=Velora RK3588\nroot=dev\n");

  ws::v1::UpdateAvailable avail;
  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string err_msg;
  assert(installer.verify_staged_from_studio(studio_staged, "1.7.0", avail, code, err_msg));
  assert(avail.source() == ws::v1::UPDATE_SOURCE_STUDIO);
  assert(avail.version() == "1.7.0");

  svc.announce_update_available(avail, out);
  assert(out.size() == 1);
  assert(out[0].msg.has_update_available());
  assert(out[0].msg.update_available().version() == "1.7.0");
  out.clear();

  // AC4: Trigger InstallUpdate over ws.v1 and verify UpdateProgress deltas + final Ack
  ws::v1::ClientMessage inst;
  inst.set_request_id(3);
  inst.mutable_install_update();  // Uses last_available()
  svc.handle(1, inst.SerializeAsString(), out);

  // Expect 4 UpdateProgress pushes (10%, 50%, 90%, 100%) + 1 Ack reply
  assert(out.size() == 5);
  assert(out[0].msg.has_update_progress() && out[0].msg.update_progress().percent() == 10);
  assert(out[1].msg.has_update_progress() && out[1].msg.update_progress().percent() == 50);
  assert(out[2].msg.has_update_progress() && out[2].msg.update_progress().percent() == 90);
  assert(out[3].msg.has_update_progress() && out[3].msg.update_progress().percent() == 100);
  assert(out[3].msg.update_progress().target_version() == "1.7.0");
  assert(out[3].msg.update_progress().fallback_version() == "1.4.0");
  assert(out[4].msg.has_ack() && out[4].msg.request_id() == 3);

  std::filesystem::remove_all(tmp);
}

}  // namespace

int main() {
  test_ac1_sole_caller_policy();
  test_ac2_usb_mount_and_discovery();
  test_ac3_verification_and_rejection();
  test_ac4_ac5_ws_v1_progress_and_studio_shared_path();
  std::cout << "sysd_update_installer_test: ALL PASSED\n";
  return 0;
}

# Implementation Spec — Story 8.1: `sysd` as the Sole Installer from a USB-A Stick

## Goal
Ensure `sysd` is the sole process allowed to talk to RAUC over D-Bus, discovers `.raucb` bundles on read-only mounted USB-A sticks (`ro,nosuid,nodev,noexec`), stages them into `/data/update` before writing the inactive slot (so removing the USB stick mid-install cannot corrupt the slot), verifies signature, hardware compatibility, signing root, and AD-26 separation (no library content inside system updates), and streams `UpdateProgress` over `ws.v1`.

## Files Changed / Added
- `yocto/meta-velora/recipes-core/rauc/files/dbus/de.pengutronix.rauc.velora.conf`: D-Bus system policy allowing only `root` and `velora-sysd` to talk to `de.pengutronix.rauc.Installer`.
- `yocto/meta-velora/recipes-core/rauc/rauc_%.bbappend`: Installs `de.pengutronix.rauc.velora.conf` and restricts `/usr/bin/rauc` permissions (`0750`).
- `yocto/meta-velora/recipes-core/velora-system-config/files/10-velora-data.conf`: Creates `/data/update` (`0750`, owned by `velora-sysd:velora-sysd`).
- `yocto/ci/static_checks.py`: Enforces `/data/update` ownership and RAUC D-Bus policy presence.
- `proto/ws_v1.proto`: Added `UpdateSource`, `UpdateAvailable` (`ServerMessage` field 34), `UpdateProgress` (`ServerMessage` field 35), and `InstallUpdate` (`ClientMessage` field 74).
- `sysd/include/velora/sysd/update_installer.hpp` & `sysd/src/update_installer.cpp`: Implemented `RaucBackend` and `UpdateInstaller`.
- `sysd/include/velora/sysd/config_service.hpp` & `sysd/src/config_service.cpp`: Wired `UpdateInstaller` into `ConfigService` (`InstallUpdate`, `UpdateAvailable`, `UpdateProgress`).
- `sysd/src/bridge_server.cpp`: Denied `kInstallUpdate` over TCP bridge (`R::kDenied` — install trigger is on-device UDS only).
- `sysd/tests/update_installer_test.cpp`: Unit and integration tests covering AC1–AC5.

---
title: 'Story 9.20: Send firmware from Velora Studio'
type: 'feature'
created: '2026-10-29'
status: 'done'
baseline_revision: '2188fa16b35605ebb7312e64a604ff731f2d7672'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/ws_v1.proto'
  - '{project-root}/proto/README.md'
  - '{project-root}/sysd/src/bridge_server.cpp'
  - '{project-root}/desktop/src-tauri/src/device.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src/device_view.js'
  - '{project-root}/desktop/src/device_view_dom.js'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/catalogs.js'
  - '{project-root}/locale/en.json'
  - '{project-root}/locale/es.json'
  - '{project-root}/locale/pt-BR.json'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need a way to send firmware updates (`.raucb`) directly from Velora Studio to the unit over USB without formatting a USB-A drive, with progress visibility across the Device view and Footer, device-side signature verification before staging, and safe reboot tracking (FR100, UX-DR77, UX-DR70).

**Approach:** Extend `ws_v1.proto` and `sysd` bridge with `StageFirmwareChunk`, `GetFirmwareUpdateStatus`, and `AbortFirmwareUpdate` messages under feature `"update.v1"` with typed error codes (`UPDATE_BAD_SIGNATURE`, `UPDATE_INCOMPATIBLE`, `UPDATE_NO_SPACE`, `UPDATE_IO`). Implement chunked streaming with SHA-256 validation in the desktop Tauri backend and add firmware bundle inspection, progress bar, device installation notice tracking, and reboot reconnection in `desktop/src/device_view.js` and `desktop/src/device_view_dom.js`.

## Boundaries & Constraints

**Always:**
- Studio shows the bundle version from its manifest alongside the unit's current version before or during sending.
- Studio streams the bundle to `sysd` in chunks with SHA-256 chunk checks; progress is displayed in both Device view and the Studio Footer.
- `sysd` stages the file under `/data/update/.staging/` and verifies signature and compatibility before announcing `"<version> ready to install"`.
- Studio **never** starts the install itself; the install is authorized solely on the device (Action Sheet with `WRITE · Install`).
- While the device is installing, Studio shows: `"Installing on Velora 73 — sound is off on the device until it restarts"`, anticipates the disconnection upon reboot, and reconnects with the new version.
- On corrupted, unsigned, or incompatible bundles, `sysd` rejects the upload with typed errors; Studio surfaces the failure message and leaves nothing installed.
- All UI strings localized in `en`, `es`, and `pt-BR`.
- Zero raw color literals in CSS (use design tokens only).

**Never:**
- Never start or trigger the RAUC install directly from Studio (the unit owns the install decision).
- Never write firmware chunks directly to active or standby rootfs partitions (staging in `/data/update` only).
- Never leave orphaned partial update files on disk if the upload is aborted or the connection drops.
- Never block audio or engine processing during firmware streaming.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Happy path stream | Valid `.raucb` file picked in Studio | Shows bundle version vs current version; streams with progress in Device view and Footer; unit announces `"<version> ready to install"` | Clean progress bar and live region announcements |
| Installing state | Unit starts installation | Studio displays `"Installing on Velora 73 — sound is off on the device until it restarts"`; expects connection drop | Ignores disconnect error during expected reboot |
| Reboot & reconnect | Unit boots into new firmware | Studio reconnects, reads new firmware version, updates status to up-to-date | Clears installing state |
| Corrupt / Bad signature | Corrupted `.raucb` file or bad signature | `sysd` rejects with `UPDATE_BAD_SIGNATURE`; Studio shows failure message; staging purged | Error banner displayed; nothing installed |
| Incompatible bundle | Bundle for different hardware | `sysd` rejects with `UPDATE_INCOMPATIBLE`; Studio shows incompatibility reason | Error banner displayed; nothing installed |
| Connection lost mid-upload | Cable pulled during streaming | Staging cleaned up; Studio reports connection lost | Clean state reset |

</intent-contract>

## Code Map

- `proto/ws_v1.proto` -- Add `StageFirmwareChunk`, `GetFirmwareUpdateStatus`, `AbortFirmwareUpdate` client messages and `StageFirmwareChunkReply`, `FirmwareUpdateStatusReply` server messages; error codes `UPDATE_BAD_SIGNATURE` (30), `UPDATE_INCOMPATIBLE` (31), `UPDATE_NO_SPACE` (32), `UPDATE_IO` (33).
- `proto/README.md` -- Document Story 9.20 protocol additions and error codes.
- `sysd/include/velora/sysd/bridge_server.hpp` & `sysd/src/bridge_server.cpp` -- Add `Route::kUpdate`, `"update.v1"` feature, firmware chunk staging, verification, toast notice trigger, and cleanup on disconnect/abort.
- `sysd/tests/bridge_test.cpp` -- Add unit tests for firmware update route, allowlist, and staging.
- `desktop/src-tauri/src/device.rs` -- Implement `inspect_firmware_bundle`, `stage_firmware_chunk`, `abort_firmware_update`, and mock responses in `respond_bridge`.
- `desktop/src-tauri/src/main.rs` -- Register Tauri commands `device_inspect_firmware_bundle`, `device_send_firmware_update`, and `device_cancel_firmware_update`.
- `locale/en.json`, `locale/es.json`, `locale/pt-BR.json` & `desktop/src/catalogs.js` -- Add localized strings for firmware update flow, progress, notices, and errors.
- `desktop/src/device_view.js` -- Update `DeviceViewController` with firmware bundle inspection, streaming state, progress tracking, and reboot handling.
- `desktop/src/device_view_dom.js` -- Render firmware update UI, bundle selection, file picker, progress gauge, and status banners.
- `desktop/src/app.js` -- Wire footer status during firmware streaming, listen for progress events, and handle reboot reconnection.
- `desktop/src/style.css` -- Add CSS styling for firmware update progress and banners using semantic tokens.
- `desktop/tests/firmware_update.test.js` -- Unit tests for firmware streaming, inspection, error reporting, and UI accessibility.

## Tasks & Acceptance

**Execution:**
1. `proto/ws_v1.proto` & `proto/README.md` -- Define additive firmware update messages, reply structures, and error codes 30..33.
2. `sysd/include/velora/sysd/bridge_server.hpp` & `sysd/src/bridge_server.cpp` -- Add `Route::kUpdate` with `"update.v1"` in message table, implement in-process chunk staging, SHA-256 verification, and toast announcement.
3. `sysd/tests/bridge_test.cpp` -- Add tests verifying `sysd` message rules table totality and firmware chunk handler.
4. `desktop/src-tauri/src/device.rs` & `desktop/src-tauri/src/main.rs` -- Implement Tauri commands for `.raucb` manifest inspection, chunk streaming with SHA-256, and progress emission.
5. `locale/en.json`, `locale/es.json`, `locale/pt-BR.json` & `desktop/src/catalogs.js` -- Add translation keys for firmware update strings.
6. `desktop/src/device_view.js` & `desktop/src/device_view_dom.js` -- Implement firmware update controller logic, DOM layout, and progress bar.
7. `desktop/src/app.js` & `desktop/src/style.css` -- Wire footer status paint and styling with design tokens.
8. `desktop/tests/firmware_update.test.js` -- Add comprehensive test suite covering manifest check, streaming progress, error rejection, and reboot reconnection.

**Acceptance Criteria:**
- Given Device › Firmware in Studio, when the user picks a `.raucb` file, then Studio displays the bundle's version and the device's current version, and streams the file to `sysd` with progress in the Device view and Footer.
- Given the file arrived, when `sysd` stages and verifies it, then the device announces `"<version> ready to install"` and the install follows the device flow; Studio never starts the install itself.
- Given the device starts installing, when in progress, then Studio shows `"Installing on Velora 73 — sound is off on the device until it restarts"`, expects the connection to drop, and reconnects after restart showing the new version.
- Given a damaged or wrongly signed file, when `sysd` rejects it, then Studio shows the device's failure message and nothing is installed.

## Design Notes

- `.raucb` Inspection: A RAUC bundle starts with custom headers or SquashFS filesystem containing `manifest.raucm`. The desktop backend inspects the header or reads `manifest.raucm` using safe extraction to obtain `version` and `compatible`.
- The device announcement `"<version> ready to install"` is triggered via `sysd` notice / toast callback, consistent with Story 8.1 and Story 9.15.
- Footer rendering follows `SENDING FIRMWARE TO VELORA 73 · <X> of <Y> MB` while streaming, keeping sound rendering completely uninterrupted.

## Verification

**Commands:**
- `cargo test --manifest-path desktop/src-tauri/Cargo.toml` -- expected: All desktop Rust unit tests pass.
- `ctest --test-dir build -R "^sysd_bridge_tests$"` -- expected: All sysd bridge tests pass with updated message table.
- `node --test desktop/tests/*.test.js` -- expected: All Node.js unit tests pass including `firmware_update.test.js`.

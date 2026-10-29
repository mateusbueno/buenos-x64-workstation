---
title: 'Story 9.21: Device view and diagnostics export'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: 'df0700850b5fe2e217125dd218fb0a7775650d97'
review_loop_iteration: 1
followup_review_recommended: false
context:
  - '{project-root}/proto/ws_v1.proto'
  - '{project-root}/desktop/src-tauri/src/device.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src/catalogs.js'
  - '{project-root}/desktop/src/app.js'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists and support contacts need a centralized Device View in Velora Studio to inspect unit hardware status (model, tier, firmware version, image variant), manage storage breakdown per installed sound library, access backup/restore and firmware update actions, and export in-RAM audio-health counters and latency histograms for diagnostics without writing to device storage or leaking proprietary audio and keys (FR120, AR44, UX-DR70).

**Approach:** Implement `DeviceViewController` and accessible DOM view in `desktop/src/device_view.js` and `device_view_dom.js`, hooked into Sidebar DEVICE items. Implement `get_diagnostics` and `export_diagnostics` Tauri IPC commands in `desktop/src-tauri/src/device.rs` and `main.rs` that query `GetDiagnostics` from the control thread via ws.v1 and save a clean, sanitized JSON diagnostic export containing counters, histograms, uptime, and device metadata.

## Boundaries & Constraints

**Always:**
- Diagnostic export request is served by the engine control thread from in-RAM atomics and never writes to device disk.
- Diagnostic export file contains zero Live Set definitions, audio samples, or encryption keys.
- Device view shows model, tier ("Stage" or "Pro"), firmware version, image variant, storage breakdown (free, used, per-library sizes), firmware update entry point, and backups status.
- Exporting diagnostics announces completion via a polite aria-live region.
- All text strings and categories are fully localized in `en`, `es`, and `pt-BR`.

**Never:**
- Never block audio rendering while querying diagnostics.
- Never write diagnostic dump files to `/var/lib/velora` on the device.
- Never leak private keys, seed tokens, or proprietary audio payload into diagnostics JSON.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Open Device View | User clicks DEVICE sidebar section or Storage | Renders device card with Model, Tier, Firmware, Variant, Storage bar, Libraries list, and Backups | Fallback gracefully with offline hints if not paired |
| Export Diagnostics (Paired) | User clicks "Export diagnostics" | Queries ws.v1 `GetDiagnostics`, receives `DiagnosticsReply`, writes formatted JSON file | Error banner if device disconnects or bridge times out |
| Data Sanitization Check | Diagnostic export generated | File verified to only contain counters, histograms, device metadata | Test fails if any LiveSet or audio payload is present |
| Zero / Empty Histograms | Freshly booted unit | JSON outputs valid empty or initial bins without crashing | Safe default serialization |

</intent-contract>

## Code Map

- `desktop/src-tauri/src/device.rs` -- Implement `fetch_diagnostics` via ws.v1 `GetDiagnostics` / `DiagnosticsReply` and diagnostic JSON serializer.
- `desktop/src-tauri/src/main.rs` -- Expose Tauri commands `get_device_diagnostics` and `save_diagnostics_export`.
- `desktop/src/catalogs.js` & `locale/*.json` -- Localization strings for Device view sections, storage labels, tier names, and diagnostics export.
- `desktop/src/device_view.js` & `desktop/src/device_view_dom.js` -- Device view model/controller and accessible DOM layout with storage meter and action buttons.
- `desktop/src/app.js` & `desktop/src/index.html` & `desktop/src/style.css` -- Wire sidebar DEVICE navigation (`side-storage`, `side-firmware`, `side-backups`), main view switcher, and styles.
- `desktop/tests/device_view.test.js` & `device_view_dom.test.js` -- Unit tests for device view model, storage formatting, diagnostics sanitization, and keyboard accessibility.

## Tasks & Acceptance

**Execution:**
- `desktop/src-tauri/src/device.rs` -- Add diagnostics request handling and JSON export generator with security assertions.
- `desktop/src-tauri/src/main.rs` -- Register Tauri commands for diagnostics.
- `desktop/src/device_view.js` -- Controller managing device metadata, storage breakdown, and diagnostics export trigger.
- `desktop/src/device_view_dom.js` -- DOM rendering for device info cards, storage gauge, installed libraries table, and diagnostics button.
- `locale/en.json`, `es.json`, `pt-BR.json` & `desktop/src/catalogs.js` -- Add localization keys for Device view.
- `desktop/src/app.js` & `desktop/src/index.html` -- Connect sidebar DEVICE triggers and view mounting.
- `desktop/tests/device_view.test.js` & `device_view_dom.test.js` -- Add test coverage for device view and diagnostics export.

**Acceptance Criteria:**
- Given a paired connection, when user navigates to DEVICE, then model, tier, firmware, image variant, storage (used/free + library breakdown), firmware, and backup status are displayed.
- Given the user clicks "Export diagnostics", when requested, then a JSON file is saved containing counters (xrun, steal, underrun), render time & latency histograms, uptime, firmware version, and device ID.
- Given the diagnostics JSON file, when scanned, then it contains no Live Set contents, sample audio, or cryptographic keys.

## Verification

**Commands:**
- `cargo test --manifest-path desktop/src-tauri/Cargo.toml` -- expected: All Rust diagnostics and device tests pass.
- `node --test desktop/tests/*.test.js` -- expected: All JavaScript device view unit and DOM tests pass.

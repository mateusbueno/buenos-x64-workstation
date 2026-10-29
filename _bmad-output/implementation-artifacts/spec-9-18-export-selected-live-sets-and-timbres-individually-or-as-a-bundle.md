---
title: 'Story 9.18: Export selected Live Sets and timbres (individually or as a bundle)'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: '9bfbc7fa159fa6d8c68c31c46e071d287d641262'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/backup.proto'
  - '{project-root}/desktop/src-tauri/src/backup_restore.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src/catalogs.js'
  - '{project-root}/desktop/src/app.js'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to export individual Live Sets, selections of Live Sets, timbres, or bundles combining Live Sets with their dependent timbres to versioned `.velorasets` and `.veloratimbre` files without performing a full backup, while strictly protecting factory/partner vendor IP (AD-10, AD-14) and receiving live size estimation and progress updates (FR122, AR29, AR30, UX-DR45, UX-DR46, UX-DR71, UX-DR91, UX-DR114).

**Approach:** Implement `.velorasets` and `.veloratimbre` protobuf schemas in `proto/backup.proto`, with Rust serialization and vendor exclusion scanning in `desktop/src-tauri/src/export_import.rs`. Provide an accessible Studio Dialog in `desktop/src/export_dialog.js` and `export_dialog_dom.js` supporting three scopes (Live Sets, Timbres, Bundle), automatic dependency locking in Bundle mode, live manifest size estimation, size warnings (>2 GB warning, free space validation), cancel cleanup, and polite aria-live progress reporting.

## Boundaries & Constraints

**Always:**
- Factory and partner timbres are stored by reference only (`library_id` and `instrument_key` at 0 B payload), never exporting vendor samples, encryption blocks, or keys.
- Automated tests scan `.velorasets` and `.veloratimbre` output files to guarantee no vendor bundle audio content is present.
- A timbre-only export containing only factory/partner timbres is blocked / not offered.
- Files carry `format_version = 1` (AD-12), device model, firmware version, ISO-8601 UTC export timestamp, and item manifest with IDs and sizes.
- Cancelling an in-flight export deletes any partial file and restores idle UI state.
- Supports both paired online mode (reading published control-thread state and librarian formatlib user bundles) and offline mode (exporting from an opened `.velorabackup`).

**Never:**
- Never write files directly to the device NVMe or storage partition.
- Never block or interrupt device audio rendering during an export.
- Never expose unencrypted factory/partner audio blocks or decryption keys.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Export Live Sets | User selects 2 Live Sets in Sets scope | Generates `.velorasets` containing only the 2 Live Sets and manifest | Export button disabled if 0 rows selected |
| Export User Timbres | User selects 1 user timbre in Timbres scope | Generates `.veloratimbre` containing formatlib bundle data | Vendor-only timbres not selectable in Timbres scope |
| Export Bundle | User selects 1 Live Set with 2 factory + 1 user timbre | Generates `.velorasets` with Live Set, locked reference-only factory timbres (0 B), and 1 user timbre payload | Warn if total > 2 GB; block if destination free space is insufficient |
| Export Cancelled | User clicks Cancel mid-export | Footer cancels, partial destination file is unlinked immediately | No corrupt file left on disk |
| Vendor IP Scan | Test attempts export with factory timbre | Verified that output contains zero plaintext vendor audio or keys | Test asserts failure if vendor audio signature found |

</intent-contract>

## Code Map

- `proto/backup.proto` -- Define `VeloraSetsExport`, `VeloraTimbreExport`, `ExportItemManifest`, `ExportTimbrePayload`, and `ExportScope`.
- `desktop/src-tauri/build.rs` -- Regenerate prost bindings from `backup.proto`.
- `desktop/src-tauri/src/export_import.rs` -- Core export logic, payload builder, size calculator, file unlinking on cancel, and automated vendor audio scanner test.
- `desktop/src-tauri/src/main.rs` -- Register Tauri commands: `estimate_export_size`, `export_selection`, `cancel_export`.
- `desktop/src/catalogs.js` & `locale/*.json` -- Localization entries for export dialog, scopes, size estimations, and aria-live announcements.
- `desktop/src/export_dialog.js` & `desktop/src/export_dialog_dom.js` -- Dialog controller and DOM rendering with roving tabindex, Space selection, live size calculations, and keyboard shortcuts.
- `desktop/src/app.js` & `desktop/src/index.html` & `desktop/src/style.css` -- Wire Export button in Title Bar, context menus, offline screen, and dialog modal styles.
- `desktop/tests/export_dialog.test.js` & `export_dialog_dom.test.js` -- Unit tests for dialog state machine, bundle dependency resolution, size formatting, and accessibility.

## Tasks & Acceptance

**Execution:**
- `proto/backup.proto` -- Add `VeloraSetsExport`, `VeloraTimbreExport`, `ExportItemManifest`, `ExportTimbrePayload`, and `ExportScope` schemas.
- `desktop/src-tauri/src/export_import.rs` -- Implement serialization, bundle packaging, free space check, cancel cleanup, and vendor IP scanning.
- `desktop/src-tauri/src/main.rs` -- Expose Tauri commands for export estimation and execution.
- `desktop/src/export_dialog.js` -- Implement export controller, dependency tracking for bundles, size calculator, and status transitions.
- `desktop/src/export_dialog_dom.js` -- Build accessible dialog DOM with tabs, item table, size indicator, warn lines, and aria-live regions.
- `locale/en.json`, `es.json`, `pt-BR.json` & `desktop/src/catalogs.js` -- Add export strings across all supported locales.
- `desktop/src/app.js` & `desktop/src/index.html` -- Connect export trigger points and render modal structure.
- `desktop/tests/export_dialog.test.js` & `export_dialog_dom.test.js` -- Add unit and DOM tests for export dialog behavior and accessibility.

**Acceptance Criteria:**
- Given a paired connection or opened `.velorabackup` offline, when user opens Export dialog, then scopes (Live Sets, Timbres, Bundle) and matching items are displayed with select-all and Space key toggling.
- Given Bundle scope, when Live Sets are selected, then referenced timbres are listed beneath, checked, and locked, with factory/partner timbres tagged "Reference only" at 0 B.
- Given row selections, when rows are toggled, then live total size estimate updates ("About ... · X Live Sets, Y timbres (Z by reference)") and Export is disabled if nothing is selected.
- Given user timbres pushing bundle over 2 GB or exceeding destination free space, then a warning line appears, and insufficient free space blocks the Export action.
- Given user confirms and exports, then `.velorasets` or `.veloratimbre` is written with format_version 1, device model, firmware version, timestamp, and item manifests.
- Given an export with factory/partner timbres, then the file contains only library_id and instrument_key, and automated tests verify zero vendor audio data.
- Given a running export, then footer displays progress ("EXPORTING · X of Y GB"), cancel cleans up partial files, and completion announces via polite aria-live region.

## Verification

**Commands:**
- `cargo test --manifest-path desktop/src-tauri/Cargo.toml` -- expected: All Rust export and IP verification tests pass.
- `node --test desktop/tests/*.test.js` -- expected: All Studio JavaScript unit and DOM tests pass.

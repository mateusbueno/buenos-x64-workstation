---
title: 'Story 9.19: Import partial exports with per-item merge decisions'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: '60db6f7a304a55f819a717856476184b7714453b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/backup.proto'
  - '{project-root}/desktop/src-tauri/src/export_import.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src/export_dialog.js'
  - '{project-root}/desktop/src/catalogs.js'
  - '{project-root}/desktop/src/app.js'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to import `.velorasets` and `.veloratimbre` partial export files and make per-item merge decisions (`Skip`, `Replace`, or `Keep both`) when colliding IDs exist, without duplicating or overwriting items by accident, and gracefully handle missing referenced factory soundbanks (FR122, FR92, AR30, UX-DR77, UX-DR91, UX-DR114).

**Approach:** Implement `.velorasets` and `.veloratimbre` parser with N/N-1 validation in `desktop/src-tauri/src/export_import.rs`. Expose Tauri commands `read_import_manifest`, `execute_import`, and `cancel_import`. In Velora Studio frontend (`desktop/src/import_dialog.js`, `import_dialog_dom.js`, and `app.js`), provide an accessible Studio Dialog displaying items with collision badges, per-item collision actions (`Skip` / `Replace` / `Keep both` with "Apply to all"), progress in Footer, polite live region announcements, and missing library reporting.

## Boundaries & Constraints

**Always:**
- Future major format versions are rejected upfront with exact message: `"Couldn't import <filename>: file made by a newer Velora Studio. Update the app."`.
- When an ID collides: default action is `Keep both` which assigns a new ID (UUIDv4 for Live Sets, new `library_id` for user timbres), appends `" (2)"` to the name, and rewrites incoming Live Set timbre references to the new ID.
- Missing factory or partner libraries do not block Live Set import; the Live Set is imported and missing libraries are reported: `"X Live Sets need libraries: <Library Name> (factory)"`.
- Live Sets are sent only via control-thread commands, and user timbres via librarian import commands; never writing files directly to `/var/lib/velora` on the unit.
- If connection drops mid-import, already finished items are kept; UI reports `"connection lost · X of Y imported. Reconnect and import again; finished items are kept."`.
- On success, polite aria-live region announces `"Imported X items · Y skipped"`.

**Never:**
- Never silently overwrite an existing item without an explicit `Replace` choice.
- Never substitute an alternate sound when a referenced factory timbre is missing.
- Never crash or corrupt local device state when receiving malformed or hostile import payloads.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Newer Format Version | File with `format_version > 1` | Refused with `"Couldn't import <filename>: file made by a newer Velora Studio. Update the app."` | Dialog displays error banner, no changes made |
| ID Collision (Keep both) | Incoming Live Set has identical UUID | Given new UUIDv4, name gets `" (2)"`, dependent Live Sets re-referenced | Collision resolved cleanly |
| ID Collision (Replace) | User selects `Replace` | Existing Live Set or timbre is updated with incoming data | Old version overwritten |
| ID Collision (Skip) | User selects `Skip` | Incoming item ignored, existing item untouched | Counted as skipped in final summary |
| Missing Factory Soundbank | Live Set uses uninstalled factory timbre | Live Set imported anyway, report lists `"1 Live Sets need libraries: Grand Piano Studio (factory)"` | Parts flagged missing, silent on unit |
| Disconnect Mid-Import | Link drops after 3 of 7 items imported | Finished 3 kept, banner shows `"connection lost · 3 of 7 imported. Reconnect and import again; finished items are kept."` | Partial progress preserved |

</intent-contract>

## Code Map

- `desktop/src-tauri/src/export_import.rs` -- Implement `inspect_import_file`, collision detection against `DeviceState`, `execute_import_plan`, ID remapping, and missing library detection.
- `desktop/src-tauri/src/main.rs` -- Register Tauri commands: `read_import_manifest`, `execute_import`, and `cancel_import`.
- `desktop/src/catalogs.js` & `locale/*.json` -- Localization strings for import dialog, collision choices (`Skip`, `Replace`, `Keep both`), "Apply to all", missing library alerts, footer status, and aria-live announcements.
- `desktop/src/import_dialog.js` & `desktop/src/import_dialog_dom.js` -- Dialog controller and DOM view with roving tabindex, collision action radios/dropdowns, "Apply to all" header, and missing library summaries.
- `desktop/src/app.js` & `desktop/src/index.html` & `desktop/src/style.css` -- Wire Title Bar `btn-import`, hidden file inputs, drag-and-drop or file pickers, and modal styling.
- `desktop/tests/import_dialog.test.js` & `import_dialog_dom.test.js` -- Unit tests for collision remapping, N-1 reader, missing library detection, and keyboard accessibility.

## Tasks & Acceptance

**Execution:**
- `desktop/src-tauri/src/export_import.rs` -- Add import reader, collision resolver, UUIDv4/name remapping (`" (2)"`), and missing library scanner.
- `desktop/src-tauri/src/main.rs` -- Expose Tauri commands for reading import manifests and executing imports.
- `desktop/src/import_dialog.js` -- Controller managing import items, collision states, Apply to All, and import execution progress.
- `desktop/src/import_dialog_dom.js` -- Accessible modal dialog with item checkboxes, collision actions, and keyboard navigation.
- `locale/en.json`, `es.json`, `pt-BR.json` & `desktop/src/catalogs.js` -- Add import translation keys across English, Spanish, and Brazilian Portuguese.
- `desktop/src/app.js` & `desktop/src/index.html` -- Connect Import triggers and progress handlers.
- `desktop/tests/import_dialog.test.js` & `import_dialog_dom.test.js` -- Add comprehensive unit and DOM test suites.

**Acceptance Criteria:**
- Given a `.velorasets` or `.veloratimbre` file, when user opens Import, then manifest is read through N/N-1 reader and items are shown with collision options (Skip / Replace / Keep both).
- Given a file with format_version > 1, when opened, then it is refused with `"Couldn't import <filename>: file made by a newer Velora Studio. Update the app."`.
- Given ID collisions, when user chooses Keep both, then item is assigned a new ID, appended `" (2)"`, and dependent Live Sets are updated.
- Given an imported Live Set with missing factory timbre, when imported, then it succeeds and lists missing libraries.
- Given a running import, then footer displays progress ("IMPORTING TO VELORA 73 · X of Y items"), and completion announces via polite aria-live region.

## Verification

**Commands:**
- `cargo test --manifest-path desktop/src-tauri/Cargo.toml` -- expected: All Rust import, collision remapping, and format version tests pass.
- `node --test desktop/tests/*.test.js` -- expected: All JavaScript import controller and DOM tests pass.

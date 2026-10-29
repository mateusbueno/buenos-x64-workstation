---
title: 'Story 9.15: Library transfer to the unit'
type: 'feature'
created: '2026-10-29'
status: 'in-progress'
baseline_revision: 'cfb4ecc20d8146a8920601e3a9d75f16c588a86a'
review_loop_iteration: 1
followup_review_recommended: false
context:
  - '{project-root}/proto/library_api.proto'
  - '{project-root}/proto/ws_v1.proto'
  - '{project-root}/sysd/src/bridge_server.cpp'
  - '{project-root}/librarian/src/library_service.cpp'
  - '{project-root}/desktop/src-tauri/src/device.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src/library_view.js'
  - '{project-root}/desktop/src/library_view_dom.js'
  - '{project-root}/desktop/src/app.js'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists who convert multi-gigabyte SFZ/SF2 sound libraries on their computer want to transfer those libraries directly to the Velora over USB while the instrument continues playing without audio dropouts (zero xruns), with atomic publishing only when complete, preflight space verification, resumption across brief reconnects, and complete cleanup on disconnection.

**Approach:**
1. Protocol Layer (`proto/library_api.proto`):
   - Additive messages: `FreeSpaceRequest` / `FreeSpaceReply`, `StageChunkRequest` / `StageChunkReply`, `GetTransferStatusRequest`, `AbortTransferRequest`.
   - Update `sysd/src/bridge_server.cpp` allowlist in `BridgeServer::library_rules()` so mediated USB bridge transparently forwards transfer commands to the `librarian` daemon.
2. Daemon Layer (`librarian/src/library_service.cpp`):
   - Handle `FreeSpaceRequest`: query `free_bytes_fn` or `statvfs` on `<data>/lib` and check against requested space + reserve.
   - Handle `StageChunkRequest`: stage bundle into `<data>/lib/.staging/transfer-<transfer_id>/bundle.vlrp`, verify per-chunk SHA-256 hash.
   - Support resuming from last verified chunk via `next_offset`.
   - On completion of final chunk: verify bundle structure, manifest, and blocks; atomically rename to `<data>/lib/<library_id>/<bundle_id>`, flip `current` symlink, insert/update catalog rows, and trigger device-side Notice Toast `"<Name> ready in Library"`.
   - On abort or connection drop: remove partial staging folder from disk immediately.
3. Desktop Backend (`desktop/src-tauri/src/device.rs` & `main.rs`):
   - Tauri commands `device_check_library_space`, `device_start_library_transfer`, `device_cancel_library_transfer`, and `device_get_transfer_status`.
   - Streams chunks (e.g. 64 KB / 256 KB) with SHA-256 hash calculation, tracking byte progress and handling reconnect resumption.
4. Studio Frontend (`desktop/src/library_view.js`, `library_view_dom.js`, `app.js`):
   - In "On Computer" library tab, render "Send to Velora" action button for each converted library.
   - Space check preflight: if library does not fit, show exact error stating required vs free space.
   - Transfer progress display: show "Transferring <Name> · <X> of <Y> GB" in Footer and Library management view.
   - Resumption: if connection briefly drops and restores in the same session, query last verified chunk and resume streaming.
   - Error handling: if connection lost and not restored, display exact message: "Transfer stopped: connection lost. Nothing was installed. Start again when connected."
   - Announce progress and completion via polite aria-live regions.
   - Localize all strings in `en`, `es`, and `pt-BR`.

## Boundaries & Constraints

**Always:**
- Real-time audio plane must remain uninterrupted (zero xruns, librarian runs off audio cores at lower priority).
- Free space check must precede any file writes to the device.
- Bundles must be verified bit-exact before publishing.
- Aborted or lost transfers must leave zero installed rows or permanent partial files on the unit.
- Localized in `en`, `es`, and `pt-BR`.
- Use CSS design tokens exclusively.

**Never:**
- Never publish an incomplete or unverified bundle to the library catalog.
- Never crash or lock up the UI thread during streaming.

</intent-contract>

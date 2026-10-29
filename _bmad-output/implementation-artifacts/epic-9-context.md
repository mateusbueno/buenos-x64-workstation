# Epic 9 Context: Velora Studio and USB

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Provide a native, driverless companion desktop app (Velora Studio on macOS, Windows, and Linux via Tauri) and class-compliant USB audio/MIDI/network connectivity. Keyboardists edit Live Sets with live sound feedback, manage setlists, perform offline and online backups/restores, export and import partial repertoires (.velorasets, .veloratimbre), stage and convert SFZ libraries, send firmware, and export diagnostics counters.

## Stories

- Story 9.1: USB-C gadget composition with class-compliant MIDI
- Story 9.2: USB audio interface that follows the codec clock
- Story 9.3: sysd ws.v1 bridge on USB NCM with a single open port
- Story 9.4: Design story: Velora Studio footer, dialog, banner, and backup/restore mockups
- Story 9.5: Velora Studio app shell offline
- Story 9.6: Pairing Velora Studio with the unit
- Story 9.7: Connection states: version mismatch, disconnect, and reconnect
- Story 9.8: Live Set view: scenes and part strips
- Story 9.9: Part Editor layout with Studio Knobs
- Story 9.10: Live edit state, stale edits, and disconnect mid-edit
- Story 9.11: Add a timbre to an empty part from Velora Studio
- Story 9.12: Write to Velora with the device's scopes
- Story 9.13: Setlists view in Velora Studio
- Story 9.14: Library management and heavy SFZ conversion on the computer
- Story 9.15: Library transfer to the unit
- Story 9.16: Backup to a .velorabackup file
- Story 9.17: Restore with Merge or Replace, and offline backup editing
- Story 9.18: Export selected Live Sets and timbres (individually or as a bundle)
- Story 9.19: Import partial exports with per-item merge decisions
- Story 9.20: Send firmware from Velora Studio
- Story 9.21: Device view and diagnostics export
- Story 9.22: Velora Studio accessibility audit
- Story 9.23: Edit zone, keyboard and arpeggiator from Velora Studio

## Requirements & Constraints

- **Single Open Port (AD-13):** The unit communicates with Studio over USB NCM exclusively via WebSocket `ws.v1` on `ws://192.168.73.1:7300` (or `localhost` for dev). No HTTP server, no SSH, no open mass-storage or raw disk exposure (NFR24).
- **Control-Thread Publishing & Safety (AD-30):** Studio commands arrive via ws.v1 and mutate state strictly via control thread commands (`CreateLiveSet`, `WriteLiveSet`, `SetPartSound`, `SetPartZone`, `SetPartArp`, etc.). Studio never writes files directly into `/var/lib/velora` on the unit.
- **Reference-Only Vendor Libraries (AD-10, AD-14):** Factory and partner soundbanks carry encrypted blocks and hardware keys. Backups and exports store only `library_id` and `instrument_key` (0 B payload size, tagged "Reference only"). Files never contain proprietary audio samples, keys, or plaintext.
- **Version Compatibility & Format Versioning (AD-12):** Archival and exchange formats carry format version headers (`format_version: 1`). N-1 versions are read cleanly; future major versions (> N) are rejected with clear user warnings (`"This backup needs a newer Velora Studio"` / `"file made by a newer Velora Studio. Update the app."`).
- **Offline First Capability:** Studio must function fully without a connected unit when viewing or editing offline backups.

## Technical Decisions

- **Desktop Stack:** Tauri 2.x (Rust backend + vanilla web frontend HTML/CSS/ES modules). Zero heavy JS frameworks, zero CDN dependencies.
- **Protobuf Schemas:** `proto/ws_v1.proto`, `proto/backup.proto`, `proto/live_set.proto`, `proto/setlist.proto`, `proto/category.proto`.
- **Packaging Formats:**
  - `.velorabackup`: Full snapshot of setlists, Live Sets, assign maps, favorites.
  - `.velorasets`: One or more Live Sets or bundle of Live Sets + timbres.
  - `.veloratimbre`: One or more user-imported formatlib timbres.
- **Concurrency & Throttling:** Edits from Studio Knobs are throttled to 50 Hz per parameter lane; rapid tweaks coalesce, preserving base revisions to prevent race conditions.

## UX & Interaction Patterns

- **Footer Status:** Always reflects connection, active sounding Live Set, live edit state, and backup/export progress.
- **Studio Dialogs:** Modal dialogs trap focus, respond to Esc to close, announce status updates in polite aria-live regions, and maintain high contrast (WCAG AAA compliant text and borders).
- **Non-destructive Merges:** Imports and restores always offer Merge (keep non-conflicting, resolve collisions via suffix/UUIDv4) or Replace with clear impact descriptions.

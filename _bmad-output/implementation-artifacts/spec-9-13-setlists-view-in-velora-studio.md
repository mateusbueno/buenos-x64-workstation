---
title: 'Story 9.13: Setlists view in Velora Studio (Mac)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'eec61b46bcda9af68751a1162c5a243af2fd0b05'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/desktop/README.md', '{project-root}/engine/host/include/velora/host/setlist_controller.hpp', '{project-root}/_bmad-output/implementation-artifacts/spec-4-6-setlists-setlist-tags-and-live-set-stepping.md']
warnings: []
deferred:
  - summary: >-
      Not verified in the real Tauri webview (browser pane with a mock bridge only; not re-viewed after the review patches); drag and drop is HTML5 events (mouse; Tauri dragDropEnabled must be disabled on Windows); the cross-setlist move is two commands (Add then Remove), not atomic: a failed Remove leaves the Live Set in both setlists; membership knowledge is partial so a DUPLICATE on the target leaves the source copy.
    evidence: |-
      desktop/README.md.
    severity: medium
  - summary: >-
      Deferred polish: warning when deleting the loaded setlist, select() clearing entries before a failed read, inert/aria-hidden for dialogs and menus, dragleave flicker in WebKit, right-click context menu, bidi/zero-width names, two announcements in one tick losing the first.
    evidence: |-
      Recorded in the README.
    severity: low
---

<intent-contract>

## Intent

**Problem:** Preparing a show's running order is quick on the computer, but Velora Studio has no Setlists view: the sidebar lists setlists read-only (Story 9.8). The player must organise setlists, their Live Sets and setlist tags in Studio, including reordering by keyboard and mouse, with every change applied and persisted exactly as the device applies it (FR97 partial, UX-DR68, UX-DR90 partial: Alt+↑/↓ and "Move to…").

**Approach:** A Setlists view in the main area opened from the SETLISTS section: it lists the setlists (name, tags, Live Set count, missing count, loaded marker) and, for the selected setlist, its Live Sets in order (name, missing flagged). Operations, each sent as the SAME ws.v1 control-thread command the device surface uses (existing messages from Stories 4.6: CreateSetlist, RenameSetlist, ReorderSetlists, DeleteSetlist, AddLiveSetToSetlist, RemoveLiveSetFromSetlist, ReorderLiveSetsInSetlist, CreateLiveSet, CreateTag, RenameTag, DeleteTag, SetSetlistTags, and GetSetlists for reads; do NOT send LoadSetlist/UnloadSetlist/step commands from this view in this story unless the device surface equivalent needs them for "loaded" display only): create/rename/delete/reorder setlists; add (from All Live Sets), remove and reorder Live Sets inside a setlist; create a new Live Set (8 empty parts, one scene: the CreateLiveSet command) and optionally add it to the selected setlist; create/rename/delete tags and set a setlist's tags (cap 8 tags per setlist as the store enforces). The view re-reads the setlist data after every command reply (the ws.v1 command Ack/CreatedReply carries the result; State.setlists pushes keep it fresh when the device changes it) and shows typed errors inline. Keyboard: a focused row moves with Alt+↑/↓; a "Move to…" menu moves a Live Set to another setlist or position; the result is announced through a live region; drag-and-drop with the mouse is an equivalent (HTML5 drag events or pointer-based) with a visible drop indicator; all of it with the Story 9.8 accessibility conventions (roving tabindex, 2 px focus ring, textContent-only rendering, locale keys in en/es/pt-BR, token colours, rem sizes). Destructive actions (delete setlist, remove Live Set from a setlist) use the Studio dialog/confirm pattern already present (or a minimal in-page confirm) and never delete Live Set files from a setlist removal.

## Boundaries & Constraints

**Always:** Reuse the foundation (device client, `window.veloraDevice`, throttled runner and DOM test shim from Story 9.8, bridge message table which already allows the setlist commands: verify each command used is a paired allow-listed row), the pure view-model + DOM split with node tests and the cargo mapping tests. One command in flight per setlist mutation sequence is fine, but UI optimistic reordering must roll back on an error reply and always converge to the engine's order after the re-read. Reorder commands send the full permutation the store expects (ReorderSetlists: a permutation of all setlist ids; ReorderLiveSetsInSetlist: the full ordered ids): never partial. Names: client-side trim and length limits matching the store's limits (read the store: kMaxSetlists 100, kMaxTags 64, kMaxTagsPerSetlist 8, kMaxLiveSetsPerSetlist 256, name rules) with the engine as the authority (typed errors shown). If the engine lacks a command the spec needs, add it additively (e.g. none expected). Missing Live Sets (kept and flagged) are displayed, can be removed, and never block reordering. Accessibility: the reorder announcements ("Moved Song X to position 3 of 12"), "Move to…" menu is a labelled listbox/menu with keyboard operation, drag handles have accessible names, no horizontal scroll at 200% text.

**Never:** No Live Set editing (9.9-9.12), no loading/stepping from this view, no tag colours beyond what the store has, no changes to the device UI, no new persistence formats, no changes to the bridge security rules other than verifying rows.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Open view | paired device | setlists listed with tags/counts/loaded marker; selecting one shows its Live Sets in order | read failure → inline error, previous data kept |
| Create setlist | name | CreateSetlist; list refreshes; new setlist selected | typed error shown (e.g. limit) |
| Rename / delete setlist | confirm for delete | commands sent; list converges | typed error rolls back |
| Reorder setlists | Alt+↑/↓ or drag | full permutation sent; announced | error → rollback + message |
| Add Live Set to setlist | pick from All Live Sets | AddLiveSetToSetlist at index; list refresh | duplicate/limit errors shown |
| Reorder Live Sets | Alt+↑/↓, drag, Move to… (other setlist or position) | ReorderLiveSetsInSetlist / Remove+Add as the device does for cross-setlist moves; announced | partial failure leaves a consistent re-read state |
| Remove Live Set | confirm | RemoveLiveSetFromSetlist; the Live Set file stays | none |
| Create Live Set | name | CreateLiveSet; optionally added to the selected setlist | typed error shown |
| Tags | create/rename/delete, set per setlist (max 8) | commands sent; chips update | typed errors |
| Missing Live Set | entry flagged missing | shown with missing marker, removable | none |
| Device changes setlists meanwhile | State.setlists/library_revision changes | view refreshes (throttled), selection kept if still present | none |
| Offline/unpaired | no device | Setlists view disabled with the existing offline hint | none |

## Code Map

- `engine/host/include/velora/host/setlist_controller.hpp`, `setlist_store.hpp` (limits, name rules, commands, typed statuses), `proto/ws_v1.proto` (the setlist messages, SetlistsReply, CreatedReply, StepReply, ErrorCodes), `sysd/src/bridge_server.cpp` (message_rules rows), `engine/host/tests/setlist_controller_test.cpp` (command semantics).
- `desktop/src/app.js`, `live_set_view.js`, `live_set_dom.js`, `catalogs.js`, `index.html`, `style.css`, `desktop/tests/*`, `desktop/src-tauri/src/device.rs` (JSON of replies), `locale/*.json`.
- Mockup/design refs: DESIGN.md/EXPERIENCE.md Studio sidebar/Setlists notes; `mockups/key-screen-studio.html`.
- New: `desktop/src/setlists_view.js` (pure view-model: ordering helpers, permutation builders, move-to logic, validation, announcements), `desktop/src/setlists_dom.js` (DOM + keyboard + drag/drop), node tests for both, an end-to-end smoke (engine-host --no-audio --data-dir + bridge + Rust client) that creates a setlist, reorders, tags and reads back.

## Tasks & Acceptance

**Execution:**
- view-model: setlists/entries/tags model, optimistic reorder with rollback, permutation builders, move-to (cross-setlist via remove+add in the order the device surface uses; atomicity caveat documented), limits, announcements
- DOM: list + detail panes, Alt+↑/↓, Move to… menu, drag and drop with indicators, dialogs (create/rename/confirm), tag editor chips, live region, inline errors
- verify the paired allow-list rows; add missing rows only if a needed message is absent
- tests: node (view-model all rows, DOM shim: keyboard moves, drag simulation, hostile names), cargo mapping test for any new reply fields, engine smoke through the bridge (create → rename → reorder → tag → add/remove → create Live Set → restart engine persists), vacuity guards (a test that fails if the smoke finds zero setlists)
- docs: desktop/README.md

**Acceptance Criteria:**
- Given the SETLISTS section, when the Setlists view opens, then it lists setlists with their Live Sets in order and tags and supports create, rename, reorder setlists and Live Sets, create a new Live Set and create/edit tags, every change sent as the same control-thread command the device uses and persisted exactly as the device persists it.
- Given a focused row, then Alt+↑/↓ moves it, a "Move to…" menu moves a Live Set to another setlist or position, and the result is announced through a live region; drag-and-drop with the mouse is an equivalent.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` double submit on Enter created duplicates; a throwing callback froze the view (pending stuck); Delete shortcut unusable on macOS (Backspace); focus lost after remove/delete/move; Move to... offered for missing entries; blank names for missing entries; error mapping gaps (STORAGE_FULL, BUSY, UPSTREAM_UNAVAILABLE, timeout wording, partial move); pluralisation; rollback yanking the user's selection and reorders built behind unmodeled adds (INVALID_ORDER)
  - `[medium]` `patch` fake engine aligned with the real store, runtime guard against forbidden messages, placeholder parity test, smoke extended with tags and delete cascade, README overstatements
  - `[low]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "setlist|sysd|bridge|control|smoke|desktop" --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: Setlists view (setlists_view.js view-model with optimistic updates, per-mutation undo and queue discipline; setlists_dom.js with Alt+Up/Down, Alt+M Move to..., F2, Delete/Backspace, drag and drop, dialogs, tag editor, live region), 101 locale keys in en/es/pt-BR, only existing allow-listed setlist commands (no load/step/jump/recall/write ever sent), smoke through the real engine and bridge incl. persistence across restart.
Verification: node tests 111, cargo tests 22, bridge/setlist smokes pass, lints pass; full ctest only the 4 pre-existing failures plus load-sensitive librarian smokes (pass alone).

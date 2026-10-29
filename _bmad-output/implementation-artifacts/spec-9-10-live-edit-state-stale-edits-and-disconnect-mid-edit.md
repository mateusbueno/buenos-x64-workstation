---
title: 'Story 9.10: Live edit state, stale edits and disconnect mid-edit (Mac, against host engine)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '50913c1'
review_loop_iteration: 0
followup_review_recommended: true
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-9-9-part-editor-layout-with-studio-knobs.md', '{project-root}/proto/ws_v1.proto', '{project-root}/desktop/README.md']
warnings: ['NOT VERIFIED ON HARDWARE OR IN THE REAL WEBVIEW: the device STUDIO label is checked headless (visibility and text), not rendered on the board; the Studio Footer and Reload banner are checked through the node DOM shim, not in the Tauri webview.']
deferred:
  - summary: >-
      Two Studio computers editing the same Live Set are not told apart: both carry base_revision, so neither makes the other stale (the revision moves only for changes that are NOT Studio edits). Detecting it needs a per-connection edit origin (the plane has no client identity in handle()) or a revision that every accepted edit moves, with the ack carrying it and the sender chaining on it.
    evidence: |-
      engine/host/src/control_plane.cpp (studio_edit), proto/README.md ("a second Studio computer ... is NOT detected").
    severity: medium
  - summary: >-
      A Live Set switch started from the panel or a footswitch (not JumpToLiveSet / NextLiveSet / PreviousLiveSet over ws.v1) moves the revision only when the new Live Set is PUBLISHED, not when the load starts, so a Studio edit that lands during the load goes to the Live Set that is leaving (it stays in that document, dirty, and the revision then moves). Not lost or misplaced, but not refused.
    evidence: |-
      EditSession::on_published (id change) vs ControlPlane::handle (jump / step accepted).
    severity: low
  - summary: >-
      The Studio banner appears on the first REFUSED edit, as the AC says, not when the unit changes: the Live Set view follows State pushes and shows the new values, so a player may look at values that are newer than the revision the editor holds. A proactive "changed" notice (State revision > base) was left out.
    evidence: |-
      PartEditorController._markStale / _adoptBase.
    severity: low
  - summary: >-
      STUDIO is shown in the Performance view Status Bar only (the only screen with a Status Bar label set); other device screens do not show it. The ◆ in the Hero, List Rows and Result Tiles already existed (current_dirty / dirty_live_set_ids) and was only re-verified, not changed. Rendering on the board / pixel check of the label position (x = gutter + 380 between STREAMING OK and the centred rotary label) is not done.
    evidence: |-
      ui/src/screens/performance_view.cpp, ui/tests/performance_view_test.cpp (headless).
    severity: medium
  - summary: >-
      Counter restart: live_set_revision starts at 1 on every engine run. A Studio view built on a previous run could, by coincidence, equal the new counter; in practice a restart drops the link and Studio loads afresh on reconnect (base re-adopted from the first State). JSON numbers are exact only to 2^53 (about 9e15 changes): unreachable.
    evidence: |-
      ControlPlane::live_set_revision_, live_edit_state.js liveRevision.
    severity: low
  - summary: >-
      Carried from 9.9 and unchanged: FM / VA / sample part parameters are not consumed by the engines (edits stored, not audible); SetPartZone / SetPartArp missing; 9.11 (Add timbre) and 9.12 (Write) must send the same base_revision and clear the Footer scope / STUDIO state on success.
    evidence: |-
      spec-9-9 deferred list.
    severity: high
---

<intent-contract>

## Intent

**Problem:** Edits from Velora Studio play on the unit at once, but nobody could tell: the device did not say that Studio was editing, the Live Set diamond and the Footer did not describe what was unsaved or how it would be saved, an edit built on a Live Set that had since changed on the instrument silently overwrote the newer change (`base_revision` was recorded and ignored), and a pulled cable left Studio guessing what the unit holds (FR98 partial, FR99 partial, UX-DR45, UX-DR72).

**Approach:** (1) Engine: a per-Live-Set **revision** (`State.edits.live_set_revision`, additive field 13): a monotonic `uint64` owned by `ControlPlane` (starts at 1) that moves with every change of the sounding Live Set that is not a Studio edit (an edit command carrying `base_revision`): changes of the live values that no Studio edit explains (panel, encoders, faders, MIDI; seen in `EditSession::sync_live_values`, which asks `ControlPlane::consume_studio_echo` whether a changed slot is the echo of a Studio performance edit still on its way), an edit command without `base_revision` (device UI), a scene recall (the before-recall hook, not the restore after a timbre rebuild), the start of a Live Set switch (jump, step), and the publication of another Live Set. An edit command whose `base_revision` differs from the current revision (older or from the future) is refused with `ERROR_CODE_STALE_REVISION` = 47 (details `base_revision`, `current_revision`), nothing changes; Studio's own edits never move it, so a drag across knobs and structural edits do not make themselves stale. (2) `State.edits.studio_edit_active` (field 14): true for 3 s after the last accepted Studio edit; the control tick pushes the State when it flips (`take_studio_activity_changed`). (3) Device UI: the Performance view Status Bar shows `STUDIO` while it is set (key `performance.status.studio`); the ◆ already followed `current_dirty` / `dirty_live_set_ids`. (4) Studio: `live_edit_state.js` (pure: unsaved scope and Footer text from `State.edits`: `UNSAVED: SCENE n` when the Write Scene scope is available, `UNSAVED: LIVE SET` for a timbre change or no scene), `PartEditorController` keeps the revision it was loaded at as `base_revision`, handles `STALE_REVISION` (drop optimistic values and queued sends, read-only knobs, banner "Changed on the Velora since you opened it · Reload"), `reload()` (GetState, new base, knobs editable, Live Set view read again), ignores late answers from an older base or a dead link; a link that drops resets everything and the first State after reconnect gives a fresh base while the Live Set view reads the unit's unsaved edits back (◆ and Footer).

## Boundaries & Constraints

**Always:** additive proto only; `engine/core` untouched (the changes are in `engine/host`); no RT change; control thread only; no color literals; new strings in en / es / pt-BR (`locale/*.json` and the embedded `catalogs.js`); the bridge forwards `base_revision` and the new error unchanged (no new bridge message).

**Never:** no Write or save here (9.12); no Add-timbre (9.11); no change of the 9.9 command table; no new ws.v1 message.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Studio edit, current revision | `base_revision == live_set_revision` | accepted; revision unchanged; STUDIO on; Live Set dirty after capture | none |
| Studio edit, old or future revision | `base_revision != live_set_revision` | `STALE_REVISION` (47) with both revisions; nothing applied | Studio: banner + Reload |
| Edit without base_revision | device UI client | accepted; revision moves | none |
| Panel / encoder / MIDI change | live value differs and no Studio echo explains it | revision moves | none |
| Scene recall / Live Set switch | recall hook, jump / step accepted, other Live Set published | revision moves (switch: at accept and at publish) | edits built before are stale |
| Timbre edit from Studio | rebuild + restore | revision does not move for the rebuild, the restore recall or the capture | none |
| Idle 3 s | no Studio edit | `studio_edit_active` false, State pushed | none |
| Cable pulled mid-edit | link drops with a coalesced edit in flight | editor reset; late answers ignored; unit keeps applied edits dirty | after reconnect: fresh base, Live Set view shows ◆, Footer shows UNSAVED |
| Older unit | no `live_set_revision` | no `base_revision` sent | nothing judged |

## Code Map

- `proto/ws_v1.proto` (ERROR_CODE_STALE_REVISION = 47, EditStatus 13 / 14, base_revision comment), `engine/host/include/velora/host/control_plane.hpp` + `src/control_plane.cpp` (revision, stale judgement, STUDIO window, echo bookkeeping, jump / step bump, `fill_state`), `engine/host/src/edit_session.cpp` (sync foreign detection, before-recall hook, publish of another Live Set), `engine/host/macos/main.cpp` (tick pushes STUDIO flips).
- `ui/src/screens/performance_view.cpp` / `.hpp` (STUDIO label), `locale/*.json`.
- `desktop/src/live_edit_state.js` (new), `part_editor.js` (base, stale, reload), `part_editor_dom.js` (banner), `app.js` (Footer, reload wiring), `index.html`, `style.css`, `catalogs.js`.
- Tests: `engine/host/tests/control_test.cpp`, `edit_session_test.cpp`, `sysd/tests/bridge_test.cpp`, `ui/tests/performance_view_test.cpp`, `desktop/tests/live_edit_state.test.js`, `part_editor_dom.test.js`, `part_editor.test.js`, `support/fake_unit.js`, `desktop/src-tauri/src/device.rs` (real engine through the real bridge).

## Tasks & Acceptance

**Execution:**
- engine revision, stale judgement, STUDIO window, echo bookkeeping, bumps
- device Status Bar STUDIO
- Studio Footer scope, base revision, STALE_REVISION banner, Reload, reconnect
- tests for every row of the matrix; docs (proto/README.md, desktop/README.md)

**Acceptance Criteria:**
- Given Part Editor edits arriving, when the control thread accepts them, then the device Status Bar shows `STUDIO` and clears it after 3 s idle.
- Given an accepted edit, then the Live Set shows ◆ (device Hero, List Rows, Result Tiles, and Studio) and the Footer reads `● LIVE EDIT ON · CHANGES PLAY ON THE VELORA` plus `UNSAVED: SCENE n` or `UNSAVED: LIVE SET`.
- Given the Live Set changed on the unit after Studio loaded it, when a Studio edit with an old `base_revision` arrives, then it is rejected with `STALE_REVISION`, Studio shows the banner and Reload applies the current snapshot.
- Given the cable is pulled mid-edit, then applied edits stay on the unit with ◆ until Write or power-off, and after reconnect Studio reloads the device state and shows them as unsaved.

## Review Triage Log

### 2026-10-26 — Self review (adversarial, same author)
- findings:
  - `[high]` `patch` Studio's own edits must not make the revision stale: a revision bumped by every accepted change would reject the second knob of a drag. Resolved by the definition (Studio edits keep it) plus the echo bookkeeping so the later capture of the live value is not taken for a controller move; two quick edits of one knob, send knobs (live + structural) and part volume are covered by `test_live_set_revision_stale_edits_and_studio_indicator`.
  - `[high]` `patch` timbre edit from Studio: the rebuild's scene recall (restore) and value capture would bump the revision; the before-recall hook skips `restoring_` and the capture is off while restoring; tested (revision unchanged after `settle()`, next edit accepted).
  - `[high]` `patch` edit arriving during a Live Set switch: the jump bumps at accept, so an edit built on the Live Set that is going away is refused instead of landing on it; the publish of another Live Set bumps again. Step (Next / Previous) bumps too. Panel / footswitch-started switches only at publish (deferred).
  - `[medium]` `patch` stale answers from before a Reload, a Live Set change or a reconnect must not re-raise the banner: `_done` compares the request's base with the held base; `reset()` and `reload()` use a token; a Reload in flight when the link drops is ignored. Tested.
  - `[medium]` `patch` Studio would follow the latest State's revision and never be stale: the base is the revision at load (first State about the shown Live Set, or Reload), not the latest push.
  - `[medium]` `patch` a revision from the future (another engine run) is stale too (the 9.9 test that accepted it was changed on purpose).
  - `[low]` `patch` revision wrap: uint64, +1 per change; the JS side reads a JSON number (exact to 2^53); the engine restarts at 1 (documented, deferred).
  - `[medium]` `defer` second Studio computer, panel-started switch, proactive banner, other device screens (see deferred).

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build --output-on-failure` -- expected: all pass
- `node --test desktop/tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: engine revision + STALE_REVISION + STUDIO window + echo bookkeeping (control_plane, edit_session, main tick), additive proto (47, EditStatus 13 / 14), device Status Bar STUDIO, Studio Footer scope (live_edit_state.js), base revision at load, stale banner and Reload, reconnect handling, tests and docs.
Verification: full `ctest -j6` 703 / 703 passed (2 skipped as before), including control_tests, edit_session_tests, sysd_bridge_tests, performance_view_test, engine_host_bridge_studio_smoke (the Rust client against the real bridge and engine, with the STALE_REVISION and revision-bump assertions); `node --test desktop/tests` 183; `cargo test --offline` 23 (the real-engine round trip runs under ctest `engine_host_bridge_studio_smoke`, it skips without `VELORA_TEST_BRIDGE`); the three lints exit 0. The 4 tests that are known to flake under parallel ctest did not fail in this run. Status stays in-progress: the STUDIO label and the Footer / banner were not seen on the board or in the real webview, and the deferred items above remain.
Residual risks: see deferred (second Studio computer, panel-started switch window, restart counter, board rendering); the echo match tolerates a device move to exactly the value Studio just sent within 2 s (taken as Studio's).

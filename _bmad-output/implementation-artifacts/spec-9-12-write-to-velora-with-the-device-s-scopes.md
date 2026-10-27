---
title: 'Story 9.12: Write to Velora with the device''s scopes (Mac, against host engine)'
type: 'feature'
created: '2026-10-26'
status: 'in-progress'
baseline_revision: '1ba0c72'
review_loop_iteration: 0
followup_review_recommended: true
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-9-11-add-a-timbre-to-an-empty-part-from-velora-studio.md', '{project-root}/_bmad-output/implementation-artifacts/spec-9-10-live-edit-state-stale-edits-and-disconnect-mid-edit.md', '{project-root}/_bmad-output/implementation-artifacts/spec-9-9-part-editor-layout-with-studio-knobs.md', '{project-root}/_bmad-output/implementation-artifacts/spec-4-9-edits-in-memory-and-write-scopes.md', '{project-root}/proto/ws_v1.proto', '{project-root}/desktop/README.md']
warnings: ['NOT VERIFIED ON HARDWARE OR IN THE REAL WEBVIEW: the Write dialog, the banner and the Footer were driven headless (node DOM shim) and in the browser harness (tests/support/serve_mock.py with the fake unit, in the Browser pane), not in the Tauri webview, with a screen reader or on the dev board. The real engine runs only on the Mac, --no-audio: a Write is proven to be accepted, to clear the unsaved state and to create / replace the files, not to survive a power cut on the board. STORAGE FULL is injected through the store''s test hook in the engine tests and in the node tests only: the engine binary does not expose that hook, so the Rust client against the real engine does not see a real ENOSPC. A cable pulled with a Write out was simulated by a transport that rejects (node, with the real controllers), not with a real cable.']
deferred:
  - summary: >-
      A Write whose answer is lost is judged by State.edits.writes_done (files the engine wrote since it STARTED) compared with the value Studio saw before sending. That counter also counts Writes from the panel, so a panel Write between the drop and the reconnect makes a Write that did not land look as if it did ("Written to Velora. Studio confirmed it after reconnecting."), and a Write that landed while a panel Write ALSO happened is judged landed (right by luck, not by identity). A Write that carries an id the engine echoes in the State (a per-command token), or WriteReply persisted for the last N requests, is the exact fix (additive).
    evidence: |-
      desktop/src/write_to_velora.js _judgeUnknown; engine/host/src/edit_session.cpp writes_done_.
    severity: medium
  - summary: >-
      Save as new leaves the sounding Live Set with its unsaved edits (the engine's rule: the new Live Set is written next to it, the current one is not replaced), so the diamond stays and the Footer keeps UNSAVED. The story says "success clears ◆": that holds for Scene (when nothing else is dirty) and Whole Live Set, not for Save as new, where the announcement says "This Live Set still has its unsaved changes (◆)". Changing it means changing the device's rule (switch to the new Live Set, or reset the old document), which the AC forbids ("the device's rules").
    evidence: |-
      engine/host/src/edit_session.cpp do_write (SAVE_AS_NEW: "the current Live Set keeps its edits"), test_write_with_base_revision, real_write_flow.
    severity: medium
  - summary: >-
      The free-name suggestion and the name-in-use pre-check use the names Studio knows (the loaded setlist's entries it has read, or All Live Sets); when the setlist's entries were never loaded the hint is empty and the first answer comes from the engine (NAME_IN_USE with its suggestion, handled the same way, so only a round trip is lost). The suggestion is not reserved: two Studios (or Studio and the panel) can take it at the same time, and the loser gets NAME_IN_USE again.
    evidence: |-
      desktop/src/app.js knownLiveSetNames, openWriteDialog; test "a name that is free here but taken on the unit by the time the Write arrives (the race)".
    severity: low
  - summary: >-
      Write is offered for the Live Set that SOUNDS only (the engine's command writes the current Live Set; there is no "write Live Set X"). Unsaved edits that sit on another Live Set (State.edits.dirty_live_set_ids lists them) cannot be written from Studio until that Live Set sounds; the button says so in its tooltip. A Live Set switched away from keeps its dirty document in the engine and is lost at power-off.
    evidence: |-
      proto/ws_v1.proto WriteLiveSet ("the CURRENT Live Set only"), writeAvailability.
    severity: medium
  - summary: >-
      The Write is not cancellable once sent and the request timeout of device_request is 10 s: a Write that takes longer (a very slow store: the engine itself waits at most 2 s for the real-time fence, then writes with fsync) is reported UNKNOWN and resolved with Check now / on reconnect, never as a failure. Not measured on a slow SD / eMMC.
    evidence: |-
      desktop/src-tauri/src/main.rs device_request (Duration::from_secs(10)); edit_session.hpp fence_timeout_s.
    severity: low
  - summary: >-
      Carried from 9.10 / 9.11 and unchanged: two Studio computers on one Live Set are not told apart (a Write by one moves no live_set_revision, so the other's view is not made stale by it; it sees the unsaved state clear in the next State); a panel-started Live Set switch moves the revision when published; the Footer / banner were not seen on the board.
    evidence: |-
      spec-9-10 and spec-9-11 deferred lists.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Velora Studio's title-bar "Write to Velora" button was disabled for a real unit ("not available in this version yet"): what a player built on the computer played on the Velora at once but stayed in memory and was lost at power-off (FR98 partial, FR99 partial, UX-DR44, UX-DR46, UX-DR77, UX-DR91 partial).

**Approach:** (1) The command is the device's own: `WriteLiveSet{scope, new_name?}` (ui `NavigationController::handle_write_confirmed` -> engine `EditSession::do_write`). `write_live_set` was already an allowed `liveset.v1` engine route of the bridge (Story 4.9): no allow-list row, no proto field and no new message were needed; a bridge test now pins the row, the pass-through of scope / new_name / base_revision and the typed answers. (2) `base_revision`: decided YES. Studio sends the revision its view was loaded at and the engine now judges a Write that carries it like an edit (`control_plane.cpp`: STALE_REVISION, nothing written) without treating it as an edit (no STUDIO, `live_set_revision` unchanged); a Write without it (the device UI) is accepted as before. (3) Studio: `write_to_velora.js` (rules, controller, truth about results), `write_to_velora_dom.js` (modal Studio Dialog with the radio group of scopes, Save as new's name field, the Studio Banner), `PartEditorController.flush / writeContext / noteStale` (+ `EditSender.whenIdle`), wiring in `app.js` (button state, Footer `WRITING TO VELORA…`, refresh after a Write, link events), locale keys in en / es / pt-BR, CSS with tokens. (4) Rust client: no new command (`device_request` already carries any allowed message); `write_to_velora_messages_map_to_json` and `real_write_flow` against the real bridge and engine.

## Boundaries & Constraints

**Always:** additive only (the one engine change makes an existing command judge an existing field; proto gets a comment only); `engine/core` untouched; no RT change; no color literals; every string in en / es / pt-BR (`locale/*.json` and the embedded `catalogs.js`); text from the unit only as `textContent`; the strict CSP unchanged; the Write reuses the device's scopes, names rules and messages (names, NAME_IN_USE wording, "Timbre changes are saved with the Live Set", the storage-full sentence); success is claimed only for a `WriteReply`.

**Never:** no second Write path; no Write of a Live Set that does not sound; no automatic retry of a Write (a retry could write twice or write values the player did not mean); no success claimed for a Write whose answer never came; no secret, token, path or Live Set content in any log or announcement (nothing is logged by this feature).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| No edits | Live Set shown has no unsaved edits | button disabled, tooltip "Nothing to write"; the dialog never opens | none |
| Edits on another Live Set | shown Live Set does not sound but the sounding one is dirty | disabled, tooltip names why | none |
| Open | click on a usable button | dialog: Live Set name, what changed (localised, "and N more"), Scene / Whole Live Set (default) / Save as new…; focus on the chosen scope | Esc / Cancel / backdrop close, nothing sent, focus back |
| Timbre change in the edits | `has_timbre_edit` | Scene disabled with "Timbre changes are saved with the Live Set", skipped by the arrow keys, cannot be chosen | engine's own refusal (35) is handled the same way |
| Scene unavailable for other reasons | no scene / no scenes / scene not saved / factory | disabled with the matching sentence; a factory Live Set only offers Save as new | engine refusal handled |
| Save as new | name field | suggested free name (engine rule); trimmed; 1..64 bytes UTF-8, no control characters; a name in use is refused here with "A Live Set named 'x' is already in this setlist" and the free name put in the field; nothing sent | engine NAME_IN_USE (race) shown the same way with the engine's suggestion |
| Confirm | Primary Button | flush edits, then ONE `WriteLiveSet` with base_revision; Footer `WRITING TO VELORA…`; title-bar and dialog buttons disabled; Esc / backdrop ignored; double click sends once | flush not done in time: dialog stays, nothing sent |
| Success | `WriteReply` | dialog closes, focus to the button or the main area, live region "Written to Velora" (+ what remains unsaved), State and Live Set view re-read: ◆ and Footer clear (Scene with other changes and Save as new keep ◆) | `nothing_to_write` says so, never "written" |
| Stale view | unit changed after Studio loaded the view | STALE_REVISION: nothing written, dialog closes, "Changed on the Velora since you opened it · Reload" (Part Editor banner), edits stay | Reload, then write again |
| Storage full | engine ENOSPC | Studio Banner "Couldn't write to Velora 73. Not enough space to write. Free space in Settings › Storage & Library. Your edits are still on the Velora (◆)." | edits stay, ◆, write again after freeing space |
| Typed refusal | BUSY, STORAGE_FAILED, NO_LIVE_SET, LIMIT_REACHED, INVALID_COMMAND, unknown | Banner "Couldn't write to Velora 73: <reason>. Your edits are still on the Velora (◆)." | edits stay |
| Bridge cannot reach the engine | UPSTREAM_UNAVAILABLE connect_failed | Banner "Couldn't write to Velora 73: connection lost. Your edits are still on the Velora (◆). Reconnect and write again." | nothing was sent |
| Connection drops with the Write out | transport error / bridge timeout | UNKNOWN: Banner "Couldn't confirm the write…", nothing announced as written, second Write blocked; after reconnect (or "Check now") `writes_done` decides: landed / did not land / nothing left / unit restarted | judged by the unit's own counter |
| Link drops with the dialog open | status leaves paired | dialog closes, nothing sent, announced | none |
| Part Editor stale or no base known | view stale / revision not adopted | nothing sent: stale banner / "Studio is still reading this Live Set" | none |

## Code Map

- `engine/host/src/control_plane.cpp` (`write_judged`: a Write with base_revision is judged like an edit but is not one), `proto/ws_v1.proto` (comment), `engine/host/tests/edit_session_test.cpp` (`test_write_with_base_revision`).
- `sysd/tests/bridge_test.cpp` (`test_write_to_velora_is_a_paired_engine_command`); `sysd/src/bridge_server.cpp` unchanged (the row exists).
- `desktop/src/write_to_velora.js` (new: names, change labels, availability, `WriteController`), `write_to_velora_dom.js` (new: `WriteDialogView`, `WriteBannerView`, `announceText`), `part_editor.js` (`EditSender.whenIdle`, `PartEditorController.flush / writeContext / noteStale`), `app.js`, `index.html` (`#write-banner`), `style.css` (`wr-*`, `foot-writing`), `catalogs.js`, `locale/{en,es,pt-BR}.json` (`studio.write.*`, `studio.footer.writing`).
- `desktop/src-tauri/src/device.rs` (`write_to_velora_messages_map_to_json`, `real_write_flow`), `engine/host/macos/engine_host_bridge_smoke.cpp` (the log check).
- Tests: `desktop/tests/write_to_velora.test.js`, `write_to_velora_dom.test.js`, `support/write_rig.js`, `support/fake_unit.js` (WriteLiveSet with `do_write`'s rules, change list, writes_done, storage-full / name hooks).
- Docs: `desktop/README.md` (new section), `proto/README.md`, `sysd/README.md`.

## Tasks & Acceptance

**Execution:**
- engine: judge a Write's base_revision; test every scope, stale, revision unchanged
- bridge: pin the `write_live_set` row, pass-through and typed answers
- Studio: model + controller (flush, stale, unknown results), DOM (dialog, banner, keyboard), wiring (button, Footer, link events), locale, CSS
- Rust client: JSON mapping test and a real write flow
- docs and sprint status

**Acceptance Criteria:**
- Given unsaved edits on the selected Live Set, when the user clicks "Write to Velora", then a Studio Dialog lists what changed and offers Scene / Whole Live Set / Save as new… with the device's rules (Scene disabled with its reason on a timbre change; Save as new suggests a free name and rejects a name in use with the device message); with no edits the button is disabled with the tooltip "Nothing to write".
- Given the user confirms, then Studio sends the device's Write command, the Footer reads "WRITING TO VELORA…", the Primary Button is disabled, and on success ◆ clears on device and Studio and a polite live region announces "Written to Velora".
- Given the write fails (connection lost, storage full), then a Studio Banner explains it and the edits stay on the unit with ◆.

## Review Triage Log

### 2026-10-26 — Self review (adversarial, same author)
- findings:
  - `[high]` `patch` a Write built on a stale view would persist values Studio never showed: the engine ignored `base_revision` on `WriteLiveSet`. Now judged (STALE_REVISION, nothing written) without being an edit; verified in the engine (`test_write_with_base_revision`: past and future revisions refused for Whole and Save as new, counters unchanged, file untouched, still dirty), through the bridge (typed 47 relayed) and against the real engine (`real_write_flow`). A Write is not an edit, so it neither lights STUDIO nor moves `live_set_revision`: Studio's next edit on the same base is accepted after the Write (also verified).
  - `[high]` `patch` a coalesced edit still waiting for its turn would be missed by the Write (it would save the previous value and the edit would land after, leaving ◆). `EditSender.whenIdle` + `PartEditorController.flush` run before the command is sent; test with a real controller, the unit's answer delayed and three quick knob edits: the last value reaches the unit before `WriteLiveSet`, the Write is sent once. A flush that times out sends nothing.
  - `[high]` `patch` a connection that drops with the Write out: the result is unknown and the old code path would have been "failed" (or worse, "success" if the ack had arrived). Transport failures after the request left are UNKNOWN: banner without a claim, second Write blocked, judged by `State.edits.writes_done` against the value read before sending (landed / not landed / nothing left / restarted); judged only after the drop was seen (epoch), "Check now" for a Write that timed out on a live link; "not connected" with the link down at send time is the one definite case. Tested for each outcome, with the drop seen before / after the rejection. See deferred (the counter also counts panel Writes).
  - `[high]` `patch` only a `WriteReply` is a success: an Ack, an empty body or a missing body are failures ("the Velora gave no answer"), never announced as written; `nothing_to_write` is announced as "Nothing to write for this scope".
  - `[high]` `patch` double click / Esc while the Write is out: the controller refuses a second confirm (one command on the wire, tested at the model, in the DOM and in the page), Esc and the backdrop do nothing, and since a disabled button drops the focus in a browser the dialog itself holds it (Tab is trapped on it).
  - `[medium]` `patch` name collision race: Studio's hint (the names it has read) can be empty or old; the engine's NAME_IN_USE (details name, suggested_name) keeps the dialog open on Save as new, shows the device message, puts the engine's suggestion into the field and the next confirm works (tested). Names are checked the engine's way (trim, ASCII-only fold, 1..64 bytes UTF-8, no control characters, lone surrogates refused) and the suggestion is a port of `suggest_free_name` checked against the same table as `test_name_suggestion_pure` (including the 64-byte cut on a character boundary).
  - `[medium]` `patch` Scene disabled reason: from `State.edits` (has_timbre_edit, scene_write_blocked_reason, scene_write_available) with the device's wording; a timbre change that arrives while the dialog is open disables Scene and a confirm on Scene is refused without sending; the engine's own refusal (35) for a timbre change Studio had not heard of switches to Whole with the reason; factory Live Sets (`factory_read_only`) disable Scene and Whole.
  - `[medium]` `patch` storage full from the engine: edits stay on the unit with ◆ (State.edits.current_dirty true, writes_done unchanged), banner with the device's sentence, the button stays usable, and the same Write succeeds once space is back. Injected through the store's fault hook in the engine (`test_storage_full`, already there) and in the fake unit; the real engine binary has no such hook (see warnings).
  - `[medium]` `patch` Save as new and Scene leave dirty state by the engine's rules: the announcement and the footer say so instead of claiming ◆ cleared (see deferred).
  - `[medium]` `patch` focus: the dialog takes the focus on the chosen scope, Tab / Shift+Tab stay inside, Esc / Cancel / backdrop / success return it to the button, or to the main area when the button is disabled by then (never to nowhere); an IME composition in the name field owns Esc / Enter until it is committed.
  - `[medium]` `patch` disconnect with the dialog open, the Live Set stopping to sound, or the edits being written elsewhere while it is open: the dialog closes by itself with an announcement; nothing is sent. Unpairing is not "reset": a Write that is out stays tracked through a drop.
  - `[low]` `patch` secrets: nothing in this feature is logged; announcements and banners carry only the device name (HelloAck.model, "Velora" when absent), Live Set / error names from the unit as text.
  - `[low]` `patch` hostile names (markup in the Live Set name, in a change text, in a NAME_IN_USE name) are text only; the DOM shim throws if innerHTML is ever assigned.
  - `[medium]` `defer` writes_done is not an identity (see deferred), Save as new keeps ◆ (engine rule), only the sounding Live Set can be written, slow-store timeout.

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -j4 --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: the device's Write command is reachable from Velora Studio with the device's scopes. The engine judges a Write's `base_revision` (stale refused, nothing written; a Write is not an edit and does not move the revision), the bridge row was already there and is now pinned by a test, Studio gained the Write dialog (what changed, Scene / Whole Live Set / Save as new with the device's rules, names and messages), flush-before-write, the `WRITING TO VELORA…` Footer, the live-region result, the failure banner, and a truthful flow for a connection that drops with the Write out. Locale in three languages, tests and docs.
Verification: full `ctest -j4` 703 / 703 run, 701 passed and the same 2 skipped as before (librarian_demo_acceptance, sample_demo_null_test); an earlier `-j6` run had `stream_test` (timing under load) and `ui_shell_test` (one segfault, 1 of ~16 runs, 0 of 12 on a quiet machine; it links only `velora_ui`, which this story does not touch) fail and both passed when re-run, so treat `ui_shell_test` as a pre-existing flake that was not investigated. Includes `edit_session_tests` (`test_write_with_base_revision`), `sysd_bridge_tests` (`test_write_to_velora_is_a_paired_engine_command`) and `engine_host_bridge_studio_smoke`, whose Rust client logged `real_write_flow: ok (timbre edit present: true)` against the real bridge and engine (stale refused, NAME_IN_USE with a suggestion, Save as new keeps the edits, Scene refused with `timbre_change`, Whole clears `current_dirty` / `dirty_live_set_ids` without moving `live_set_revision`, a repeated Write is `nothing_to_write`; the CHECK on that log line was shown to fail when the expected text was mutated); `node --test desktop/tests` 276 / 276 (55 new in write_to_velora.test.js and write_to_velora_dom.test.js); `cargo test --offline` 26 / 26; the three lints exit 0. Browser harness (serve_mock.py, fake unit, Browser pane): the button went from disabled "Nothing to write" to enabled after an edit, the dialog opened with focus on Whole Live Set, Down selected Save as new with the name "Perfect 2", a double click on the Primary Button sent one `WriteLiveSet` (scope 2, base_revision = the unit's Live Set revision), the Footer read `WRITING TO VELORA…` with both buttons disabled, then the dialog closed, focus went to the main area, the live region said "Written to Velora", ◆ and `UNSAVED` cleared; storage full showed the banner with ◆ kept and Dismiss worked; a stale Write raised the Part Editor's Reload banner and wrote nothing. Status stays in-progress: not seen in the Tauri webview, with a screen reader, in light theme or on the board; the deferred items remain.
Residual risks: see deferred and warnings (the unknown-result judgement rests on `writes_done`; Save as new keeps ◆ by the engine's rule; only the sounding Live Set can be written; storage full is injected, not real; nothing seen in the real webview, with a screen reader or on the board).

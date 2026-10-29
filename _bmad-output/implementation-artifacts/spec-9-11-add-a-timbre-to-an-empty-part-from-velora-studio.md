---
title: 'Story 9.11: Add a timbre to an empty part from Velora Studio (Mac, against host engine and librarian)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'bc6e0bd'
review_loop_iteration: 0
followup_review_recommended: true
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-9-10-live-edit-state-stale-edits-and-disconnect-mid-edit.md', '{project-root}/_bmad-output/implementation-artifacts/spec-9-9-part-editor-layout-with-studio-knobs.md', '{project-root}/_bmad-output/implementation-artifacts/spec-9-8-live-set-view-scenes-and-part-strips.md', '{project-root}/proto/library_api.proto', '{project-root}/desktop/README.md']
warnings: ['NOT VERIFIED ON HARDWARE OR IN THE REAL WEBVIEW: the picker was driven headless (node DOM shim) and in the browser harness (tests/support/serve_mock.py with a fake unit), not in the Tauri webview, with a screen reader or on the dev board. The real librarian / bridge / engine run only on the Mac, --no-audio: the assignment is proven to be accepted, rebuilt and read back, not heard.']
deferred:
  - summary: >-
      The engine does not look the library up when it takes SetPartTimbre. A timbre whose library vanished between the list and the assignment is therefore ACKED and the part rebuilds as MISSING_LIBRARY (silent, empty name, "Missing library" in warn): never another timbre, but the player is told after the fact (live region + the strip), not refused up front with an error. Refusing it needs an engine-side resolve of the reference before the rebuild (a typed NO_SUCH_TIMBRE), which changes the engine's rule that a Live Set may reference libraries that are not installed.
    evidence: |-
      engine/host/src/edit_session.cpp do_set_timbre (validate() only checks the reference's shape); real_timbre_flow in desktop/src-tauri/src/device.rs (a made-up library id is Acked and reads MISSING_LIBRARY).
    severity: medium
  - summary: >-
      The timbre list is read whole (at most 5000 timbres) and searched on the computer. A unit with more timbres shows the first 5000 (by library, then catalog order) with "The Velora has more timbres than Studio lists" and the search cannot reach the rest. The Library API has no server-side search or category filter; adding one (additive: ListInstruments / a new SearchInstruments with query and category) is the fix.
    evidence: |-
      desktop/src-tauri/src/device.rs MAX_PICKER_TIMBRES, list_timbres; proto/library_api.proto ListInstruments.
    severity: medium
  - summary: >-
      The list is read each time the picker opens (up to 25 round trips for 5000 timbres, paged at 200 rows); there is no cache and no invalidation by library revision, and the list is not refreshed while the picker is open (a library imported on the unit meanwhile appears at the next open). The library_revision in State.setlists does not cover imports.
    evidence: |-
      TimbrePickerController.open / _load.
    severity: low
  - summary: >-
      Only EMPTY sample parts offer "+ Add timbre". Replacing the timbre of a part that has one, choosing an engine (organ / FM / VA) for an empty part, and clearing a timbre are not in this story (SetPartTimbre can do the first two; the picker never sends engine_type_id).
    evidence: |-
      live_set_dom.js (_empty), PartEditorController.assignTimbre (no engine type).
    severity: low
  - summary: >-
      A request that is out when the cable is pulled is judged by the unit: if it landed, the part is rebuilt there with the diamond and the Footer shows UNSAVED: LIVE SET after reconnect (9.10 behaviour), but Studio does not announce "your timbre was added" for it (the picker is gone, the answer is dropped). Not verified with a real cable pull; the abort path is unit tested.
    evidence: |-
      TimbrePickerController.abort, tests "the link dropping ...".
    severity: low
  - summary: >-
      Carried from 9.10 and unchanged: two Studio computers on one Live Set are not told apart (both carry base_revision); a panel-started Live Set switch moves the revision only when published; the Footer / STUDIO label were not seen on the board. 9.12 (Write) must clear the unsaved state after a successful write.
    evidence: |-
      spec-9-10 deferred list.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** An empty part in Velora Studio's Live Set view is a dashed "+ Add timbre" that does nothing (a disabled placeholder since 9.8), so a player cannot fill a Live Set from the computer without going back to the instrument (FR97 partial, FR98 partial, UX-DR41 partial).

**Approach:** (1) Library API through the bridge: no allow-list row is needed (`list_libraries`, `list_instruments` were already allowed, and the assignment is the existing engine command `SetPartTimbre` of `params.v1`, the one the device itself uses); the bridge now clears `LibraryInfo.source_path` (the absolute device path of an import) from every `LibraryList` it relays. Proto: no change. (2) Rust client: `library_api.proto` is compiled into the app; `DeviceClient::list_timbres` pages `ListLibraries` / `ListInstruments` (200 rows, halving on a reply too big for one frame, a vanished library skipped, at most 5000 timbres, any other failure is an error) and the Tauri command `device_library_timbres` / `window.veloraDevice.libraryTimbres()` expose it. (3) Studio: `timbre_picker.js` (model and controller: search, cap, navigation, choose, failure states, late replies dropped), `timbre_picker_dom.js` (modal combobox dialog, focus trap, live regions, IME-safe), `PartEditorController.assignTimbre` (one `SetPartTimbre` with `base_revision`, STALE_REVISION handled like a knob), wiring in `app.js` (an empty strip opens it on click / Enter / Space; the Live Set view is read again after the Ack; the picker closes on disconnect or when the part or Live Set changed under it), `applyState` keeps the strip showing what was chosen while the unit rebuilds, locale keys in en / es / pt-BR.

## Boundaries & Constraints

**Always:** additive only (no proto change); `engine/core` and the engine untouched; no RT change; no color literals; every string in en / es / pt-BR (`locale/*.json` and the embedded `catalogs.js`); text from the unit only as `textContent`; the strict CSP unchanged (no inline script, the picker uses classes in `style.css`); the assignment carries `base_revision` like every other Studio edit and bumps nothing for Studio itself.

**Never:** no import over the bridge (the allow-list rows for `import_request` / `preflight_request` stay denied); no Write (9.12); no substituting a timbre; no assignment by name; no engine type chosen by the picker; no request that changes the unit while the picker only lists or searches.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Open on an empty strip | click / Enter / Space on a "+ Add timbre" strip of the sounding Live Set | dialog, focus in the search field, timbres by category, "N timbres" in a polite live region | list failure: message + Try again |
| Live Set not sounding | empty strip of a Live Set that does not sound | no dialog; "Play this Live Set on the Velora to add a timbre to its parts." | none |
| Empty catalog | no library installed | "No timbres are installed on this Velora yet. ..." | none |
| Huge catalog | 5000 timbres | 200 rows drawn, "Showing the first 200: type more to narrow the list"; truncated unit says so | none |
| Search | "rhodes" finds "Rhôdes", words in any order, library and category label count, full-width text | list narrows, first match active, "N of M" / "No timbre matches" | none |
| IME composition | compositionstart .. compositionend | nothing filtered, Enter / Esc / arrows belong to the IME until the text is committed | none |
| Keyboard | Up / Down / PageUp / PageDown / Enter / Esc / Tab | move / add / close / stay inside the dialog; focus returns to the strip | none |
| Choose | Enter or click on a row | ONE `SetPartTimbre{part, library_id, instrument_key}` + base_revision; the Live Set view is read again; strip shows the timbre, ◆ lit, Footer `UNSAVED: LIVE SET`; announced | a second Enter while it is out sends nothing |
| Esc / Cancel / backdrop | any state but an assignment in flight | closes, nothing sent, "Timbre picker closed. Nothing changed." | none |
| Engine refuses | BUSY (rebuild in progress), INVALID_COMMAND, INVALID_PART | dialog stays on the same list with the engine's message; the part is unchanged | the player retries or picks another |
| Stale revision | the unit's controls changed the Live Set after Studio loaded it | STALE_REVISION: picker closes, the Part Editor banner (Reload) is raised, nothing assigned | Reload, then pick again |
| Library vanished | library removed between the list and the assignment (or later) | the engine takes the reference; the strip reads "⚠ Missing library", no name, announced; never another timbre | see deferred |
| Disconnect while open | link drops with the picker open or the request out | picker closes (nothing assigned by it), "The Velora disconnected. Nothing was changed."; a late answer is dropped; after reconnect Studio reads the unit (◆ if it landed) | none |
| Part filled / Live Set changed under the picker | the unit or another client fills the part, or another Live Set is selected | picker closes without assigning | `assignPickedTimbre` also refuses a part that is no longer empty |
| Older unit | no `live_set_revision` | no base_revision sent, nothing is ever stale | none |

## Code Map

- `sysd/src/bridge_server.cpp` (`on_library_frame`: `source_path` cleared), `sysd/tests/bridge_test.cpp` (library fake with replies, `test_library_forwarding`).
- `desktop/src-tauri/build.rs` (library_api.proto), `src/device.rs` (`library_call`, `list_timbres`, tests incl. `real_timbre_flow`), `src/main.rs` (`device_library_timbres`).
- `engine/host/macos/engine_host_bridge_smoke.cpp` + `CMakeLists.txt` (a real `librarian-service` over two SFZ libraries imported with the librarian CLI, `engine_host_bridge_studio_smoke` only).
- `desktop/src/timbre_picker.js`, `timbre_picker_dom.js` (new), `part_editor.js` (`assignTimbre`), `live_set_dom.js` (empty strip opens the picker), `live_set_view.js` (`applyState` rebuild guard), `app.js`, `style.css`, `catalogs.js`, `locale/{en,es,pt-BR}.json` (`studio.timbre_picker.*`, `studio.live_set.add_timbre_tooltip` / `_hint`; `add_timbre_soon` removed).
- Tests: `desktop/tests/timbre_picker.test.js`, `timbre_picker_dom.test.js`, `live_set_dom.test.js`, `support/fake_unit.js` (`empties`, catalog, SetPartTimbre, `listTimbres`), `support/mock_device.js`.
- Docs: `desktop/README.md` (new section), `proto/README.md` (`timbre_ref` row), `sysd/README.md` (library allow-list note).

## Tasks & Acceptance

**Execution:**
- bridge: strip `source_path`; test with a fake librarian
- Rust client: list timbres through the allow-list, tests against a fake bridge and a real librarian + bridge + engine
- Studio: picker model + DOM, assignment through the Part Editor controller (base_revision, stale), strip wiring, locale, CSS
- docs and sprint status

**Acceptance Criteria:**
- Given an empty part, when the user clicks "+ Add timbre", then a picker lists the device-installed timbres (Library API through the bridge) by category with search, is fully operable by keyboard and closes with Esc without any change; choosing one assigns it live as a structural change (◆, Footer `UNSAVED: LIVE SET`, Whole Live Set scope).
- Given the chosen timbre's library is later missing on the device, when the Live Set is shown again, then the strip shows "Missing library" in warn as in Story 9.8, never another timbre.

## Review Triage Log

### 2026-10-26 — Self review (adversarial, same author)
- findings:
  - `[high]` `patch` library vanished between the list and the assignment: the engine does not validate the reference, so the call is Acked and the part reads MISSING_LIBRARY. Verified against the real engine (`real_timbre_flow`: a made-up library id is Acked, the part reaches `MISSING_LIBRARY`, `GetLiveSetView` gives `missing_library: true` and an empty name). The picker closes as "assigned", the page re-reads the view and announces "the library of this timbre is missing" in place of the success text. An up-front engine refusal would change engine rules: deferred (first item).
  - `[high]` `patch` the strip flicked back to "+ Add timbre" under the player's choice: right after the Ack the State still reports the old generation (EMPTY_TIMBRE) until the rebuild publishes. `applyState` does not overlay a part's kind while `State.loading.loading` is true; tested (and the old behaviour without the flag).
  - `[high]` `patch` a choice must never land on another timbre: rows are identified by `(library_id, instrument_key)`, not by name (two libraries may both have "grand": tested), a row that is not drawn (above the cap, filtered out) or forged cannot be chosen, the list is never re-sorted under a pending choice, a second Enter while the request is out sends nothing, Esc does not take back a request that is out.
  - `[high]` `patch` stale revision during the pick: the assignment carries the revision the view was loaded at; STALE_REVISION closes the picker, raises the 9.10 banner and nothing is assigned (fake unit and real engine); while the banner is up nothing is sent; Reload works.
  - `[medium]` `patch` late replies: a list for a picker that was closed, re-opened for another part or aborted is dropped (tokens); an assignment answer after an abort or after `reset()` (cable pulled, Reload) is not believed (no banner raised on a view that is gone). Tested.
  - `[medium]` `patch` empty and huge lists: an empty catalog has its own message; the drawn rows are capped at 200 and the cap is said; the unit-side list is capped at 5000 and `truncated` is said; one heading per run of one category; a query is cut at 80 characters.
  - `[medium]` `patch` search with IME and diacritics: text is folded NFKC + NFD without combining marks + NFC, so "rhodes" finds "Rhôdes", full-width text matches, kana keep their voiced marks; while an IME composes the list is not filtered and Enter / Esc / arrows belong to the IME (`isComposing` / keyCode 229); text typed while the list loads is kept.
  - `[medium]` `patch` focus: the focus moves to the search field on open, Tab / Shift+Tab stay inside, Esc / Cancel / backdrop / success / abort return it to the strip (the strip cell survives re-renders); the focus is never stolen by a closed controller.
  - `[medium]` `patch` disconnect while open: `applyDeviceStatus` aborts the picker (nothing assigned by it); a Live Set / part change under an open picker closes it as well, except while its own assignment is in flight (the part legitimately stops being empty then).
  - `[medium]` `patch` device paths: `LibraryInfo.source_path` carried the absolute import path through the bridge (a pre-existing leak on an allowed message); the bridge clears it, tested, and the Rust test asserts it never appears.
  - `[low]` `patch` the rebuild timing: the page re-reads the view right after the Ack (the unit's edited copy already holds the timbre); the State signature (part.timbre key) re-reads again when the push arrives.
  - `[low]` `patch` the disabled placeholder is gone: `add_timbre_soon` removed from all catalogs; the tooltip and the accessible name say what Enter does.
  - `[medium]` `defer` engine-side validation of the reference, server-side search, caching, replacing / clearing a timbre (see deferred).

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -j6 --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: bridge strips device paths from library lists; Rust client lists the installed timbres through the existing library allow-list (paged, capped, typed failures) and the page assigns with the existing `SetPartTimbre` + `base_revision`; Studio picker (model, DOM, keyboard, IME, focus, live regions), strip wiring, rebuild-safe strip, Footer / diamond through the 9.10 state, missing-library strip, locale in three languages, tests and docs. No allow-list row and no proto change were needed.
Verification: full `ctest -j6` 703 / 703 passed (2 skipped as before: librarian_demo_acceptance, sample_demo_null_test), including `sysd_bridge_tests` (source_path removed, instruments relayed) and `engine_host_bridge_studio_smoke`, which now runs a real `librarian-service` over two imported SFZ libraries, the real bridge and engine, and the Rust client (`real_timbre_flow: ok (2 timbres listed)`: list, assignment with base_revision, view read back, stale refused, bad part, unknown library reads MISSING_LIBRARY with an empty name; the CHECK on that log line was shown to fail when the expected text was mutated); `node --test desktop/tests` 221 / 221 (38 new in timbre_picker.test.js and timbre_picker_dom.test.js plus the changed live_set_dom test); `cargo test --offline` 25 / 25; the three lints exit 0. Browser harness (serve_mock.py, fake unit, headless-style keyboard): Enter on an empty strip opened the dialog with focus in the field, typing "rhodes" + Enter assigned (base_revision 5, strip shows the timbre, diamond lit, Footer `UNSAVED: LIVE SET`, live-region text), Space + Esc closed with the focus back on the strip and nothing sent. Status stays in-progress: not seen in the Tauri webview, with a screen reader, in light theme or on the board; the deferred items remain.
Residual risks: see deferred (a vanished library is accepted and shown as Missing library rather than refused; the 5000-timbre ceiling and no server-side search; the picker lists at open only; a cable pulled with a request out is not announced); the real flow runs with two single-instrument SFZ libraries, so paging against the real librarian is covered only by the fake-bridge test (200 / 250 rows and the too-large halving), not by a large real catalog; `State.loading` is relied on to tell a rebuild from a normal State (verified in the node model, not under audio).

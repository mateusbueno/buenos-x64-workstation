---
title: 'Story 9.8: Live Set view in Velora Studio: scenes and part strips (Mac)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '0191f0e61ba049ee7a54cf5ba33ee4704867d239'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/desktop/README.md', '{project-root}/sysd/README.md', '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/key-screen-studio.html']
warnings: []
deferred:
  - summary: >-
      Not verified in the real Tauri webview (only the built-in browser with a stub bridge); latency measured at the view-model level, not through the real Tauri event path; Part Editor cards below the strips still simulated until Story 9.9; "+ Add timbre" is a disabled placeholder (9.11); 16-part Pro view shows the first 8 plus a note; Write to Velora disabled while paired.
    evidence: |-
      desktop/README.md.
    severity: medium
  - summary: >-
      Known limits: dirty_live_set_ids capped at 128 (non-sounding sets past the cap may lose the diamond), the diamond tooltip is mouse-only and role="row" with display:contents may lose row semantics in WKWebView (accessibility follow-ups), unavailable libraries all render as "Missing library" (reason carried in unavailable_reason), engine-side LRU cache of resolved names keyed by library link, no per-connection rate limit on GetLiveSetView beyond the bridge's.
    evidence: |-
      Documented in READMEs.
    severity: low
---

<intent-contract>

## Intent

**Problem:** With the device connection in place (sysd bridge + Studio device client), Velora Studio still renders simulated data. The player must see a whole Live Set on the computer: its scenes and its parts at a glance, navigated from the SETLISTS section of the sidebar, without changing what the unit plays (FR97 partial, UX-DR14, UX-DR30, UX-DR40, UX-DR41, UX-DR67, UX-DR90 partial, UX-DR91 partial).

**Approach:** (1) Engine read API (additive ws.v1, control thread, READ-ONLY): `GetLiveSetView{live_set_id}` → `LiveSetViewReply` with the Live Set's name, whether the device has unsaved edits for it (dirty ◆), the scenes (index, name, empty flag) and the parts (index, on/off, timbre name, category, volume, key range low/high, organ registration string for organ parts, "missing library" flag, empty flag) assembled from the stored Live Set (and the unsaved-edit overlay when the device holds one) plus the library catalog/resolver for timbre name and category; it never loads, publishes or recalls anything (test: sounding generation, scene and arbiter untouched). The bridge's message table gets the new message as an allowed paired read. Setlist listing for the sidebar reuses `GetSetlists` (setlists with entries; `include_all_live_sets` exists since Story 8.9). (2) Studio JS: a pure, unit-testable view-model module (`desktop/src/live_set_view.js`, ES module, no DOM) that turns State pushes, SetlistsReply and LiveSetViewReply into the view model (header with name and ◆, 8 scene tiles (current filled, empty dashed), 8 part strips: `PART n` + LED, timbre name, category in accent, level meter, `VOL n`, key range, organ registration on organ parts, "+ Add timbre" on empty parts, "Missing library" in warn with glyph for missing libraries) and applies deltas without full reload; DOM rendering in `app.js`/`index.html`/`style.css` following `mockups/key-screen-studio.html` and DESIGN.md tokens (rem sizes, body ≥13 px, labels ≥11 px at 600, 2 px focus ring); sidebar: SETLISTS expands to setlists and their Live Sets; selecting a Live Set requests its view (read-only) and shows it in the main area; the Live Set that sounds follows State pushes live (scene change, part on/off, volume) within 100 ms of the delta arriving (measured at the view-model/render level with an injected clock); ◆ in warn with `aria-label="Unsaved changes"` and a tooltip; keyboard: arrow keys move within the scene grid and the part grid, Enter on a part strip selects it (selection state exposed for Story 9.9); all strings through the shared locale catalog keys (en, pt-BR, es). Simulated mode (no device) keeps today's behaviour. When paired, the editor area for the selected part (existing editor) keeps working in simulated mode until Story 9.9; show the real Live Set view for the main strips/scenes, and do not let "Write to Velora" act on real data in this story.

## Boundaries & Constraints

**Always:** READ-ONLY semantics for selection: selecting a Live Set in Studio never sends Jump/Recall/Load. Real data only from the device client (`window.veloraDevice`: state store, request helper from the foundation); mapping code is pure and tested with node (`node --test`) including edge cases; no hard-coded colours (token lint passes), no inline handlers (CSP set in the foundation: keep it passing); accessibility: scene/part grids are `role="grid"` or equivalent with roving tabindex, arrow-key navigation (wrap rules documented), Enter selects, live-region announcement for the selected part, the ◆ labelled; deltas update the DOM in place (no full re-render of unaffected tiles); 8 parts and 8 scenes fixed layout (Pro 16 parts: show the first 8 and note 16-part Studio view as deferred). Engine changes are additive (new ClientMessage/ServerMessage bodies, no number reuse), control thread only, no RT involvement, no allocation on RT; bridge table updated with a descriptor-walking test still green. The Rust client JSON mapping of the new reply must keep u64 fields safe (strings or documented). Existing tests (sysd, bridge, engine, cargo, UI lints) stay green.

**Never:** No Part Editor (9.9), no write/live-edit commands (9.10-9.12), no Add-timbre flow (9.11: "+ Add timbre" is a disabled placeholder with tooltip), no setlist editing (9.13), no change to what the unit plays, no 16-part layout.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Select Live Set | sidebar → Live Set | GetLiveSetView sent, view shown; unit untouched | request failure → inline error banner, previous view kept |
| Header | view with unsaved edits | name in desktop-live-set-name style; ◆ warn with aria-label "Unsaved changes" + tooltip | none |
| Scenes | 8 tiles | current filled, others normal, empty dashed | none |
| Parts | mixed parts | strips: PART n + LED, timbre name, category (accent), meter, VOL n, key range, organ registration on organ parts, "+ Add timbre" on empty | none |
| Missing library | part whose library is missing | "Missing library" in warn with glyph, never another timbre's name | none |
| Delta | device changes scene/part on-off/volume | view updates within 100 ms of the delta, in place | none |
| Unknown/unsounding set | selected set is not the sounding one | static view from GetLiveSetView; live deltas only for the sounding set | none |
| Keyboard | focus in scene/part grid | arrows move, Enter selects part, announcements | none |
| Offline | no device | existing offline UI | none |
| Pro device | 16 parts | first 8 shown + note | none |
| Edits overlay | device holds unsaved edits for the set | view reflects the overlay and ◆ | none |

## Code Map

- `proto/ws_v1.proto` (State, PartState, EditStatus, SetlistsReply/SetlistEntry, GetSetlists), `engine/host/src/control_plane.cpp`/`control_server.cpp` (handlers), `engine/host/src/live_set_store.cpp`/`live_set_loader.cpp`/`library_resolver.cpp`/`live_set_detail.hpp`/`scene_controller.cpp`/`edit_session.cpp` (stored Live Set, overlay, missing-library detection, timbre names), `librarian/*` catalog (categories) -- sources for the view; `sysd/src/bridge_server.cpp` (message_rules table); `desktop/src-tauri/src/device.rs` (JSON mapping), `desktop/src/app.js`, `index.html`, `style.css`, `tokens.css`; mockup `key-screen-studio.html` + DESIGN.md/EXPERIENCE.md (Studio sections); `locale/*.json` and tools/ci lints (strings, colour literals).
- New: `desktop/src/live_set_view.js` (+ `desktop/tests/live_set_view.test.js` run by `node --test`), engine test for GetLiveSetView, bridge allow-list update, ctest entry for the node test if node is available.

## Tasks & Acceptance

**Execution:**
- ws.v1 GetLiveSetView/LiveSetViewReply (additive) + engine handler (read-only, overlay-aware, missing-library flag, category/timbre names) + bridge table
- Studio view-model module + DOM rendering + sidebar + keyboard + live deltas + locale strings
- tests: engine (read-only guarantee, overlay, missing library, unknown id, pro 16 parts), node view-model tests (all rows, delta latency with injected clock), cargo mapping test, bridge test for the new rule, end-to-end smoke (engine-host + bridge + Rust client requesting a view); if feasible verify rendering in the built-in browser with a mock bridge object and attach screenshots notes in the result
- docs: desktop/README.md, proto/README.md

**Acceptance Criteria:**
- Given a paired connection and the published state, when the user expands SETLISTS and selects a Live Set, then the main area shows the header (name, ◆ in warn when the device has unsaved edits for it), 8 scene tiles (current filled, empty dashed) and 8 part strips with the specified content, and selecting a Live Set does not change what the unit plays.
- Given state deltas from the device, then the view updates within 100 ms of the delta arriving without a full reload.
- Given keyboard use, then arrow keys move in the grids, Enter selects a part strip, the ◆ has aria-label "Unsaved changes" and a tooltip.
- Given a part whose library is missing, then the strip shows "Missing library" in warn with a glyph, never another timbre's name.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` stale-view races (reply older than a structural change, dropped re-read advancing the signature, render failure corrupting the diff base, pre-outage state after reconnect); request storms (throttled re-reads); State from an older unit turned every part OFF (view_fields presence flag); change_summary cap hid timbre edits; error banner and title lifecycle; wrong announcements for empty parts; hard-coded aria-labels; catalog drift between app.js and locale json
  - `[medium]` `patch` control-thread cost of GetLiveSetView (bounded LRU of resolved names), id echo, read-only test gaps, DOM module with shim tests, vacuous 100 ms test replaced
  - `[question]` fresh unit showing the diamond: checked — not an engine bug (current_dirty=0 on a fresh data dir)

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "control|sysd|bridge|live_set|smoke" --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: ws.v1 GetLiveSetView/LiveSetViewReply (read-only, overlay-aware, LRU of resolved names), PartState view fields, bridge rule, Studio view-model (live_set_view.js), DOM module with shim tests, sidebar with setlists and Live Sets, header with warn diamond, 8 scene tiles and 8 part strips, keyboard grids, in-place deltas, throttled re-reads, locale catalogs in sync.
Verification: engine/bridge/control tests pass, node tests (46) and cargo tests (21) pass, lints pass; full ctest only the 4 pre-existing failures.

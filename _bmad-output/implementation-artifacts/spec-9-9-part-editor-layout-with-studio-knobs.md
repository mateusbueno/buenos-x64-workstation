---
title: 'Story 9.9: Part Editor layout with Studio Knobs (Mac, against host engine)'
type: 'feature'
created: '2026-10-26'
status: 'in-progress'
baseline_revision: '99cea91e64fdabfd6e07b2603c521af359d53f45'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/desktop/README.md', '{project-root}/proto/params.json', '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/key-screen-studio.html', '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/studio-part-editor-fm-va.html']
warnings: ['REMAINING LIMITATION (engine follow-up, see Auto Run Result addendum): three Part Editor knobs are stored but NOT audible: part_attack / part_release on organ and FM parts (the organ has no amp envelope; the FM contour is its operator envelopes) and va_unison_spread (the VA voice sums its unison voices to mono before the filter). Everything else is audible: organ, FM (fm_*: the part''s next note; fm_brightness live), VA (va_*: continuous ones live and smoothed, discrete ones next note), sample part_attack / part_release, part_brightness / part_eq_* on every engine, sends, output, volume, inserts, ASSIGN. See desktop/README.md and engine/core/README.md.']
deferred:
  - summary: >-
      WHAT STAYS of the former engine gap (the gap itself is closed, see the Auto Run Result addendum): part_attack / part_release are still stored only on organ and FM parts (organ: no amp envelope; FM: the contour is the six operator envelopes and the registry says nothing about how a second amp envelope combines with them), and va_unison_spread is stored only (the VA voice sums the unison voices to one mono signal before the filter; a stereo spread needs a stereo filter path). FM / VA discrete patch parameters apply to the part's NEXT note (patch copied at note start), not to sounding notes. Not verified on real hardware (host build only).
    evidence: |-
      engine/core/README.md "Registry part parameters in the engines", part_editor.js INERT_BY_ENGINE, engine/core/tests/part_params_test.cpp.
    severity: medium
  - summary: >-
      Other deferrals: SetPartZone and SetPartArp commands (zone, keyboard, octave, transpose, arpeggiator are read-only in the editor; no existing ws.v1 command, the device UI edits them in-process only), ASSIGN part-scope params land on State.encoders.selected_part, one tab stop per knob (spec asked roving order), insert param labels generic, base_revision recorded and ignored until Story 9.10, only the sounding Live Set is editable, --no-audio host snaps perf edits back (no render thread), Studio not checked in the real Tauri webview, 1280 px window computed not measured.
    evidence: |-
      READMEs.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Velora Studio's Part Editor (below the part strips) is still the simulated editor from Story 9.5/12.13 and does not edit the unit. The player must see every parameter of the selected part on one screen, edit it with mouse or keyboard through Studio Knobs, and hear the change on the unit immediately (FR97 partial, FR98 partial, FR103 Studio side of ASSIGN editing, UX-DR42, UX-DR43, UX-DR91 partial). The parameters come from the generated registry (`proto/params.json`) so a parameter added to the registry appears without hand-written UI per field.

**Approach:** (1) Registry for Studio: extend the generator pipeline (`proto/gen_params.py` or a new script wired into the existing generation, checked by CI like the other generated outputs) to emit `desktop/src/params_registry.js` (ES module: id, name, class, type, scope, unit, min, max, default, enum values, cc, assignable, locale_key, plus a UI grouping: section = sound / amp-envelope+eq / effects / zone+keyboard+arp+assign, and per-engine applicability (organ drawbars/controls, sample, FM, VA, from the registry's id ranges or names: derive from data, document the rule; if the registry lacks grouping metadata add additive fields to params.json with the generator validating them)). (2) Pure view-model `desktop/src/part_editor.js` (no DOM): builds the four-section model for the selected part from the registry + the part's current values (State.performance/master/parts + inserts/sends from State/PartState + LiveSetView data) + engine type; Studio Knob model: value, step rules (↑/↓ = 1 step, Shift ±10, PgUp/PgDn = large step, Home/End = min/max, Enter = numeric value entry, wheel/scroll, drag), clamping and quantisation per type (int/bool/enum/float), formatting with unit, `aria-valuetext`; envelope polyline from attack/decay/sustain/release values; effects rows (insert type, bypass, reorder, "+ Add insert", reverb/delay sends), zone & keyboard (low/high key, octave, transpose, keyboard on/off), arpeggiator section (from registry arp parameters), ASSIGN · KNOBS (THIS LIVE SET) map editor (K1..K8 slots assigned from assignable params via the existing SetAssignSlot/ClearAssignSlot). (3) DOM `desktop/src/part_editor_dom.js`: four hairline-divided sections, no menus/tabs, Studio Knobs as ARIA `slider` (role, aria-label name, aria-valuemin/max/now, aria-valuetext with unit), keyboard handling, pointer drag (pointer events, drag distance mapping, fine with Shift), wheel, value entry on Enter; reflow 4 → 2 → 1 columns at 150%/200% text size (CSS container/media queries on rem), no horizontal scroll; token colours only; textContent-only; locale keys en/es/pt-BR (parameter labels from registry locale_key via the shared locale catalog; add missing keys). (4) Sending edits: each change is sent as a ws.v1 edit message that the engine already supports for that parameter class (performance params → `SetPerformanceParam`; structural: part timbre/output/inserts/sends/zone via the existing 4.9 commands `SetPartTimbre`, `SetInsertChain`, `SetSendLevels`, `SetPartOutput`, `SetMasterParams`, `SetPartVolume`, ASSIGN slot commands, arp via the part arp command if present: read `proto/ws_v1.proto` to map each registry class/scope to its command and document the table; if a registry class has no command, show the control read-only with a reason tooltip and list it as deferred) and carries `base_revision`: add an additive envelope field to `ClientMessage` (`uint64 base_revision = 2`; 0 = not provided, accepted as today) that the engine records for the edit; ENFORCEMENT (STALE_REVISION rejection and Banner) is Story 9.10, not here: document that base_revision is carried and ignored until 9.10. Sends are coalesced per knob (latest value wins, at most one in flight per parameter, throttled ~30 ms) so a drag does not flood the bridge (the bridge limit is 500 msg/s); the unit plays the change immediately (performance edits become timestamped UMP; structural edits publish a generation) and the Studio view shows the value the engine confirms (State push), reverting a knob to the confirmed value on a typed error. The bridge message table must allow every command used (paired, edit class) and keep the descriptor-walking test green. (5) Replace the simulated editor cards in paired mode (keep simulated mode offline unchanged); with a real selection show the Part Editor for the selected real part (strips' selection event `velora:part-selected` from Story 9.8).

## Boundaries & Constraints

**Always:** The registry is the single source of the parameter list (a unit test adds a fake param to a registry fixture and sees it rendered). Edits go only through existing control-thread commands (plus the additive base_revision envelope); no new engine behaviour except recording/ignoring base_revision. Knob values come from engine-confirmed State (optimistic display allowed while a send is in flight, reverted on error/timeout). Performance edits are not saved: the ◆ and Write flow is Story 9.10/9.12 (do not add write UI here beyond what exists). Organ parts show drawbars (9) and controls; FM/VA parts show their Sound sections using the existing FM/VA registry entries (Story 12.13's simulated editor had sections for FM/VA: reuse its layout/strings where they match the registry). Accessibility: knobs are focusable sliders (roving order by visual order), 2 px focus ring, ARIA sliders with value text, value-entry input labelled, live region for Enter-entry validation errors, no information by colour alone; text sizes rem, body ≥13 px, labels ≥11 px at weight 600. 16-part Pro: the editor edits whichever part is selected (strips show 8; note). Existing desktop node/cargo tests, engine and bridge tests stay green; the string/colour lints stay green.

**Never:** No Write/save (9.12), no stale-revision enforcement or live-edit banner/footers (9.10), no Add-timbre picker (9.11), no Library API usage, no new proto messages except the envelope field, no changes to RT code, no hand-written per-parameter UI.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Open editor | select a real part | four sections built from the registry for the part's engine; values from State | unknown engine → generic sections |
| Registry growth | param added to registry fixture | appears without UI code | none |
| Knob keys | ↑/↓, Shift, PgUp/PgDn, Home/End, Enter | value changes per rules, clamped/quantised; aria-valuetext updates | invalid entry → inline message, value unchanged |
| Drag / wheel | pointer drag, wheel | value changes, coalesced sends | none |
| Send | knob changes | edit sent with base_revision; unit plays it; confirmed State updates the knob | typed error → knob reverts + inline message |
| Bool / enum | switch / select-like knob | exact values | none |
| Effects | bypass, reorder, add insert, sends | SetInsertChain/SetSendLevels sent with the full chain | typed error reverts |
| ASSIGN | assign K1..K8 | SetAssignSlot/ClearAssignSlot | typed error reverts |
| No command for a class | registry class without command | control read-only with reason, listed as deferred | none |
| Reflow | 150%/200% text | 4 → 2 → 1 columns, no horizontal scroll | none |
| Offline | no device | existing simulated editor | none |
| Flood | rapid drag | at most one in flight per parameter, throttled | none |

## Code Map

- `proto/params.json`, `proto/gen_params.py`, `proto/params.cmake`, `proto/CMakeLists.txt` (generation wiring and CI check of generated files), `proto/ws_v1.proto` (edit commands, ClientMessage envelope, ErrorCodes, State/PartState/ParamValue), `engine/host/src/control_plane.cpp` (handlers; where base_revision is accepted), `sysd/src/bridge_server.cpp` (message_rules), `engine/host/tests/control_test.cpp`.
- `desktop/src/app.js` (current simulated editor and strings), `index.html` (editor-container), `style.css`, `tokens.css`, `live_set_view.js` / `live_set_dom.js` (selection event, State store), `catalogs.js`, `locale/*.json`, `desktop/src-tauri/src/device.rs` (request JSON mapping: new envelope field), tests under `desktop/tests`.
- Design: `mockups/key-screen-studio.html`, `mockups/studio-part-editor-fm-va.html`, DESIGN.md/EXPERIENCE.md (Part Editor, Studio Knob specs).
- New: `desktop/src/params_registry.js` (generated), `part_editor.js`, `part_editor_dom.js`, node tests, engine/bridge tests for base_revision acceptance, smoke (engine-host + bridge + Rust client sending a performance edit and seeing the confirmed State).

## Tasks & Acceptance

**Execution:**
- registry generation to JS + CI drift check + grouping metadata
- additive ClientMessage.base_revision + engine records/ignores + bridge/Rust mapping
- part_editor.js (sections, knob model, keyboard rules, envelope polyline, effects, zone/arp, ASSIGN, command mapping table, coalescing, optimistic + revert)
- part_editor_dom.js + CSS reflow + ARIA sliders + locale keys; integrate with app.js (real mode) while keeping simulated mode
- tests: node (registry-driven rendering, knob rules incl. every key, quantisation, coalescing with fake clock, revert on error, reflow class logic), DOM shim (ARIA attributes, keyboard, hostile strings), engine/bridge/cargo tests, end-to-end smoke; browser check with the mock bridge if feasible
- docs: desktop/README.md, proto/README.md

**Acceptance Criteria:**
- Given a selected part, then the Part Editor shows four hairline-divided sections (Sound; Amp envelope + EQ with envelope polyline; Effects with bypass, reorder, "+ Add insert", reverb/delay sends; Zone & keyboard + Arpeggiator + ASSIGN · KNOBS) with no menus or tabs, and the parameters shown come from the generated registry.
- Given a Studio Knob, when the user drags, scrolls, types a value or uses the keys, then the value changes and is exposed as an ARIA slider with name, unit and aria-valuetext, and each change is sent as a ws.v1 edit carrying base_revision and plays on the unit immediately.
- Given 150%/200% text size, then the editor reflows from 4 to 2 to 1 columns with no horizontal scrolling.

## Spec Change Log

- 9.9 engine follow-up: INERT_BY_ENGINE shrank from "everything but the organ" to part_attack / part_release (organ, FM) and va_unison_spread; FakeUnit.consumes mirrors the engines.
- 9.9 review: the editor hints inert knobs (INERT_BY_ENGINE), sends send both SetPerformanceParam and SetSendLevels, tab order is one stop per knob (deviation from the roving order the spec asked for), the VA amp envelope replaces the generic attack / release for VA parts.

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` part switch with a queued send applied another part's value (SetPerformanceParam part 2 value 0); a send knob stayed pending forever; read-back raced the RT apply and snapped knobs back; structural drags flooded the generation ledger; DOM signature rebuilds destroyed focus and broke drags; send semantics (initial vs live); VA oscillator groups produced identically named sliders; linear steps unusable on ms/Hz ranges; reflow depended on window width
  - `[high]` `defer` engines do not consume registry part parameters (see deferred) — Studio marks inert knobs
  - `[medium]` `patch` accessibility (live regions, modifiers, colour-only pending), unit formatting, missing labels, zone ranges, README overclaims

### 2026-10-27 — Engine follow-up (closes the deferred MAIN GAP), self-review
- findings:
  - `[high]` `patch` defaults vs base patch: the block loop forwards the registry defaults as initial values, and the FM engine's base patch (EP) is not the registry default, so wiring the parameters naively would have changed every existing FM sound. Fixed with "engagement" (engine/core/include/velora/core/part_params.hpp): an initial value engages only when it differs from the default, a live edit (new `Engine::set_part_param_edit`, used by `BlockRenderer::apply_param`) always engages, `reset_play_state()` at a Live Set switch restores the base patch. Tested bit-identical.
  - `[high]` `patch` a value that comes back through the 32-bit event scale is not exactly the default (0 dB arrives as ~3.5e-9 dB), which would have kept the Mixer routed and the tone stage running for a knob "at its default": 0.001 dB dead band in `PartTone::set`.
  - `[medium]` `patch` denormals: `EqEffect` kept double states that decay to 1e-308 and the float output could cross 1e-38 inside a block; states are flushed below 1e-30 (`eq.cpp`, also helps the master EQ), `PartTone` flushes state and output. Smoothers snap onto their target (VA live parameters, FM brightness, VA sustain glide), so no endless tail and the voice returns to the unsmoothed path.
  - `[medium]` `patch` VA sustain level stepped when edited in the sustain stage: now glides (~10 ms), bit-identical when unchanged.
  - races reviewed, none found: engines are written only from the render thread between segments (UMP queue / generation adoption), the Mixer reads the RT-owned last-value table; sample envelope state is voice-local (workers read only their voices), part_attack / part_release arrays are written between segments while workers idle at the barrier; 1 vs 3 partitions bit-identical under moving part_attack / part_release (test).
  - `[medium]` `defer` part_attack / part_release (organ, FM) and va_unison_spread stay inert, see deferred. `[low]` `defer` the part tone stage is not counted in the FX budget (fill_fx_costs).

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "control|sysd|bridge|params|smoke|desktop" --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` and `cd desktop/src-tauri && cargo test --offline` -- expected: all pass
- `python3 tools/ci/check_ui_strings.py`, `check_no_color_literals.py`, `check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: registry -> desktop/src/params_registry.js (generated, drift-tested, grouping and engine applicability derived from data), part_editor.js (four-section model, Studio Knob rules, command table, coalescing sender with optimistic/confirm/revert, inert-by-engine hints), part_editor_dom.js (ARIA sliders, keyboard, drag, wheel, value entry, focus-preserving in-place updates, rem-based reflow), ClientMessage.base_revision (recorded, ignored until 9.10), ~200 locale keys.
Verification: ctest control/sysd/bridge/params/smoke/desktop 28/28; node tests 169; cargo tests 23; lints pass; full ctest only the 4 pre-existing failures. Status stays in-progress: most FM/VA/sample parameters have no audible effect yet (engine gap).

### Addendum: engine follow-up (2026-10-27) -- the deferred MAIN GAP
Summary: the engines now play the registry part parameters. FmEngine: ids 1000..1103 edit the part's patch (next note; fm_brightness also live). VaEngine: ids 1200..1266 edit the patch; the continuous ones also move sounding notes (per-sample ~5 ms one pole, cutoff in the octave domain, ADSR re-timed in place, sustain glides, LFO rate). SampleEngine: part_attack / part_release replace the region's ampeg attack / release. Mixer: new per-part `PartTone` (brightness shelf + 3-band EQ, RT-safe biquads, identity = bit-exact and trivial routing) plays part_brightness / part_eq_* for EVERY engine incl. the organ. New `Engine::set_part_param_edit` and "engagement" keep every default sound unchanged. Studio: INERT_BY_ENGINE shrank to part_attack / part_release (organ, FM) and va_unison_spread; FakeUnit.consumes mirrors the engines; desktop/README.md, proto/README.md, engine/core/README.md updated.
Stays inert and why: part_attack / part_release on organ and FM (no amp envelope on the organ; the FM contour is the operator envelopes and the registry does not say how a second envelope combines); va_unison_spread (mono unison sum, a stereo spread means a stereo filter path).
Verification (local, no CI, no Docker): cmake build + `ctest -j4` (704 tests; only setlist_controller_tests failed once under parallel load and passes alone: pre-existing flake), new `part_params_tests`; `node --test desktop/tests` 276; `cargo test --offline` 26; `check_core_deps.py`, `check_ui_strings.py`, `check_no_color_literals.py`, `proto/tests/test_gen_params.py` pass. RTSan variant (`rtsan_part_params_test`) is Docker/Linux only and was not run.
Status stays `in-progress`, deliberately: (1) SetPartZone / SetPartArp are still deferred (zone, keyboard, octave, transpose and the arpeggiator are read-only in the editor, so "edit every parameter of the selected part" is not wholly met); (2) the reflow AC was computed, not measured in the real Tauri webview; (3) audibility was verified in the host engine tests only, not on the unit; (4) FM / VA discrete parameters sound on the next note, not on a sounding one.

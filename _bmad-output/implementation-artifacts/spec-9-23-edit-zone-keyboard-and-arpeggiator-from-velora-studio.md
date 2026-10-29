---
title: 'Story 9.23: Edit zone, keyboard and arpeggiator from Velora Studio'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: '3f283a3664c62ff76f21272a81f1ec461bda39c6'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/desktop/README.md', '{project-root}/proto/params.json', '{project-root}/proto/ws_v1.proto', '{project-root}/desktop/src/part_editor.js']
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** In Velora Studio's Part Editor (Story 9.9), part zone controls (low key, high key, keyboard on/off, octave, transpose) and arpeggiator controls (on/off, mode, rate, octaves, gate, swing, latch) are rendered read-only because the engine lacks `SetPartZone` and `SetPartArp` ws.v1 commands.

**Approach:** Add additive ws.v1 messages `SetPartZone` and `SetPartArp` to `ClientMessage`, extend `PartState` with octave, transpose and arpeggiator fields, implement their dispatch in `ControlPlane` and `EditSession`, and wire the Part Editor controls in `desktop/src/part_editor.js` to send these commands with `base_revision` and reflect state pushes.

## Boundaries & Constraints

**Always:**
- Schema changes in `proto/ws_v1.proto` are additive only; field numbers are never reused.
- `SetPartZone` and `SetPartArp` carry `base_revision` and are rejected with `ERROR_CODE_STALE_REVISION` if `base_revision` does not match `live_set_revision()`.
- Validations: `low_key` and `high_key` in 0..127 and `low_key <= high_key`; `octave` in -4..4; `transpose` in -12..12; `arp_mode` in 0..4; `arp_rate` in 0..7; `arp_octaves` in 1..4; `arp_gate` in 1..200%; `arp_swing` in 50..75%.
- Edits record against the current Live Set in `EditSession` with change keys `part.zone`, `part.octave`, `part.transpose`, `part.keyboard`, `part.arp` and mark dirty state (`UNSAVED`).
- Desktop UI controls in Studio update immediately, handle keyboard and pointer drag, and reflect device state pushes.

**Never:**
- No breaking changes to existing `ClientMessage` or `PartState` fields.
- No allocation or blocking on the real-time audio threads.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Edit Part Zone | `SetPartZone` with `low_key=36`, `high_key=84`, valid `base_revision` | `Snapshot` updated, `low_key=36`, `high_key=84` applied, `Ack` returned, State push reflects change | Refuse out of range with `ERROR_CODE_OUT_OF_RANGE` |
| Invalid Zone Range | `SetPartZone` with `low_key=90`, `high_key=60` | Nothing changed | `ERROR_CODE_OUT_OF_RANGE` |
| Stale Revision Zone | `SetPartZone` with obsolete `base_revision` | Nothing changed | `ERROR_CODE_STALE_REVISION` |
| Edit Part Arp | `SetPartArp` with `arp_on=true`, `arp_rate=2` (1/16) | `Snapshot::parts[p].arp` updated, `Ack` returned, State push reflects change | `ERROR_CODE_OUT_OF_RANGE` if param invalid |
| Studio Part Editor UI | User changes octave or rate knob in Studio | UI knob moves, edit throttled and sent to engine, `live_edit_state` dirty | Reverts on rejection |

</intent-contract>

## Code Map

- `proto/ws_v1.proto`:
  - `PartState`: add `octave`, `transpose`, `arp_on`, `arp_mode`, `arp_rate`, `arp_octaves`, `arp_gate`, `arp_swing`, `arp_latch`.
  - Add `message SetPartZone` and `message SetPartArp`.
  - `ClientMessage`: add `set_part_zone = 64` and `set_part_arp = 65`.
- `engine/host/src/control_plane.cpp`:
  - `fill_from`: populate new `PartState` fields from `Snapshot::parts[part]`.
  - `ControlPlane::dispatch`: handle `kSetPartZone` and `kSetPartArp`, validate inputs, update `work.parts[part]`, commit and trigger `on_structural`.
- `engine/host/src/edit_session.cpp`:
  - `on_structural`: handle `kSetPartZone` and `kSetPartArp` to persist into `d->working` LiveSet.
- `desktop/src/part_editor.js`:
  - `commandKind`: map `zone` and `arp` parameters.
  - Wire knob definitions and value resolution for zone and arpeggiator controls.
- `desktop/tests/part_editor.test.js`:
  - Add unit tests for zone and arp knob edits and command generation.
- `engine/host/tests/`:
  - Unit tests for `SetPartZone` and `SetPartArp` handling and stale revision refusal.

## Tasks & Acceptance

**Execution:**
- `proto/ws_v1.proto` -- add `SetPartZone`, `SetPartArp` and `PartState` extensions -- additive schema.
- `engine/host/src/control_plane.cpp` -- dispatch `SetPartZone` and `SetPartArp` and fill state -- engine control plane integration.
- `engine/host/src/edit_session.cpp` -- record zone and arp edits in `on_structural` -- LiveSet edit persistence.
- `desktop/src/part_editor.js` -- make zone and arpeggiator knobs editable and command-producing -- Studio Part Editor UI.
- `desktop/tests/part_editor.test.js` -- add tests for zone and arp editing -- test coverage.
- `engine/host/tests/control_plane_test.cpp` (or `edit_session_test.cpp`) -- test ws.v1 `SetPartZone` and `SetPartArp` -- engine test coverage.

**Acceptance Criteria:**
- Given the Part Editor's zone, keyboard and arpeggiator controls, when the user edits them, then Studio sends additive ws.v1 commands `SetPartZone` and `SetPartArp` carrying `base_revision`.
- Given an invalid `base_revision`, when `SetPartZone` or `SetPartArp` is sent, then the engine returns `ERROR_CODE_STALE_REVISION`.
- Given an accepted edit, when the published state is emitted, then `PartState` fields reflect it, and the Part Editor controls reflect the values.

## Spec Change Log

## Review Triage Log

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "control_plane|edit_session|live_set" --output-on-failure` -- expected: all pass
- `cd desktop && node --test tests` -- expected: all pass

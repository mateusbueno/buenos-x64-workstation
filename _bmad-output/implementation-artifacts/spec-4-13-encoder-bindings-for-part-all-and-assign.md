---
title: 'Story 4.13: Encoder bindings for PART, ALL and ASSIGN'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '3136469'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/engine/core/include/velora/core/assign_events.hpp'
  - '{project-root}/engine/host/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 4.13 "Encoder bindings for PART, ALL and ASSIGN" from `_bmad-output/planning-artifacts/epics.md` (starts at line 2856):
- In encoder mode PART and with a knob-target part selected: encoders 1–8 change that part's delay send, brightness, attack, release, EQ low, mid, mid freq, and high by relative steps.
- In encoder mode ALL: turning an encoder changes that fixed function across all 8 parts by the same relative amount (each clamped to range); the ring shows the relative offset from the moment ALL mode was entered (centered at 0.5f).
- In encoder mode ASSIGN: Story 4.10 mapping applies; the ring shows the mapped parameter's normalized value (0–100%).
- No pickup: across all modes, turning encoders after a scene or Live Set change applies immediately from the current stored value.
- State publication: engine publishes `ring_values` (0–100%) and `ring_led_fills` (0..255) in `EncoderStatus` for hardware LED rings.

**Covers:** FR107 (engine: fixed functions DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH; relative in ALL; no pickup), FR106 (partial: knob target selected by part-button long press), FR101 (partial), UX-DR52 (ring value), UX-DR85 (encoder ring ↔ target value).

</intent-contract>

## Code Map

- `proto/params.json` -- registered fixed performance parameters: `send_delay_level` (401), `part_brightness` (402), `part_attack` (403), `part_release` (404), `part_eq_low` (405), `part_eq_mid` (406), `part_eq_mid_freq` (407), `part_eq_high` (408).
- `proto/ws_v1.proto` -- added `ring_values` (field 14) and `ring_led_fills` (field 15) to `EncoderStatus`.
- `engine/core/include/velora/core/assign_events.hpp` -- defined `kPartModeParams`, `EncoderState` relative offset tracking and resetting, and `compute_ring_value` helper template.
- `engine/core/src/block_renderer.cpp` -- implemented PART and ALL routing in `apply_encoder_step`, offset resetting on entering ALL mode and generation changes.
- `engine/host/src/control_plane.cpp` -- populated `ring_values` and `ring_led_fills` in `fill_encoders`.
- `engine/core/tests/assign_test.cpp` -- unit tests for PART mode fixed functions, ALL mode relative offsets and clamping, and no pickup across modes.
- `engine/host/tests/assign_map_test.cpp` -- end-to-end integration tests for PART, ALL, and ASSIGN modes with published ring status.
- `tools/offline-render/CMakeLists.txt` & `tools/offline-render/README.md` -- updated offline render tests for PART mode encoder routing.

## Tasks & Acceptance

- [x] Register required performance parameters in `proto/params.json`.
- [x] Update `proto/ws_v1.proto` with `ring_values` and `ring_led_fills` in `EncoderStatus`.
- [x] Implement PART mode fixed functions routing in RT block renderer.
- [x] Implement ALL mode multi-part routing with relative offset tracking centered at 0.5f.
- [x] Ensure immediate actuation without pickup across all modes after scene/Live Set changes.
- [x] Publish `ring_values` (0.0..1.0) and `ring_led_fills` (0..255) in `ws::v1::State`.
- [x] Comprehensive unit and integration tests passing bit-identically with 100% test pass rate.

## Verification

- `python3 proto/tests/test_gen_params.py` -> 29/29 tests pass.
- `cmake --build build` -> Clean build.
- `ctest --test-dir build -R "assign|encoder_rings"` -> 37/37 tests pass.

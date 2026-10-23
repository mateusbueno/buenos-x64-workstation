---
title: 'Story 4.12: Fader bindings with pickup'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '5c25264'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/engine/host/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 4.12 "Fader bindings with pickup" from `_bmad-output/planning-artifacts/epics.md` (starts at line 2827):
- Faders 1–8 and 9th fader in MIXER drive part 1–8 volumes and the REV master send level.
- In DRAWBARS mode with an organ part selected (the only organ, or chosen by part-button long press / SelectOrganPart), faders 1–9 drive the 9 drawbars (16' to 1') of that organ part.
- Pickup rule: on scene, Live Set or fader-mode change, or change of the selected organ, if a fader's physical position differs from the stored value of its new target, the fader changes nothing until its position crosses the stored value; once crossed, it takes over and follows.
- State publication: engine publishes per fader: target, stored value, physical position and pending/taken-over, updated on every change.
- DRAWBARS refusal: if DRAWBARS is requested when no organ part exists in the Live Set, the engine refuses the mode change with a "no organ" result (`ERROR_CODE_NO_ORGAN`) and the faders stay in MIXER.

**Covers:** FR105 (engine: pickup rule and state), FR104 (engine: fader binding per mode, organ selection), FR52 (REV fader drives the reverb send), AR16 (partial: non-motorized faders, pickup after recall).

</intent-contract>

## Code Map

- `proto/ws_v1.proto` -- error codes `ERROR_CODE_NO_ORGAN`, `ERROR_CODE_NOT_ORGAN_PART`; `FaderMode`, `FaderTargetType`, `FaderBindingState`, `FaderStatus`; client messages `SetFaderMode`, `SelectOrganPart`, `MoveFader`.
- `engine/host/include/velora/host/control_plane.hpp` -- `FaderHandler` seam interface and registration.
- `engine/host/src/control_plane.cpp` -- routing of fader messages, state population, and publication notification on snapshot commits.
- `engine/host/include/velora/host/fader_bindings.hpp` -- `FaderController` declaration, pickup crossing tracking, organ selection and MCU bindings.
- `engine/host/src/fader_bindings.cpp` -- implementation of fader mode switching, organ resolution, crossing takeover logic, and target parameter actuation.
- `engine/host/tests/fader_bindings_test.cpp` -- unit tests covering all acceptance criteria.

## Tasks & Acceptance

- [x] Implement `FaderController` and `FaderHandler` seam.
- [x] Support MIXER mode: Faders 1-8 part volumes, Fader 9 REV master send (`kSendReverbMaster`).
- [x] Support DRAWBARS mode: Faders 1-9 drawbars 16' through 1' of selected organ part.
- [x] Refuse DRAWBARS mode when no organ part exists in the Live Set (`ERROR_CODE_NO_ORGAN`), staying in MIXER.
- [x] Multi-organ selection via `SelectOrganPart` and MCU part-button long press.
- [x] Implement crossing pickup rule from below and from above; prevent parameter changes while pending.
- [x] Publish `FaderStatus` on every change in `ws::v1::State`.
- [x] Comprehensive test suite in `engine/host/tests/fader_bindings_test.cpp`.

## Verification

- `cmake --build build --target fader_bindings_test`
- `./build/engine/host/fader_bindings_test` -> All tests pass.
- `ctest --test-dir build -R "fader_bindings_test|control_tests|scene_controller_tests|pickup_ghost_test|drawbars_view_test"` -> 100% pass.

---
title: 'Story 7.4: Arpeggiator settings in Live Sets and scenes'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: 'f5aefa8d36fcfabb577a52f8bd8d80168cfd9e3d'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/proto/params.json'
  - '{project-root}/proto/live_set.proto'
  - '{project-root}/engine/host/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** In `_bmad-output/planning-artifacts/epics.md` (`### Story 7.4`, line 4689), the arpeggiator parameters (on/off, mode, rate, octaves, gate, swing, latch per part) are not yet registered in `proto/params.json`, `make_default_live_set()` does not yet initialize parts with the default arpeggiator settings, `LiveSetLoader` does not yet populate `Snapshot::parts[p].arp` from `velora.liveset.v1.Part::arp`, `SceneController::apply_scene()` does not yet merge `ScenePart::arp` overrides into `work.parts[p].arp`, and unpopulated older Live Sets do not explicitly default to the specified initial values.

**Approach:**
1. In `proto/params.json`, register the 7 arpeggiator parameters per part as structural, part-scope (`arp_on`, `arp_mode`, `arp_rate`, `arp_octaves`, `arp_gate`, `arp_swing`, `arp_latch` with IDs 900..906) along with the `arp_mode` and `arp_rate` enums. Confirm `tempo_bpm` remains performance, master-scope, and Live-Set-scoped (never saved in a scene).
2. Run generator (`proto/gen_params.py`) and verify that C++ (`params_gen.hpp`) and Protobuf (`ws_v1_params.proto`) definitions build cleanly.
3. In `engine/host/src/live_set_store.cpp`, update `make_default_live_set()` so that each part initializes its `ArpSettings` with initial values: Off, Up, 1/8, 1 octave, Gate 50%, Swing 50%, Latch Off.
4. In `engine/host/src/live_set_detail.hpp`, extend `fill_part_values()` to populate `core::Snapshot::parts[p].arp` from `lsp::Part::arp()`, using initial defaults when unset or missing (guaranteeing backwards compatibility with v1 files saved before Story 7.4 without format bump or migration).
5. In `engine/host/src/scene_controller.cpp`, extend `apply_scene()` so that `ScenePart::arp` overrides are merged field-wise on top of the base Live Set's `b.arp`, ensuring scene recalls perform parameter-only snapshot swaps that start, stop, or reshape the arpeggiator without changing the tempo.
6. Verify with unit and integration tests across `live_set_test.cpp`, `scene_controller_test.cpp`, `live_set_loader_test.cpp`, and test opening older files without format bumps.

## Boundaries & Constraints

**Always:**
- Arpeggiator parameters in `proto/params.json` must be classified as `structural` and `part` scope.
- Live Set tempo must remain Live-Set-scoped (`tempo` on `LiveSet`, never in `Scene` or `ScenePart`).
- New Live Sets created via `make_default_live_set()` must hold initial arpeggiator values: Off (`enabled: false`), Up (`pattern: ARP_PATTERN_UP`), 1/8 (`rate: ARP_RATE_EIGHTH`), 1 octave (`octave_range: 1`), Gate 50% (`gate: 50.0f`), Swing 50% (`swing: 50.0f`), Latch Off (`hold: false`).
- Older Live Sets without `ArpSettings` or with unset fields must load with the initial defaults without format version bump, migration, or loss of unknown fields.
- Scene recalls must swap snapshots without changing the Live Set tempo and without re-allocating or interrupting unrelated parts.

**Never:**
- Do not bump `FormatVersion` (remains `FORMAT_VERSION_1`).
- Do not allocate or lock on the RT thread.
- Do not store tempo in `Scene` or `ScenePart`.

</intent-contract>

## Code Map

- `proto/params.json` -- register `arp_on`, `arp_mode`, `arp_rate`, `arp_octaves`, `arp_gate`, `arp_swing`, `arp_latch` and `arp_mode`, `arp_rate` enums
- `proto/gen_params.py` -- generates `params_gen.hpp` and `ws_v1_params.proto`
- `engine/host/src/live_set_store.cpp` -- initialize `ArpSettings` in `make_default_live_set()`
- `engine/host/src/live_set_detail.hpp` -- parse `lsp::Part::arp` to `core::ArpConfig` in `fill_part_values()`
- `engine/host/src/scene_controller.cpp` -- merge `ScenePart::arp` in `apply_scene()`
- `engine/host/tests/scene_controller_test.cpp`, `engine/host/tests/live_set_test.cpp`, `engine/host/tests/live_set_loader_test.cpp` -- unit tests verifying scene arpeggiator switches, initial defaults, and older Live Set backwards compatibility

## Tasks & Acceptance

**Execution:**
- `proto/params.json` -- add arpeggiator parameters (900..906) and enums -- registry classification
- `engine/host/src/live_set_store.cpp` -- set initial arp values in `make_default_live_set()` -- initial values on creation
- `engine/host/src/live_set_detail.hpp` and `engine/host/src/scene_controller.cpp` -- wire `ArpSettings` into `Snapshot` and `apply_scene` -- scene recall parameter-only snapshot swap
- `engine/host/tests/{scene_controller_test,live_set_test,live_set_loader_test}.cpp` -- verify all four ACs: registry classification, initial defaults, scene 1 <-> scene 2 recall with tempo unchanged, older file backward compatibility without format bump

**Acceptance Criteria:**
- Given the parameter registry in `proto/`: when arpeggiator parameters are added (on/off, mode, rate, octaves, gate, swing, latch per part), then they are classified as structural, scene-scoped, and Live Set tempo is Live-Set-scoped.
- Given a new Live Set: when created, each part holds initial arpeggiator values (Off, Up, 1/8, 1 octave, Gate 50%, Swing 50%, Latch Off) and every scene copies them.
- Given scene 1 with Arp Off on part 2 and scene 2 with Arp On, Up-Down, 1/16T on part 2: when recalling scene 2 then scene 1, the arpeggiator starts and stops as a parameter-only snapshot swap, and tempo does not change.
- Given a Live Set saved before this story whose arpeggiator fields are unset: when opened, it loads with initial values above, no format version bump or migration needed, unknown fields preserved, and Write produces a file N and N-1 readers open.

## Verification

**Commands:**
- `python3 proto/tests/test_gen_params.py` -- expected: all pass
- `cmake -S . -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: pass

## Auto Run Result

- **Implementation**: Added 7 arpeggiator parameters (900..906) and enums to `proto/params.json` and generator `proto/gen_params.py`. Initialized default arpeggiator settings in `make_default_live_set()` (`engine/host/src/live_set_store.cpp`), populated snapshot in `live_set_detail.hpp`, implemented scene overrides merging in `scene_controller.cpp`. Maintained test fixture byte-compatibility in `live_set_fixtures.hpp`.
- **Verification**:
  - `python3 proto/tests/test_gen_params.py` passed 29/29 tests.
  - `ctest --test-dir build -R "(live_set|scene_controller|arp_|arpeggiator|transport_clock)"` passed 61/61 tests (100%).
  - `python3 tools/ci/check_core_deps.py` passed with pure core dependencies.
- **Review**: Findings addressed; backwards compatibility with pre-Story 7.4 files confirmed without schema format bump.

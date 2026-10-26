---
title: 'Story 3.6: Attack and release velocity with selectable curves (Mac math, tests and sysd wiring)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '2fa243aa704b52422891b826eb2f30e889e679d4'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/sysd/README.md', '{project-root}/engine/host/include/velora/host/keybed_calibration.hpp', '{project-root}/engine/host/include/velora/host/keybed_reader.hpp']
warnings: []
deferred: ['Real keybed calibration measurement (attack and release travel time ranges, vel_fast_ns / vel_slow_ns / rel_fast_ns / rel_slow_ns): placeholders marked TODO(measure on prototype)', 'Keybed link wiring in engine-host: attach the KeybedReader to the VelocityCurveHolder when the link exists', 'Fixed curve velocity is a documented constant (100); sysd has no field for it']
---

<intent-contract>

## Intent

**Problem:** The keybed reader (Story 3.5) emits Note On with a provisional linear time→velocity mapper and a fixed release velocity. The story needs the real chain: attack velocity measured from contact times, mapped through the keybed calibration table and then through the player's selected velocity curve exactly once into a MIDI 2.0 16-bit velocity; release velocity measured from the release contact times and carried on the Note Off so a sample instrument with release triggers plays a louder release sample for a fast release than a slow one; the curve selected through the sysd config (CLI for now) without restart (FR70, FR71, AR39 partial, NFR44).

**Approach:** In `engine/host`: (1) a `VelocityCurve` family (Light, Normal, Heavy, Fixed; the sysd enum VelocityCurve has NORMAL/LIGHT/HEAVY/FIXED) implemented as monotonic mappings over a normalised keybed-calibrated travel time, concrete `VelocityMapper` implementations that compose "keybed calibration table → selected curve" (calibration table = per-keybed-model time range and shape stored in `KeybedCalibration`; extend the spec/table data model additively, with built-ins for the 2- and 3-contact 73-key models), applied once in the reader; the engine and sample engine apply no curve (add a test that injects a known contact time and checks the voice's velocity end to end through BlockRenderer with the sample/organ engine). (2) Release velocity: the reader measures the time between the release-velocity contact pair opening (table: which pair; for 2-contact keybeds define the rule and document) and maps it (own calibration range, same curve policy or a documented separate release mapping) to a 16-bit release velocity on the Note Off UMP; the sample engine uses release velocity to select/scale release-trigger samples (check what exists: `trigger=release` regions fire on note off; add velocity-layer selection/gain by release velocity if missing, minimal and additive) so fast releases are louder (render test). (3) Config: the engine's sysd callback applies `velocity_curve` to the reader (thread-safe setter, effective for the next note, no restart); document the CLI (`velora-sysdctl set velocity_curve light`) and that survival across reboot comes from sysd persistence (test with a real sysd store).

## Boundaries & Constraints

**Always:** Curve applied exactly once, in the reader's `VelocityMapper` chain; MCU, engine core and sample engine never apply a curve. Each curve, evaluated over the keybed's full measured time range, reaches MIDI velocity 1 and 127 (and the 16-bit equivalents) and its 127 steps are spread without gaps or plateaus (unit test: for each curve, over a dense sweep of times, the set of 7-bit velocities produced is exactly 1..127, each bin non-empty, monotonic non-increasing in time, and 16-bit values strictly monotonic where the curve is not Fixed; Fixed returns a constant velocity — document the value and that it is excluded from the 1..127 spread requirement but reaches the configured fixed velocity). Time is injected (no clocks); the reader's clock/drift code from Story 3.5 is untouched. The setter that changes the curve must be safe against the reader running on its own thread (atomic enum or mutex-free swap). The default (no config) is Normal. Existing keybed/panel/organ tests must pass; the provisional LinearVelocityMapper may stay as a test helper or be replaced — keep the 3.5 tests green (update expectations only where the specified chain legitimately changes velocities, and say which).

**Never:** No Settings UI row (Story 10.2), no pedal work (3.11), no aftertouch, no hardware measurement of real keybed calibration (table values are placeholders marked TODO(measure on prototype) except where the math needs them), no change to the MCU protocol, no change to sysd's proto (velocity_curve already exists).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Normal curve sweep | travel times across the calibrated range | velocities cover 1..127 with no gap or plateau | none |
| Light / Heavy | same sweep | same coverage, Light yields higher velocity than Normal at a given time, Heavy lower (monotone ordering) | none |
| Fixed | any time | constant configured velocity | none |
| Out of range time | faster than fastest / slower than slowest | clamped to 127 / 1 (16-bit equivalents) | none |
| Known contact time end to end | inject contact_k + Note On via the reader into BlockRenderer | the voice's velocity equals the curve(calibration(time)) once, not twice | none |
| Curve change | sysd velocity_curve changes while playing | next note uses the new curve, notes already sounding unchanged | none |
| Reboot | restart sysd and engine | selected curve restored | none |
| Release velocity | fast vs slow release | Note Off carries higher vs lower release velocity | none |
| Release sample | sample instrument with release-trigger regions | fast release plays a louder release sample than slow (render test) | none |
| Missing release pair data | 2-contact keybed or lost contact | documented default release velocity, counted | none |

## Code Map

- `engine/host/include/velora/host/keybed_calibration.hpp`, `src/keybed_calibration.cpp` (VelocityMapper, LinearVelocityMapper, KeybedCalibrationSpec, two_contact_73/three_contact_73) -- extend with calibration range data and the chain.
- `engine/host/include/velora/host/keybed_reader.hpp`, `src/keybed_reader.cpp` -- Note On velocity computed at the velocity-completing contact, Note Off release velocity at the release contact; set_velocity_mapper.
- `engine/host/macos/main.cpp`, `include/velora/host/sysd_config_client.hpp` -- sysd callback (clock_source applied there as in Story 7.7): apply velocity_curve to the reader; the keybed reader is not yet instantiated in engine-host (no keybed link): create the plumbing (a registry/holder object the callback updates) so the future link wiring picks it up, and test the callback path with a reader instance.
- `engine/core/include/velora/core/sample_engine.hpp`, `src/sample_engine.cpp`, `engine/core/include/velora/core/ump.hpp` -- Note Off release velocity handling and release-trigger region selection; `formatlib/include/velora/format/instrument_model.hpp` (trigger=release).
- `proto/ws_v1.proto` (VelocityCurve), `sysd/README.md`, `tools/sysdctl` -- config CLI.
- Tests: `engine/host/tests/keybed_reader_test.cpp`, `engine/core/tests/sample_engine_test.cpp` and offline-render tests for release-trigger renders (`tools/offline-render/tests`), `engine/host/tests/clock_source_test.cpp` (pattern for sysd callback tests), `sysd/tests/*`.

## Tasks & Acceptance

**Execution:**
- velocity curves + calibration chain + tests (coverage/monotonic/clamp/ordering)
- reader: attack velocity via chain, release velocity on Note Off, thread-safe curve setter
- sample engine: release-velocity-aware release-trigger selection/gain (minimal), render test
- sysd callback wiring + holder; test persistence with a real sysd in a temp dir
- end-to-end test: contact time → voice velocity exactly once
- docs: engine/host/README.md, sysd/README.md (CLI usage)

**Acceptance Criteria:**
- Given the contact times, then the reader maps them through the calibration table and the selected curve once into a 16-bit velocity and no other stage applies a curve (end-to-end velocity test).
- Given the shipped curves, then each reaches velocity 1 and 127 over the full range with 127 steps spread without gaps or plateaus (unit test), Fixed is constant.
- Given the curve stored in sysd, then changing it via the CLI takes effect for the next note without restart and survives reboot.
- Given a key release, then the Note Off carries a release velocity and a fast release plays a louder release sample than a slow one (render test).
- Real keybed calibration measurement is recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` external MIDI note offs changed release layer/gain (64 -> ~-8 dB) once the host flag is on — release velocity zeroed for external note offs; bounce of a lower contact ended the release measurement early; fast re-strike not covered — fixed + tests
  - `[medium]` `patch` release_pair default made existing custom specs invalid; coverage test swept clamped regions and reused production code; release-stamp raises polluted clock_clamps; README overstatements
  - `[low]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "keybed|velocity|sample|release|sysd|clock|control|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: velocity_curve (calibration range on a log axis -> Normal/Light/Heavy/Fixed -> 16-bit), CalibratedVelocityMapper with atomic curve setter, release velocity from the release contact pair carried on Note Off, Engine::release_voice and SampleEngine release_velocity_triggers (off by default), sysd callback applying velocity_curve, tests incl. end-to-end single-application and persistence via sysd.
Verification: targeted tests pass; full ctest passes except the 4 pre-existing failures; core deps and UI strings lint pass.

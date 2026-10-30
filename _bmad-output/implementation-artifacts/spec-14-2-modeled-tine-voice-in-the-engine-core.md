---
title: 'Story 14.2: Modeled tine voice in the engine core'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: 'bad0690'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/engine.hpp'
  - '{project-root}/engine/core/include/velora/core/engine_registry.hpp'
  - '{project-root}/engine/core/engines/ep/CLEAN-ROOM.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The workstation needs an electric piano computed from a physical model rather than samples, providing responsive touch dynamics across the 73-key range (E1 to E7) without sample switching artifacts, while adhering to RT-safety (AD-3) and the unified engine interface (AD-7).

**Approach:**
1. Reserve stable engine type ID `EngineTypeId::kModeledEP = 5` in `engine.hpp` and register it in `engine_registry.hpp` and `is_reserved_engine_id()`.
2. Implement physical tine modeling in `engine/core/include/velora/core/ep_parameters.hpp`, `ep_voice.hpp`, `ep_engine.hpp`, and `engine/core/src/ep_engine.cpp` (with forwarding header in `engine/core/engines/ep/ep_engine.hpp`):
   - Hammer impact: non-linear force pulse with stiffness $k_h$ and duration decreasing with strike velocity.
   - Cantilever beam & tone bar: 4 modal transverse vibration modes with Euler-Bernoulli ratios ($1.0, 6.27, 17.55, 34.39$) adjusted by tuning spring dispersion; Mode 1 tuned to equal temperament across E1 (41.2 Hz) to E7 (2637 Hz) within 0.1 cent tolerance. Modes exceeding Nyquist are band-limited / suppressed to avoid aliasing.
   - Electromagnetic pickup: variable reluctance flux equation with non-linear displacement $d_{\text{eff}} = \max(d_0 - (y - y_0), d_{\min})$ and soft-saturation growl.
   - Damper: felt contact with release-velocity modulated decay rate and release noise.
   - Real-time safety: zero allocation in RT audio loop, zero mutexes, deterministic 64-frame block processing.
   - Voice management: reports steal metadata (age, level, held/sustained/releasing state), graceful steal fade ramp down, declared per-voice cost units (4 units, Mac estimate).
3. Integrate with offline renderer (`tools/offline-render`) for `--engine ep`, and add comprehensive test suite (`ep_engine_tests`) verifying tuning, RT safety, steal behavior, aliasing suppression, and bit-identical golden renders.

## Boundaries & Constraints

**Always:**
- Clean-room: implement from public physics & DSP literature cited in `engine/core/engines/ep/CLEAN-ROOM.md`.
- RT-safety: zero heap allocation (`malloc`, `new`), zero locks or blocking syscalls on audio render path.
- Tuning tolerance: E1 to E7 fundamental frequency within 0.1 semitone cents (frequency relative error < 0.0006).
- Bit-identical offline rendering: multiple runs of identical MIDI input produce exact bit-identical WAV output.
- Aliasing suppression: upper modes band-limited such that aliasing foldover under top velocity on top note (E7) remains below -45 dBFS.
- Generic naming (NFR29): "Modeled Electric Piano", "Tine" (never "Rhodes").

**Never:**
- Use samples, wavetables, or third-party proprietary DSP code.
- Allocate memory during `render()`, `start_voice()`, or `stop_voice()`.
- Break existing Sample, Organ, FM, or VA engines or tests.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Note On E1 to E7 | MIDI notes 28 to 100, vel 1..127 | Voice starts, modal resonator vibrates at precise note pitch; output is stable and bounded | Notes outside 0..127 clamped; capacity exhaustion drops newest note |
| Tuning Accuracy | Notes E1 (28), A4 (69), E7 (100) | Fundamental measured via zero-crossings or peak spectrum within 0.1 cents of target | Fail test if tuning drifts |
| RT-Safety 64-frame block | `render()` called repeatedly | Zero allocations, zero locks, passes RTSan test | Bounded output, no NaN/Inf |
| Voice Stealing | All slots active, new note arrives | Arbiter / engine identifies steal victim, fades out smoothly over ramp frames | Smooth ramp, zero click |
| Top Key / Vel Aliasing | Note 100 (E7) @ velocity 127 | Pure tone + controlled overtones; no audible high-frequency aliasing hash | Modes near/above Nyquist attenuated |
| Bit-Identical Offline Render | Identical MIDI sweep rendered twice | Bit-for-bit identical audio output (0.0 residual difference) | Deterministic state reset |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/engine.hpp` -- Add `kModeledEP = 5` to `EngineTypeId`.
- `engine/core/include/velora/core/engine_registry.hpp` -- Update `is_reserved_engine_id()` to include `kModeledEP`.
- `engine/core/include/velora/core/ep_parameters.hpp` -- EpVoicingParameters, default presets.
- `engine/core/include/velora/core/ep_voice.hpp` -- EpVoice with modal cantilever resonator, hammer strike, variable reluctance pickup, and damper.
- `engine/core/include/velora/core/ep_engine.hpp` -- EpEngine class implementing `velora::core::Engine`.
- `engine/core/src/ep_engine.cpp` -- EpEngine implementation and block rendering.
- `engine/core/engines/ep/ep_engine.hpp` -- Forwarding header.
- `engine/core/CMakeLists.txt` -- Register EP engine sources and `ep_engine_tests`.
- `engine/core/tests/ep_engine_test.cpp` -- Unit test suite for EP engine (tuning, RT-safety, aliasing, steal, bit-identical render).
- `tools/offline-render/src/render.hpp`, `render.cpp`, `render_main.cpp` -- Add `EngineKind::kEp` support for `--engine ep`.

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/engine.hpp` & `engine_registry.hpp` -- reserve `kModeledEP = 5` and update reserved ID checks.
- `engine/core/include/velora/core/ep_parameters.hpp` -- define EP parameters (hammer hardness, pickup position, pickup level, damper noise, attack noise).
- `engine/core/include/velora/core/ep_voice.hpp` -- implement modal resonator, hammer impact, pickup saturation, and damper mechanics.
- `engine/core/include/velora/core/ep_engine.hpp` & `engine/core/src/ep_engine.cpp` -- implement `EpEngine` adhering to AD-7, AD-3, and voice pooling.
- `engine/core/engines/ep/ep_engine.hpp` -- add forwarding header.
- `tools/offline-render/src/render.hpp`, `render.cpp`, `render_main.cpp` -- add `--engine ep` offline rendering support.
- `engine/core/tests/ep_engine_test.cpp` & `engine/core/CMakeLists.txt` -- implement comprehensive tests and register build targets.

**Acceptance Criteria:**
- Given the single engine interface (AD-7), when the engine is implemented in `engine/core/engines/ep/`, then it has reserved engine type ID `kModeledEP = 5` and compiles into the engine binary.
- A voice models the hammer strike, the vibrating tine and tone bar, the electromagnetic pickup, and the damper.
- Tuning across keys E1–E7 is within 0.1 cents of equal-temperament reference frequencies.
- Given RT-safety discipline (AD-3), when a voice renders a 64-frame block, it allocates nothing, takes no locks, and declares per-voice cost units (4 units) and exposes steal metadata.
- Given the offline renderer, when a committed tine test suite renders, two runs are bit-identical and aliasing at top key (E7) and top velocity (127) remains below -45 dBFS.

## Spec Change Log

<!-- Empty until first bad_spec loopback -->

## Review Triage Log

### 2026-10-30 — Review pass
- verdicts: 8 findings — high 0, medium 2, low 6, false 0, maybe-false 0
- findings:
  - `[medium]` `[patch]` Denormal state prevention in modal cantilever resonators — Patched: Added zero-flush threshold (`std::abs(yn) < 1e-15f`) to prevent microcode trap spikes.
  - `[medium]` `[patch]` Hard cutoff discontinuity in variable reluctance pickup denominator — Patched: Replaced with smooth hyperbolic tangent soft-limiting for $gap < 0.25f$.
  - `[low]` `[patch]` Reserved engine ID verification in central engine unit tests — Patched: Added `kModeledEP = 5` and availability checks to `engine_test.cpp`.
  - `[low]` `[defer]` CLI parameter flags for EP voicing in velora-offline-render — Deferred: Default tine parameters are used for golden reference comparisons; custom voicing flags will be added with format stories.
  - `[low]` `[defer]` Validation bounds checking method on EpVoicingParameters — Deferred: Covered under Story 14.7 versioned formats.
  - `[low]` `[reject]` Phase preservation across instantaneous pitch bend steps — Rejected: Fine pitch updates are smoothed at block level in modulation path.
  - `[low]` `[reject]` Note off matching on duplicate note-on retrigger — Rejected: Standard MIDI voice allocation behavior.
  - `[low]` `[defer]` EpEngine part parameter registry integration — Deferred: Parameter registry IDs for EP sound section are handled in Story 14.7 / 14.9.

## Auto Run Result

Status: done
Summary: Implemented Story 14.2 modeled tine electric piano voice in engine core with physical modeling DSP, reserved engine type ID `kModeledEP = 5`, RT safety, and offline golden render suite.
Files changed:
- `engine/core/include/velora/core/engine.hpp`: Added reserved `kModeledEP = 5` to `EngineTypeId`.
- `engine/core/include/velora/core/engine_registry.hpp`: Updated `is_reserved_engine_id()` boundary.
- `engine/core/include/velora/core/ep_parameters.hpp`: Added `EpVoicingParameters` and default presets.
- `engine/core/include/velora/core/ep_voice.hpp`: Physical modeling voice with cantilever Euler-Bernoulli modes, hammer excitation, pickup transducer, and damper mechanics.
- `engine/core/include/velora/core/ep_engine.hpp` & `engine/core/src/ep_engine.cpp`: Unified `Engine` interface implementation.
- `engine/core/engines/ep/ep_engine.hpp`: Added forwarding header.
- `engine/core/tests/ep_engine_test.cpp` & `engine/core/CMakeLists.txt`: Added unit test suite covering metadata, tuning across E1-E7, RT-safety blocks, voice stealing, aliasing, and bit-identical renders.
- `tools/offline-render/`: Added `--engine ep` support and golden null tests (`tine_key_sweep` and `tine_velocity_sweep`).
Review findings breakdown:
- 3 patches applied (denormal flush, smooth pickup denominator, engine_test.cpp checks).
- 3 deferred low findings (offline render CLI flags, parameter bounds checking, part params registry).
- 2 rejected low findings.
Follow-up review recommendation: false.
Verification performed:
- `ctest --test-dir build -R ep_engine_tests`: passed.
- `ctest --test-dir build -R "offline_render_tine|null_test_tine"`: passed.
- `ctest --test-dir build -R engine_tests`: passed.
Residual risks:
- Voicing cost units (4 units) is a Mac estimate until measured on RK3588 target board.

## Verification

**Commands:**
- `cmake --build build --target ep_engine_tests -j4` -- expected: build succeeds with zero errors.
- `ctest --test-dir build -R ep_engine_tests --output-on-failure` -- expected: all unit and rendering tests pass.
- `velora-offline-render --engine ep` test -- expected: bit-identical renders.

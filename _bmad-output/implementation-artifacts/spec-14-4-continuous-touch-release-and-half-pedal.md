---
title: 'Story 14.4: Continuous touch, release and half-pedal'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '6f07cbf'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/engine.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_engine.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_voice.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_parameters.hpp'
  - '{project-root}/engine/core/include/velora/core/sample_engine.hpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists require an electric piano response that follows dynamic touch continuously without velocity switching steps, responds to release velocity with variable damper feel and noise, and implements continuous half-pedal damping following the workstation's standard half-pedal law (Story 3.11).

**Approach:**
1. Continuous Touch Dynamics:
   - Verify monotonic increase in audio RMS / peak level and spectral centroid from MIDI velocity 1 to 127 across test keys across both Tine and Reed timbres.
   - Enforce smooth continuity: no discrete jump between consecutive velocities exceeding a threshold of 0.75 dB in level and 5% in spectral centroid.
2. Release Velocity Dynamics:
   - Connect note-off release velocity (1..127) to damper felt contact mechanics in `EpVoice`.
   - Modulate damper damping time ($t_{60,\text{rel}}$) and release scrape noise amplitude ($g_{\text{damper}}$) proportionally to release velocity, demonstrating measurable differences between low (vel 1) and high (vel 127) releases.
3. Continuous Half-Pedal Law (Story 3.11):
   - In `EpEngine`, implement `set_part_cc(part, 64, value32)` and route to active voices.
   - Follow standard half-pedal law: `scale(cc64) = 1.0 - (1.0 - 0.1) * (cc64 / 63.5)` for $0 < cc64 < 63.5$.
   - For sustained and releasing notes, modulate damping continuously with pedal position without steps or clicks.
   - Guarantee zero memory allocations and zero blocking calls on the audio render path.

## Boundaries & Constraints

**Always:**
- Strict monotonicity in level and brightness over velocity 1..127.
- Zero allocations, zero mutexes on audio render path (RT-safety AD-3).
- Preserve bit-identical reproducibility for existing tine and reed golden renders.
- Implement exact half-pedal law matching `SampleEngine::half_pedal_scale`.

**Never:**
- Use velocity layer thresholds, multisample switching, or discrete gain tables.
- Block the audio thread when receiving CC 64 or note-off events.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Velocity Sweep 1..127 | Note-on velocities 1 to 127 | Monotonically increasing RMS level and spectral centroid; max step < 0.75 dB | Clamped to 1..127 |
| Low vs High Release | Release with vel 1 vs vel 127 | Vel 1 produces quieter damper noise and longer ring; vel 127 produces louder scrape and fast stop | Values outside 1..127 clamped |
| Half-Pedal Range 0..63.5 | CC 64 values 0, 16, 32, 48, 63 | Continuous damping rate scaling; decay slows smoothly as pedal approaches 63.5 | Values > 63.5 hold sustained voices |
| Pedal Movement during Ring | Pedal CC 64 moves while notes are sounding | Damper contact adjusts dynamically in real time without audio clicks | Clamped and smooth |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/ep_voice.hpp` -- Add half-pedal scale damping and release velocity modulation.
- `engine/core/include/velora/core/ep_engine.hpp` -- Declare `set_part_cc` override and pedal state tracking.
- `engine/core/src/ep_engine.cpp` -- Implement `set_part_cc` for CC 64 and voice routing.
- `engine/core/tests/ep_engine_test.cpp` -- Add tests for velocity monotonicity, release velocity differentiation, and continuous half-pedal law.

## Tasks & Acceptance

**Execution:**
- Update `ep_voice.hpp` to incorporate `half_pedal_scale` and release velocity modulation.
- Update `ep_engine.hpp` and `ep_engine.cpp` to handle `set_part_cc(part, 64, value32)`.
- Add test cases in `ep_engine_test.cpp` verifying touch monotonicity, release velocity effect, and half-pedal damping curve.
- Run build and test suite to ensure zero regressions across all engines.

**Acceptance Criteria:**
- Given a tine or reed timbre, when a velocity sweep renders from 1 to 127 on sampled keys, level and spectral brightness change monotonically with no step above the threshold.
- Given a held note, when it is released at low and high release velocity, the damper sound differs measurably between the two.
- Given the existing half-pedal law (Story 3.11), when the pedal moves through its half-pedal zone during a sustained note, damping changes continuously and follows that law.
- No pedal or release handling allocates or blocks on the audio path.

## Spec Change Log

## Review Triage Log

## Verification

**Commands:**
- `cmake --build build --target ep_engine_tests velora-offline-render -j4`
- `./build/engine/core/ep_engine_tests`
- `ctest --test-dir build -R "ep|null_test_tine|null_test_reed" --output-on-failure`

---
title: 'Story 14.3: Modeled reed voice in the engine core'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '4da5299'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/engine.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_engine.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_voice.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_parameters.hpp'
  - '{project-root}/engine/core/engines/ep/CLEAN-ROOM.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The workstation engine supports modeled tine electric piano voices (Story 14.2), but musicians also require the classic modeled reed electric piano voice (electrostatic pickup, flat steel reed blade, and distinct biting timbre) from the same engine core, capable of playing alongside tine voices on different parts without regression.

**Approach:**
1. Extend `EpVoice` in `engine/core/include/velora/core/ep_voice.hpp` to implement the reed physical model when `EpInstrumentType::kReed` is selected:
   - Cantilever steel reed blade modal dispersion: tip solder mass shifts mode 2 ratio to ~5.6–5.8 with faster clang dissipation and prominent mid-range body.
   - Electrostatic variable-capacitance pickup transducer model: polynomial non-linear displacement current ($s(t) = g_{\text{pu}} v_{\text{tip}} (1 + 2\xi + 3\xi^2 + 4\xi^3)$) with soft saturation, generating characteristic reed bark and even/odd harmonic distortion under forte playing.
   - Tuning across E1 to E7 within the stated tolerance.
2. Support concurrent playback of tine and reed timbres across different parts in `EpEngine`. Ensure the tine golden files of Story 14.2 remain bit-for-bit unchanged.
3. Add offline render CLI support and a committed reed test suite (`reed_key_sweep` and `reed_velocity_sweep`) in `tools/offline-render/tests/ep_suite/golden/` proving bit-identical renders and RT safety.

## Boundaries & Constraints

**Always:**
- Clean-room DSP: derived from acoustics literature cited in `engine/core/engines/ep/CLEAN-ROOM.md` (no samples or reverse engineering).
- Tine non-regression: playing reed voices on other parts or selecting reed must not change tine audio or break existing tine golden files.
- RT-safety (AD-3): zero allocations and zero locks in render path for both reed and tine voices.
- NFR29 generic naming: "Reed", "Modeled Electric Piano" (never "Wurlitzer").

**Never:**
- Use sample playback or audio tables for reed sound generation.
- Mutate shared voice state across channels or parts.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Reed Instrument Selection | Part voicing set to `EpInstrumentType::kReed` | Voice switches to reed modal ratios and electrostatic transducer physics | Bounded output, stable decay |
| Concurrent Tine & Reed | Part 0 playing Tine, Part 1 playing Reed | Both parts render concurrently with correct distinct timbres | Parts remain completely isolated |
| Tine Golden Regression | Render tine key and velocity sweeps | Output matches Story 14.2 golden files with 0.0 dB residual (-300 dB) | Fail test if tine output changes |
| Reed Bit-Identical Suite | Render reed key and velocity sweeps twice | Both runs produce bit-identical WAV output | Exact float bit parity |
| Reed Aliasing at E7 Vel 127 | Note 100 @ velocity 127 on reed voice | High modes band-limited; aliased foldover below threshold | Bounded high-frequency energy |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/ep_parameters.hpp` -- Reed voicing defaults and instrument type selection.
- `engine/core/include/velora/core/ep_voice.hpp` -- Implement reed modal dispersion and electrostatic pickup non-linear model.
- `engine/core/tests/ep_engine_test.cpp` -- Add tests for reed initialization, tuning E1-E7, concurrent tine/reed playback, and bit-identical renders.
- `tools/offline-render/tests/ep_suite/golden/` -- Golden reed MIDI, WAV, and `.case` files.
- `tools/offline-render/CMakeLists.txt` -- Register reed offline render null tests.
- `tools/offline-render/src/render_main.cpp` -- Add `--ep-type tine|reed` option.

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/ep_voice.hpp` -- implement reed modal mechanics and electrostatic non-linear pickup model.
- `tools/offline-render/src/render_main.cpp` -- add `--ep-type tine|reed` option for offline rendering.
- `tools/offline-render/tests/ep_suite/golden/` -- generate and store `reed_key_sweep` and `reed_velocity_sweep` golden files.
- `tools/offline-render/CMakeLists.txt` -- add reed offline render and null tests.
- `engine/core/tests/ep_engine_test.cpp` -- add reed tuning, concurrency, and RT-safety tests.

**Acceptance Criteria:**
- Given the engine from Story 14.2, when an instrument selects the reed type, a voice models the hammer strike, vibrating reed, electrostatic pickup, and damper, with tuning across E1–E7 within test tolerance.
- When tine and reed notes play at once on different parts, both render correctly and the tine golden files of Story 14.2 remain bit-for-bit unchanged.
- Given the offline renderer, when a committed reed test suite renders (key sweep and velocity sweep), two runs are bit-identical and stored as golden files, and RT-safety checks pass.

## Spec Change Log

<!-- Empty until first bad_spec loopback -->

## Review Triage Log

- `hammer_sample_idx_` halted increment at `hammer_duration_`: Fixed by incrementing through `hammer_duration_ + 32` so attack transient shuts off cleanly.
- `noise_seed_` PRNG determinism: Fixed by seeding deterministically on voice start with note and velocity.
- Unsigned underflow on notes < 28: Fixed with `std::max(0.0f, static_cast<float>(note_) - 28.0f)`.
- Upper mode frequency band-limiting: Corrected Nyquist thresholding to prevent high harmonic suppression.
- `render_split()` additive buffer contract: Fixed by using scratch buffers and accumulating without clobbering destination buffers or zeroing unassigned parts.
- `voice_info()` missing `out.note`: Fixed by assigning `out.note = v.note()`.
- `begin_steal_fade()` re-triggering: Guarded to return `false` if voice is already steal fading.
- `EpVoice::stop()` during steal: Ignored while stealing to prevent release transient during fade.
- Offline render CLI validation: Enforced `--ep-type` exclusivity to EP engine and rejected invalid FM/Organ flags when running `--engine ep`.
- Offline render golden tests: Registered `null_test_reed_key_sweep` and `null_test_reed_velocity_sweep` passing at -300 dB residual alongside tine null tests.

## Verification

**Commands:**
- `cmake --build build --target ep_engine_tests velora-offline-render -j4` -- expected: clean build.
- `ctest --test-dir build -R "ep|null_test_tine|null_test_reed" --output-on-failure` -- expected: all pass, including tine non-regression.

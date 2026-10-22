---
title: 'Story 12.1: Clean-room 6-operator FM voice in the engine core'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '9cf264a'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/engine.hpp'
  - '{project-root}/engine/core/include/velora/core/block_renderer.hpp'
  - '{project-root}/engine/core/include/velora/core/ump.hpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The workstation needs a clean-room 6-operator FM synthesis engine in `engine/core` that plays classic FM sounds (electric pianos, bells, basses, brass, poly synths) with high fidelity, full 32-algorithm routing matrix, operator rate/level envelopes, pitch envelope, LFO, and keyboard/velocity scaling, without depending on or vendoring any GPL code (Dexed, MSFA).

**Approach:** Implement `FmEngine` and `FmVoice` in `engine/core/engines/fm/` (or accessible headers/sources under `velora/core/` and compiled into `velora_core`).
1. Clean-room documentation in `engine/core/engines/fm/CLEAN-ROOM.md` documenting published DSP literature (Chowning 1973, expired US Patents 4,018,121 and 4,178,822, Roads "Computer Music Tutorial") and confirming no GPL source code was read, copied, or used.
2. Complete parameter mapping in `engine/core/engines/fm/SYSEX-MAP.md` mapping all 155+ standard 6-OP voice parameters.
3. 6 operators per voice:
   - Sine NCO oscillator with phase accumulator at sample rate (48 kHz).
   - Frequency calculation: base note frequency + pitch envelope + LFO pitch mod + pitch bend, multiplied by operator ratio (coarse 0..31, fine 0..99, detune -7..+7) or fixed frequency mode (1 Hz to 9772 Hz).
   - Operator envelopes: 4 rates (R1, R2, R3, R4: 0..99) and 4 levels (L1, L2, L3, L4: 0..99). Time and level curves matching classic FM rate-to-slope mapping.
   - Output level (0..99) with velocity sensitivity (0..7), keyboard rate scaling (0..7), and keyboard level scaling (breakpoint, left/right curves: linear/exponential, depth 0..99).
   - Pitch envelope: 4 rates (0..99) and 4 levels (0..99, where 50 is center/no detune).
   - LFO: 6 waveforms (triangle, saw down, saw up, square, sine, S&H), speed (0..99), delay (0..99), pitch mod depth (0..99), amp mod depth (0..99), key sync, pitch mod sensitivity (0..7).
   - Routing algorithms: all 32 classic algorithms implemented via a clean modulation buffer matrix, with operator feedback (levels 0..7 with 2-sample running average for stability).
4. Real-time safety: zero allocation in RT block loop, zero locks, deterministic 64-frame block processing.
5. Voice management: exposes steal metadata (age, level, held/sustained state), graceful steal fade ramp down, per-voice cost units.
6. Offline test suite & aliasing checks: tests for algorithm matrix, feedback sweep, envelope stages, velocity response, pitch bend, and anti-aliasing limit test.

## Boundaries & Constraints

**Always:**
- Clean-room: implement from public math & DSP papers only.
- RT-safety: no heap allocation, no locks, no filesystem or OS calls in audio path.
- MIDI 2.0 UMP consumption (Note On/Off, Pitch Bend, Mod Wheel CC 1, Sustain CC 64, Expression CC 11, Channel Pressure).
- Polyphony: capacity for up to 128 voices, with per-voice steal fading and metadata.
- Compile cleanly into `velora_core`.

**Never:**
- Read or vendor Dexed, MSFA, or any GPL code.
- Add third-party trademarks ("DX7" or proprietary patch names) into shipped sources or binaries.
- Break existing Sample or Organ engine audio or tests.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Algorithm 1-32 | Note 60 on algorithms 1 to 32 | Carriers sum to voice output; modulators modulate phases per algorithm matrix | Matrix validated for all 32 algs |
| Feedback 0..7 | OP with feedback | Level 0 = pure sine; Level 7 = rich harmonics/noise-like spectrum without divergence | Feedback clamped, averaged |
| Rate/Level Envelopes | Note On -> sustain -> Note Off | R1->L1, R2->L2, R3->L3; sustain at L3; release R4->L4 | Envelopes bounded 0.0..1.0 |
| Velocity Sensitivity | Note On with vel 1 vs 127 | Level scaled exponentially according to operator velocity sensitivity | Bounded |
| Fixed Frequency Mode | Operator mode = Fixed | Frequency independent of key, set by coarse/fine frequency | Bounded within 1..9772 Hz |
| Steal Fade | Arbiter triggers steal | Voice fades to zero over requested ramp frames without pop/click | Bounded monotone attenuation |

</intent-contract>

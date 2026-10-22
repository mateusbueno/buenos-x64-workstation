---
title: 'Story 2.21: Effects surface — sends, master and outputs'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '15eb7fac15d48243be1c0d486ca9e663a89eeab3'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need direct on-device control over reverb, delay, per-part sends, master 3-band sweepable EQ, master compressor, line-in level, and per-part Main/Aux output routing on the 7" 1024x600 screen without a computer.

**Approach:**
1. Extend `EffectsView` with section tabs: `INSERTS`, `SENDS`, `MASTER`, `OUTPUTS`.
2. SENDS section:
   - Reverb master controls: type, decay time, level, damping.
   - Delay master controls: time (ms), sync note, feedback, tone/damping, level.
   - Per-part send levels (Parts 1-8): reverb send and delay send sliders/rows (0-100%).
   - Sends commands via `ws::v1::SetSendLevels` and `ws::v1::SetMasterParams`.
3. MASTER section:
   - 3-band EQ: Low Gain (-15..+15 dB), Mid Gain (-15..+15 dB), Mid Freq (200 Hz..8 kHz), High Gain (-15..+15 dB), Bypass.
   - Master Compressor: Threshold (-60..0 dB), Ratio (1..20), Attack (0.1..100 ms), Release (10..1000 ms), Makeup (0..24 dB), Bypass, plus live Gain Reduction display.
   - Line-in level slider / ratio (0..100%).
   - Sends commands via `ws::v1::SetMasterParams` and `SetPerformanceParam`.
4. OUTPUTS section:
   - For each part 1..8: `SegmentedControl` with `{"MAIN", "AUX"}`.
   - Changing output sends `ws::v1::SetPartOutput` smoothly without pop or drop.
5. Verification:
   - Unit tests in `ui/tests/effects_view_test.cpp` verifying all 4 sections, command generation, parameter formatting, and state synchronization.

## Boundaries & Constraints

**Always:**
- Use tokens from `DESIGN.md` (no color literals).
- All touch targets in these widgets must be >= 56px (`touch_target_min`).
- Touch and panel equivalents: VALUE turn moves/changes, WRITE confirms, touch selects.
- Zero audio allocations or locks; UI decoupled via domain socket.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Section switch | Tap "SENDS" tab | Displays Sends section with reverb, delay, and per-part send rows | Remembers previous parameter view |
| Adjust send level | Move Part 1 reverb send to 45% | `SetSendLevels` sent with part=0, reverb=0.45; engine applies | Dispatched cleanly |
| Master EQ mid freq | VALUE turn on mid freq | Updates mid frequency between 200 Hz and 8 kHz; formatted as Hz/kHz | Clamped to registry range |
| Part output toggle | Tap "AUX" on Part 3 | `SetPartOutput` sent with part=2, output=PART_OUTPUT_AUX | State reflects AUX |

</intent-contract>

## Tasks & Acceptance

- [x] Add section tab selector (`INSERTS`, `SENDS`, `MASTER`, `OUTPUTS`) in `EffectsView`
- [x] Implement `SENDS` section with reverb/delay master controls and per-part send rows
- [x] Implement `MASTER` section with sweepable 3-band EQ, compressor with GR, and line-in level
- [x] Implement `OUTPUTS` section with per-part Main/Aux `SegmentedControl`s sending `SetPartOutput`
- [x] Add tests in `ui/tests/effects_view_test.cpp` covering sends, master, and outputs
- [x] Verify 100% test pass and all CI linters green

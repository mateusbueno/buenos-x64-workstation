---
title: 'Story 14.6: Objective comparison against the development references'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '4b2824a'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md'
  - '{project-root}/engine/core/engines/ep/CLEAN-ROOM.md'
  - '{project-root}/tools/ep-compare/'
  - '{project-root}/engine/core/tests/ep_engine_test.cpp'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Implement an offline tool (`tools/ep-compare/ep_compare.py` / `ep_objective_comparison_tests`) that objectively measures the modeled electric piano (tine and reed) against physical reference measurements recorded in Story 14.1 (tim.kahn Rhodes Mark II / OldBassMan Wurlitzer 200a).
   The tool measures and reports per key:
   - Partial frequencies and modal ratios (Mode 1 $f_1$, Mode 2 $f_2 \approx 6.27\times$ tine, $5.65\times$ reed, Mode 3 $f_3$).
   - Decay times ($t_{60}$ in seconds across registers: lower notes 5.0–12.0s, upper notes 1.5–4.0s).
   - Dynamic spectral brightness and level scaling across MIDI strike velocities (1..127).
   
2. Tolerances & Test Suite:
   - Tolerances set from development references recorded in `tools/ep-compare/tolerances.json`:
     - Fundamental frequency within 0.1 cents.
     - Modal ratios within +/- 2.5% of theoretical Euler-Bernoulli cantilever (tine) and flat steel reed (reed).
     - Decay time $t_{60}$ within +/- 15% of reference decay envelopes.
     - Velocity dynamic range >= 40 dB across velocity 1..127.
   - The comparison runs in the test suite on a committed subset of notes: E1 (28), A2 (45), C4 (60), A4 (69), C6 (84).
   
3. IP & Scope Compliance:
   - The comparison report states explicitly that real-instrument development references pass through an analog tube/transformer preamp, which bare transducer physical models do not inherently model.
   - Zero reference audio samples are vendored, included in the product, or shipped in customer packages (strict NFR53 compliance).
</intent-contract>

## Implementation Plan
1. Create `tools/ep-compare/ep_compare.py`:
   - Command-line tool with `--tool-summary`, `--verify`, `--tolerance-file`, `--report`.
   - Supports rendering or generating tone models and analyzing partial ratios, decay envelopes ($t_{60}$), and spectral velocity dynamics.
   - Includes full self-test mode (`--self-test`).
2. Create `tools/ep-compare/tolerances.json`:
   - Machine-readable tolerances for tine and reed across the committed test subset notes.
3. Create C++ unit test / verification test in `engine/core/tests/ep_objective_test.cpp` (or in `ep_engine_test.cpp`) and register in `engine/core/CMakeLists.txt` / ctest:
   - Validates partial modal ratios, decay rates, and velocity scaling against committed tolerances.

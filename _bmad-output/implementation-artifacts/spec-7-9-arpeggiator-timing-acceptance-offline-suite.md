---
title: 'Story 7.9: Arpeggiator timing acceptance (offline suite)'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '5cd3f6c3e853f5115ad88fca9c2fcb392e2ddbec'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/engine/core/include/velora/core/arpeggiator.hpp']
warnings: []
deferred:
  - summary: >-
      Board acceptance not done: external-clock jitter <=1 ms over DIN and USB (72/120/174 BPM, 10 min) and cable-pull behaviour need the rig; the host RenderDriver path is not driven by this core-level suite.
    evidence: |-
      Suite drives BlockRenderer only (engine/core cannot depend on engine/host).
    severity: medium
---

<intent-contract>

## Intent

**Problem:** There is no CI gate proving the arpeggiator is sample-accurate on the internal clock when driven through the full engine path (NFR7, FR75, FR76, AR2 partial). Unit tests exist per feature (Stories 7.2/7.3) but no acceptance suite that renders two arpeggiated parts at several tempos, all rates (including triplets) and swing values and compares every onset with an independently computed grid frame.

**Approach:** Add an offline timing acceptance suite (a CTest target that fails the build on any deviation) that drives `BlockRenderer` (and the host `RenderDriver` path where practical) with the arpeggiator active on 2 parts, records note-on/off frames per part, and checks each against a closed-form expected frame computed in the test from tempo, rate ticks, swing and the transport clock's grid definition (not by calling the arpeggiator's own scheduling code). Also add the report/analysis tooling for the later board rig only if it is small; the rig itself is out of scope.

## Boundaries & Constraints

**Always:** Expected frames are computed independently in the test (frames per tick from tempo and sample rate, swing delaying odd steps per the documented rule, gate length), with exact (zero-frame) tolerance for the internal clock; cover at least tempos 72, 120, 174 and two more (including a non-integer frames-per-tick case), all 8 rates (1/4..1/32 and 4 triplets), swing 50, 58.3, 66.7 and 75 %, 2 parts with different rates/swings active at the same time, tempo change mid-run if the transport supports it (state the expectation from the documented behaviour), block-boundary-crossing onsets, and a long run (e.g. 10 simulated minutes at one tempo) with no drift (deviation 0 frames at the end). A deviation of even 1 frame fails the test with a message naming part, tempo, rate, swing and step index. Deterministic, no wall-clock.

**Never:** No change to arpeggiator or transport behaviour (if the suite finds a real discrepancy, report it as a finding in the result and keep the test asserting the documented behaviour; do not weaken the expectation). No board rig, no external-clock hardware tests, no DIN/USB clock work (Stories 7.7, 7.9 board parts stay deferred).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Grid exactness | 2 parts, tempos × rates × swings | every onset on its computed frame | any 1-frame deviation fails |
| Triplets | 1/4T..1/32T | onsets on 640/320/160/80-tick grid frames | same |
| Swing | 50..75 % | odd steps delayed by the exact computed frames | same |
| Non-integer frames/tick | tempo with fractional frames per tick | onsets follow the transport clock's rounding rule, no cumulative drift over 10 min | same |
| Independence | part A and part B at different settings | each part's onsets unaffected by the other | same |
| Block boundaries | onset exactly at block end / start | lands on the correct block and in-block offset | same |

## Code Map

- `engine/core/include/velora/core/arpeggiator.hpp`, `src/arpeggiator.cpp`, `transport_clock.hpp`, `event_processor.hpp` -- behaviour under test (grid rules, swing, gate); documented in headers.
- `engine/core/tests/arpeggiator_test.cpp` (test rig, `test_rates_tempos_and_grid_alignment`, swing cases), `transport_clock_test.cpp`, `rtsan_arpeggiator_test.cpp` -- rigs and patterns to reuse.
- `engine/core/include/velora/core/block_renderer.hpp`, `engine/host/include/velora/host/render_driver.hpp` -- the engine path to drive offline; `tools/offline-render` for the offline renderer harness conventions.
- `engine/core/CMakeLists.txt` (or tools/offline-render CMake) -- register a CTest target named `arp_timing_acceptance`.
- New: `engine/core/tests/arp_timing_acceptance_test.cpp` (or in tools/offline-render if the offline renderer is the natural place).

## Tasks & Acceptance

**Execution:**
- `engine/core/tests/arp_timing_acceptance_test.cpp` + CMake -- the suite per the matrix, closed-form expectations, failure messages naming the combination
- result doc -- if any discrepancy between documented behaviour and observed onsets is found, list it under `## Auto Run Result`

**Acceptance Criteria:**
- Given the offline suite in CI, when the arpeggiator is active on 2 parts at several tempos, rates (incl. triplets) and swing values, then every arpeggiated onset lands on its exact computed grid frame and any deviation fails the build.
- The board-only acceptance (external clock jitter ≤1 ms over DIN/USB, cable-pull behaviour) is recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass (single focused reviewer; test-only change)
- findings:
  - `[medium]` `patch` rounding ties never exercised — tempos 800/1600/2400 added with tie-count assertions
  - `[medium]` `patch` block-boundary assertion vacuous — five per-case exact checks
  - `[medium]` `patch` tempo-change rule undocumented — documented in arpeggiator.hpp, model follows the 64-frame catch-up window
  - `[low]` `patch` offs/velocity/unexpected parts — compared and failing

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "arp|transport|clock" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

- Added `engine/core/tests/arp_timing_acceptance_test.cpp` and CTest target `arp_timing_acceptance` (engine/core/CMakeLists.txt). 38,893 onsets and 38,774 offsets compared with zero-frame tolerance; no discrepancy between documented behaviour and observed frames was found.
- Coverage: tempos 72, 120, 174, 97.3, 133.7 bpm x 8 rates x swing 50/58.3/66.7/75 with two simultaneous parts at different rate/swing; independence runs; mid-run tempo change (takes effect at the block after the parameter event, position continuous); 10 simulated minutes at 174 bpm with 0-frame end deviation; in-block offsets 0 and 63 asserted covered.
- Not done: driving the host `RenderDriver` path (engine/core tests cannot depend on engine/host; BlockRenderer path only).
- Deferred (board-only): external clock jitter <= 1 ms over DIN/USB, cable-pull behaviour, the board rig and report tooling (Stories 7.7 / 7.9 board parts).

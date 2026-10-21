---
title: 'Story 7.1: Single tempo and transport clock on the RT plane'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: '01e25666412a46f4dcce5c1218f1bfdfd2fa24fe'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Story 7.1 in `_bmad-output/planning-artifacts/epics.md` (`### Story 7.1`, ~line 4608): the RT plane has a tempo value (`tempo_bpm`, Story 2.11) but no clock, so nothing can place events on a musical grid.

**Approach:** add one `TransportClock` to `BlockRenderer`. Position is an exact integer rational (frames x tempo-in-tenths-of-bpm, in 1/30000 tick units at 960 PPQN), never accumulated floats. The clock reads the existing `tempo_bpm` value once per block (same point as the Mixer), advances per block, and exposes tick-at-frame and frame-of-tick queries. Epic 7.6 will drive it from external clock.

## Boundaries & Constraints

**Always:** exactly one clock; 960 PPQN; tempo 20-300 bpm at 0.1 resolution (clamped, NaN -> 120); a tempo change applies from the next block and keeps the musical position continuous; no allocation/locks in the RT path; the existing `tempo_bpm` registry id 802 and `BlockRenderer::tempo_bpm()` stay unchanged; existing goldens unchanged.

**Never:** arpeggiator, event processors, external clock, transport start/stop, UI, new registry ids or Live Set format changes.

</intent-contract>

## Code Map

- `engine/core/include/velora/core/transport_clock.hpp`, `engine/core/src/transport_clock.cpp` -- new: the clock (integer math)
- `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` -- `process_block(OutputBuses)` owns it: set tempo before `mixer_.begin_block`, advance at block end; `tempo_bpm()` is the source (line ~469)
- `engine/core/include/velora/core/delay.hpp` -- `delaymap::tempo_bpm()` clamp reused
- `engine/core/tests/delay_test.cpp` -- pattern for BlockRenderer rigs and tempo param events; `engine/core/CMakeLists.txt` -- test wiring
- `engine/core/README.md` -- document the clock

## Tasks & Acceptance

**Execution:**
- `transport_clock.{hpp,cpp}` -- implement `TransportClock` (set_tempo_tenths, advance, ticks_at, frame_of_tick nearest-frame, tenths_from_bpm) -- exact rational grid
- `block_renderer.{hpp,cpp}` -- own and drive the clock, expose `clock()` -- single clock on the RT plane
- `engine/core/tests/transport_clock_test.cpp` (+ CMake) -- unit tests, tempo change continuity, 10-minute grid at 120 and 97.3 bpm through the BlockRenderer
- `engine/core/README.md` -- Transport clock section; `sprint-status.yaml` -- 7-1 done

**Acceptance Criteria:**
- Given the clock, when any frame in a block is queried, then the tick position derives from the integer frame count and tempo with no drift
- Given a tempo change, when the next block renders, then the clock uses the new tempo and the position is continuous
- Given 10 minutes at 120 and 97.3 bpm, when every sixteenth's frame is computed, then it equals the exact rational position rounded to the nearest frame

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: pass

## Auto Run Result

Status: done. `TransportClock` (960 PPQN, integer 1/30000-tick units, tempo in tenths of bpm) owned by `BlockRenderer`, set from `tempo_bpm()` before `Mixer::begin_block` and advanced per block. Files: `engine/core/{include/velora/core/transport_clock.hpp,src/transport_clock.cpp,tests/transport_clock_test.cpp}` new; `block_renderer.{hpp,cpp}`, `CMakeLists.txt`, `README.md` updated. Review: 4 patches (1 medium, 3 low), 0 deferred, rejections recorded above. Follow-up review recommended: false. Verification: full ctest 543/543 (2 skipped, pre-existing), `check_core_deps.py` pass, `transport_clock_tests` pass after patches. RTSan not run on macOS.

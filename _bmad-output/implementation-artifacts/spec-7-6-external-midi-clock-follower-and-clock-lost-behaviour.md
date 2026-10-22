---
title: 'Story 7.6: External MIDI clock follower and clock-lost behaviour'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: '941f7902a7d4fb7d72d19dc3d851dc78b019867c'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/engine/core/include/velora/core/transport_clock.hpp'
  - '{project-root}/engine/core/include/velora/core/arpeggiator.hpp'
  - '{project-root}/engine/core/include/velora/core/block_renderer.hpp'
  - '{project-root}/engine/core/include/velora/core/ump.hpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** In `_bmad-output/planning-artifacts/epics.md` (`### Story 7.6`, line 4742), when playing with an external drum machine or sequencer, the RT engine has no external MIDI clock follower. If the clock source is external, the engine must estimate tempo and phase from incoming MIDI clock ticks (`0xF8`), Start (`0xFA`), Continue (`0xFB`), and Stop (`0xFC`) UMP events, tracking tempo within 2 beats, attenuating jitter to ensure arpeggiator notes land within ±1 ms of ideal grid positions, continuing without stopping on clock dropout while raising `clock_lost`, and handling transport Start/Continue/Stop cleanly.

**Approach:**
1. In `engine/core/include/velora/core/ump.hpp`, define constants and helpers for MIDI System Real Time UMP messages (type `0x1`, status `0xF8` Timing Clock, `0xFA` Start, `0xFB` Continue, `0xFC` Stop).
2. In `engine/core/include/velora/core/clock_follower.hpp` and `engine/core/src/clock_follower.cpp`, implement `ClockFollower` running on the RT plane:
   - Second-order Delay-Locked Loop (DLL) filter: estimated period $\tau$ and phase $\theta$.
   - Handles incoming ticks at 24 ppqn (40 ticks per clock in 960 PPQN). Tracks tempo changes within 2 beats (48 ticks) with critical damping to avoid overshoot.
   - Phase and tempo updates drive `TransportClock`.
   - Clock lost detector: if no clock tick arrives for longer than 3 ticks at the last tempo (minimum 150 ms / 7200 frames at 48 kHz) without a preceding Stop message, latches `clock_lost` state while continuing to run `TransportClock` at the last tempo. Resuming clock clears `clock_lost` and smoothly re-locks without double triggers.
   - Start/Continue: aligns the arpeggiator step grid to the next clock tick.
   - Stop: leaves `TransportClock` running at the last tempo without raising `clock_lost`.
3. In `engine/core/include/velora/core/block_renderer.hpp` and `engine/core/src/block_renderer.cpp`, integrate `ClockFollower`:
   - Expose `set_clock_source(ClockSource)` and `clock_follower()`.
   - Process realtime clock UMP events through the follower. When external clock is enabled, drive `clock_` from `follower_`.
4. In `engine/core/tests/clock_follower_test.cpp`, verify DLL tracking within 2 beats, clock lost timing thresholds, Start/Continue/Stop semantics, and re-lock without double trigger.
5. In `tools/offline-render/tests/clock_follower_render_test.cpp`, generate synthetic clock streams at 72, 120, 174 BPM with ±0.5 ms jitter and a tempo ramp 90->130 BPM, render with arpeggiator active, assert every Note On is within ±1 ms of ideal tick position, and write the timing verification report.

## Boundaries & Constraints

**Always:**
- Follower runs on the RT plane without allocations, locks, system calls, or floats that can NaN/diverge.
- Audio RT thread safety (`VELORA_NONBLOCKING`).
- Tempo clamp 20.0 to 300.0 BPM (200 to 3000 tenths).
- Jitter tolerance: synthetic stream with ±0.5 ms jitter must yield Note Ons within ±1 ms of ideal position.
- Clock loss: > 3 ticks (min 150 ms) without Stop raises `clock_lost` and keeps running at last tempo.
- Stop message keeps running at last tempo without raising `clock_lost`.
- Start/Continue aligns the step grid to the next clock tick.

**Never:**
- Never crash, allocate memory, or block on the RT thread.
- Never lose tempo continuity or create double triggers on clock re-lock.

</intent-contract>

## Code Map

- `engine/core/include/velora/core/ump.hpp` -- System Real Time UMP constants (`0xF8`, `0xFA`, `0xFB`, `0xFC`) and helpers
- `engine/core/include/velora/core/clock_follower.hpp`, `engine/core/src/clock_follower.cpp` -- RT plane DLL clock follower and clock lost detector
- `engine/core/include/velora/core/transport_clock.hpp`, `engine/core/src/transport_clock.cpp` -- helper to align position to tick at a specific frame
- `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` -- integrate `ClockFollower` into `BlockRenderer`
- `engine/core/tests/clock_follower_test.cpp` -- unit tests for follower DLL, clock lost, Start/Stop/Continue
- `tools/offline-render/tests/clock_follower_render_test.cpp` -- offline render with jitter & ramp, ±1 ms note onset verification and timing report

## Tasks & Acceptance

**Execution:**
- `ump.hpp` -- add System Real Time UMP constants & inspection methods
- `clock_follower.{hpp,cpp}` -- implement 2nd-order DLL filter, clock lost detection, Start/Continue/Stop
- `transport_clock.{hpp,cpp}` and `block_renderer.{hpp,cpp}` -- connect follower to `TransportClock`
- `engine/core/tests/clock_follower_test.cpp` -- test tracking within 2 beats, clock lost > 3 ticks (min 150ms), Stop without clock lost, re-lock without double trigger
- `tools/offline-render/tests/clock_follower_render_test.cpp` -- test arpeggiator following 72, 120, 174 BPM with ±0.5 ms jitter and 90->130 ramp, check Note Ons <= 1 ms error, output report

**Acceptance Criteria:**
- Given timestamped MIDI clock (0xF8), Start, Continue and Stop UMP events in device time: when clock source is external, then follower on RT plane estimates tempo and phase with a filter tracking tempo changes within 2 beats, driving the single transport clock in place of Live Set tempo.
- Given synthetic clock stream at 72, 120, 174 BPM with ±0.5 ms jitter, plus a ramp from 90 to 130 BPM: when arpeggiator runs following it in offline renderer, then every arpeggiated Note On lies within ±1 ms of ideal position, and results are written to a test report.
- Given clock ticks stop arriving for longer than 3 ticks at last tempo (minimum 150 ms) without Stop: when follower detects it, then clock keeps running at last tempo, engine publishes clock_lost state with that tempo, and state clears when clock resumes, re-locking without double trigger.
- Given MIDI Start or Continue: when it arrives, arpeggiator step grid aligns to next clock tick; after Stop, arpeggiator keeps running at last tempo without raising clock_lost.

## Verification

**Commands:**
- `ctest --test-dir build -R clock_follower` -- expected: all pass
- `ctest --test-dir build -R "(arpeggiator|transport_clock|live_set)"` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: pass

## Auto Run Result

- **Implementation**: Implemented `ClockFollower` in `engine/core/src/clock_follower.cpp` and `engine/core/include/velora/core/clock_follower.hpp` with 2nd-order critically damped DLL filter tracking tempo changes within 2 beats (48 ticks). Handled System Real Time UMP messages (`0xF8`, `0xFA`, `0xFB`, `0xFC`) in `ump.hpp`. Implemented clock loss timeout (> 3 ticks at last tempo, minimum 150 ms / 7200 frames), re-lock without backward time jumps or double triggers, Start/Continue grid realignment, and Stop continuing last tempo without warning. Integrated into `BlockRenderer` with `atomic<ClockSource>` and `LastValueTable::store_clock_lost()`.
- **Review Findings Addressed**:
  - Unlocked start phase synchronization: derived `tick_count_ = (cur_tick + 20) / 40` to maintain strictly non-decreasing monotonic position on initial lock.
  - Continue realignment: aligned tick count and re-anchored DLL filter; verified grid realignment to next 40-tick boundary.
  - Stopped state clearing on resumed clock ticks.
  - Proper floor division in `TransportClock::ticks_at` for negative `units_`.
  - Multi-path fallback for timing report file output in offline tests.
- **Verification**:
  - `clock_follower_tests` passed all unit tests (monotonicity, timeout thresholds, Start/Continue/Stop semantics, re-lock).
  - `clock_follower_render_test` passed all synthetic jitter (±0.5 ms) and 90->130 BPM ramp scenarios with Note On timing errors ≤ 0.803 ms (< 1.0 ms requirement). Report generated at `_bmad-output/implementation-artifacts/clock-follower-timing-report.md`.
  - Regression suite (27/27 tests) passed 100%.
  - `python3 tools/ci/check_core_deps.py` passed with pure core dependencies.

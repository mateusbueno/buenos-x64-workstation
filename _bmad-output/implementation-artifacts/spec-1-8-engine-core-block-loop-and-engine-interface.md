---
title: 'Story 1.8: Engine core block loop and engine interface'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '4c3eabef6cae0f632a1b561b003e7c686a0d43b5'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** There is no engine core yet. Every DSP story needs a platform-independent block loop, a UMP-only event input, one engine contract and RT-safe primitives to build on.

**Approach:** In `engine/core` (and interfaces in `engine/ports` only if needed) implement a single-threaded serial block renderer: 64-frame stereo float blocks at 48 kHz, a UMP event input with per-event target frame, a static engine interface with per-voice cost and steal metadata, a registry of four reserved engine type IDs (Sample, Organ, FM, VA), a lock-free SPSC queue, a pre-allocated arena, and a test-tone engine; unit-test it including under RTSan.

## Boundaries & Constraints

**Always:** C++20, no heap allocation, locks or blocking calls in the render path (RTSan clean, functions marked `[[clang::nonblocking]]` where supported, guarded by a macro so gcc/Apple clang still build); `engine/core` includes only core/ports/std (the purity check must keep passing); UMP (MIDI 2.0 Universal MIDI Packet, 64-bit MIDI 2.0 channel voice messages for note on/off at minimum) is the only event type the core accepts; a note-on at target frame N inside a block starts the voice exactly at frame N; voices start/stop only via calls from the core; engines are compiled in, no plugin loading; engine type IDs for Sample, Organ, FM, VA are reserved and stable (fixed numeric values, documented).

**Never:** Add threads or the parallel phases (Story 1.18), a real voice-pool arbiter with floors (Story 1.19), real sample/organ/FM/VA DSP, audio device I/O (Story 1.10), file I/O, or any dependency outside the standard library. No MIDI 1.0 byte-stream path into the core.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Offset onset | note-on at frame 37 of a block | frames 0..36 exactly 0.0; onset at frame 37 | none |
| Offset release | note-off at frame k | release begins exactly at frame k | none |
| Multiple events one block | events at frames 10 and 50 | both applied at their frames, in frame order, ties in arrival order | none |
| Event beyond block | target frame >= 64 | deferred to the next block (frame offset minus 64) or rejected with typed status; behavior is documented and tested | typed status, no crash |
| Reserved engine not compiled | voice request for FM or VA type | block output silence for that request; status `ENGINE_NOT_AVAILABLE`; never served by another engine | typed status |
| Unknown engine type id | id outside the four reserved | rejected with typed status `ENGINE_UNKNOWN` | typed status |
| Queue full | SPSC push when full | returns false, nothing lost already queued | caller handles |
| Pool exhausted | more note-ons than voices | newest note-on dropped with typed status (stealing comes in Story 1.19) | typed status |
| Non-UMP input | anything not a UMP packet | not representable: the core API takes only a UMP type | n/a |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/` and `engine/core/src/` -- existing placeholder (`version.hpp`, `version.cpp`); add new headers/sources here
- `engine/core/CMakeLists.txt` -- existing static lib `velora_core` linking `velora_ports`, RTSan test wiring (`rtsan_safe_test`, `rtsan_unsafe_test` pattern) to follow for new RTSan tests
- `engine/core/tests/core_test.cpp` -- existing minimal test style (bare main, exit codes); keep that style
- `tools/ci/check_core_deps.py` -- purity gate; must keep passing; run it
- `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md` -- AD-5 (block phases), AD-6 (voice contract), AD-7 (engine interface), AD-8 (UMP, frame offset) -- read-only
- `_bmad-output/planning-artifacts/epics.md` Story 1.8 (line ~976) -- ACs -- read-only

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/ump.hpp` -- UMP packet type (MIDI 2.0 channel voice note on/off, plus a frame-stamped event wrapper {ump, target_frame}) -- only accepted event type
- `engine/core/include/velora/core/spsc_queue.hpp` -- fixed-capacity lock-free SPSC ring, no allocation after construction -- event and control handoff
- `engine/core/include/velora/core/arena.hpp` -- pre-allocated bump arena -- scratch memory for the RT path
- `engine/core/include/velora/core/engine.hpp` -- static engine interface (cost units per voice, steal metadata: start time, current level, held/sustained/releasing; start/stop voice and render calls driven by the core; sound parameters addressed by `param_id`) plus `EngineTypeId` enum for Sample, Organ, FM, VA with fixed values and statuses ENGINE_NOT_AVAILABLE/ENGINE_UNKNOWN -- single contract for all four engines
- `engine/core/include/velora/core/engine_registry.hpp` + src -- compile-time registry of compiled-in engines; unavailable reserved types give typed status -- no plugin loading
- `engine/core/include/velora/core/test_tone_engine.hpp` + src -- sine voices with linear attack/release envelope implementing the interface -- test engine
- `engine/core/include/velora/core/block_renderer.hpp` + src -- 64-frame stereo float 48 kHz block loop: drain queue, apply events at exact frame offsets, render, sum -- core of the story
- `engine/core/tests/` -- tests for every matrix row (frame-37 onset test asserting exact silence before and exact first non-zero at frame 37; ENGINE_NOT_AVAILABLE; queue full; pool exhausted; event ordering) plus an RTSan test rendering 10,000 blocks with note on/off traffic (built only when RTSan is available; wired like the existing rtsan tests, expected to pass) -- covers ACs and matrix
- `engine/core/CMakeLists.txt` -- add sources and tests -- wiring
- `engine/core/README.md` -- one line on the new modules -- docs

**Acceptance Criteria:**
- Given a note-on scheduled at frame 37, when the block renders, then output is exactly silent before frame 37 and the voice onset starts at frame 37
- Given the RTSan build, when 10,000 blocks with note traffic render, then RTSan reports no violations (test passes in CI; locally skipped if unsupported)
- Given a voice request for FM or VA with no compiled engine, then silence plus typed ENGINE_NOT_AVAILABLE and no other engine serves it
- Given the core API, then only UMP events can be submitted

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test` -- expected: pass
- RTSan test is verified in CI only (no RTSan clang locally unless Homebrew llvm is installed); say so in the report

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~45 findings — patched 8 groups, rest deferred
- findings:
  - `[medium]` `patch` arena overflow/bad alignment, no trivially-destructible check, movable — overflow-safe bounds, align check, static_assert, copy/move deleted
  - `[medium]` `patch` full pending buffer stalled the queue and starved imminent events — event dropped with typed `kEventOverflow`, queue always drains
  - `[medium]` `patch` UMP group ignored so groups collided — only group 0 handled, others counted as ignored
  - `[low]` `patch` null output pointers; unchecked registry bind; non-finite gain — guarded
  - `[medium]` `patch` tests missing for set_param, ignored UMP, arena limits, capacity 0, pending overflow; RTSan test happy-path only and unasserted — tests added; RTSan test now covers deferral, reserved engine, pool-full and asserts counters
  - `[low]` `defer` set_channel_engine thread-safety (setup-only, documented), non-atomic diagnostic counters, concurrent SPSC test (TSan), velocity-0 policy, duplicate note retrigger, sustained state, magic numbers, per-voice vs per-engine cost granularity
  - `[unverified]` `defer` RTSan run happens first in CI (std::sin/pow are not libc-intercepted)

## Auto Run Result

Status: done. Implemented the serial engine core in `engine/core`: UMP types, SPSC queue, arena, `Engine` interface with four reserved engine type IDs (Sample=1, Organ=2, FM=3, VA=4) and typed statuses, registry, test-tone engine, `BlockRenderer` (64-frame stereo, exact in-block onset, deferral beyond block), plus tests and an RTSan block-loop test. Verification: macOS build with ctest 6/6 pass, purity check and self-test pass; RTSan test runs in CI only. Follow-up review recommended: false.

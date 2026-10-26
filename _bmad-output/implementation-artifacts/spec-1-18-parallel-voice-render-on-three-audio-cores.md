---
title: 'Story 1.18: Parallel voice render on the three isolated cores (Mac correctness)'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '0c9dd6b25b1701c441fcbdc54b61501d02a0cb4f'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/engine/core/include/velora/core/engine.hpp']
warnings: []
deferred:
  - 'Dev-board timing: 256 stereo sample voices within 70 % of 1.333 ms over 1 h with a histogram, workers pinned to the 3 isolated A76 cores via LinuxThreadAffinity (Linux build, not exercised on the Mac)'
---

<intent-contract>

## Intent

**Problem:** The block loop renders every voice serially, which cannot reach 256 stereo sample voices within the 1.333 ms period on the target. The story adds a parallel voice-render phase over 3 worker threads (pinned to the 3 isolated A76 cores on the board) with a barrier and a deterministic summation (AR14, NFR2, NFR10). Only correctness is accepted on the Mac; the timing budget is accepted on the dev board.

**Approach:** Add a worker pool to the engine core and a parallel path in the sample engine's render: each block runs serial event processing, then the voices are rendered by 3 workers into per-worker scratch buffers (a fixed, deterministic partition of voice slots to workers), then a barrier, then a summation of the scratch buffers into the outputs in fixed worker order (0,1,2). Workers synchronise lock-free with atomics (no mutex, no syscall; pinning via a port/hook invoked once at start, not per block). The partition and summation order are fixed, so rendering with 1 execution thread (all 3 partitions run inline) and with 3 threads is bit-identical.

## Boundaries & Constraints

**Always:** Voices write only their own state and their worker's scratch (voice contract). Serial event processing, voice start/stop, steals and arbiter work stay on the main audio thread before the parallel phase. The parallel path is opt-in (a renderer/engine setting; default off = today's exact serial behaviour, so every existing test and golden is unchanged). With it on, the partition is fixed at 3 partitions regardless of how many threads execute them; execution thread count 1 (inline) and 3 (pool) must give bit-identical output. No locks, allocation or syscalls on the RT path; workers wait with atomics (spin/yield-free pause), never sleep on a mutex/condvar in the block path; thread creation and affinity are done at setup. Pinning is a port interface (`engine/ports`) with a no-op/macOS-friendly default and a Linux `sched_setaffinity` implementation compiled only on Linux (and not used by tests). RT-safety: the existing rtsan/rt-traffic patterns must pass for the parallel path (build-time only on macOS where RTSan is unavailable; still add the rt-traffic test). engine/core must keep passing tools/ci/check_core_deps.py.

**Never:** No timing/budget claims on the Mac (the 70 % of 1.333 ms histogram is dev-board work, record it as deferred). No changes to FM/VA/organ engines' internals (they stay on the main thread; only the sample engine's voices go parallel), no change to Engine's public contract other than additive hooks, no change to the default serial output.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Determinism | same input, 256 sample voices, 1 thread vs 3 threads (same 3-way partition) | bit-identical output buffers | any differing sample fails with block/frame |
| Parallel vs serial | parallel mode vs legacy serial | numerically equal within a documented float tolerance (summation order differs); default-off path bit-identical to today | tolerance exceeded fails |
| Voice churn | steals, note-offs, fades, releases during the block | identical 1- vs 3-thread output; no voice touches another's state | none |
| Few voices | 0 or 1 active voice | still correct; barrier does not hang | none |
| Worker count config | 1 or 3 execution threads | both accepted; other values clamped/rejected with a typed result | clamp |
| Shutdown | stop with workers running | threads joined cleanly, no hang | none |
| Per-part split | voices of different parts (render_split) | each part's output summed from per-worker scratch of that part | none |

## Code Map

- `engine/core/include/velora/core/engine.hpp`, `sample_engine.hpp`, `src/sample_engine.cpp` (`render`, `render_split`, `render_voice`, voice slots) -- where voices render; add the parallel render with per-worker scratch and fixed partition.
- `engine/core/include/velora/core/block_renderer.hpp`, `src/block_renderer.cpp` -- block phases (begin_block, segments, render_split); where the setting lives and phases are sequenced.
- `engine/ports/include/velora/ports/*.hpp`, `engine/ports/CMakeLists.txt` -- add a thread-affinity/pinning port (no-op default, Linux implementation).
- `engine/core/include/velora/core/spsc_queue.hpp`, `rt.hpp` (VELORA_NONBLOCKING) -- conventions for lock-free RT code.
- `engine/core/tests/` (sample engine, rtsan_*_test.cpp, rt-traffic pattern), `engine/core/CMakeLists.txt`, `tools/offline-render` (offline renderer to run the 1 vs 3 worker comparison), `tools/ci/check_core_deps.py`.
- New: `engine/core/include/velora/core/worker_pool.hpp` (+ src) and tests `worker_pool_test.cpp`, `parallel_render_test.cpp`.

## Tasks & Acceptance

**Execution:**
- `worker_pool.*` -- lock-free barrier pool (3 partitions; 1 = inline, 3 = threads), setup-time thread start and pinning hook, clean shutdown
- `sample_engine.*` -- parallel render: deterministic voice-to-partition assignment, per-worker scratch (per part), barrier, fixed-order summation; opt-in; default path untouched
- `block_renderer.*` -- setting plumbed through (serial events → parallel render → barrier → sum)
- ports pinning interface + Linux impl (not exercised on Mac)
- tests -- determinism 1 vs 3 threads with 256 sample voices incl. churn (steals, offs, fades); parallel-vs-serial tolerance; default-off bit-identical to baseline; few/zero voices; shutdown; rt-traffic (no alloc/syscall) test for the parallel block path

**Acceptance Criteria:**
- Given the block loop, then each block runs serial event processing, parallel voice render on 3 workers with per-worker scratch, a barrier, then summation, with workers synchronising lock-free.
- Given the same input rendered with 1 and with 3 workers in the offline renderer, then outputs are bit-identical.
- Dev-board timing (256 voices within 70 % of 1.333 ms over 1 h, histogram) is recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~45 findings — patches 8 groups, deferred 1, rest rejected
- findings:
  - `[high]` `patch` start() not exception-safe; stop vs run hang; unbounded barrier wait (caller now helps via atomic partition claiming); pin failures silent; FTZ/DAZ not propagated; dangling pool pointer after renderer destruction — fixed + tests
  - `[medium]` `patch` shared-state audit of render_partition (no race found, documented) + TSan run in Docker (clean); capacity vs voices_.size(); includes; scratch alignment; traffic-test and CLI checks
  - `[medium]` `defer` board timing and pinning; production enablement
  - `[low]` `reject` idle spin/yield burn on workers (intended on isolated cores), static slot%3 partitioning, scheduling policy

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "parallel|worker|sample|arbiter|rt_traffic|null|offline" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: WorkerPool (3 fixed partitions, lock-free atomics, caller-helps barrier, FP-state propagation, pin port), SampleEngine parallel render with per-partition scratch and fixed-order summation, BlockRenderer opt-in setting, offline renderer --voice-threads.
Verification: macOS targeted + full ctest pass except 4 pre-existing failures; parallel tests TSan-clean in the Linux Docker build; core deps check passes.
Residual risks: see deferred.

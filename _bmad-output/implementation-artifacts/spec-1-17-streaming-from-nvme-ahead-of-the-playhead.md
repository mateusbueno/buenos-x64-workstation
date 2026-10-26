---
title: 'Story 1.17: Streaming from NVMe ahead of the playhead (Mac portable reader)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '2d7450b02d1658fe8d28995f522e74de8081e256'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-1-16-instrument-heads-decoded-into-locked-ram.md', '{project-root}/engine/host/README.md', '{project-root}/engine/core/include/velora/core/sample_engine.hpp']
warnings: []
deferred: ['Dev-board: the 1 h run at 256 voices of streaming with the SSD at least 90% full, the io_uring + O_DIRECT BlockSource (Linux; the port is the seam, TODO(board)), real-time priorities of the I/O threads on cores isolated from the audio cores and their measurement, and replacing the working latency figures (p99.99 2 ms, margin 1 ms) with the measured p99.99', 'Streaming of protected libraries (refused with kStreamProtectedUnsupported; needs per-block decrypt and the damage maps over rings, the EdgeBlockReader of Story 6.5 is the building block)', 'The ring pool as part of the tier profile budgets (it is a stream buffer budget of its own, StreamConfig::budget_bytes, outside head_budget_bytes)', 'Pitch modulation (bend, LFO, pitch envelope) beyond the ratio the heads, loop segments and rings are sized for; voices whose block consumption exceeds the ring starve (counted)', 'Enforcing StreamConfig::budget_bytes from engine-host (the v1 pool is 33,423,360 bytes at 340 rings of 12288 frames)', 'Linux I/O thread priority: the SCHED_FIFO branch of PosixThreadPriority is compiled but never exercised, the audio role is applied by nobody, and no core isolation: "below the audio thread" is enforced on macOS only until the board', 'First-read latency: the first read of a stream is up to 1.5 chunks (a sliver is extended); the 2 ms + 1 ms working figure must be re-checked against that read size by the board qualification (tools/ssd-qualify measures 4 KiB-class reads)', 'fence() timeout policy: after 5 s the instrument leaks its block source, reader and lease (never frees what a stuck read may use); a different policy (kill, quarantine and report to Diagnostics) is a product decision', 'The ThreadSanitizer job covers stream_test, stream_rt_traffic_test and the parallel render tests; it does not cover head_mode_test (Live Set loader in stream mode) or engine-host']
---

<intent-contract>

## Intent

**Problem:** After Story 1.16 a note past its head fades out (placeholder underrun). Long samples must continue seamlessly from the SSD: I/O edge threads (never RT) read the following blocks, decode them and fill a per-voice ring buffer far enough ahead that the voice switches from head to stream without a discontinuity; if data has not arrived the RT thread never waits: the voice outputs silence with a short fade for the missing frames, an underrun counter increments and playback continues (FR1, AR20, AR13 partial, NFR2 partial, NFR17). The Linux `io_uring` + `O_DIRECT` implementation and the 1 h / 256-voice board run are dev-board work; a portable reader prototypes everything on the Mac.

**Approach:** (1) A `BlockSource`/`StreamReader` port in `engine/ports` (read a sample's frames [start, start+n) decoded to planar float into a caller buffer, blocking, I/O thread only) with a portable host implementation (`pread` on the bundle file with the OS no-cache flag where available, decoding the bundle's sample format on the I/O thread, same decoder as the loader so the floats are bit-identical to the fully-resident path); a documented seam for the Linux io_uring implementation (not implemented, TODO(board)). (2) A stream manager in `engine/host`: a pool of I/O edge threads (configurable count, default 2; thread priority below the audio thread through a small thread-priority port, no-op/portable on Mac, SCHED_FIFO-below-audio on Linux documented) serving read requests from a lock-free SPSC/MPSC request queue fed by the RT thread (RT only pushes small request records and reads ring buffers; no syscalls, locks or allocation on RT) and filling per-voice ring buffers (pre-allocated pool sized from the voice capacity: ring frames sized from chunk size and look-ahead margin derived from p99.99 + margin like the head sizing, document the formula) with single-producer (I/O) single-consumer (RT) indices (acquire/release atomics). (3) Engine integration (`SampleEngine`, additive and opt-in with the stream mode flag; default off preserves today's behaviour and head-mode behaviour): a voice that has a tail registers a stream at note start (the request is non-blocking; failure to get a ring = counted underrun for that voice = today's fade), plays from the head, and when the playhead reaches the head end it continues from the ring buffer sample-accurately (interpolation reads straddling the head/stream boundary must be exact: the ring holds frames starting at head_end - guard so Hermite/linear interpolation windows are identical to a fully-resident render); looping samples with loop points beyond the head restream from the loop start (design: the stream is a position-addressed ring; loop wrap schedules a re-seek, with the loop-start head region kept resident so the wrap is seamless: size and document it); the voice ends/releases the stream when it finishes (ring returned to the pool; leak-free). (4) Underrun: when the ring does not yet hold the needed frames the RT voice outputs silence with a short fade (reuse the 1.16 fast fade) for the missing frames, increments the underrun counter (`Diagnostics.record_stream_underrun` path from 1.16/1.21) and keeps running (it may recover if data arrives: define: fade out and end the voice on the first underrun like 1.16, OR continue with silence-fill and resume when data arrives; choose the one that keeps bit-exactness when data is on time and document). (5) Tests: bit-for-bit equality of a long sustained note rendered via streaming (rings filled by real I/O threads reading a temp bundle with the portable reader, with deterministic scheduling in tests: a synchronous "inline" reader mode for the bit-identity test and a threaded test with real delays) vs the fully-resident render from Story 1.15 for several notes incl. velocity layers/round robin, loops, pitched ratios up to the sized maximum and a sample rate ratio; underrun test with an artificially slow/blocked reader (a fake reader that stalls) proving the RT render never waits (render time bounded, no blocking), silence + fade for missing frames, counter increments, and playback continues; stress: many voices (≥256 on Mac with small samples) streaming concurrently, zero underruns with a fast reader; lifecycle: no ring leaks after voices end, generation release and cancel; RT-safety test (allocation counter or rt-traffic pattern) for the audio-thread path in stream mode; ThreadSanitizer run in Docker (Linux build, like Story 1.18) on the stream tests.

## Boundaries & Constraints

**Always:** `engine/core` stays OS-free: core sees only ring-buffer pointers/indices and a request-push function pointer/port; threads, files and priorities live in `engine/host` and `engine/ports`. Opt-in via config (`stream_mode`), default off; head mode must be on for stream mode. Output when data is on time is bit-identical to the resident render (this is the acceptance check). RT thread never blocks: only atomics and pre-allocated memory. Ring/stream pool is bounded and pre-allocated at load (counted against the head budget / a separate stream-buffer budget documented in the tier profile terms: use the existing head budget accounting or add a clearly named stream budget). All reader errors (EIO, short read, file gone) become counted underruns and a typed host-side error log, never a crash. Stream request ordering and cancellation: a voice that ends before its data arrives cancels (generation/epoch counter so late fills never write into a recycled ring: ABA-safe). Works with the parallel voice render of Story 1.18 (voices on worker threads push requests: the request queue must be MPSC-safe or per-worker SPSC). Thread priority: I/O threads run below the audio thread (document and implement via the port where possible).

**Never:** No io_uring implementation (document the seam), no board 1 h run, no changes to protected-bundle semantics (refuse stream mode for protected bundles like head mode, typed status), no changes to the Diagnostics wire format, no sample-rate conversion changes, no heads-off streaming (stream requires heads).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Long note, data on time | sustained note past the head | output bit-identical to fully-resident render | none |
| Head→stream boundary | interpolation window straddles the boundary | identical samples | none |
| Loop beyond head | looping sample, loop points past the head | seamless wrap, identical to resident render | none |
| Reader stalls | fake reader blocks | RT render never waits; silence + fade for missing frames; underrun counted; playback continues | none |
| Reader error | EIO/short read/file removed | counted underrun, host error log | no crash |
| Voice ends early | note-off/steal before data arrives | stream cancelled; late fill discarded (epoch); ring recycled | none |
| 256 voices | many small streamed samples | zero underruns with a fast reader | none |
| Pool exhausted | no free ring | voice fades with a counted underrun (no allocation on RT) | none |
| Stream mode off | default | identical to Story 1.16/1.15 behaviour | none |
| Parallel render | voices on worker threads | requests safe, output identical to single thread | none |

## Code Map

- `engine/core/include/velora/core/sample_engine.hpp`, `src/sample_engine.cpp` (voice state, head end handling from 1.16: `idx >= end_idx`, `has_tail`, `stream_underrun_events`), `sample_bank.hpp`, `engine.hpp`, `worker_pool.hpp` (Story 1.18), `diagnostics.hpp`.
- `engine/ports/include/velora/ports/sample_source.hpp` (SamplePcm total_frames/has_tail), new ports for the stream reader and thread priority; `engine/host/include/velora/host/sample_bundle_loader.hpp`, `src/sample_bundle_loader.cpp` (decoder, head mode), `head_memory.*`, `live_set_loader.*`/`live_set_switcher.*` (lifetime/leases/generations, head budget accounting), `formatlib/include/velora/format/bundle.hpp` (sample block layout and decode).
- `engine/host/macos/main.cpp` (flags: --stream-mode, --io-threads), `engine/host/CMakeLists.txt`, `tools/ci/docker-check.sh` (TSan pattern from Story 1.18), `engine/host/README.md`.
- Tests: `engine/host/tests/head_mode_test.cpp` (bundle fixtures, resident-vs-head renders), `engine/core/tests/sample_engine_test.cpp`, `parallel_render_test.cpp`, `rtsan_*` patterns.

## Tasks & Acceptance

**Execution:**
- ports (stream reader, thread priority) + portable pread reader with the shared decoder + io_uring seam doc
- stream manager (I/O threads, request queue, ring pool, epoch/ABA safety, sizing formula, priority)
- engine integration (opt-in): stream registration at note start, head→stream continuation with exact interpolation windows, loop restream with resident loop-start region, underrun path, cleanup
- tests per matrix row incl. bit-identity vs resident, stall test, 256-voice test, lifecycle/leak, parallel render, RT-safety, TSan in Docker
- docs and deferred list

**Acceptance Criteria:**
- Given a voice playing from its head, when the playhead advances, then I/O edge threads (not RT) read the following blocks, decode them and fill a per-voice ring far enough ahead that the voice switches from head to stream without a discontinuity, and the render of a long sustained note matches the fully-in-RAM render bit-for-bit.
- Given the RT thread needs stream data that has not arrived, then it does not wait; the voice outputs silence with a short fade for the missing frames, an underrun counter increments and playback continues.
- The 1 h, 256-voice, SSD-≥90%-full board run, io_uring/O_DIRECT and RT priorities on isolated cores are recorded as deferred (portable prototype only).

## Spec Change Log

## Review Triage Log

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "stream|head|sample|loader|live_set|switch|offline|rt_traffic|parallel|null" --output-on-failure` -- expected: all pass
- TSan: build and run the stream tests in the Linux Docker image (as in Story 1.18) -- expected: no warnings
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: ports `BlockSource` / `StreamService` / `StreamRing` / `StreamFence` and `ThreadPriority` (`engine/ports`), the portable bundle block source (same decoder as the resident loader, macOS `F_NOCACHE`), `StreamManager` (locked ring pool sized by a documented formula, MPMC request queue, I/O edge threads with the priority port, epoch + `io_busy` ABA handshake, `fence`, typed error log, inline `pump()` mode), stream voices in `SampleEngine` (head -> stream sample exact, loop re-seek with resident loop segments, starve-and-recover underrun policy, ring release at every voice end), `--stream-mode` / `--io-threads` in engine-host, docs.
Review fixes (second pass): the producer no longer maps rings to threads (every worker scans all rings under a per-ring try-lock, so a stalled read freezes one ring, and announcements can never be dropped); a worker stops after the chunk in progress when a new stream is announced; the first read of a stream is never a sliver; retry back-off in time; fence() has a timeout and the instrument then leaks instead of freeing; the ring lookahead has a block term and engine-host passes sample rate, pitch ratio and block size; no 64-bit modulo per tap; a short starve ends with a 64 frame crossfade (no step); `kStreamNeedsReader` replaces a dangling reader reference and the protected provider returns `kStreamProtectedUnsupported`; timing-dependent tests report their attempts and need a clean run within two (outside sanitizer builds) and a correct prefix when starved; new tests for each.

Unrelated Linux build fixes made so the Docker jobs build (keep in the commit, separately explainable): `#include <cstring>` in `engine/host/src/library_resolver.cpp`, `#include <algorithm>` in `formatlib/src/factory_fm_va.cpp`, `Threads::Threads` linked to `velora_host`, and `git` in `tools/ci/docker/Dockerfile` (the configure step fetches LVGL; the image is rebuilt on the next docker-check run and the jobs need network at configure time).

Verification: `stream_test` (bit identity with the resident render over 11 scenarios, Hermite and linear, inline and threaded with delays; stall, errors, truncated file, ABA, pool exhaustion, leaks, cancel, 256 voices, parallel render), `stream_rt_traffic_test` (no allocation on the audio thread), `head_mode_test` (Live Set loader), engine-host flag smoke tests, the full targeted ctest; ThreadSanitizer (clang-20) and RealtimeSanitizer in Docker (`tools/ci/docker-check.sh tsan`, `rtsan_stream_test`); core deps lint.

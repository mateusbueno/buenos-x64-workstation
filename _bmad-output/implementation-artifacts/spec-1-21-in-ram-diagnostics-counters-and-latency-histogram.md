---
title: 'Story 1.21: In-RAM diagnostics counters and latency histogram'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '7e2b866ed59df52722a935017886fa23e43f42e3'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md']
warnings: []
deferred:
  - summary: >-
      steady_clock::now() on the render path is not proven RTSan-clean; rtsan variants did not run on macOS.
    evidence: |-
      VELORA_HAVE_RTSAN is empty on this Mac; run rtsan_host_render_test on Linux/CI.
    severity: medium (unverified)
  - summary: >-
      Device xruns (CoreAudio overload / ALSA) and stream underruns are not fed into the counters yet.
    evidence: |-
      RenderDriver::record_xrun and record_stream_underrun exist; feeders belong to the device adapters and Story 1.17.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** The engine keeps no xrun or stream-underrun counter and no latency histograms; steals exist only inside the voice arbiter. Soak runs, the demo and field units cannot prove or diagnose audio health (FR120, AR44, NFR15 partial).

**Approach:** Add lock-free in-RAM diagnostics (counters + two fixed-bin histograms) written on RT/host render paths with relaxed atomics and no syscalls, publish them through a new `ws.v1` `GetDiagnostics` request answered by the control thread, and add a dev CLI `velora-diag` that queries the engine's Unix socket.

## Boundaries & Constraints

**Always:** RT updates are atomic ops only (no syscall, lock, allocation; passes rtsan/rt-traffic test). `engine/core` must not include protobuf (tools/ci/check_core_deps.py). The control thread serializes the reply. Proto grows by addition only: `ClientMessage.get_diagnostics = 53`, `ServerMessage.diagnostics_reply = 23`. Steal and exhausted counts come from the existing `VoiceArbiter::steals()/exhausted()`.

**Never:** No disk writes by the engine for diagnostics. No Velora Studio export (Epic 9). No dev-board/ALSA/real USB timestamp work (Story 1.20), no changes to existing message numbers.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Counters | xrun / steal / stream underrun recorded | matching counter +1, others unchanged | none |
| Render histogram | block render durations incl. one over deadline | each in correct bin; over-deadline also counts an xrun | none |
| Latency histogram | stamped event rendered with known delay | recorded in the bin of (onset time − event host_ns) | late/dropped events not recorded |
| Bin overflow | value above last bin edge | goes into last (overflow) bin | none |
| Diagnostics request | after Hello, client sends GetDiagnostics | reply holds all counters, both histograms (bin edges + counts), uptime_ms | before Hello → existing HANDSHAKE_REQUIRED |
| CLI | `velora-diag --socket <path>` | prints counters, histograms, uptime; exit 0 | unreachable socket → message on stderr, exit 1 |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/snapshot.hpp` -- `LastValueTable` holds RT→control atomics (e.g. content_integrity at ~165); new diagnostics struct (counters + histogram bins, `VELORA_NONBLOCKING` record methods) goes alongside, pure std types.
- `engine/core/src/voice_arbiter.cpp:444,347-422` / `voice_arbiter.hpp:205-210` -- existing `steals()`, `exhausted()` relaxed atomics to publish.
- `engine/core/src/block_renderer.cpp:635-888` -- block loop; ends by storing content_integrity (884-887); publish steals there if desired.
- `engine/host/src/render_driver.cpp:5-57` + `include/velora/host/render_driver.hpp:13-21` -- `render_block*` time the render (xrun if > frames/sample_rate); `drain` (~line 50, after `renderer_.submit`) records event-to-onset latency; `RenderCounters` lives here.
- `engine/host/macos/coreaudio_output.cpp:164-185` -- callback; optional overload hook for xrun. Stream underrun: no core streaming yet, expose counter (fed by line-in underruns/future streaming) that stays 0 on Mac.
- `proto/ws_v1.proto:529-600` -- add `GetDiagnostics`, `DiagnosticsReply` (xrun, voice_steal, voice_exhausted, stream_underrun, uptime_ms, render_time bins/edges, latency bins/edges).
- `engine/host/src/control_plane.cpp:487+,697` + `include/velora/host/control_plane.hpp` -- add `case kGetDiagnostics` + `fill_diagnostics` (pattern of `kGetPerformanceValues`); uptime from `clock_()`.
- `engine/host/CMakeLists.txt:160-164,275-277,305-317` -- test registration templates; `engine/host/tests/control_test.cpp` (Rig, Client, CHECK) and `rtsan_control_test.cpp` templates.
- `engine/host/macos/CMakeLists.txt` -- smoke-test pattern; `tools/` pattern for a new `velora-diag` target linking `velora_control` (UDS client copied from control_test.cpp `Client`).
- `proto/README.md` -- document the new messages.

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/diagnostics.hpp` (+ wiring in `snapshot.hpp`/`block_renderer`) -- atomic counters and log-spaced histogram bins -- RT-safe in-RAM storage
- `engine/host/src/render_driver.cpp` -- time block renders, count xruns, record event-to-onset latency -- fills both histograms
- `proto/ws_v1.proto`, `engine/host/src/control_plane.cpp`, `control_plane.hpp` -- GetDiagnostics/DiagnosticsReply -- control-thread serialization
- `tools/diag/` (CMake + `velora_diag.cpp`), root `CMakeLists.txt` -- `velora-diag` CLI
- tests: core unit test for counters/bins/overflow; host control test for the request (incl. before-Hello); rt-traffic/rtsan test for no-syscall updates; smoke test running `velora-diag` against `engine-host --no-audio`; update `proto/README.md`

**Acceptance Criteria:**
- Given the running engine, when an xrun, steal or stream underrun is recorded, then the matching counter increments atomically with no syscall and both histograms update in RAM.
- Given a ws.v1 `GetDiagnostics` request over the UDS, when `velora-diag` sends it, then it receives all counters, both histograms and uptime, serialized by the control thread.
- Given an engine run, then nothing is written to disk by the diagnostics path (counters and records stay in RAM).

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~30 findings — patches 7, deferred 2, rest rejected as low/cosmetic or by-design
- findings:
  - `[medium]` `patch` steal/exhausted wiring untested — added test_diagnostics_voice_totals
  - `[medium]` `patch` xrun/latency in RenderDriver untested, tautological xrun assert — added diag_render_driver_test, tightened rtsan_host_render_test
  - `[low]` `patch` host_ns==0 latency pollution — guarded
  - `[low]` `patch` CLI SIGPIPE/EINTR — MSG_NOSIGNAL, SO_NOSIGPIPE, poll retry
  - `[low]` `patch` empty Clock in ControlPlane ctor — guarded
  - `[false]` `reject` stereo process_block skips voice totals — it delegates to the bus overload
  - `[medium]` `defer` steady_clock under RTSan; device xrun/stream underrun feeders
  - `[low]` `reject` late events excluded from latency (documented design), histogram snapshot non-atomic, CLI options (--json/watch), feature string, smoke-test hygiene, bin-edge hard-coding

## Verification

**Commands:**
- `cmake --build build -j && ctest --test-dir build -R "diag|control|content_integrity|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0
- `ctest --test-dir build --output-on-failure -j8` -- expected: no regressions (if full suite is slow, at least engine/core and engine/host)

## Auto Run Result

Summary: in-RAM diagnostics (counters + render-time and event-to-onset histograms), ws.v1 GetDiagnostics/DiagnosticsReply, `velora-diag` CLI.
Files: engine/core diagnostics.hpp, snapshot/block_renderer wiring; engine/host render_driver, control_plane; proto/ws_v1.proto + README; tools/diag; tests (diagnostics, control, diag_render_driver, voice_arbiter, rt_traffic, smoke).
Verification: 17/17 targeted ctest pass; core deps check passes. Pre-existing failures at baseline: tokens_verify, protected_keyless_test, sysex_receiver_test, library_resolver_test.
Residual risks: RTSan not run on macOS; xrun on device and stream underrun feeders pending.

---
title: 'Story 1.10: Engine plays live on the developer''s Mac (macOS host adapters)'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '175d3ddde20ca4ec5d6f2c98191d1d53b6fff335'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The engine core can only render offline. The developer has no way to hear it or play it from a USB MIDI keyboard on the Mac before the board arrives.

**Approach:** Under `engine/host/` add a macOS-only `engine-host` dev app with a CoreAudio output adapter (48 kHz, 64-frame buffer) driving `BlockRenderer` on the audio thread, and a CoreMIDI input adapter that converts incoming MIDI (UMP) to timestamped `TimedUmp` events with in-block frame offsets and hands them to the audio thread through the existing SPSC queue. Platform-neutral port interfaces go in `engine/ports/`; everything Apple-specific stays in `engine/host/macos/`.

## Boundaries & Constraints

**Always:** Apple code compiles only on APPLE (CMake guard; Linux/gcc builds and CI must stay green and unchanged); `engine/core` is not modified except for a minimal hook strictly required (report it); the audio callback does no allocation, locking or blocking (use the SPSC queue, pre-allocated buffers; mark with `VELORA_NONBLOCKING`); MIDI events are stamped with host time on arrival in the CoreMIDI callback (mach host time -> ns), converted on the audio thread to an in-block frame offset using a simple fixed-scheduling-delay rule (document it; same rule shape as the live engine: device time -> frame with a fixed delay), never reordered; timing figures printed by the app are labelled "host, not representative"; platform-independent pieces (the host-time-to-frame mapping, ring of timestamps) are pure C++ with unit tests that run on all platforms.

**Never:** Add DSP, ALSA, Linux code, UI, or a GUI; require audio hardware or a MIDI keyboard for ctest to pass (device-dependent checks are manual and must be skipped gracefully if no device); use non-Apple third-party libraries; claim latency/xrun budgets (those are accepted only on the dev board).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Start with default output | run `engine-host` on a Mac with an output device | opens CoreAudio at 48 kHz, 64-frame buffer (or reports the closest the device grants), plays test tone on incoming notes | clear message and non-zero exit if no device or format cannot be set |
| MIDI note arrives | virtual or hardware MIDI source sends note-on | converted to UMP note-on with host timestamp, applied at the mapped in-block offset | none |
| MIDI 1.0 and UMP sources | CoreMIDI delivers either | both normalised to UMP (MIDI 2.0 channel voice) via the UMP-protocol input port | non-note messages ignored (counted) |
| Late event | timestamp earlier than the current block start | applied at frame 0 of the next block and counted as late | counter |
| Far-future event | timestamp beyond the scheduling horizon | clamped or deferred per documented rule, never crashes | counter |
| Queue full | audio thread behind | event dropped and counted; callback never blocks | counter |
| No MIDI source | none connected | app runs; connects to sources that appear later | message |
| Quit | SIGINT or `q` | stops audio, disposes MIDI client, exits 0 | none |

</intent-contract>

## Code Map

- `engine/ports/include/velora/ports/` -- add platform-neutral interfaces (e.g. `audio_output.hpp`, `midi_input.hpp`) only as needed
- `engine/host/CMakeLists.txt`, `include/velora/host/`, `src/` -- existing host_info placeholder lib `velora_host`; extend with portable code (time mapping) and add the macOS subdirectory under `engine/host/macos/` guarded by `if(APPLE)`
- `engine/core/include/velora/core/block_renderer.hpp` -- `submit(TimedUmp)` (SPSC, producer side must be a single thread), `process_block(l, r)`, `kBlockFrames`, `kSampleRate`, `last_status`, counters; read-only unless a hook is essential
- `engine/core/include/velora/core/ump.hpp` -- `make_note_on/off`, UMP word layout
- `engine/core/CMakeLists.txt` -- RTSan test wiring pattern to reuse for a host RTSan test
- `tools/ci/check_core_deps.py` -- must keep passing; `.github/workflows/ci.yml` -- macOS job runs ctest; no audio device there so device tests must not be in ctest
- `_bmad-output/planning-artifacts/epics.md` Story 1.10 (~line 1038) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `engine/host/src/time_mapper.{hpp,cpp}` (portable) -- maps device-time ns of an event to a frame offset relative to the block start given the block's start time ns and a fixed scheduling delay; classifies late/in-block/future -- shared rule, unit-testable
- `engine/host/macos/coreaudio_output.{hpp,mm|cpp}` -- CoreAudio (AudioUnit/HAL) output at 48 kHz stereo float with 64-frame buffer request; render callback drains the queue through BlockRenderer with the mapper; reports achieved buffer size -- audio adapter
- `engine/host/macos/coremidi_input.{hpp,cpp}` -- MIDIClient, UMP input port (kMIDIProtocol_2_0, falls back to 1.0 and upconverts), connects all sources and new ones, stamps host time, pushes to a lock-free queue -- MIDI adapter
- `engine/host/macos/main.cpp` -- `engine-host` app: wires adapters, prints "host, not representative" timing info, handles quit -- dev app
- `engine/host/tests/` -- unit tests for time_mapper (all matrix rows that are portable), and a macOS-only test using a CoreMIDI virtual source plus an offline (non-device) driver of the same render path that proves a note-on sent through CoreMIDI arrives as a UMP event at the expected mapped frame; device-free so it can run in ctest (skip gracefully with a clear message if CoreMIDI is unavailable) -- coverage
- `engine/host/CMakeLists.txt` -- APPLE-guarded targets linking CoreAudio/CoreMIDI/AudioToolbox/CoreFoundation frameworks; an RTSan-enabled variant of the audio-path test where the compiler supports it -- wiring
- `engine/host/README.md` -- how to run `engine-host`, rules, labelling of timing figures -- docs

**Acceptance Criteria:**
- Given a Mac, when `engine-host` starts, then it opens CoreAudio at 48 kHz with a 64-frame buffer request and plays the test tone for notes
- Given a CoreMIDI note-on, when received, then it reaches the engine as timestamped UMP at the mapped in-block offset (verified device-free with a virtual MIDI source)
- Given Linux/gcc builds, then nothing Apple-specific is compiled and all existing tests still pass
- Given timing output, then it is labelled "host, not representative"

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test` -- expected: pass
- Manual: run `build/engine/host/macos/engine-host` for a couple of seconds headless if an output device exists (a short `--duration 2` flag is allowed) and report what it printed; do not claim audible verification

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~45 findings — patched 9 groups, rest deferred
- findings:
  - `[high]` `patch` far-future events clamped to the horizon could reorder a note-off before its note-on — events beyond the horizon are now held in order and never clamped
  - `[medium]` `patch` CoreAudio slicing/carry/block-start logic untested — extracted to portable `block_slicer.hpp` with tests for callback sizes 64/100/512/1
  - `[medium]` `patch` deferred (multi-block) events and the note-on-only audible case unverified; RTSan host test did not mix offsets — tests added with exact absolute frame assertions
  - `[medium]` `patch` CoreMIDI test turned real failures into skips and leaked objects; per-packet timestamps ignored; stale sources — fixed
  - `[low]` `patch` TimeMapper overflow/zero config/duplicated constants; CoreAudio unchecked property writes, null buffers, stale carry, repeated open; MIDI 2.0 velocity-0 policy; stdin EOF busy loop and lax --duration; BlockSink not nonblocking — fixed
  - `[low]` `defer` output latency/safety offset not added to block start time (host figures labelled not representative); default device rate/buffer not restored on exit; device-change/unplug listeners; dropped note-off recovery (panic path); cross-thread SPSC/TSan test; CI assertion that the CoreMIDI test ran rather than skipped
  - `[unverified]` `defer` RTSan on the host render path and the 10-minute live RTSan play session (AC) run only in CI/manually; Linux gcc build of portable host code first checked in CI

## Auto Run Result

Status: done. Added port interfaces (`audio_output.hpp`, `midi_input.hpp`, `nonblocking.hpp`), portable host code (TimeMapper fixed 3 ms delay rule, MidiIngest MIDI 1.0/2.0 → UMP, RenderDriver, BlockSlicer), and macOS-only CoreAudio/CoreMIDI adapters plus the `engine-host` dev app (`--duration`, labelled "host, not representative"). `engine/core` untouched. Verification: macOS build and ctest pass, `engine-host --duration 2` ran (device granted 48 kHz/64 frames, 0 notes), purity check passes. Not verified: audible playback, 10-minute RTSan live session, Linux build (CI). Follow-up review recommended: false.

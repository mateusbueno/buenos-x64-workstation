---
title: 'Story 7.2: Event-processor framework and arpeggiator note generation'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: '15e23278682add457b2692a1d2ca111bbc3d92c0'
review_loop_iteration: 0
followup_review_recommended: true
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/engine/core/README.md'
  - '{project-root}/tools/offline-render/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Story 7.2 in `_bmad-output/planning-artifacts/epics.md` (`### Story 7.2`, ~line 4633): parts cannot transform incoming note events into rhythmic arpeggiated patterns locked to the Story 7.1 `TransportClock`.

**Approach:** add a per-part `EventProcessor` framework to `engine/core` (serial phase of `BlockRenderer`, 8 part slots, zero allocation/locks, designed so a later sequencer uses the same slot type) and an `Arpeggiator` processor implementing `EventProcessor` (Arp On/Off, Mode Up / Down / Up-Down / Random / As played, Octaves 1-4, Rate 1/4, 1/8, 1/16, 1/32 and their triplets, Gate % with minimum 1 frame, seeded per part for Random). Wire arpeggiator configuration in `PartCfg` / `Snapshot` and add offline-render CLI options (`--arp`), committed MIDI test files, exact emitted note-frame verification across modes/rates/octaves at two tempos with 2 active parts, and a golden WAV null-test in CTest.

## Boundaries & Constraints

**Always:**
- `EventProcessor` interface in `engine/core` (`event_processor.hpp`): per-part processor slot in the serial phase of `BlockRenderer` (`parts 0..7`), consumes the part's UMP input (`NoteOn`/`NoteOff` with in-block frame offset `0..kBlockFrames-1`) and emits `TimedUmp` events with exact in-block frame positions `0..kBlockFrames-1` in frame-sorted order before voice allocation (`apply_part_note`), without dynamic allocation or locks (`VELORA_NONBLOCKING`). Designed cleanly so a future sequencer processor uses the same slot interface (`reset`, `process_input`, `process_block` / step emission given `const TransportClock&`, etc.).
- `Arpeggiator` (`arpeggiator.{hpp,cpp}`):
  - Modes: `Up`, `Down`, `UpDown` (end notes not repeated when >1 note in the expanded octave pool; single note repeats every step; 2 notes alternate low, high, low, high), `Random` (deterministic PRNG seeded per part `part_index`, e.g. splitmix64/xorshift, reset on `reset()` so offline renders and tests are 100% reproducible), `AsPlayed` (in key-press order across octaves: base notes in press order in octave 0, then octave 1, etc., or each octave shifting the press-ordered list by `+12*oct` clamped/filtered to valid MIDI notes `0..127` — specifically `note + 12 * oct <= 127`).
  - Octaves: `1..4`.
  - Rates (at 960 PPQN): `1/4` (960 ticks), `1/8` (480 ticks), `1/16` (240 ticks), `1/32` (120 ticks), `1/4T` (640 ticks), `1/8T` (320 ticks), `1/16T` (160 ticks), `1/32T` (80 ticks).
  - Gate %: percentage of step duration in frames (`1..200%`, default `50%`; note length = `max(1, llround(step_frames * (gate_percent / 100.0)))`). If gate >= step or wraps across steps on the same pitch, a clean Note Off is emitted before re-striking the same pitch so voices and the arbiter stay balanced.
  - Grid alignment: each generated Note On lands on the exact frame of its grid tick (`tick` a multiple of `step_ticks`, using `TransportClock::frame_of_tick(tick)`). When the first note is pressed while no notes were held, the arpeggiator triggers on the grid (if the key-down frame is on a grid tick, at that tick; otherwise on the grid ticks from the first key-down or next grid tick — document and keep deterministic; specifically, anchor or quantize to the `TransportClock` grid `tick = ceil(ticks_at(frame) / step_ticks) * step_ticks`, firing immediately at `frame` if `frame_of_tick` of the current grid tick equals `frame`).
  - Arp Off: notes pass through unchanged at their exact input frames. Switching Arp Off while notes are held immediately ends any currently sounding arpeggiated note(s) (emits Note Off) and sounds the currently held input notes normally (emits Note On for held keys), and releasing those held keys later emits their Note Off normally. Switching Arp On while keys are held ends the pass-through sounding notes and arpeggiates the held keys.
- In `BlockRenderer`: works both in plain channel==part mode (channels `0..7`) and in Live Set mode (after `PartRouter` zone/transpose fanout per part `0..7`). All existing tests and goldens remain bit-identical when Arp is Off (the default).
- In `tools/offline-render`: add `--arp PART:MODE:RATE:OCTAVES:GATE` (repeatable per part `0..7`), commit a MIDI fixture in `tools/offline-render/tests/arp/`, test all modes, rates, and octaves at two tempos plus a 2-part simultaneous render verifying exact emitted note-on/note-off frame lists, and wire a committed golden WAV null-test in `tools/offline-render/CMakeLists.txt`.

**Never:**
- Do not add `proto/params.json` registry IDs or `live_set.proto` serialization yet (that is Story 7.4).
- Do not implement Swing, Latch, or sustain-pedal holding of arpeggiated patterns yet (that is Story 7.3 — though keep `ArpConfig` extensible for `swing` and `latch`).
- Do not allocate or lock on the RT thread.

</intent-contract>

## Code Map

- `engine/core/include/velora/core/event_processor.hpp` -- new: `EventProcessor` interface and fixed-capacity event buffer for per-part serial-phase UMP processing
- `engine/core/include/velora/core/arpeggiator.hpp`, `engine/core/src/arpeggiator.cpp` -- new: `ArpMode`, `ArpRate`, `ArpConfig`, and `Arpeggiator` processor
- `engine/core/include/velora/core/snapshot.hpp` -- `PartCfg` gains `ArpConfig arp{}` (default `enabled = false`)
- `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` -- per-part processor slots (`std::array<Arpeggiator, params::kPartCount>` and optional custom `EventProcessor*` slot per part), segment rendering sliced at both input event frames and processor-emitted event frames, plus an optional test/offline observation hook or trace buffer for emitted part note frames
- `engine/core/tests/arpeggiator_test.cpp`, `engine/core/CMakeLists.txt` -- unit and renderer tests for `EventProcessor` slot, all modes/rates/octaves/gate, Arp On<->Off transitions while holding notes, and RTSan target
- `tools/offline-render/src/render.hpp`, `tools/offline-render/src/render.cpp`, `tools/offline-render/src/render_main.cpp` -- `--arp` CLI option and `RoutingOptions::arp`
- `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/tests/arp/`, `tools/offline-render/CMakeLists.txt` -- committed MIDI test file(s), exact frame-list test across modes/rates/octaves at two tempos with 2 active parts, and golden WAV null-test
- `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` -- documentation and status update

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/event_processor.hpp` -- define `EventProcessor` slot interface and bounded `EventBuffer` -- reusable serial-phase processor abstraction (AR17)
- `engine/core/include/velora/core/arpeggiator.hpp` and `engine/core/src/arpeggiator.cpp` -- implement `Arpeggiator` (Up, Down, Up-Down without repeating end notes, per-part seeded Random, As played; Octaves 1-4; Rates 1/4..1/32 + triplets; Gate % min 1 frame; Arp On/Off live transition handling) -- core arpeggiator logic locked to `TransportClock`
- `engine/core/include/velora/core/snapshot.hpp`, `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` -- integrate per-part event processor slots and `PartCfg::arp` into `BlockRenderer` serial phase before voice allocation -- sample-accurate note emission in both plain and Live Set modes
- `engine/core/tests/arpeggiator_test.cpp`, `engine/core/tests/rtsan_arpeggiator_test.cpp`, `engine/core/CMakeLists.txt` -- test custom `EventProcessor` slot, all modes/rates/octaves/gate, Arp Off pass-through and mid-chord On<->Off switching, and RTSan coverage -- verify core ACs
- `tools/offline-render/src/render.{hpp,cpp}`, `tools/offline-render/src/render_main.cpp`, `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/tests/arp/`, `tools/offline-render/CMakeLists.txt` -- add `--arp` CLI flag, committed MIDI fixture, offline frame-list verification at 2 tempos on 2 parts across modes/rates/octaves, and committed golden WAV null-test (with positive and negative null-test checks) -- verify offline renderer ACs
- `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` -- document event-processor framework and `--arp`, mark `7-2` done

**Acceptance Criteria:**
- Given the serial phase of the block loop, when the event-processor framework is added, then each part has an optional processor slot that consumes the part's UMP input and emits UMP with exact frame positions before voice allocation, runs without allocation or locks, and is designed so a later sequencer processor uses the same slot type.
- Given an arpeggiator on a part with Arp On and notes held, when blocks render, then notes are generated in Mode Up, Down, Up-Down (end notes not repeated), Random (seeded per part for reproducible tests) or As played, across Octaves 1-4, at Rate 1/4, 1/8, 1/16, 1/32 or their triplets, with note length = Gate % of the step (minimum one frame), and each generated Note On lands on the exact frame of its grid position from the Story 7.1 clock.
- Given Arp Off, when notes are played, then they pass through unchanged, and switching Arp Off while notes are held ends arpeggiated notes and sounds the held notes normally.
- Given the offline renderer, when committed MIDI test files render each mode, rate and octave setting at two tempos, with the arpeggiator active on 2 parts, then the emitted note frames match expected frame lists exactly, and a golden render null-tests in CI.

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: pass

## Auto Run Result

- **Summary:** Implemented the per-part `EventProcessor` framework (`event_processor.hpp`) in the serial phase of `BlockRenderer` and the `Arpeggiator` note generator (`arpeggiator.{hpp,cpp}`) supporting Modes `Up`, `Down`, `UpDown` (end notes not repeated), `Random` (deterministic per-part seed), and `AsPlayed`, across Octaves `1..4`, Rates `1/4..1/32` and triplets, and Gate `1..200%` (min 1 frame), locked to `TransportClock`, plus `--arp` in `velora-offline-render`, committed MIDI/golden fixtures, and unit/RTSan/offline tests.
- **Files changed:**
  - `engine/core/include/velora/core/event_processor.hpp` — new `EventBuffer` and `EventProcessor` interface
  - `engine/core/include/velora/core/arpeggiator.hpp`, `engine/core/src/arpeggiator.cpp` — new `ArpMode`, `ArpRate`, `ArpConfig`, and `Arpeggiator` implementation
  - `engine/core/include/velora/core/snapshot.hpp` — added `ArpConfig arp` to `PartCfg`
  - `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` — integrated per-part processor slots, FIFO event slicing, note observer hook, and Live Set teardown
  - `engine/core/tests/arpeggiator_test.cpp`, `engine/core/tests/rtsan_arpeggiator_test.cpp`, `engine/core/CMakeLists.txt` — unit, renderer, and RTSan traffic tests
  - `tools/offline-render/src/render.{hpp,cpp}`, `tools/offline-render/src/render_main.cpp` — `--arp` CLI option and emitted-note observation
  - `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/tests/arp/{make_midi.py,arp.mid,arp.case,arp.wav}`, `tools/offline-render/CMakeLists.txt` — offline frame-list tests and golden WAV null-test
  - `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` — documentation and sprint status update
- **Review findings breakdown:** 11 patches applied (7 medium, 4 low), 0 deferred, remaining findings rejected as out-of-scope (Stories 7.3/7.4) or cosmetic.
- **Follow-up review recommendation:** `true` (patched counts by verdict: `high: 0, medium: 7, low: 4`; specific risk to verify if re-reviewed: interaction of FIFO `pending_` sequence ordering with multi-part `EventProcessor` emissions at identical in-block frames).
- **Verification:** `cmake --build build -j8`, `ctest --test-dir build --output-on-failure` (570/570 passed, 2 env-gated demo tests skipped), `python3 tools/ci/check_core_deps.py` passed.
- **Residual risks:** Sustain pedal holding of arpeggiator patterns and seamless scene/Live Set switching of running arpeggiators are implemented in Story 7.3.

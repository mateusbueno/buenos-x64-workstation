---
title: 'Story 3.5: Keybed reader on the SoC with drift-tracked device time and contact_k events'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '033fe451ddaa1f890d07057ded19b8096c5f0622'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/mcu/common/MCU-ID-MAP.md', '{project-root}/engine/core/ORGAN.md']
warnings: []
deferred:
  - summary: >-
      No real input thread, serial link, CLOCK_MONOTONIC_RAW call, RT-plane consumer or periodic key-state scheduler: the reader is a core with injected time (dev/prototype board work).
    evidence: |-
      report_key_state/release_all exist as APIs; nothing schedules them. report_key_state recovers only lost opens, not lost closes.
    severity: medium
  - summary: >-
      Velocity is a provisional linear mapper (Story 3.6 replaces it); calibration table has no JSON loader yet.
    evidence: |-
      VelocityMapper interface and KeybedCalibrationSpec struct are in place.
    severity: low
---

<intent-contract>

## Intent

**Problem:** Real key contacts from the keybed MCU cannot yet reach the engine: nothing converts MCU ticks to device time, tracks MCU clock drift, or turns per-contact frames into `contact_k` controllers and Note On/Off UMP (FR5, FR11, FR73, AR16).

**Approach:** Add a keybed reader core in `engine/host` (input-edge-thread logic, no hardware I/O): decode MCU frames, estimate tick→CLOCK_MONOTONIC_RAW-ns offset and drift from periodic TIME_SYNC round trips, apply a data-driven keybed calibration table (2 or 3 contacts, velocity-measuring contact pair, key index→note mapping), and push stamped UMP only into an SPSC queue. Testable on the Mac with recorded frames and a simulated MCU.

## Boundaries & Constraints

**Always:** The reader never touches the RT plane except through the existing SPSC/host event queue (`StampedUmp` with host_ns). Use the existing frame library (`mcu_frame.hpp`, ids.json) and `contact_events.hpp` (`make_contact_event`, contact index k = frame contact + 1). `contact_k` is emitted at each closure/open; Note On at the contact that completes the velocity measurement (the calibration table's velocity contact pair), Note Off when the key opens (first contact to open after the key was sounding). Calibration tables are data: switching a 2-contact to a 3-contact table changes behaviour with no code change. Event order is never reversed (monotonic converted times). A frame CRC/decode error or seq gap is counted; the reader never leaves a stuck note: a key whose open event was lost is released when the next frame for that key or a periodic key-state check shows it open. Frame time conversion is a pure function of an injected clock so tests are deterministic.

**Never:** No UART/SPI/IRQ drivers, no real-hardware code (dev/prototype board later). No velocity *curve* (Story 3.6): Note On velocity here comes from a provisional linear time→16-bit velocity mapping behind a replaceable `VelocityMapper` interface, and the engine/MCU apply no curve. No aftertouch, no release-velocity semantics beyond a fixed release velocity (3.6/3.7). No changes to the frame protocol.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Drift | simulated MCU clock ±200 ppm over 12 h simulated, periodic TIME_SYNC | converted times within 20 µs of true times, never reordered | none |
| 3-contact press | contacts 0,1,2 close in order | contact_k 1,2,3 closed events at their times; Note On at the velocity pair's completing contact | none |
| 2-contact table | same keybed with 2-contact table | contact_k 1,2; Note On at contact 2 | none |
| Release | contacts open (3 then 2 then 1 order of opening) | contact_k open events; Note Off once at the key's release contact | none |
| Lost open | open frame lost, later frame for same key shows open / key-state check | note released, no stuck note | counted |
| Bad frame | CRC error or seq gap | counted in reader counters; processing continues | none |
| Unknown key | key index outside table | frame dropped and counted | counted |
| Time-sync loss | no TIME_SYNC reply for a long time | keeps last offset/drift; counts missed syncs | counted |

</intent-contract>

## Code Map

- `engine/host/include/velora/host/mcu_frame.hpp`, `src/mcu_frame.cpp` -- frame decode, `KeyContact`, `TimeSync`, `Hello` (keybed_contacts); `mcu/common/MCU-ID-MAP.md`.
- `engine/core/include/velora/core/contact_events.hpp` -- `make_contact_event`; `ump.hpp` for Note On/Off construction (MIDI 2.0, 16-bit velocity).
- `engine/host/include/velora/host/midi_ingest.hpp`, `render_driver.hpp`, `time_mapper.hpp` -- `StampedUmp`/host-ns stamping and the host event queue the reader pushes into.
- `engine/core/include/velora/core/spsc_queue.hpp` -- SPSC queue; `engine/core/ORGAN.md` -- contact groups / how the organ consumes contact_k then Note On.
- `engine/host/CMakeLists.txt` (+ tests templates `tests/mcu_wrapper_test.cpp`, `control_test.cpp`) -- registration.
- New: `engine/host/include/velora/host/keybed_reader.hpp`, `src/keybed_reader.cpp`; `keybed_calibration.hpp` (table struct + 2/3-contact built-ins and a loader from a plain struct/JSON); `tests/keybed_reader_test.cpp`.

## Tasks & Acceptance

**Execution:**
- `engine/host/{include,src}/keybed_calibration.*` -- calibration table (contact count, velocity pair, key→note map, time→velocity provisional mapper interface) -- data-driven keybed models
- `engine/host/{include,src}/keybed_reader.*` -- frame ingest, TIME_SYNC offset/drift estimator (linear fit / filtered), UMP emission into the SPSC queue, per-key state machine, lost-open recovery, counters -- the reader core
- `engine/host/tests/keybed_reader_test.cpp` + CMake -- tests for every matrix row, incl. 12 h simulated ±200 ppm (±20 µs, no reordering), organ render test: recorded slow press shows busbar groups entering at each contact time (offline render through `BlockRenderer` with the organ), 2- vs 3-contact tables, switching tables without code change, bad-frame/seq-gap, lost-open, unknown key
- `engine/host/CMakeLists.txt`, README note -- register tests and document the reader

**Acceptance Criteria:**
- Given a simulated MCU drifting ±200 ppm for 12 simulated hours, when event times are compared to true times, then they are within 20 µs and in order.
- Given a calibration table, when a key closes contact by contact, then contact_k events and a Note On at the velocity-completing contact are emitted; opens produce contact_k open and a Note Off; a different table changes behaviour with no code change.
- Given the organ on a part, when a recorded slow press is rendered, then busbar groups enter at each contact time.
- Given CRC errors or seq gaps, then they are counted and no note sticks.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~35 findings — patches 9 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` duplicate close retrigger; stale vel_from timestamp on partial-release retrigger — fixed + tests
  - `[medium]` `patch` rejected_frames overwrite; duplicate HELLO treated as restart; >35 min tick gap unwrap; rejected sync samples not advancing ref; recovery flag misses drop causes — fixed + tests
  - `[medium]` `patch` calibration validation (duplicate notes, release<vel_to), config sanitising, copy/move deleted, includes; missing branch tests (RTT rejection, fallbacks, spurious, queue_full, mapper) — added
  - `[medium]` `defer` no thread/link/scheduler (core only by spec); provisional velocity, no JSON loader
  - `[low]` `reject` emit far-future clamp, magic-number naming, report_key_state flag semantics, test-cost nits

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "keybed|mcu|control|organ|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: keybed reader core (DriftClock tick→host-ns fit, calibration tables, per-key contact state machine, contact_k + Note On/Off into HostEventQueue, lost-open recovery, counters). Organ render shows busbar groups entering at each contact time.
Verification: 103/103 targeted ctest pass; core deps check passes; 12 h ±200 ppm sim worst error 11.9 µs.
Residual risks: no real thread/link; provisional velocity mapper (Story 3.6).

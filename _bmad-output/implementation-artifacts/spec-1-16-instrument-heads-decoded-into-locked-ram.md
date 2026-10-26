---
title: 'Story 1.16: Instrument heads decoded into locked RAM at load (Mac logic and qualification tool)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '8a9d53b9b2582400a05da5780f8b02d59dcc9165'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/engine/host/include/velora/host/sample_bundle_loader.hpp', '{project-root}/engine/ports/include/velora/ports/sample_source.hpp', '{project-root}/engine/host/include/velora/host/live_set_loader.hpp']
warnings: []
deferred: ['Dev-board: the 1 h velora-ssd-qualify run at 256 voices of streaming with the SSD at least 90% full, and replacing the working HeadSizing defaults (p99.99 2 ms, margin 1 ms) with the measured figure', 'Head mode for protected libraries (refused with kHeadProtectedUnsupported; needs per-head decrypt and damage maps over heads)', 'Real streaming of the tail past the head (Story 1.17; the stream-underrun placeholder fades the voice and counts it)', 'A systemd engine service unit with LimitMEMLOCK=infinity (documented in yocto/README.md; there is no engine service unit yet); engine-host (macOS) has --head-mode flags only']
---

<intent-contract>

## Intent

**Problem:** Today the sample loader decodes every sample of an instrument fully into ordinary RAM (Story 1.15). The product needs an attack that never waits on decode or disk with a library far larger than RAM: each sample gets a short decoded "head" in locked, pre-faulted memory, sized from the qualified SSD latency (p99.99 + margin) at the sample's rate and the highest playback pitch the mapping allows; the instrument is playable only once all heads are resident; a head budget refuses loads that do not fit with a typed error stating required vs available; and the first audio block of a note reads only head memory (FR4, AR19, AR20, NFR3/4 partial, EXT11). The streaming of the rest of each sample is Story 1.17 and the on-board p99.99 measurement on the target SSD is dev-board work.

**Approach:** On the Mac (logic prototype): (1) `HeadSizing`: a pure function computing the head length in frames for a sample from p99.99 (ns), margin (ns), sample rate and the maximum pitch ratio the instrument's regions can play it at (tune/transpose/pitch range from the region table), plus a minimum (e.g. one engine block plus interpolation guard) and a cap at the sample length; a sample shorter than its head is fully resident (no tail). (2) The bundle loader gains a head mode (opt-in via `SampleLoadOptions`/config, default off = today's behaviour so every existing test/golden is unchanged): decode only the head of each sample into memory that is `mlock`ed and pre-faulted (touch every page; on macOS/Linux; if `mlock` fails (RLIMIT_MEMLOCK) fail with a typed status `kHeadLockFailed` unless an explicit `allow_unlocked_heads` dev option is set, in which case it logs and records `locked=false` in the result), on the control/loader thread, never RT; the `SampleSource` port exposes per-sample head data plus `total_frames` and a flag whether a tail exists (tail playback past the head is NOT implemented here: define the engine behaviour for the sample position passing the head end of a sample that has a tail as a typed, counted "stream underrun" placeholder: the voice ends with a short fade, counted in `Diagnostics.record_stream_underrun` — Story 1.17 replaces it with real streaming). (3) The instrument becomes playable only after all its heads are resident (the existing resident/ready publication of the loader/generation path, test it). (4) Head budget: the existing `head_budget_bytes` (Story 4.5) estimate (`instrument_head_bytes`) must use the real head sizes when head mode is on; a load that would exceed the budget is refused with the existing typed `kHeadBudgetExceeded` carrying required vs available bytes; already-loaded instruments keep playing (test). (5) Test that note onset reads only head memory: after load with head mode on, make the bundle's sample file unreadable (rename/chmod 000/unlink the bundle after load, or a counting FS port that fails any read) and render a note onset block(s): output is bit-identical to the fully-resident render for the head length, and no file reads happen (counter). (6) A portable qualification tool `velora-ssd-qualify` (tools/): reads random aligned blocks from files of a directory with the OS no-cache flag (`O_DIRECT` on Linux, `F_NOCACHE` on macOS, aligned buffers) at a configured streaming rate for N seconds (default short for tests, hours for the board), reports min/median/p99/p99.99/max latency in a machine-readable report (JSON) and the recommended head length for given margin; designed to be run on the dev board later (documented procedure; the 1 h run at 256 voices with the SSD ≥90% full is NOT run here).

## Boundaries & Constraints

**Always:** `engine/core` stays free of OS dependencies (mlock/pre-fault live in `engine/host`; core only sees memory pointers through the ports); allocation of heads happens on the loader thread; RT code only reads. Default behaviour unchanged (head mode off) so goldens/null tests stay bit-identical; with head mode on and heads covering the whole sample, output equals the fully-resident render bit-for-bit. Pre-fault by writing/touching every page after `mlock`; heads freed (munlock) when the instrument generation is released (lease/refcount path of Story 4.5; no leaks: test with a counting allocator/status). Typed errors for lock failure and budget. Document that `ulimit -l`/RLIMIT_MEMLOCK must allow the head budget on the board (systemd `LimitMEMLOCK=infinity` for the engine service, note for yocto/README).

**Never:** No streaming/tail reads (1.17), no io_uring, no measurement claims on the target SSD (the tool exists, the board run is deferred), no change to protected-bundle semantics except routing through the same head path if it shares the loader (if protected samples complicate it, support head mode only for unprotected bundles and refuse head mode for protected ones with a typed status, recorded as deferred), no change to RT-visible behaviour with head mode off.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Head sizing | p99.99 2 ms + margin 1 ms, 48 kHz, max pitch ratio 2 | head ≥ (3 ms × 48000 × 2) frames + guard, capped at sample length | none |
| Short sample | sample shorter than head | fully resident, no tail flag | none |
| Load with heads | instrument with long samples | only heads decoded, mlocked, pre-faulted; instrument ready after all heads | lock failure → kHeadLockFailed (or unlocked dev option) |
| Onset from heads | bundle unreadable after load | first blocks render from heads, identical to resident render, zero file reads | none |
| Past the head (no streaming yet) | note sustains beyond the head of a sample with a tail | voice fades out, stream underrun counted | none |
| Budget | required > available | kHeadBudgetExceeded with required vs available; others keep playing | typed error |
| Release | generation released | heads munlocked and freed, no leak | none |
| Head mode off | default | identical to Story 1.15 behaviour | none |
| Qualify tool | short run on a temp dir | JSON report with percentiles and recommended head length | missing/unreadable dir → clear error |

## Code Map

- `engine/host/include/velora/host/sample_bundle_loader.hpp`, `src/sample_bundle_loader.cpp` (decode, `instrument_head_bytes`, LoadHooks, protected path), `live_set_loader.hpp`/`src/live_set_loader.cpp` (head_budget_bytes, StageControl, estimates, kHeadBudgetExceeded), `live_set_switcher.*` (head accounting from Story 4.7: current_head_bytes uses estimates; keep consistent with real head sizes).
- `engine/ports/include/velora/ports/sample_source.hpp` (+ tests), `engine/core/include/velora/core/sample_engine.hpp`, `src/sample_engine.cpp`, `sample_bank.hpp` (voice playback from sample data; add head/tail awareness additively), `engine/core/include/velora/core/diagnostics.hpp` (record_stream_underrun).
- `formatlib/include/velora/format/bundle.hpp` (SampleInfo, frames, bits, rate), `engine/core/include/velora/core/engine.hpp`.
- `tools/` (new `tools/ssd-qualify/` with CMake), `yocto/README.md` (memlock note), `engine/host/README.md`.
- Tests: `engine/host/tests/sample_bundle_loader_test.cpp`, `live_set_loader_test.cpp`, `engine/core/tests/sample_engine_test.cpp`, `tools/offline-render/tests/*`.

## Tasks & Acceptance

**Execution:**
- HeadSizing pure function + tests (pitch ratio from region table, minimum, cap, short samples)
- loader head mode (opt-in): head-only decode, mlock + pre-fault, typed lock failure, port exposure (total_frames/tail flag), release path munlock/free
- engine: playback from head, safe behaviour past the head (fade + underrun counter), unchanged when head covers the sample
- budget integration: estimates use real head sizes; kHeadBudgetExceeded with required/available
- onset test with the file made unreadable; bit-identity vs resident render
- velora-ssd-qualify tool + JSON report + tests + procedure doc (board run deferred)
- docs and deferred list

**Acceptance Criteria:**
- Given a head length from p99.99 + margin, then every sample gets a head covering it at the sample's rate and highest playback pitch in mlocked pre-faulted memory on a non-RT thread, and the instrument is playable only after all heads are resident.
- Given a head budget, when a load would exceed it, then it is refused with a typed error stating required vs available, and loaded instruments keep playing.
- Given resident heads, when a note starts, then the first audio block reads only head memory (verified with the bundle unreadable and a read counter).
- The ≥1 h p99.99 measurement on the dev board SSD is recorded as deferred; the tool exists and is tested with a short run.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` budget counted payload while locked memory was page-rounded per sample (7-14x overshoot on 16 KiB pages, thousands of mappings) — one arena per instrument, estimate equals mapped size
  - `[medium]` `patch` loop wrapped back into head audio during the underrun fade; false underrun for loops ending in the head's last frames; renderer underrun counter never resynced; protected-refusal checks unreachable and their test hit another status; onset test partly vacuous; ssd-qualify opened every file (EMFILE) and claimed 256-voice load at QD1; flag handling and README errors
  - `[low]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "head|sample|loader|live_set|switch|ssd|offline|rt_traffic|null" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: HeadSizing (p99.99 + margin x rate x highest pitch ratio, loop-aware), head mode in the bundle loader (one mlocked, pre-faulted arena per instrument, typed lock failure and dev override, playable only when all heads resident, cancel/failure free everything), port total_frames/has_tail, engine fade + stream-underrun counter past the head, budget estimate equals mapped size, velora-ssd-qualify, engine-host head flags.
Verification: targeted tests (210) pass; full ctest only the 4 pre-existing failures plus load-sensitive librarian tests (pass alone); core deps lint passes.

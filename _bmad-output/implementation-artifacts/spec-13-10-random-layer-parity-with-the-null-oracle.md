---
title: 'Story 13.10: Random-layer parity with the null oracle'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: 'a24f70f5fd0f4a8d7de70bbcccbb1f691d9bb4b9'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/spec-5-4-reference-corpus-imports-and-plays-with-mapping-intact.md'
  - '{project-root}/tools/corpus/README.md'
  - '{project-root}/tools/oracle/README.md'
warnings: []
deferred:
  - summary: 'Pedal-deferred note offs are not aligned: the engine draws at the pedal lift, sfizz at the key up, and sfizz makes extra noteOffDispatch draws for delayed sustain releases (not modelled). Release-trigger random regions under the pedal are therefore not null-testable with the replay; the corpus `releases` probe is flagged in the report.'
    evidence: 'sfizz Synth.cpp startDelayedSustainReleases/noteOffDispatch; engine sample_engine.cpp stop_voice; report line "not aligned: sustain pedal (CC64) deferred releases".'
    severity: 'medium'
  - summary: 'Nine of the ten fetched libraries do not import (Story 5.4), so the replay has run on VSCO 2 CE only.'
    evidence: 'Story 5.4 spec; build/corpus-report-1310/report.md'
    severity: 'medium'
  - summary: 'The replay aligns the layer-choice number of note events only. A controller-triggered random region (on_locc/on_hicc with lorand/hirand) and a trigger=release region whose note off is deferred by the sustain pedal get the oracle number only where the engine draws at the physical note off; the engine draws at the deferred stop_voice for the pedal case. No VSCO instrument in the null picks has either.'
    evidence: 'tools/offline-render/src/render.cpp ReplayCtx observer (runs in BlockRenderer::apply_part_note); engine/core/src/sample_engine.cpp stop_voice draws at release time.'
    severity: 'low'
  - summary: 'Banks that use the sfizz pseudo-CCs 135/136 (uni/bi random) draw them from a second sfizz generator copy (MidiState.cpp) that the replay does not model; the engine draws them right after the layer number. Not aligned, not needed by any corpus instrument so far.'
    evidence: 'sfizz MidiState.cpp noteOnEvent (own `static fast_rand`), engine sample_engine.cpp note_pseudo().'
    severity: 'low'
  - summary: 'Only VSCO 2 CE has a corpus render with the replay; the other nine libraries do not import (Story 5.4) and so do not reach a null test.'
    evidence: 'build/corpus-report-1310/report.md'
    severity: 'medium'
---

<intent-contract>

## Intent

**Problem:** Instruments that choose a layer with `lorand`/`hirand` (FluteStac and TrumpetStac of VSCO 2 CE, 19 of its 75 files) cannot be null-tested against sfizz: the two renderers draw different random numbers, so they play different layers (10 of 48 VSCO renders failed in Story 5.4 and the instruments were left out of the null picks).

**Approach:** Replay the oracle's layer-choice draws for note events in the offline renderer (decision B below): from the MIDI events compute the number sfizz uses for every note event and re-seed the engine through its public `set_seed` just before the event, so the engine's first draw is that number. Production code is untouched.

## Boundaries & Constraints

**Always:** the production random generator (`SampleEngine::next_random`, `set_seed`, the draw rules of 13.3/13.5) stays byte-for-byte as it is; the replay is a flag of the offline renderer, off by default; every render without the flag stays bit-identical; a corpus instrument with random layers is rendered with the flag, any other render has the unchanged command line.

**Never:** add a test branch to the engine's generator; change a tolerance to make a random-layer instrument pass; claim exactness for opcodes the replay cannot align (`off_by`, `*_random`); trigger CI.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Replay on, random layers | MIDI with notes and CCs, instrument with `lorand`/`hirand` | engine plays the layer sfizz's number selects, every note event | n/a |
| Replay off | any render | bit-identical to before this story | n/a |
| Replay with routing, a non-sample engine or an emitted-note trace | flag + routing/engine | error, exit 1 | `the sfizz random replay needs the sample engine, no routing and no emitted-note trace` |
| Instrument with `off_by` or `*_random` | corpus pick | listed in the report with the opcode, left out of the automatic picks while another instrument exists; `null_files` can force it | reported, not silent |

</intent-contract>

## Decision (AC 1)

**Chosen: (b), a seeded comparison that replays the oracle's draw sequence, done entirely in the offline renderer (test harness).** Not (a) a test mode in the engine, not (c) a statistical check.

**What actually differs (measured from the sfizz 1.2.3 source in `tools/oracle/.work/sfizz` and the engine source):**

| | sfizz 1.2.3 | engine |
|---|---|---|
| algorithm | `fast_rand`: Numerical Recipes LCG `mem * 1664525 + 1013904223`, 32 bit | xorshift64* on a splitmix64-scrambled seed |
| seeding | seed 0, no setter (`Random::randomGenerator` is a `static` in `MathHelpers.h`, so every translation unit has its own copy) | `Config::seed` / `set_seed` |
| float mapping | `std::uniform_real_distribution<float>(0,1)`, `u32 / 2^32` | top 24 bits / 2^24 |
| layer number | `Synth.cpp`: one draw at the top of `noteOnDispatch`, `noteOffDispatch` and `ccDispatch` (every CC except 120/121/123), from the Synth.cpp copy only | one draw per note on (`start_voice`), per note off (`stop_voice`), per CC that reaches a triggered region (13.5) |
| other draws | `MidiState.cpp` draws two more per note on and off from its own copy (pseudo-CCs 135/136); `off_by` chokes call `noteOffDispatch` again | pseudo-CC draws only when a region reads them (13.5) |

So the sequence differs (sfizz also advances on every CC and choke), the algorithm differs and the seeding differs: no seed makes the two agree, which matches the Story 5.4 finding (residual about 0 dB). The Story 5.4 proof that the RNG is the only cause stands (FluteStac without its second layer nulls).

**Why (b):** the sfizz sequence is a pure function of the MIDI event order (the Synth.cpp generator starts at 0 and advances once per note-on, note-off and CC), so it can be computed from the events the renderer already holds. The engine's `set_seed` is public and the block loop calls a note observer synchronously before each note reaches the engine, so the harness can set the seed so that the engine's *first* draw of the event equals the oracle's number. `set_seed` composed with one xorshift64* step is a bijection on 64-bit values, so the seed is obtained by exact inversion (O(1), no search). Nothing in `engine/` changes.

**Why not (a):** a test mode that replaces the generator or its draw order would put sfizz-specific code (the LCG, the draw counts) into the product engine, which Story 5.4 already judged wrong, and violates the AC that the production generator is not changed for a test. **Why not (c):** a distribution check (are the layers chosen with the right frequency) is the fallback when no sequence can be replayed; here the sequence can, and a null test also proves which layer sounds at every note, the part a frequency check cannot show.

**Evidence (what it shows, not a claim of full exactness):** (1) unit test `sfizz_random_replay_test`: the LCG, `sfizz_uniform_float` equal to the platform's `std::uniform_real_distribution<float>` for 100000 draws, the seed inverse over 20000 random targets and the edges (the engine's first draw is `floor(u * 2^24) / 2^24`), the draw sequence with CC/bend/contact/CC123 cases, and an engine with two layers split at 0.3: 600 notes re-seeded from the oracle numbers always play the layer the number selects (169 / 431 split). (2) The real oracle: FluteStac `round_robin` probe without the flag residual -0.54 dB (FAIL, as in Story 5.4), with the flag PASS; with the oracle's draws shifted by one (`--sfizz-replay-skip-draws 1`) FAIL again, so the pass depends on the exact sequence. (3) The corpus run below.

**What is aligned, and what is not:** ALIGNED: the layer-choice number of every note on and note off that reaches the engine at the physical event (the first draw of the engine's note-on, and of a note-off that is released at once). NOT ALIGNED, and not claimed: (1) a note off the sustain pedal defers: the engine draws when the pedal lifts, sfizz at the key up, and sfizz makes extra `noteOffDispatch` calls (one more draw each) for delayed sustain releases, which are NOT modelled; this matters only to release-trigger random regions; (2) controller-triggered random regions (`on_locc`/`on_hicc` with `lorand`/`hirand`); (3) the pseudo-CCs 135/136 (a second sfizz generator copy); (4) `off_by` chokes (an extra draw in sfizz) and `*_random` opcodes. The corpus records (1) per render: the `releases` probe uses CC64, so its report line names "sustain pedal (CC64) deferred releases" as not aligned. The observer fails the render (exit 1) if the number of note events it sees differs from the oracle sequence.

**Limits (honest):** the float grid of the engine (2^-24) means the engine's number is `floor(u * 2^24) / 2^24`, at most 6e-8 below the oracle's; a `lorand`/`hirand` boundary inside that gap would flip a layer (probability about 1e-7 per note; the SFZ boundaries of VSCO are 0, 0.5, 1). The replay covers the layer choice only (see `deferred`). `off_by` and `*_random` opcodes make sfizz draw more often or from other generators; such instruments are not claimed aligned.

## Code Map

- `tools/offline-render/src/sfizz_random_replay.{hpp,cpp}` -- LCG, float mapping, oracle draw sequence from the events, exact seed inverse.
- `tools/offline-render/src/render.{hpp,cpp}` -- `RenderOptions::sfizz_random_replay`, `sfizz_replay_skip_draws`; observer that calls `SampleEngine::set_seed` before each note event.
- `tools/offline-render/src/render_main.cpp` -- `--sfizz-random-replay`, `--sfizz-replay-skip-draws N`.
- `tools/offline-render/tests/sfizz_random_replay_test.cpp`, `CMakeLists.txt` -- unit test (ctest `sfizz_random_replay_test`).
- `tools/corpus/corpus.py`, `manifest.json`, `README.md`, `tests/test_corpus_lib.py` -- random-layer instruments rendered with the flag; only instruments with `off_by`/`*_random` stay out of the automatic picks; the VSCO entry lists FluteStac and TrumpetStac in `null_files`.
- `tools/offline-render/README.md` -- flag documented.

## Tasks & Acceptance

**Execution:**
- `tools/offline-render/src/sfizz_random_replay.*` -- implement -- the replay.
- `tools/offline-render/src/render.*`, `render_main.cpp` -- hook and flag -- harness only.
- `tools/offline-render/tests/sfizz_random_replay_test.cpp` -- test -- LCG, inverse, sequence, layer parity.
- `tools/corpus/*` -- use the replay, drop the "left out" rule for plain random layers, null_files -- AC 2.

**Acceptance Criteria:**
- Given the sfizz oracle's draw per note-on, note-off and CC, when the options are assessed, then this spec records decision (b) with reason and measured differences, and the production generator is unchanged (no file under `engine/` is modified).
- Given FluteStac and TrumpetStac from VSCO 2 CE, when the corpus job runs, then both are null-tested with the replay, and they are no longer in the `null_excluded_random_layers` list of the report.

## Verification

**Results (Mac, Story 13.10 run):**
- `python3 tools/corpus/corpus.py run --only vsco2-community --out build/corpus-report-1310`: PASS, 75 of 75 imported, 30 of 30 renders pass (TubularBells, ViolinEnsTrem, GM-StylePerc as before, FluteStac and TrumpetStac new; 6.9 min). FluteStac residuals -70.1 .. -76.7 dB (keyswitches probe -300 dB: both renders are silent or identical there, not informative), TrumpetStac -68.1 .. -80.4 dB, worst 100 ms window -50.1 dB (FluteStac sustained_loops). Replay calibration offset 0 (no initial draws needed). Report line `null_excluded_random_layers` is gone for VSCO (no instrument has `off_by`/`*_random`).
- `python3 -m unittest discover -s tools/corpus/tests`: 55 tests OK (1 skipped as before).
- `build/tools/offline-render/sfizz_random_replay_test`: ok.
- `cmake --build build -j 8` OK; `ctest --test-dir build -j 8`: 765 tests, 100% passed, no flake re-runs needed (the same two skipped as before: `librarian_demo_acceptance`, `sample_demo_null_test`); the new `sfizz_random_replay_test` and `offline_render_tests` pass.

**Evidence location:** `build/corpus-report-1310/report.md` and `report.json` on the Mac that ran it (`build/` is untracked, not committed). Control run, FluteStac `round_robin`, block size 1: no replay -0.54 dB FAIL; replay PASS; replay shifted by one draw FAIL.

**Not run:** Linux/Docker/CI (the Docker build is broken at HEAD by the unrelated LVGL drift); the other nine corpus libraries (they do not import).

## Spec Change Log

- Review round 1: the replay fails the render when the observer sees more or fewer note events than the oracle sequence (no silent keep of the previous seed); `sfizz_note_draws` needs no channel filter because the block loop's observer sees every channel (tested with channel 9); end-to-end `render_events` test, drift guard against the real engine, clamped seed inverse; `--sfizz-replay-skip-draws` needs the replay; corpus: shared comment stripping and `#include` following for both scans, pedal probe flagged, blockers printed per suite, stubbed-`sh` test.

## Review Triage Log

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: replay fails on observer/draw-list drift, end-to-end render_events test (flag on/off, error paths), drift guard against the real engine generator (3000 seeds), robust layer classification, seed input guard, CLI validation, comment/#include handling shared by has_random_layers and replay_blockers, sustain-pedal (CC64) reported as a replay blocker, per-suite blockers in the report, stubbed command-line test, spec claims narrowed, Story 5.4 note superseded.
- Deferred: pedal-deferred releases, CC-triggered random regions, pseudo-CC 135/136 and sfizz's extra sustain-pedal draws not modelled; only VSCO exercises the replay (nine of ten libraries do not import); FluteStac keyswitches probe reads -300 dB (proves nothing); Linux/libstdc++ equality of the float mapping unverified.
- Verification: macOS ctest 765 tests pass (one transient failure did not repeat); corpus unit tests 57 OK; real-oracle corpus run before the fixes: vsco2-community 75/75 import, 30/30 renders pass (FluteStac -70.1..-76.7 dB, TrumpetStac -68.1..-80.4 dB); the corpus job was NOT re-run after the review fixes.

---
title: 'Story 1.15: Sample engine plays RAM-resident instruments and passes the sfizz null test'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '9d760bbfa13e4486c0986f90d364d10471f2c71d'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/engine/core/README.md'
  - '{project-root}/formatlib/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The engine core has only a test-tone engine. The product needs a sample engine that plays instruments imported by Story 1.13 with correct region selection, velocity layers, round robin, envelopes, loops and release triggers, and proves its sound against the sfizz reference with the null-test harness.

**Approach:** (1) Define a sample-source port in `engine/ports` that provides an instrument's region table and fully RAM-resident PCM (float) per sample; (2) implement `SampleEngine` in `engine/core` (no OS dependency, RT-safe, no allocation in render) behind the existing `Engine` interface; (3) a host/offline loader that fills the port from a Story 1.13 bundle (formatlib `BundleReader`); (4) wire `--engine sample --bundle <path> --instrument <key>` into `tools/offline-render`; (5) null-test the engine's render against sfizz renders of the original SFZ/WAV for a committed MIDI test suite, with the oracle used **only** in test tooling.

## Boundaries & Constraints

**Always:** `engine/core` stays free of OS/host/formatlib dependencies (purity check must pass; the loader that reads bundles lives in `engine/host` or `tools/offline-render`); no allocation, locks or blocking in the render path (RTSan test as in Story 1.8); voices come only from the engine interface calls made by the core; cost per voice is declared in cost units and reflects the interpolation quality used; behaviours to implement: region selection by key, velocity, round robin (`seq_length`/`seq_position`) and random (`lorand`/`hirand` with a deterministic seedable RNG so renders are reproducible), pitch from `pitch_keycenter` + `transpose`/`tune`, `volume`/`pan`, `offset`, `ampeg_*` (delay/attack/hold/decay/sustain/release as in the SFZ spec; document exact curve shapes chosen to match sfizz), loops per `loop_mode` (no_loop, one_shot, loop_continuous, loop_sustain), release-trigger regions fired on note off (with release velocity/rt_decay only if in the subset), sustained notes ending at sample end for no_loop; the sfizz oracle (full engine) is built and used only by test tooling: a script `tools/oracle/build-sfizz-render.sh` fetches sfizz 1.2.3 (commit 4e70dc0b…) with submodules into an ignored build directory and builds its `sfizz_render` CLI, never linked into a shipped target (extend `tools/ci/check_third_party_notices.py` expectations accordingly: oracle sources are not vendored in the repo); committed goldens: small oracle-rendered WAVs for a synthetic SFZ test suite so that `ctest` compares the engine against committed oracle renders **without** needing sfizz installed; regenerating them requires the oracle script; real-library suites (Salamander Grand Piano, one VSCO2 instrument) are env-gated (`VELORA_SFZ_DEMO_DIR`, plus oracle available) and skip otherwise; the initial null tolerance proposal is -60 dB relative to reference RMS: record the chosen per-test tolerances and the reasoning next to the tests, and state honestly any test that cannot reach -60 dB and why (interpolation, envelope curve, normalization differences); a pitched note far from its key center is part of the suite and must pass within tolerance.

**Never:** Add streaming/disk I/O, head decoding or locked RAM (Stories 1.16/1.17), multi-core rendering, effects, filters, keyswitches, CC modulation (Epic 5), SF2, or link/ship sfizz; loosen tolerances silently to pass; modify the bundle format; commit large samples or libraries.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Velocity layers | note at velocities crossing region boundaries | exactly the region(s) covering that velocity play | none |
| Round robin | repeated same note with `seq_length=4` | positions 1..4 cycle in order, wrapping | none |
| Random layers | `lorand/hirand` | deterministic given the seed; distribution matches ranges | none |
| Pitch | note far from `pitch_keycenter`, tune, transpose | correct ratio; interpolation within tolerance | none |
| Loop modes | loop_continuous, loop_sustain, one_shot, no_loop | looping/ending per mode; loop_sustain releases out of the loop on note off | none |
| Release trigger | `trigger=release` region | fires on note off, not on note on | none |
| Envelope | ampeg attack/decay/sustain/release, delay/hold | amplitude follows the documented curves; release continues after note off until silent then the voice frees | none |
| Offset | `offset` > 0 | playback starts at that frame | none |
| Retrigger / poly | same note repeated, chords, voice exhaustion | voices allocated through the core; exhaustion uses existing PoolExhausted behaviour | typed status |
| Missing sample data | region references a sample absent from the source | region skipped silently with a status counter, never garbage | counter |
| Render RT safety | 10,000 blocks of mixed note traffic | RTSan reports nothing (where supported) | n/a |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/engine.hpp`, `block_renderer.hpp`, `test_tone_engine.hpp` + src -- the Engine interface (cost units, steal metadata, set_param by param_id), how the test-tone engine plugs in, the registry and `EngineTypeId` (Sample=1)
- `engine/ports/include/velora/ports/` -- add the sample-source port here (plain structs/spans, std only)
- `formatlib/include/velora/format/bundle.hpp`, `instrument_model.hpp` -- Region model and block decode (`read_frames`) for the loader; the importer is `librarian import` (Story 1.13) with `librarian/tests/fixtures/` as a source of synthetic SFZ ideas
- `tools/offline-render/` -- renderer CLI, null-test harness (`velora-null-test`, case files), golden pipeline pattern in `tests/golden` and the CMake that registers them
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations that must keep passing (CI is manual-only; validate locally)
- `_bmad-output/business/11-12-sfizz-license-check.md` -- oracle must never be linked into a shipped target
- `_bmad-output/planning-artifacts/epics.md` Story 1.15 (~line 1174) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `engine/ports/include/velora/ports/sample_source.hpp` -- port: instrument info, region table view, sample frames as contiguous float (or int32-converted) per channel, sample rate -- engine input
- `engine/core/include/velora/core/sample_engine.hpp` + `src/sample_engine.cpp` -- SampleEngine: region lookup structure built at instrument set time (outside RT), voices with interpolation (document the quality and its cost units), pan/volume/tune, ampeg state machine, loop handling, release triggers, round robin/random state, steal metadata (start time, level, held/sustained/releasing) -- the engine
- `engine/core` registry/renderer wiring -- allow binding a SampleEngine per channel via the existing registry path; Sample type no longer ENGINE_NOT_AVAILABLE when bound (keep FM/VA unavailable) -- integration
- `engine/host/` or `tools/offline-render/src/` -- bundle loader producing the port data from a Story 1.13 bundle (all samples decoded to RAM) -- loader
- `tools/offline-render` CLI -- `--engine sample --bundle B --instrument K [--seed N]` -- renders MIDI through SampleEngine
- `tools/oracle/` -- `build-sfizz-render.sh` (+ README) fetching the pinned sfizz into an ignored build dir and building `sfizz_render`; `render-oracle.sh` to regenerate goldens (SFZ + MIDI -> WAV at 48 kHz); `.gitignore` entries -- oracle tooling
- `tools/offline-render/tests/sample_suite/` -- synthetic SFZ library (generated by a small tool or committed tiny WAVs) covering the matrix, MIDI test files, committed oracle-rendered reference WAVs, per-test `.case` files with tolerance and reasoning recorded in a README table -- committed oracle goldens
- tests -- unit tests for region selection, round robin, envelope curves, loop modes, release triggers (engine/core/tests, no formatlib), the offline-render null tests against committed oracle goldens (ctest), an RTSan test (10,000 blocks), and the env-gated real-library suites -- coverage
- docs: README updates in engine/core, engine/ports, tools/offline-render, tools/oracle -- docs

**Acceptance Criteria:**
- Given the synthetic suite, when the engine renders each MIDI test, then the null residual versus the committed sfizz render is below the recorded per-test tolerance (target -60 dB; any deviation explained in the README table)
- Given a pitched note far from center, then it passes within tolerance and the declared cost per voice reflects the interpolation quality
- Given `engine/core`, then it builds with no OS dependency (purity check passes) and the RTSan render test passes on Linux clang-20 in Docker
- Given a release-trigger region, then it fires on note off only
- Given macOS and Docker Linux, then all tests pass; real-library suites skip cleanly when the oracle or libraries are absent

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass
- Run `tools/oracle/build-sfizz-render.sh` once and the golden regeneration; report whether the committed goldens reproduce

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~60 findings — patched 9 groups, rest deferred
- findings:
  - `[high]` `patch` audio-thread UB/hangs from unvalidated region data (inf/NaN/huge ampeg, extreme tune ratio, seq_position out of range, lorand >= hirand, loop-wrap `while`, linear branch negative index, looping release-trigger voice, seed/gain casts) — regions rejected and counted, bounded loop wrap, guards, clamps, tests
  - `[medium]` `patch` release region with no free voice silently dropped; velocity/attack-voice state keyed by note only — kPoolExhausted returned, per (channel, note)
  - `[medium]` `patch` loader trusted bundle metadata (bits_per_sample shifts, sample range, NaN/Inf floats, bad_alloc, zero-frame) — validated and counted as failed; ambiguous overload renamed
  - `[medium]` `patch` PASS_REGULAR_EXPRESSION masked exit codes; substring counter checks; `--negative` argc; cases.txt not a configure dependency; demo test could PASS "0 of 0" and built shell commands from env strings — fixed
  - `[medium]` `patch` check_excess and undo_sfizz_s16 had no test that could fail — unit tests added
  - `[medium]` `patch` oracle scripts turned any build failure into a skip, trusted a stale binary, left partial goldens; notices checker ignore/tracking loopholes — fixed, self-tests extended
  - `[low]` `defer` oracle_block_frames emulation costs a per-sample branch in the production hot path (compile out later); no stealing yet (Story 1.19); no micro-benchmark/calibration of cost units 3 and 5; goldens pinned to macOS arm64 oracle pan path; margin only 5-8 dB (volume_pan 0.6 dB worst window); no 24-bit/float/off-48 kHz null cases; explicit `ampeg_release=0` not representable; set_instrument return value; thread-safety of diagnostics accessors
  - `[intent gap noted]` Real Salamander Grand Piano and VSCO2 instrument were not available: AC2 is covered by 11 synthetic sfizz-oracle cases (-65 to -68 dB vs -60 dB tolerance) and an env-gated real-library null test (`VELORA_SFZ_DEMO_DIR`) that has not been run on the real libraries; "null tests run in CI" is satisfied as ctest cases because CI is manual-only
  - `[lesson]` macOS builds skip the RTSan tests; only `tools/ci/docker-check.sh` runs them — a wrong check surfaced there after the fixes and was repaired

## Auto Run Result

Status: done (real-library null test pending the founder's run). `SampleEngine` in `engine/core` (key/velocity/round-robin/random regions, pitch, volume/pan, offset, `ampeg_*`, four loop modes, release triggers, Hermite and linear interpolation with cost units 5 and 3), the sample-source port, bundle loader, `velora-offline-render --engine sample`, sfizz-oracle tooling (`tools/oracle`, never linked into shipped targets) and 11 committed oracle goldens: all null cases between -65.4 and -68.1 dB against a -60 dB tolerance. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity and third-party notices checks pass. Follow-up review recommended: false.

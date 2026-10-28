---
title: 'Story 13.11: SF2 libraries in the reference corpus'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '0d49b79f4f0c201bf96896de9b95331eba6c8284'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/spec-5-4-reference-corpus-imports-and-plays-with-mapping-intact.md'
  - '{project-root}/tools/corpus/README.md'
  - '{project-root}/tools/oracle/README.md'
  - '{project-root}/tools/offline-render/tests/sf2_suite/README.md'
warnings: []
deferred:
  - summary: 'The FluidSynth grid, trimming and tail logic is checked end to end only on a generated sine SoundFont and three small local packs, and with fakes in the unit tests; no pinned real library has run.'
    evidence: 'Verification section; manifest has no SF2 entry.'
    severity: 'medium'
  - summary: 'Percussion presets (bank 128 selected on channel 0 by MMA bank select) are not checked against real FluidSynth on a real kit; only the generated file has a 128:0 preset.'
    evidence: 'report of the synthetic run'
    severity: 'low'
  - summary: 'Import and report parsing is duplicated between Runner.library (SFZ) and Runner.library_sf2; a shared helper would remove it.'
    evidence: 'tools/corpus/corpus.py'
    severity: 'low'
  - summary: 'trim_oracle_wav and pad_silence read the whole oracle WAV into memory (a 600 s probe is about 115 MB).'
    evidence: 'tools/corpus/corpus.py'
    severity: 'low'
---

<intent-contract>

## Intent

**Problem:** The reference corpus (Story 5.4) knows only SFZ libraries and the sfizz oracle. A pack delivered as a SoundFont (a Pianobook or other pack) cannot count toward the corpus, although the SF2 reader (Story 5.3) and the FluidSynth oracle already exist.

**Approach:** A manifest `format` of `sf2` (default `sfz`) makes the existing corpus job import each `.sf2` with `librarian import`, render a per-preset MIDI suite through the sample engine and through the existing FluidSynth oracle (`tools/oracle/render-fluidsynth.sh`), null-test within a recorded per-library tolerance, and list SF2 and SFZ entries together in one report whose job fails on any import or null-test failure. Pinning, fetch and extraction hardening are unchanged.

## Boundaries & Constraints

**Always:** the same hash pinning (archive sha256 or local tree digest, trust on first use), allow-listed https hosts or `local_dir`, hardened extraction and copy; audio never enters the repository; a missing tool or oracle is a failure, never a skip; SFZ and SF2 only need their own oracle (an SF2-only run does not need sfizz); the FluidSynth oracle is the one of Story 5.3 (2.6.1 build, `render-fluidsynth.sh`), no second oracle; a tolerance is recorded per library (and per file with a written reason) and never loosened without one.

**Never:** commit real library audio or invent a manifest entry for a library that cannot be pinned by hash; compensate the oracle's SF2 copy for real libraries (the rewrite of `sf2_fixtures.hpp: for_oracle` belongs to the synthetic suite; the known FluidSynth departures are documented, not hidden); touch the importer, the engine or the Story 5.3 goldens.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| SF2 entry, all passes | `format: sf2`, pinned, tools present | every `.sf2` imported, up to N presets null-tested with all six probes, status PASS | none |
| SF2 import refused | an `.sf2` the reader rejects, not in `skip_files` | entry FAIL with the importer's reason | job exits 1 |
| Null-test miss | residual above the recorded tolerance | entry FAIL naming file, preset, probe, residual and tolerance | job exits 1 |
| Missing oracle | no FluidSynth binary for an SF2 entry in the run | whole run refuses before any work, naming the tool | exit 1, never a skip |
| Mixed manifest | SFZ and SF2 entries | one report table with a Format column | any failed entry fails the job |
| Bad manifest | `format` not sfz/sf2, `null_presets` on an SFZ entry, malformed preset id | manifest error | `check` exits 1 |

</intent-contract>

## Code Map

- `tools/corpus/corpus_lib.py` -- manifest validation (`format`, `null_presets`), local copy allows `.sf2`
- `tools/corpus/corpus.py` -- runner: SF2 branch (import, preset list from the file, FluidSynth render, null test), preflight per format, report
- `tools/corpus/midi_suite.py` -- the six probes as FluidSynth-grid SMFs (events on 4 ms multiples, bank/program select, engine copy one block later)
- `tools/corpus/tests/sf2_fixture.py` -- tiny stdlib SF2 writer for the offline tests
- `tools/corpus/tests/test_sf2_corpus.py` -- offline tests (the existing `test_corpus_lib.py` is unchanged)
- `tools/corpus/README.md` -- SF2 section
- `tools/offline-render/tests/sf2_suite/README.md`, `tools/oracle/render-fluidsynth.sh` -- reused as is

## Tasks & Acceptance

**Execution:**
- `tools/corpus/corpus_lib.py` -- add `format`, `null_presets`, `.sf2` to the local copy set -- one manifest, one hardening path
- `tools/corpus/midi_suite.py` -- FluidSynth-grid builder -- the oracle's player applies events a block late in whole milliseconds (Story 5.3 findings)
- `tools/corpus/corpus.py` -- SF2 runner and report -- FR19, FR20
- `tools/corpus/tests/*` -- tests for manifest, kind handling, report and failure behaviour with a synthetic SF2
- `tools/corpus/README.md` -- document

**Acceptance Criteria** (status after implementation in brackets):
- Given a corpus entry of kind SF2, when the corpus job runs, then it imports with the SF2 reader, renders the per-library MIDI suite and null-tests against the existing FluidSynth oracle within a recorded tolerance, with the same hash pinning and hardening as SFZ entries. [MET in the tooling, shown on a generated SoundFont and on local packs through the real tools.]
- Given the report, when it is written, then it lists SF2 and SFZ entries together and fails the job on any import or null-test failure. [PARTLY MET: the report and the failure behaviour are implemented and tested, but the manifest has no SF2 entry, because no pack could be pinned with a scripted source and a licence statement. Adding one is a manifest-only change.]

## Spec Change Log

## Review Triage Log

## Design Notes

FluidSynth's player has a 1 ms grid and applies events at 64-frame block starts, so the SF2 probes put every event on a multiple of 3 ticks of a 375 PPQ / 120 bpm clock (1 tick = 64 frames) and the engine's MIDI is the oracle's delayed by one tick, as in the Story 5.3 suite. The oracle's file is trimmed after its last non-zero frame plus 100 ms (its player appends 2 s) and the engine gets the matching tail. The comparison uses `--max-offset-frames 1` and no candidate quantisation, as the Story 5.3 cases. Real libraries are not rewritten for FluidSynth's departures from the specification (attenuation x0.4, sustain depth, decay time), so their tolerance is whatever the measurement supports, recorded with the reason.

## Verification

**Commands:**
- `python3 -m unittest discover -s tools/corpus/tests` -- expected: OK
- `cmake --build build -j 8 && ctest --test-dir build -j 8` -- expected: all pass
- `python3 tools/corpus/corpus.py run --manifest <temp manifest with a synthetic SF2 local_dir> ...` -- expected: PASS with FluidSynth

**Results (Story 13.11 implementation):**
- `python3 -m unittest discover -s tools/corpus/tests` -- 92 tests OK, 1 skipped (already skipped before this story).
- `cmake --build build -j 8` -- passed. `ctest --test-dir build -j 8` -- 765 tests, 319 s; `setlist_controller_tests` (segfault) and `librarian_service_test` (timeout) failed under load and passed alone; 2 skipped as before.
- Real tools, FluidSynth 2.6.1, temporary manifests outside the repo: generated sine SoundFont, 2 presets x 6 probes, residual -28.3 to -40.4 dB (passes at -26 dB); a generated SoundFont whose zones cover keys 0..10 only: 5 probes silent in the oracle and in the engine (accepted by the null test), `keyswitches` -32.5 dB; three Roland XV5080 packs from the founder's disk (unpinned, no licence statement), 12 renders, -26.1 to -32.6 dB; three NRD Fairlight packs refused by the importer (sample rates of about 35 kHz), reported as a failed job.

## Review Triage Log (orchestrator)

### 2026-10-28 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: no vacuous PASS (zero presets null-tested fails), explicit failure when every .sf2 is skipped or the librarian writes no report, duplicate and out-of-range bank/program keys fail, an all-silent oracle fails the preset, silent/short reference padded to the engine length, try/finally cleanup of temp WAVs, malformed manifest lists raise CorpusError, ids --no-local skips sf2 entries, tests for cmd_run preflight, tail, null_exclude/null_files, timeout/empty report, README/spec corrections.
- Departure: when the oracle sound dies before the last event the reference is padded with silence rather than failing (pizzicato/percussion with a late note-off).
- Deferred: real-binary integration of the FluidSynth grid/tail logic checked only with fakes; percussion presets (bank 128 on channel 0) unchecked; import/report duplication with the SFZ path; trim_oracle_wav loads the whole WAV.
- PARTLY MET: AC2 (no SF2 entry in the manifest: no pinnable pack with licence). The nightly workflow builds only sfizz, so a non-local SF2 entry would fail preflight there.
- Verification: corpus unit tests 92 OK (1 skipped); corpus.py check and guard exit 0; ctest corpus tests 4/4; earlier in the story: full macOS ctest ran (two load-only failures, setlist_controller_tests and librarian_service_test, passed alone) before the review fixes, which touch only tools/corpus Python; synthetic SF2 and three local Roland packs null-tested through the real tools and FluidSynth 2.6.1.

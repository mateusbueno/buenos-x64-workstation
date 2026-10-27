---
title: 'Sample end, first/legato triggers and release_key'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: 'dafb4782e726c43451b23fc2aa8f8d93515ad5ed'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/epic-5-context.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-2-importer-value-rules-and-inert-opcodes.md', '{project-root}/formatlib/README.md']
warnings: []
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux was therefore not run for this story.
    evidence: |-
      Story 13.2 deferred entry 1.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** The importer rejects `end`, `trigger=first`, `trigger=legato` and `trigger=release_key`, and the bundle and sample engine have no sample end or those trigger modes, so Manthey Klaviano (153 regions with `end`, release samples on `release_key`) cannot import or play with its mapping.

**Approach:** Add `end` and three trigger modes to the region model, the bundle (format minor 2, additive trigger/end table) and the engine. `end` ends playback at `min(end, length) - 1`; `end=0` disables the region; `first`/`legato` are decided at note-on from the held-key state the engine already keeps on the control path; `release_key` fires from the physical key-up (`key_released`), `release` keeps firing from the pedal-aware stop path.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0 or 1 as today); the reader accepts minor <= 2 (N and N-1 and the Epic 1 minor 0), rejects newer; a region without `end` and with an old trigger renders bit-identically; no allocation, lock or syscall in the render thread (the held-key bitmap is a fixed array in the part state); no RNG draw is added for banks without `release_key` regions.

**Never:** `rt_decay` or any other opcode (13.4+); change `trigger=release` semantics; trigger CI; commit or push.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| end truncates | `end=100`, sample 1000 frames | plays frames 0..99, last played frame 99 | none |
| end beyond file | `end=5000`, sample 1000 | last played frame 999 (`min(end,len)-1`) | none |
| end equals length | `end=1000`, sample 1000 | identical to a region without `end` | none |
| end zero | `end=0` | region stored, never plays (not counted as unplayable) | none |
| release_key, pedal down | key up, pedal down | release_key region fires at the key-up; `release` region waits for pedal up | none |
| first | key down, no other key held | `first` region fires, `legato` does not | none |
| legato | key down while another key is held | `legato` fires, `first` does not | none |
| reader, newer minor | version word minor 3 | kUnsupportedVersion | kUnsupportedVersion |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `TriggerMode` gains kFirst/kLegato/kReleaseKey, `Region` gains `has_end`/`end`, validation, `has_trigger_extension`.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 2`; minor 2 appends a trigger/end table after the minor 1 table.
- `formatlib/src/sfz_import.cpp` -- `end`, `trigger` values, opcode table.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields and mapping.
- `engine/core/src/sample_engine.cpp`, `sample_engine.hpp` -- effective end in `prepare` and `render_voice`, first/legato in `start_voice`, release_key in `key_released`.
- `engine/core/src/block_renderer.cpp` -- bind the generation after a key-up that started release_key voices.
- Tests: `formatlib/tests/{sfz,bundle}_test.cpp`, `engine/core/tests/sample_engine_test.cpp`, `librarian/tests/opcode_test.cpp`.
- Docs: `formatlib/README.md`, `engine/core/README.md`, `tools/corpus` (no Manthey exclusion exists).

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add fields and minor 2 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept `end`, `first`, `legato`, `release_key` -- importer.
- engine ports, loader, sample engine, block renderer -- playback.
- tests and README -- coverage and docs.

**Acceptance Criteria:**
- Given a region with `end`, when it plays, then the last played frame is `min(end, sample length) - 1` (exclusive end), `end=0` disables the region, and the importer and bundle store the field; a test covers a truncated sample (end beyond the file), end equal to the length and end=0.
- Given `trigger=release_key`, when the key is released while the sustain pedal is down, then it fires at the physical key-up while `trigger=release` keeps waiting for the pedal; the engine tells the two apart in its release path and a render test with the pedal covers both.
- Given `trigger=first` and `trigger=legato`, when notes overlap, then first fires only for the first note of a phrase and legato only for overlapping notes, using held-key state on the control path with no allocation in the render thread; the bundle `format_version` minor is bumped, the reader accepts N and N-1, and existing demo bundles still render bit-identically.
- Given Manthey Klaviano from the corpus, when the corpus job runs, then it imports, its MIDI suite renders and the sfizz null test passes within its recorded tolerance; the manifest keeps no exclusion for it.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (two tests skip)
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 708/708 pass, two skipped (librarian_demo_acceptance, sample_demo_null_test). Linux Docker not run (LVGL drift, see deferred).

Corpus: Manthey Klaviano exists on this machine (`~/Downloads/Manthey Klaviano`, pinned tree digest matched). `python3 tools/corpus/corpus.py run --only pianobook-manthey-klaviano`: imports 153 regions, 0 unplayable, all six MIDI suites render and null-test against sfizz (worst residual -71.1 dB, tolerance -60 dB): PASS. The manifest never carried an exclusion for it, so none was removed; no manifest change.

Format: minor 2 = minor 1 layout (table present, possibly empty) + a 16-byte-per-record trigger/end table; the region record's trigger byte stays 0/1. Bundles without the new fields keep minor 0/1 and their bytes (existing float goldens unchanged and passing). The reader accepts minor 0..2.

Known limits: release_key uses the note-on velocity (no release-velocity layering) and a full voice pool drops the sample silently (`Engine::key_released` returns void); `rt_decay` is still unsupported (not in this story's ACs); `end` with `offset >= end` makes the region unplayable rather than rejecting the file. followup_review_recommended: true (control path in block_renderer bind_generation after key up).

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap; intent layer skipped). Patched: all-keys-released clears held bits on CC120/123, key_released returns bool and generation binds only when a voice started, tests for protected round trip, end with streamed tail, release_key gates, region-record trigger byte rejection.
- Deferred: key held across instrument swap; same key struck twice (1-bit held); release velocity unused for release_key; importer warning for end<=offset; release_key full voice pool drop uncounted; router-active generation test not written; Linux Docker unbuilt (LVGL drift).
- Verification: macOS ctest 708/708 (2 skipped); Manthey Klaviano corpus run null-tested at -71.1 dB vs -60 dB tolerance.

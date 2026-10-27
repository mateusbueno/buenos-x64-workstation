---
title: 'CC at voice start and sfizz pseudo-CCs'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '684e3251d3baba568a5146ebab359a524fdb612c'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-2-importer-value-rules-and-inert-opcodes.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-4-velocity-and-amplitude-model.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: ['oversized']
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story.
    evidence: |-
      Story 13.2, 13.3 and 13.4 deferred entries.
    severity: medium
  - summary: >-
      Not implemented here, still rejected with `unsupported_features` naming the opcode: `*_stepcc<N>` and `*_smoothcc<N>` (quantising and smoothing are continuous modulation), CC numbers 128-130, 132-134, 138, 139 and above 141 in `*_oncc`/`*_curvecc` (pitch bend, aftertouch, note-off velocity, note number, note gate and the unnamed ones), `locc`/`hicc`/`lohdcc`/`hihdcc`/`set_cc`/`set_hdcc` with a number above 127 (sfizz never refreshes a gate on a pseudo-CC after load), `cutoff_oncc`/`resonance_oncc` and every other filter or EQ target (Story 13.8), `width_oncc`/`position_oncc`, `delay_oncc`, `end_oncc`, `loop_*_oncc`, LFO and flex-EG targets (Story 13.9), `on_locc`/`on_hicc` (Story 13.7), `sustain_cc`, `ampeg_release_shape`.
    evidence: |-
      sfz-scope-decision.md tiers 4, 6, 7 and 9; sfizz Region.cpp / Synth.cpp / MidiState.cpp read for this story.
    severity: low
---

<intent-contract>

## Intent

**Problem:** The importer rejects `locc`/`hicc` gates, `set_ccN`, every `*_oncc`/`*_curvecc` and the stored `<curve>` blocks are consumed by nothing, so Karoryfer, Osiris and Salamander style regions (pedal/wheel layers, release time on a CC, tune and amplitude on a CC) cannot import or play, and `ampeg_dynamic` is still silently inert.

**Approach:** Carry per-region CC gates and CC modulation entries, plus an instrument-level setup (CC defaults and the referenced `<curve>` tables), through the model, the bundle (format minor 4) and the engine. The engine reads the part's CC state (and the note-derived pseudo-CCs) once, when a voice starts, as sfizz does at note-on for a constant controller; a CC moving during a held note does not change that voice.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0 to 3 as today); the reader accepts minor 0..4 and rejects a newer one; the writer stamps minor 4 only when a region has a gate or a CC modulation entry or the instrument has a CC default or curve; a region without the new opcodes renders bit-identically (including the random sequence: the engine draws pseudo-CC randoms only for a bank that uses 135/136); pseudo-CCs 131, 135, 136, 137, 140, 141 are resolved from the note event and are never read from `set_part_cc`; deterministic; no allocation, lock or syscall in the render thread (curve tables are built in `prepare`); `ampeg_*_oncc` together with `ampeg_dynamic` (value 1/true, any level) is rejected as `unsupported_value` on `ampeg_dynamic`; gates are evaluated for attack, release and release_key triggers.

**Never:** continuous (per-block) modulation, `*_stepcc`, `*_smoothcc`, filter or EQ targets, LFO/EG matrix, controller-triggered voices (see `deferred`); change the velocity law or the Story 13.3/13.4 behaviour; trigger CI; commit or push.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| gate boundaries | `locc64=64 hicc64=100`, CC64 = 63, 64, 100, 101 | plays at 64 and 100 only (hicc N<127 covers CC value N up to N+1 exclusive, as sfizz's fill-gap) | none |
| CC change between notes | gate region, CC64 moved 0 -> 80 between two notes | silent, then plays | none |
| set_cc seed | `<control> set_cc64=90`, no CC message yet | the part starts with CC64 = 90; a later CC message wins | `set_cc` for a controller no region/crossfade uses is dropped, recorded in `ignored_values` |
| oncc amplitude | `amplitude_oncc74=100`, CC74 = 64 | gain x curve(64/127) at voice start | none |
| curvecc | `amplitude_curvecc74=7` with `<curve> curve_index=7 v000=0 v127=1 v064=0.9` | the stored curve is applied (linear between points, ends 0 and 1) | unknown index: linear default (sfizz) |
| envelope | `ampeg_release_oncc100=2` | release = ampeg_release + 2 * curve(CC100) seconds, fixed at voice start | |
| pseudo velocity | `pan_oncc131=100` | pan = pan + 100 * curve(vel/127) percent | none |
| pseudo random | `amplitude_oncc135=100` | uniform [0,1) from the engine RNG, deterministic for a seed; 136 bipolar [-1,1) | none |
| alternate / key delta | 137, 140, 141 | 137: 0,1,0,1 per note on of the part; 140: note minus the previous note on (0 for the first), 141 its absolute value; 140/141 are raw semitones | |
| ampeg_dynamic | `ampeg_dynamic=1 ampeg_release_oncc72=2` | file rejected: unsupported_value `ampeg_dynamic` | `ampeg_dynamic=1` alone stays accepted |
| rejected forms | `volume_stepcc1`, `cutoff_oncc131`, `amplitude_oncc133`, `locc131`, `set_cc200` | unsupported_features naming the opcode | |
| over the bound | 9 gates or 17 modulation entries in one region | `unsupported_value` | |
| reader | minor 5 | kUnsupportedVersion; minor 4 with no cc records, defaults or curves: kCorruptManifest | |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `CcGate`, `CcMod`, `CcMap` in `Region`, `InstrumentCcSetup` (defaults, curve definitions), `has_cc_extension`, validation.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 4`; minor 4 appends cc records, CC defaults and curves per instrument; `BundleWriter::set_instrument_cc_setup`, `InstrumentInfo::cc_setup`.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp` -- opcode table, aliases (`*_ccN`, `ampeg_<eg>ccN`, `lorealccN`, `set_realccN`), curve index assignment (sfizz CurveSet rules), `cc_setup`, `ampeg_dynamic` rejection.
- `librarian/src/import.cpp` -- hands `cc_setup` to the writer and verifies the round trip.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields, `InstrumentView` cc_defaults and curves.
- `engine/core/include/velora/core/sample_bank.hpp`, `sample_engine.hpp`, `src/sample_engine.cpp` -- gates, pseudo-CC state per part, curve tables at `prepare`, voice-start modulation of amplitude, volume, pan, tune, offset and the envelope; CC seed at bank bind.
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/CMakeLists.txt` -- one sfizz null-test case per family.
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/sample_engine_test.cpp`, `engine/host/tests/sample_bundle_loader_test.cpp`, `librarian/tests/*`.
- Docs: `formatlib/README.md`, `engine/core/README.md`, `tools/corpus/README.md`, `tools/offline-render/tests/sample_suite/README.md`.

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add the CC map and minor 4 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes, assign curve indices, prune defaults and curves to what regions use, reject `ampeg_dynamic` + `ampeg_*_oncc` -- importer.
- engine ports, loader, sample engine -- gates, seed, pseudo-CCs, voice-start modulation.
- librarian import -- write and verify the setup.
- tests, sfizz fixtures and goldens, READMEs -- coverage and docs; the unit test for the I/O matrix rows.

**Acceptance Criteria:**
- Given regions with `locc`/`hicc` gates, when a note starts, then the region plays only when the per-part CC value is inside the range, `set_ccN`/`set_hdccN` seed the initial values, and a test covers boundaries and a CC change between notes.
- Given `*_oncc` and `*_curvecc` on envelope, offset, amplitude, pan and tune, when a note starts, then the CC value is sampled at voice start through the stored `<curve>`, and sfizz's pseudo-CCs (131 velocity, 135/136 random, 137 alternate, 140/141 key delta) are resolved from the note, never from a real controller; `ampeg_dynamic` is no longer inert and the importer rejects `ampeg_*_oncc` with `ampeg_dynamic`.
- Given a CC that moves during a held note, when the null test runs, then the divergence from sfizz's continuous per-block modulation is documented as a known limit (READMEs) and the corpus keeps such probes out of the null picks with a manifest reason where the tooling supports it.
- Given kamoepiano301, Scarypiano, Osiris and Steel Drum, when imported, then they import. **NOT MET by this story.** None of the four imports yet. Blocked by other stories: `bend_up`/`bend_down` (13.7, Scarypiano), `group`/`note_polyphony` (13.6, Osiris), `cutoff_oncc131`/`cutoff_curvecc131` (13.8, kamoepiano301); and by opcodes outside Epic 13's story list: velocity crossfades `xfin/xfout_lovel/hivel` (Steel Drum), `ampeg_release_shape`, `sustain_cc` (kamoepiano301, Osiris), `width_oncc`/`width_curvecc` (Steel Drum), and `..` sample paths in Osiris' included map files. The CC opcodes of this story no longer block any of them.
- Given the bundle format, when the fields are added, then minor 4 is written only when needed, minors 0..4 read, and existing bundles are byte-identical.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (timing flakes `librarian_service_smoke`, `librarian_crash_campaign` re-run serially)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh <new cases>` -- expected: goldens written by the local sfizz oracle
- `python3 tools/corpus/corpus.py run --only <id> --local ...` -- expected: reports which libraries import now
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: all pass; `librarian_service_smoke`, `librarian_crash_campaign` and `librarian_install_service_test` failed once under -j 8 and passed serially (timing flakes). Two skipped (librarian_demo_acceptance, sample_demo_null_test). Linux Docker not run (LVGL drift).

Landed: `locc/hicc/lohdcc/hihdcc` (+`lorealcc/hirealcc`), `set_cc/set_hdcc/set_realcc` (control only), `{amplitude,volume,gain,pan,tune,pitch,offset}_{oncc,cc}` and `_curvecc` (offset_curvecc accepted and ignored with a warning, as sfizz), `ampeg_{delay,start,attack,hold,decay,sustain,release}_{oncc,cc,curvecc}` and `ampeg_<eg>cc`; controllers 0..127 and 131, 135, 136, 137, 140, 141; `<curve>` resolved with sfizz CurveSet rules; format minor 4; `ampeg_dynamic` + `ampeg_*_oncc` rejected. Still rejected: see `deferred`.

sfizz null residuals (tolerance -60 dB): cc_gates -67.9, cc_modulation -67.6, cc_envelope -65.3, cc_pseudo -69.4; strip135 negatives fail as required. 135/136 (random) are unit-tested only.

Corpus (local caches): VSCO2 still PASS (75/75, null tests pass). Scarypiano now blocked only by `bend_up/bend_down` (13.7). Osiris blocked by `group`, `note_polyphony` (13.6), `ampeg_release_shape`, `sustain_cc`, and `..` sample paths in included map files. Steel Drum blocked by `xfin/xfout_lovel/hivel` and `width_oncc/curvecc`. kamoepiano301 blocked by `cutoff_oncc131/curvecc131` (13.8), `ampeg_release_shape`, `sustain_cc`. So the AC "all four import" is NOT met by this story alone. No null runs were possible for them.

Known limit: controllers that move under a sounding voice are not followed (README, `null_exclude` in the corpus manifest tooling; no manifest entry needed yet).

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: lazy random draws on note-off, rebind/seed coherence (masks), release-voice 131 uses released note velocity, NaN/non-finite curve guards, inverted gates rejected, curve-only amplitude entry dropped, ampeg_dynamic true/on, release_key gate test, README/epics corrections, loader test made real.
- Deferred: part-attack override on eg_cc path; 135/136 draw order pinned only by determinism; key-delta (140) curve clamping vs sfizz not null-tested; per-part pseudo-CC state vs sfizz global; RTSan/Docker run (LVGL drift).
- NOT MET: AC "kamoepiano301, Scarypiano, Osiris and Steel Drum import" (blocked by 13.6, 13.7, 13.8 and opcodes outside the epic).
- Verification: macOS ctest 726 tests pass (one ui_shell_test flake under -j 8, passes on rerun); sfizz null residuals -65.3..-69.4 dB vs -60 dB.

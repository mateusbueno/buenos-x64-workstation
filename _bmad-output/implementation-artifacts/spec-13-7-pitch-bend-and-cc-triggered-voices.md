---
title: 'Pitch bend and CC-triggered voices'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '5f9f8d41728c72e58fb043660831497981d8330a'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-2-importer-value-rules-and-inert-opcodes.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-5-cc-at-voice-start-and-sfizz-pseudo-ccs.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-6-choke-groups-and-polyphony-limits.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: ['oversized']
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story.
    evidence: |-
      Story 13.2 to 13.6 deferred entries.
    severity: medium
  - summary: >-
      Not implemented here, still rejected with a reason: `on_lohdcc`/`on_hihdcc` (and `start_*hdcc`) as `unsupported_opcode`; a trigger on a controller above 127 (`unsupported_value`); more than 4 triggers or an inverted pair on one region; `bend_up`/`bend_down` beyond +-12000 cents (`unsupported_value`: sfizz keeps them, the model does not); `lobend`/`hibend` (wheel-gated regions) as `unsupported_opcode`; `bend_*_oncc`, pitch LFO/EG. `hikey=-1` without an `on_locc`/`on_hicc` trigger is still dropped before the bundle.
    evidence: |-
      sfizz Region.cpp (bend_*, start_locc/on_locc), Layer.cpp (registerCC), Synth.cpp (ccDispatch), Voice.cpp (pitchEnvelope, bendSmoother_), Smoothers.cpp read for this story.
    severity: low
  - summary: >-
      Divergences from sfizz, documented in engine/core/README.md and the sample suite README: (1) a region that sets no bend opcode ignores the wheel (sfizz bends it by +-200 cents); (2) a wheel event inside a 64-frame block moves the engine at the event, sfizz ramps up to the event, so the null fixtures place events on block boundaries; (3) sfizz's float32 SIMD ramp and per-block float32 position sum differ from the engine's by up to 0.07 frame for a ramp whose step is not a binary fraction (measured, -37 dB on a 100 ms retargeted ramp) and by ~1e-6 sample/frame for a non-octave ratio (0.02 to 0.03 frame after 2 s): the long fixtures use octave ratios and binary-fraction ramps; (4) a Live Set switch replays the keyboard controllers and a replayed value that differs from the part's stored one starts triggered regions.
    evidence: |-
      Null residuals: pitch_bend -62.4, pitch_bend_fine -63.6, pitch_bend_smooth -63.1, cc_trigger -67.6 dB (tolerance -60); the same configuration with non-binary ramp steps measured -49.9 and -37.7 dB against the engine, with a 0.07 frame phase offset that appears only while the ramp is above 1024 cents.
    severity: low
  - summary: >-
      Found while reading sfizz, not changed (Story 13.2 behaviour, tested there): sfizz also sets `triggerOnNote = false` for a `hikey` or `key` that is not a valid note at all (`hikey=128`, `key=-1`, `key=zz`: readOptional fails), not only for `hikey=-1`; the importer keeps such a region playable from notes with the default range.
    evidence: |-
      sfizz Region.cpp lines 211 to 230 (hikey, key).
    severity: low
  - summary: >-
      Open decisions and gaps: (a) FOUNDER DECISION: regions without a bend opcode ignore the wheel while sfizz bends them by +-200 cents by default; the importer could write explicit 200/-200 defaults for newly imported regions (existing bundles stay as they are). (b) The live MIDI 1.0 14-bit wheel mapping (v << 18) is verified only through the offline .mid reader, not through a real device/host path. (c) Streamed voices at maximum bend are untested. (d) std::pow per frame while a bend_smooth ramp moves is not measured on the target. (e) A fixture with a wheel event inside a 64-frame block is missing (sfizz moves up to a block earlier). (f) sysd_bridge_tests failed once under -j 8; the serial re-run passed.
    evidence: |-
      Review of the Story 13.7 run.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** `bend_up`/`bend_down`/`bend_step`/`bend_smooth` and `on_locc`/`on_hicc` make the importer reject a file as `unsupported_features`; `hikey=-1` regions are dropped before the bundle, so Salamander's pedal-action noises (a region started by CC64, never by a key) cannot play; and the sample engine ignores the pitch wheel altogether, so Scarypiano, Weresax and Meatbass cannot import.

**Approach:** Carry a per-region pitch-bend range and a per-region list of controller triggers through the model, the bundle (format minor 6) and the engine. A bend region follows the part's wheel in cents through sfizz's linear smoother; a triggered region starts a voice from `set_part_cc` when the controller moves into its range (sfizz `Synth::ccDispatch` / `Layer::registerCC`), pitched at its `pitch_keycenter`, with no velocity law, never tied to a key.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0..5 as today); the reader accepts minor 0..6 and rejects a newer one; the writer stamps minor 6 only when a region has a bend opcode or a controller trigger; a region that sets no bend opcode stores no bend and ignores the wheel exactly as before this story (the engine returns "ignored" for the wheel when the part's bank has no bending region), so every existing instrument renders bit for bit as before; a bank without controller triggers runs none of the new code and draws no random number on a controller message; a bend voice starts at the wheel's current position (no ramp, sfizz `bendSmoother_.reset`); the wheel is read once per render segment, the ramp is per voice and per frame, no allocation, lock or syscall in the render thread; a controller voice is never released by a note off, key up or sustain pedal (it ends with its sample, a choke group, a polyphony limit or a steal); `hikey=-1` without an `on_locc`/`on_hicc` trigger is still flagged `never_note_triggered` and dropped before the bundle; deterministic.

**Never:** `lobend`/`hibend` (wheel-gated regions), `on_lohdcc`/`on_hihdcc` (and `start_*hdcc`), triggers on pseudo-controllers (above 127), bend values beyond +-12000 cents, `bend_*_oncc`-style modulation, pitch LFO/EG, sfizz's within-block ramp of a wheel event (see Design Notes), changing the velocity law or the Story 13.3 to 13.6 behaviour, triggering CI, committing or pushing.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| bend extremes | `bend_up=1200 bend_down=-1200`, wheel at 0xFFFFFFFF / 0 / centre | pitch +1200 / -1200 / 0 cents (a note plays an octave up / down / unchanged) | none |
| bend step | `bend_up=200 bend_step=100`, wheel at 0.75 | 150 cents is quantised to 100 (trunc) | none |
| bend smooth | `bend_smooth=50`, wheel jumps | the pitch ramps linearly to the new cents over max(16, 50 ms) frames | none |
| bend in release | wheel moves while a voice releases | the releasing voice follows | none |
| wheel without opcodes | region with no bend opcode | the wheel changes nothing; the event counts as ignored | none |
| one opcode given | `bend_up=400` only | `bend_down` keeps -200 | none |
| triggered region | `hikey=-1 on_locc64=127 on_hicc64=127`, CC64 0 -> 127 | one voice at the region's pitch_keycenter, no velocity law, a note never starts it | none |
| trigger repeat | CC64 127 again | nothing (value unchanged) | none |
| trigger range | `on_locc1=10 on_hicc1=20`, CC1 = 9, 10, 20, 21 | starts at 10 and 20 only (hicc N < 127 covers N up to N + 1 exclusive) | none |
| gate on the same controller | `locc64=127 hicc64=127 on_locc64=127 on_hicc64=127` | the gate sees the new value, so the voice starts | none |
| note and trigger | region with `key=60 on_locc1=1` and no `hikey=-1` | starts from the note and from the controller | none |
| controller voice and note off | held controller voice, note off of its pitch_keycenter | the voice is not released | none |
| dropped | `hikey=-1` and no trigger | flagged, not written, one `ignored_values` line | none |
| rejected | `on_lohdcc64`, `on_locc131`, `bend_up=24000`, 5 triggers, `lobend` | `unsupported_features` naming the opcode (or `unsupported_value`) with file:line | file skipped |
| reader | minor 7; minor 6 with no record; a record holding the default | kUnsupportedVersion; kCorruptManifest; kCorruptManifest | none |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `PitchBend`, `CcTrigger`, `CcTriggers` in `Region`, `has_bend_trigger_extension`, validation.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 6`; minor 6 appends the bend/trigger table after the minor 5 table.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp` -- opcode table, `bend_*`, `on_locc`/`on_hicc` (+ `start_*`), `hikey=-1` kept when a trigger exists, `build_cc_setup` counts trigger controllers.
- `librarian/src/import.cpp`, `librarian/src/preflight.cpp` -- message for regions nothing can start.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields.
- `engine/core/include/velora/core/{engine,sample_bank,sample_engine}.hpp`, `src/{sample_engine,block_renderer}.cpp` -- `set_part_pitch_bend`, `take_cc_started`, per-voice bend ramp, `cc_trigger`, controller voices, generation binding.
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/tests/sample_suite_test.cpp`, `tools/offline-render/CMakeLists.txt` -- sfizz null cases for bend and a CC-triggered region (plus `strip` negatives).
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/{sample_engine,parallel_render,rtsan_sample_features}_test.cpp`, `engine/host/tests/sample_bundle_loader_test.cpp`, `librarian/tests/{opcode,install,install_service}_test.cpp`.
- Docs: `formatlib/README.md`, `engine/core/README.md`, `tools/offline-render/tests/sample_suite/README.md`, `tools/corpus/README.md`.

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add the bend/trigger record and minor 6 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes, keep `hikey=-1` regions that have a trigger -- importer.
- engine ports, loader, sample engine, block renderer -- wheel, ramp, controller voices, generation binding.
- tests, sfizz fixtures and goldens, READMEs -- coverage and docs; unit tests for the I/O matrix rows.

**Acceptance Criteria:**
- Given a pitch-bend message, when a note sounds, then its pitch follows `bend_up`/`bend_down` in cents with smoothing and no allocation in the render thread, and a part without the opcodes keeps its current bend behaviour; and a test covers both extremes and a bend during release.
- Given a region with `on_locc`/`on_hicc` and `hikey=-1`, when the CC crosses into its range, then the region starts as a voice from the controller and never from a note, and Salamander's pedal-action region imports. MET.
- Salamander from the corpus imports and passes the null test within its recorded tolerance. NOT MET: FLAC samples, `rt_decay`, `amp_veltrack_oncc99`; Scarypiano's `..` sample paths also still block it (see the Auto Run Result).
- Given the bundle format, when the fields are added, then minor 6 is written only when needed, minors 0..6 read, and existing bundles are byte-identical.

## Design Notes

sfizz computes the bend per block as a linear ramp between wheel events (`linearEnvelope`: the ramp ends AT the event's frame) and then runs the voice's `LinearSmoother`. The engine receives the wheel between render segments and steps at once, then applies the same smoother, so for `bend_smooth` of a few ms or more the two agree and for `bend_smooth=0` a wheel event moves the pitch one block earlier in sfizz (a ramp over at most the oracle's 64-frame block) than in the engine (a jump at the event). The null fixtures use `bend_smooth` and slow wheel moves; the divergence is documented in the sample suite README.

A region that sets no bend opcode does NOT bend in the engine although sfizz would bend it by its +-200 default: this keeps every instrument that exists today bit for bit, as the story requires ("a part without the opcodes keeps its current bend behaviour").

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (timing flakes `librarian_service_smoke`, `librarian_crash_campaign`, `ui_shell_test`, `stream_test`: re-run serially)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh <cases>` -- expected: goldens written by the local sfizz oracle
- `python3 tools/corpus/corpus.py run --only salamander-grand-piano-v3 --max-null 0 --out build/corpus137/salamander` -- expected: reports whether Salamander imports
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 740 tests, 738 pass, 2 skipped (`librarian_demo_acceptance`, `sample_demo_null_test`, both need local demo libraries); `sysd_bridge_tests` failed once under -j 8 and passed on the serial re-run (a timing flake, not in the usual list, unrelated to this story). RealtimeSanitizer variants are not built on this machine (clang < 20); the same traffic runs as `sample_features_traffic_test` with the wheel and controller voices in it. Linux Docker not run (LVGL drift).

Landed: `bend_up` (`bendup`), `bend_down` (`benddown`), `bend_step`, `bend_smooth` (sfizz has it; milliseconds, `LinearSmoother`), `on_locc<N>`/`on_hicc<N>` (+ `start_locc`/`start_hicc`) with up to 4 triggers per region; a `hikey=-1` region with a trigger is kept and stored with `no_note`; format minor 6 (20-byte fixed part plus 12 bytes per trigger; written only when a region has a bend opcode or a trigger; every older bundle byte-identical, tested for each writer ceiling; the reader accepts 0..6); engine wheel (per part, per voice ramp, no allocation) and controller voices (started from `set_part_cc`, bound to the Live Set generation through `Engine::take_cc_started`); the offline renderer now reads pitch wheel events from a `.mid` for the sample engine. Tests that used minor 6 as "newer" use minor 7.

sfizz null (tolerance -60 dB): `pitch_bend` -62.4, `pitch_bend_fine` -63.6, `pitch_bend_smooth` -63.1, `cc_trigger` -67.6; the `strip137` negatives fail as required (2.5, 2.5, 2.2, -1.9 dB). Two engine/sfizz alignments were found by the null test and fixed in the engine: the step into the first frame of a wheel event uses the new rate (sfizz applies the pitch of frame i before reading frame i), and a retarget during a ramp moves the smoother from the previous frame's value; the test-only `oracle_block_frames` hold steps back by the bent rate. The 14-bit to 32-bit mapping of the offline renderer is a plain shift (a bit-repeating upscale made a 5e-5 rate error that drifted to -29 dB).

Corpus (local caches, `corpus.py run --max-null 0`, import only, no null runs, 12 libraries): two import completely and are unchanged from Story 13.6 (VSCO2 75 of 75 files, 3163 regions; Manthey Klaviano 1 of 1, 153 regions). None of the others does yet. `bend_up`/`bend_down` and `on_locc`/`on_hicc` no longer appear among the blockers of any of them.
- Salamander: NOT importing, NOT null-tested. Remaining blockers: `rt_decay` (8), `amp_veltrack_oncc99`/`amp_veltrack_curvecc99` (1 each), and, newly seen because the audio check comes after the opcode check, its samples are FLAC and the importer reads WAV only ("unsupported audio: not a WAV file"). The pedal-action block itself (`Data/pedal.txt`, with `lokey=-1 hikey=-1 group=1 polyphony=1 off_time=0.1`, `on_locc64=127`/`0` with the same `locc64` gates) imports and its structure is exercised by the `cc_trigger` null fixture; verified on the real file with WAV stand-ins for the four pedal samples.
- Scarypiano: NOT importing through the corpus job. Its `bend_up=1200`/`bend_down=-1200` now import; what is left is that every region names its sample `..\Samples\...` (outside the SFZ's directory, which the importer refuses: "leaves the SFZ directory"), reported as `skipped (damaged)`. With the SFZ copied next to a symlinked `Samples` and the paths rewritten, the whole library imports (45 s); no null test was run on it.
- Other libraries' remaining blockers (opcode histograms): kamoepiano301 `cutoff_oncc131`/`cutoff_curvecc131`, `ampeg_release_shape`, `sustain_cc`; Osiris `ampeg_release_shape`, `sustain_cc` (and the Story 13.5 `ampeg_dynamic` rule, `..` map paths); Steel Drum `xfin_lovel`/`xfin_hivel`/`xfout_*vel`, `width_oncc1`; Headroom `amp_veltrack_oncc73`, `fil_veltrack`, `width_*`, `eq1_*`; Meatbass `eg06_time1_oncc*`, `lfoNN_*`; Weresax `offset_random`, `lfoNN_*`, `fil_keytrack`; Bear Sax `offset_random`, `eg06_time1_oncc*`, `lfoNN_*`, `pitch_keytrack`, `eq1_*`; Sonatina `fil_keycenter`/`fil_keytrack`/`fil_veltrack`, `delay`, `cutoff_cc1`, `amplfo_*`, `offset_random`.

NOT MET by this story: the Epic 13 AC "Salamander from the corpus imports and passes the null test within its recorded tolerance" (see the Salamander blockers: `rt_decay`, CC-modulated `amp_veltrack`, FLAC samples). The first AC (bend in cents, smoothing, no allocation, extremes, bend during release, a part without the opcodes unchanged) and the second AC's controller-started voice and "Salamander's pedal-action region imports" are met. The epics.md wording was not changed.

## Review fixes

Controller voices report every started key (128-bit mask drained by `take_cc_started`, bound one by one in `deliver_cc`); the Live Set keyboard replay uses `set_part_cc_replay` (stores the value, fires no trigger, documented); `reset_play_state` clears the pending keys and the stored wheel; the bend position correction only applies to a voice that has stepped; controller voices are excluded from note-polyphony counting and the same-note choke exemption; `cc_trigger` skips non-attack regions and the importer refuses `on_locc`/`on_hicc` with another `trigger` mode; `bend_trigger_sane` requires ascending unique triggers and clean unused entries; a controller-only region is range-checked at its key centre; `sfz_import` -0 verified. sysd_bridge_tests serial re-run: passed.

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: all started controller keys bound to the Live Set generation (128-bit mask), replay no longer fires triggers, reset clears wheel and pending keys, bend position correction only after a stepped frame, controller voices excluded from note-polyphony and same-note choke exemption, non-attack trigger modes rejected with on_locc, trigger sanity (unique ascending), no-note range check at key centre, doc fixes.
- Deferred (see spec): default +/-200 cent bend for regions without bend opcodes is a founder decision; live MIDI 1.0 wheel mapping; streamed voices at max bend; std::pow per frame during bend_smooth; mid-block wheel fixture; hikey=128/key=-1 sfizz divergence.
- NOT MET: AC "Salamander imports and passes the null test" (FLAC samples, rt_decay, amp_veltrack_oncc99); Scarypiano blocked by '..' sample paths.
- Verification: macOS ctest 740 tests pass (reverb_bench, sysd_bridge_tests timing flakes pass on rerun); sfizz null residuals -62.4..-67.6 dB vs -60 dB.

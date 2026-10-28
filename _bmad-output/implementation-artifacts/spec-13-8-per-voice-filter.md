---
title: 'Per-voice filter'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '1b460f2bced0fd2ae0d06364e11a75702cc92d76'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-4-velocity-and-amplitude-model.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-5-cc-at-voice-start-and-sfizz-pseudo-ccs.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-7-pitch-bend-and-cc-triggered-voices.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: ['oversized']
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story.
    evidence: |-
      Story 13.2 to 13.7 deferred entries.
    severity: medium
  - summary: >-
      Not implemented here, still rejected with a reason (`unsupported_features`, or `unsupported_value` for a value): `fil_random`/`cutoff_random`, `fil_gain` and `fil_gain_oncc` (gain filter types), `fil_veltrack_oncc`/`fil_veltrack_curvecc`, `cutoff*_stepcc`/`_smoothcc` and `resonance*_stepcc`/`_smoothcc` (continuous modulation), `cutoff*_chanaft`/`_polyaft`, a third filter (`cutoff3`, `fil3_*`: sfizz keeps two), every `fil_type` outside lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p (lpf_4p/6p, hpf_4p/6p, bpf_1p, brf_*, apf_1p, pkf_2p, eq*, comb ...), `fileg_*`/`fillfo_*`/`egNN_cutoff`/`lfoNN_cutoff` (Story 13.9), `fil_gain`-typed shelves, more than 4 `cutoff*_oncc` or `resonance*_oncc` entries on one filter (`unsupported_value`), a second filter on a region whose first filter has no `cutoff` (sfizz would run the missing one at cutoff 0).
    evidence: |-
      sfizz Region.cpp lines 575 to 715 (filters), FilterPool.cpp (setup/process), OpcodeCleanup.re (aliases) read for this story.
    severity: low
  - summary: >-
      kamoepiano301 still does not import. Remaining blockers: `sustain_cc` and `ampeg_release_shape` (outside Epic 13's story list) and `ampeg_dynamic=1` together with `ampeg_releasecc64`/`ampeg_release_curvecc64` (the Story 13.5 rule; the corpus histogram does not show it). Headroom Piano still does not import: `amp_veltrack_oncc73`, `width_oncc76`/`79` and `width_curvecc76`/`79`, `eq1_*`/`eq2_*` (EQ, Story 13.9). The Epic 13 AC "kamoepiano301 and Headroom Piano import and pass" is NOT MET by this story.
    evidence: |-
      build/corpus138/all/report.json and build/corpus138/kh/report.json; import log of a scratch copy of kamoepiano301.
    severity: high
  - summary: >-
      kamoepiano301 `sustained_loops` null fails at -54.9 dB (5 other suites -94 to -104 dB) on a scratch copy with the four blocking opcodes removed for both renderers; it also fails at -55.0 dB with every cutoff/resonance/fil opcode removed, so it is not the filter's. Uninvestigated.
    evidence: |-
      scratch run with the corpus Runner on a copy of kamoepiano301.sfz (notes held 8 to 10 s).
    severity: medium
  - summary: >-
      Thin null margin: `fil_keytrack` passes at -61.3 dB against -60 (margin 1.3 dB), worst 100 ms window -50.9 dB at key 96 (12 semitones above its region centre). Not investigated.
    evidence: |-
      sample_null_fil_keytrack.
    severity: medium
  - summary: >-
      Release-trigger voices and controller-started voices with a filter are not null-tested against sfizz; a controller-started voice uses the region's pitch_keycenter for key track where sfizz uses the controller number. The CC for the cutoff/resonance is read at voice start only (sfizz follows it per block). The RealtimeSanitizer variant (`rtsan_sample_features_test`) is not built on this machine (clang < 20): the render-thread coefficient computation is NOT RT-verified until it runs on Docker/Linux clang. The cap of 4 CC entries per target and filter is a documented bound not justified from the corpus (kamoepiano301 uses 1).
    evidence: |-
      engine/core/README.md "Per-voice filter (Story 13.8)"; kamoepiano301 uses one cutoff_oncc per region.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** The sample engine has one filter per region with a cutoff that is fixed when the bank is prepared, and the importer rejects `fil_keytrack`, `fil_keycenter`, `fil_veltrack`, `cutoff_oncc`/`cutoff_cc`/`cutoff_curvecc`, `resonance_oncc`/`resonance_cc` and a second filter. kamoepiano301 (`cutoff_oncc131`/`cutoff_curvecc131`) and Headroom Piano (`fil_veltrack`) open the filter with touch, so they cannot import; Sonatina and Weresax use `fil_keytrack`/`fil_keycenter`/`fil_veltrack` in hundreds of files.

**Approach:** Carry a per-filter tuning (key track, velocity track, CC modulation of cutoff in cents and of resonance in dB) and an optional second filter through the model, the bundle (format minor 7) and the engine. The coefficients of each filter are computed once per voice, when the voice starts, from the note, its velocity and the part's controllers (sfizz `FilterHolder::setup` plus the constant CC modulation of that instant), and kept in the voice; the render path only runs the biquad ticks, as before.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0..6 as today); the reader accepts minor 0..7 and rejects a newer one; the writer stamps minor 7 only when a region carries a filter tuning or a second filter; a region without the new opcodes renders bit for bit as before (the voice copies the region's prepared coefficients, the same `sample_filter_tick` runs); the cutoff of a voice is `clamp(clamp(cutoff * 2^((keytrack * (note - keycenter) + veltrack * vel/127) / 1200)) * 2^(sum of cutoff_oncc depth * curve(cc) / 1200))` to 0..20000 Hz as sfizz does (the base is clamped before the CC factor), the resonance is `resonance + sum of resonance_oncc depth * curve(cc)` dB; the CC values are read once at voice start (pseudo-CCs 131, 135, 136, 137, 140, 141 as in Story 13.5); filters run in series, first then second; coefficients are computed in the render thread at voice start with no allocation, lock or syscall; non-finite or extreme values never reach the biquad (cutoff clamped to [1 Hz, min(20 kHz, 0.49 fs)], resonance to +-60 dB, Q >= 0.001); deterministic (no random draw is added).

**Never:** continuous (per-block) cutoff modulation, `*_stepcc`, `*_smoothcc`, `fil_random`, `fil_gain`, other filter types, EQ, LFO/EG matrix (see `deferred`); changing the Story 5.2 filter formulas or the Story 13.5 CC model; triggering CI; committing or pushing.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| keytrack | `cutoff=1000 fil_keytrack=100 fil_keycenter=60`, note 72 | cutoff 2000 Hz | none |
| keytrack below centre | same, note 48 | cutoff 500 Hz | none |
| veltrack | `cutoff=500 fil_veltrack=2400`, velocity 127 / 64 / 0 | 2000 Hz / ~1000 Hz / 500 Hz | none |
| velocity as CC | `cutoff=500 cutoff_oncc131=2400 cutoff_curvecc131=7` | 500 * 2^(2400 * curve7(vel/127) / 1200) Hz | unknown curve index: linear (sfizz) |
| CC at voice start | `cutoff_oncc74=1200`, CC74 = 0 / 127 | 1 x / 2 x the base cutoff, fixed for the life of the voice | none |
| resonance on a CC | `resonance=0 resonance_oncc1=12`, CC1 = 127 | Q = 10^(12/20) | none |
| clamp | `cutoff=18000 fil_veltrack=12000`, vel 127 | 20 kHz | none |
| second filter | `cutoff=2000 cutoff2=500 fil2_type=hpf_2p` | lpf_2p at 2000 then hpf_2p at 500, in series | none |
| no filter | `fil_veltrack=1200` or `cutoff_oncc1=100` without `cutoff` | region gets no filter, `ignored_opcode` warning (as Story 5.2) | warning |
| rejected | `fil_random`, `fil_gain`, `cutoff_stepcc1`, `fil3_type`, `fil_type=lpf_4p`, `cutoff2` without `cutoff` | `unsupported_features` / `unsupported_value` with file:line | file skipped |
| extreme | cutoff 1 Hz and 20 kHz, resonance +-60 dB, NaN/inf/subnormal inputs, 10^5 frames | finite output, no growth, no NaN | none |
| reader | minor 8; minor 7 with no filter record; a record holding the default | kUnsupportedVersion; kCorruptManifest; kCorruptManifest | none |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `FilterMod`, `FilterTune`, `Filter2`, `FilterMap` in `Region`, `has_filter_extension`, validation.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 7`; minor 7 appends the filter table after the minor 6 table.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp` -- opcode table, `cutoff[N]`/`resonance[N]`/`fil[N]_*` aliases, filter CC entries, curve and CC usage in `build_cc_setup`.
- `librarian/src/import.cpp`, `librarian/src/preflight.cpp` -- carried unchanged unless a message names the filter.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields.
- `engine/core/include/velora/core/{sample_bank,sample_engine,sample_filter}.hpp`, `src/sample_engine.cpp` -- per-voice coefficients (`start_voice`), second filter in `render_voice`, curve tables in `prepare`.
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/tests/sample_suite_test.cpp`, `tools/offline-render/CMakeLists.txt` -- one sfizz null case per opcode family and `strip138` negatives.
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/{sample_features,sample_engine,parallel_render,rtsan_sample_features}_test.cpp`, `engine/core/tests/sample_bench.cpp`, `engine/host/macos/main.cpp` (declares `second_filter_voices`), `engine/host/tests/sample_bundle_loader_test.cpp`, `librarian/tests/*`.
- Docs: `formatlib/README.md`, `engine/core/README.md`, `tools/offline-render/tests/sample_suite/README.md`, `_bmad-output/planning-artifacts/epics.md` (status note).

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add the filter table and minor 7 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes below, reject the rest with a reason -- importer.
- engine ports, loader, sample engine -- per-voice coefficients and the second filter -- playback.
- tests, sfizz fixtures and goldens, bench, READMEs -- coverage and docs; unit tests for the I/O matrix rows.

**Opcodes that land:** `cutoff`, `cutoff1`, `cutoff2`; `resonance`, `resonance1`, `resonance2`; `fil_type`, `filtype`, `fil1_type`, `fil2_type` (lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p); `fil_keytrack`, `fil1_keytrack`, `fil2_keytrack` (cents per key, 0..1200); `fil_keycenter`, `fil1_keycenter`, `fil2_keycenter`; `fil_veltrack`, `fil1_veltrack`, `fil2_veltrack` (cents at velocity 127, +-12000); `cutoff_oncc<N>`, `cutoff_cc<N>`, `cutoff_curvecc<N>` and the `cutoff1`/`cutoff2` forms (cents, +-12000); `resonance_oncc<N>`, `resonance_cc<N>`, `resonance_curvecc<N>` and the `resonance1`/`resonance2` forms (dB, 0..96). Controllers: 0..127 and the pseudo-CCs of Story 13.5. Everything else is rejected (see `deferred`).

**Acceptance Criteria:**
- Given a region with these opcodes, when a note starts, then the filter coefficients are computed per voice, in the render thread without allocation or locks, within the existing per-voice CPU budget (measured by `sample_bench`), and the NFR8 voice floors still hold in the rt-traffic and parallel-render tests.
- Given the null test against sfizz, when a fixture uses each opcode, then it passes within a recorded tolerance, and the filter's stability at extreme cutoff and resonance is tested including NaN and subnormal inputs.
- Given kamoepiano301 and Headroom Piano, when the corpus job runs, then they import and pass, or the manifest records the exact remaining opcode and reason. **NOT MET.** Neither imports; the exact remaining opcodes are in `deferred` and the Auto Run Result, `tools/corpus/manifest.json` is unchanged (its `skip_files` admits only out-of-scope tiers).
- Given the bundle format, when the fields are added, then minor 7 is written only when needed, minors 0..7 read, and existing bundles are byte-identical.

## Spec Change Log

Entries below are decisions taken while implementing, not review loopbacks.
- Added `Config::second_filter_voices` / `kSampleFilter2CostUnits` (3): the measured cost of a two-filter voice (about 9.5 to 9.9 units against a declared 8) did not fit the existing extended declaration, so the second filter is a separate per-engine declaration that leaves every existing floor and budget unchanged (see Design Notes).
- `sample_filter_tick` gained a finiteness guard on the new state (NaN, infinity or beyond 1e18 clears the state and answers silence) so a NaN or infinite sample cannot poison a filter for good.

## Review Triage Log

No review pass has run on this story yet (status in-review).

## Design Notes

The filter map is a separate minor 7 table rather than new `CcTarget` values: the minor 4 reader validates targets 0..11, so a bundle with filter targets in the cc table would have been a minor 4 bundle that a minor 4 reader misreads; a new table keeps every older bundle byte-identical and a newer bundle explicit. The curves and `set_cc` seeds that filter entries need reuse the instrument-level Story 13.5 setup (`build_cc_setup` counts them).

The voice owns its coefficients (`fk`, `fk2`) in every case: a region without a tuning copies the prepared ones, so the Story 5.2 arithmetic and the null tests are untouched, and a tuned region differs only in who computed the numbers.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (timing flakes `librarian_service_smoke`, `librarian_crash_campaign`, `ui_shell_test`, `stream_test`, `sysd_bridge_tests`, `reverb_bench`: re-run serially)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh <new cases>` -- expected: goldens written by the local sfizz oracle
- `build/engine/core/sample_bench` -- expected: prints the per-voice figures with one and two filters
- `python3 tools/corpus/corpus.py run --only pianobook-kamoepiano301 headroom-piano --out build/corpus138/kh` -- expected: reports whether they import
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see Story 13.7 deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 750 tests, 748 pass, 2 skipped (`librarian_demo_acceptance`, `sample_demo_null_test`, both need local demo libraries); no timing flake this run. RealtimeSanitizer variants are not built on this machine (clang < 20); the same traffic runs as `sample_features_traffic_test` with tuned and second-filter regions in it. Linux Docker not run (LVGL drift).

Landed: `cutoff`/`cutoff1`/`cutoff2`, `resonance`/`resonance1`/`resonance2`, `fil_type`/`filtype`/`fil1_type`/`fil2_type`, `fil_keytrack`, `fil_keycenter`, `fil_veltrack` (and `fil1_`/`fil2_` spellings), `cutoff[1|2]_oncc/_cc/_curvecc<N>`, `resonance[1|2]_oncc/_cc/_curvecc<N>` with controllers 0..127 and the Story 13.5 pseudo-CCs; format minor 7 (filter table, written only for a region with a tuning or a second filter, reader 0..7, older bundles byte-identical, tested plain and protected); per-voice coefficients computed at voice start (`SampleEngine::voice_filter`, sfizz `FilterHolder::setup` in float); second filter in series; `Config::second_filter_voices` (cost 11 for an extended Hermite engine; default and extended-only engines unchanged); finiteness guard in `sample_filter_tick`. Still rejected: see `deferred`. Tests that used minor 7 as "newer" now use minor 8.

sfizz null (tolerance -60 dB): `fil_keytrack` -61.3, `fil_veltrack` -66.9, `cutoff_oncc` -66.8, `resonance_oncc` -67.3, `filter2` -66.7 dB; the `strip138` negatives fail as required (-5.3, -2.4, -4.0, -3.6, 0.0 dB). `fil_keytrack` has the thinnest margin (worst 100 ms window -50.9 dB at the 96 key, 12 semitones above its region centre); not investigated beyond that.

CPU (sample_bench, host, -O3, stereo Hermite voice = 5 units = about 0.37 us per 64-frame block): first filter +2.2 units (declared 2; the Story 5.2 figure of 1.6 to 2.0 grew by about 0.3 with the finiteness guard), second filter +2.1 to 2.5, a tuned filter costs the same per block as an untuned one, starting a voice costs 0.03 us plain and 0.07 us with a tuned filter (coefficients: sin, cos, pow per filter, once per note). One filter + crossfade 7.3 to 7.7 units (declared 8); two filters + crossfade 9.5 to 9.9 (declared 11 with `second_filter_voices`). Not measured on the board.

NFR8 floors and rt-traffic: `voice_arbiter_tests` unchanged and passing (the default engine's cost is unchanged); `parallel_render_tests` has a new filter instrument (tuned, two filters, controllers 1/11/74 moving): 1 vs 3 threads bit-identical, serial within 1e-5 (measured 5.1e-7 mono, 3.7e-8 split), same voice table; `sample_features_traffic_test` 10,000 blocks plain and Live Set, deterministic, no hanging voices, `undeclared_second_filter_voices() == 0`.

Corpus (local caches, full `corpus.py run`, 12 libraries): VSCO2 still passes (75 of 75 files, 3163 regions, null tests -54.5 to -99.8 dB) and Manthey Klaviano still passes (153 regions, -71 to -89 dB), so regions without the new opcodes are unchanged. Not importing:
- kamoepiano301: NOT importing. `cutoff_oncc131`/`cutoff_curvecc131` and `cutoff` now import. Remaining: `sustain_cc` and `ampeg_release_shape` (outside Epic 13's story list) and `ampeg_dynamic=1` together with `ampeg_releasecc64` + `ampeg_release_curvecc64` (90 regions; the Story 13.5 rule that continuous envelope modulation is not implemented; the corpus histogram does not show this one because it counts only unsupported opcodes). On a scratch copy with those four opcodes removed for BOTH renderers, the instrument imports (90 regions) and 5 of the 6 corpus suites null-test at -94.2 to -104.2 dB (`range`, `velocity`, `round_robin`, `keyswitches`, `releases`); `sustained_loops` (notes held 8 to 10 s) fails at -54.9 dB, and fails at -55.0 dB with every cutoff/resonance/fil opcode also removed, so that failure is not the filter's (not investigated).
- Headroom Piano: NOT importing. `cutoff=40 fil_veltrack=11000` (Intimate Piano) now import. Remaining: `amp_veltrack_oncc73`, `width_oncc76`/`79` and `width_curvecc76`/`79`, `eq1_*`/`eq2_*` (EQ, Story 13.9).
- Others, unchanged in kind: Salamander `rt_decay`, `amp_veltrack_oncc99`; Steel Drum `xfin/xfout_lovel/hivel`, `width_oncc1`; Osiris `ampeg_release_shape`, `sustain_cc` and `..` map paths; Scarypiano `..` sample paths; Meatbass/Bear Sax/Weresax `eg06_*`, `lfoNN_*`, `offset_random`, `eq1_*`; Sonatina `delay`, `amplfo_*`, `offset_random`, `lfo01_*`. No `fil_*`, `cutoff*` or `resonance*` opcode of this story appears among any library's blockers any more; the filter-related ones left are `fileg_*`, `lfoNN_cutoff`, `var01_cutoff` (Story 13.9).

NOT MET by this story: the Epic 13 AC "kamoepiano301 and Headroom Piano import and pass, or the manifest records the exact remaining opcode and reason". Both still fail import, for the opcodes above; the reasons are recorded here and as a status note in `epics.md`, but `tools/corpus/manifest.json` was NOT changed: its `skip_files` rule admits only files needing a tier marked out of scope, and none of these tiers is. The other three ACs (per-voice coefficients at voice start within the declared budget with the floors intact, sfizz null per opcode plus extreme/NaN/subnormal stability, bundle minor 7 only when needed) are met.

Known limits and risks: the controller is read at the voice start only (sfizz follows it per block; fixtures move it before the note); a filter opcode without `cutoff` means no filter (sfizz runs it at cutoff 0) and `cutoff2` without `cutoff` is refused; for a controller-started voice the key track uses the region's `pitch_keycenter` where sfizz uses the controller number; a release-trigger voice uses the velocity the engine already uses for its amplitude (no sfizz null for filtered release voices); `second_filter_voices` raises the macOS host's per-voice declaration from 8 to 11 units (budget 2^20, no floor affected) and is not calibrated on the board; the finiteness guard adds about 0.3 units to every filtered voice.


## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: -0 normalisation for all filter floats, lone fil_keycenter no longer stamps minor 7 or takes the per-voice path, pseudo-CC 136/137/140/141 filter-entry tests, strip138 exact name matching, tests express format minors via kFormatMinor, README cost figures reconciled (guard adds ~0.3 unit; single-filter 2.2 exceeds declared 2; host declares 11 units/voice), deferred entries for unmet items.
- NOT MET: AC "kamoepiano301 and Headroom Piano import and pass, or the manifest records the exact remaining opcode and reason" (both still fail import; reasons in spec and epics.md note, manifest unchanged).
- Deferred: kamoepiano301 sustained_loops -54.9 dB (not the filter), fil_keytrack 1.3 dB null margin, release-trigger/controller voices with filters untested, CC read at voice start only, RTSan unbuilt, 4-entry cap unjustified, 11-unit declaration uncalibrated on the board.
- Verification: macOS ctest 750 tests pass (engine_host_library_smoke timing flake passes alone); sfizz null residuals -61.3..-67.3 dB vs -60 dB.

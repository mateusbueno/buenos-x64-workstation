---
title: 'Region EQ and LFO/EG modulation'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '4afaeb5d2ff76de43f28b69c306ebc8ab89d413d'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/planning-artifacts/sfz-modulation-design.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-5-cc-at-voice-start-and-sfizz-pseudo-ccs.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-8-per-voice-filter.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: ['oversized']
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story and the RealtimeSanitizer variant (`rtsan_sample_features_test`, clang >= 20) is not built on this machine: the render-thread modulation (LFO, EG, coefficient computation every 16 frames) is NOT RT-verified until it runs on Docker/Linux clang.
    evidence: |-
      Story 13.2 to 13.8 deferred entries; `sample_features_traffic_test` (10,000 blocks, plain and Live Set, with the new modulation regions) is the local stand-in.
    severity: medium
  - summary: >-
      Not implemented, still rejected at import with `unsupported_features` (file:line, never ignored): the generic `lfoNN_*` matrix (`lfoNN_freq`, `_wave`, `_phase`, `_delay`, `_fade`, `_count`, `_ratio`, `_scale`, `_offset`, steps and every target `lfoNN_pitch`, `_volume`, `_cutoff`, `_pan`, `_eqNfreq`, `_eqNgain`, `_eqNbw`, with `_oncc` depths and `lfoN_freq_lfoM`), the flex EG `egNN_*` (`eg06_time1_oncc109` alone is 2090 lines of Meatbass), `var01_mod`, CC-driven depth and frequency of the dedicated LFOs and EGs (`amplfo_depth_oncc`, `amplfo_freq_oncc`, `pitcheg_depth_oncc`, ...), `*_depthchanaft`, `*_freqchanaft`, the EG `vel2attack|decay|delay|hold|release|sustain` and `_oncc` times on `pitcheg_`/`fileg_`, `pitcheg_dynamic`, LFO `beats`, `phase`, `ratio`, `count`, wave 12 (random sample and hold: sfizz's global RNG), `eq4_*` and above, `eqN_*stepcc/smoothcc`, `eqN_*` as LFO/EG targets.
    evidence: |-
      Design note section 10; `build/corpus138/all/report.json` histograms (Meatbass, Bear Sax, Weresax `lfoNN_*`/`eg06_*`; Sonatina `lfo01_*`).
    severity: high
  - summary: >-
      The CC that drives an EQ band or the cutoff is read at voice start only (sfizz follows it per block); a controller that moves under a sounding voice does not move its band.
    evidence: |-
      Story 13.5 limit, restated; fixtures move controllers before the note.
    severity: medium
  - summary: >-
      Documented differences from sfizz 1.2.3 that are not modelled in production (the null fixtures avoid them): a voice that starts inside an sfizz block sees its pitch modulation late in that first block; an EG whose delay ends exactly at a block end is never released by sfizz (every EG with a delay at 1-frame blocks, the corpus oracle's setting); a decay ending a block inside the 0.001 threshold hands over to sustain early (emulated only under `oracle_block_frames`). With the corpus oracle (1-frame blocks) an `ampeg_delay` or `pitcheg_delay` > 0 may therefore make sfizz hold a voice that this engine releases.
    evidence: |-
      engine/core/README.md "Differences from sfizz" (Story 13.9); sample_suite README "Story 13.9".
    severity: medium
  - summary: >-
      Existing bug found, not fixed: the filtered branch of `render_voice` pans with the region's constant gains (`cl`, `cr`), so a `pan_oncc` on a region with a filter or an EQ band is ignored for the pan; the plain branch honours it.
    evidence: |-
      `render_voice` chain branch (`cl`/`cr`), `start_region` (`chan_l` from the pan entries only reaches `gain_l`).
    severity: low
  - summary: >-
      The 18-unit modulation declaration (29 units for the worst-case voice) is a literal from host figures (26.4 to 28.5 units measured, noise included; `sample_bench` now fails when the worst-case voice exceeds the declaration by more than 10 %) and is not calibrated on the board; it raises the macOS host's per-voice declaration from 11 to 29 units (budget 2^20, no floor affected).
    evidence: |-
      `build/engine/core/sample_bench`; `engine/host/macos/main.cpp`.
    severity: medium
  - summary: >-
      Corpus: no library imports completely after this story (see Auto Run Result). The Epic 13 AC "Sonatina and the Karoryfer libraries import and pass the null test, or the remaining differences are documented per opcode" is documented, not met.
    evidence: |-
      Corpus re-run in the Auto Run Result; `tools/corpus/manifest.json` unchanged.
    severity: high
  - summary: >-
      sfizz 1.2.3 quirks not reproduced in production (the null fixtures avoid them): a voice that starts inside a block sees its pitch modulation late in that first block; an EG whose delay ends exactly at a block end is never released; a decay that ends a block inside the 0.001 threshold hands over to sustain early (emulated only under `oracle_block_frames`). The pitch/filter EG release after a sample end is applied at the end of the same frame (the amplitude EG's first released value is at that frame, the modulation EG's at the next): one frame, not modelled further.
    evidence: |-
      engine/core/README.md "Differences from sfizz" (Story 13.9); sample_suite README "Story 13.9".
    severity: medium
  - summary: >-
      Per-block CC following is not implemented: a controller that moves under a held note does not move an EQ band, `cutoff_oncc`, or the base of a cutoff that an LFO/EG modulates (sfizz follows it per block).
    evidence: |-
      Story 13.5 limit; `init_mod` reads the controllers once.
    severity: medium
  - summary: >-
      Existing bug, not fixed: `pan_oncc` is ignored on a region that is filtered or has an EQ band (the chain branch of `render_voice` pans with the region's constant `cl`/`cr`).
    evidence: |-
      `render_voice` chain branch; `start_region` (`chan_l` from the pan entries reaches only `gain_l`).
    severity: low
  - summary: >-
      `Voice` grew by the `ModVoice` block (about 1 KB) and `*v = Voice{}` re-zeroes it at every voice start: `sample_bench` voice start (plain) went from 0.026 to 0.034 us on the host Mac (about 0.008 us per note on). Not reduced (a region without modulation could skip the block, which would need a restructure of `Voice`).
    evidence: |-
      `build/engine/core/sample_bench`, voice start lines before and after the story.
    severity: low
  - summary: >-
      Corpus blockers outside this story, found by the re-run: sample paths with `..` (746 of 747 Sonatina files, all Meatbass and Bear Sax, 12 of 13 Weresax are `damaged` first), FLAC samples (Headroom Piano), `delay`, `delay_random`, `offset_random` (Sonatina, Karoryfer), `lfoNN_*`/`egNN_*` (deferred above), `amp_veltrack_oncc73`, `width_oncc*`, `pitch_keytrack`, `pitch_veltrack`.
    evidence: |-
      build/corpus139/report.json and import.jsonl; Auto Run Result.
    severity: high
---

<intent-contract>

## Intent

**Problem:** Headroom Piano is blocked by `eq1_*`/`eq2_*`, Sonatina (747 files) by `amplfo_*` (256 files), `eq1_*`, `fileg_*` and `fillfo_*`, and the Karoryfer libraries by `lfoNN_*`, `eg06_*` and `eqN_*`; the engine has no region EQ, no LFO and no pitch or filter EG.

**Approach:** Taken up on measured need (the scope decision's rule, applied to the 13.8 corpus report; the decision record stays an unratified proposal). A design note against sfizz comes first; the story then lands a coherent subset with fixed targets: region EQ bands 1 to 3, the dedicated `amplfo_`/`pitchlfo_`/`fillfo_` LFOs, the `pitcheg_`/`fileg_` EGs. Everything else stays rejected and is recorded as deferred.

## Boundaries & Constraints

**Always:** the design note `_bmad-output/planning-artifacts/sfz-modulation-design.md` fixes the matrix, LFO phase, smoothing and sum rules (from sfizz 1.2.3) and the cost before the code; a bundle without the new table is byte-identical to before (minor 0..7), the writer stamps minor 8 only when a region has a `ModMap`, the reader accepts 0..8 and rejects a newer one; a region without the new opcodes renders bit for bit as before; modulation runs on the render thread without allocation or locks, deterministically (no random draw), and 1 versus 3 worker partitions are bit-identical; the clocks are the voice's own (a host block size cannot change the sound); an LFO or EG without a depth, and a filter LFO/EG on a region without `cutoff`, does nothing in sfizz and is dropped with a warning; non-finite or extreme values never reach a filter (clamps of the design note, the 13.8 finiteness guard).

**Never:** the generic `lfoNN_`/`egNN_` matrix, CC-driven depths/frequencies, continuous CC following, random sample and hold, `eq4`+ (see `deferred`); changing the Story 5.2 filter formulas, the Story 13.5 CC model or the amplitude EG's port; triggering CI; committing or pushing; editing the scope decision beyond its 13.9 rows and a short update section.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| EQ band | `eq1_freq=800 eq1_bw=1 eq1_gain=9` | peak biquad, +9 dB at 800 Hz, applied after the filters | none |
| default frequencies | `eq2_gain=-8` alone / `eq3_gain=10` alone / `eq1_freq=300 eq2_gain=3` | band 2 at 500 Hz / band 3 at 5000 Hz (bands before are transparent) / band 2 at 0 Hz clamped to 1 Hz (sfizz's positional rule) | none |
| EQ velocity and controller | `eq1_vel2gain=12`, `eq1_gain_oncc81=18` + curve | gain = base + vel * 12 + 18 * curve(CC), fixed for the voice | none |
| amplitude LFO | `amplfo_depth=6 amplfo_freq=4 amplfo_wave=0..7` | gain x 10^(6 * wave(phase) / 20), phase from 0, delay then fade | wave 12: `unsupported_value` |
| LFO without depth | `amplfo_freq=5` only | no LFO stored (sfizz connects it with depth 0) | warning |
| pitch EG | `pitcheg_depth=2400 pitcheg_attack=0.2 pitcheg_sustain=50` | cents = depth * ADSR, released with the amplitude EG | none |
| filter LFO/EG | `cutoff=800 fillfo_depth=1200 fillfo_freq=3`, `fileg_depth=2400` | cutoff x 2^(cents / 1200), recomputed every 16 frames, coefficients smoothed (1 ms) | without `cutoff`: dropped, warning |
| rejected | `lfo01_freq`, `eg06_time1_oncc109`, `amplfo_depth_oncc1`, `pitcheg_vel2attack`, `eq4_freq`, `eq1_type=notch`, a fifth `eq1_gain_oncc` | `unsupported_features` / `unsupported_value` with file:line | file skipped |
| extreme | depth 1200 cents at 100 Hz, EG +-12000 cents, bands at 1 Hz / 20 kHz, bw 0.001 / 4, gain +-96 dB, 10^4 frames | finite output, no growth | none |
| reader | minor 9; minor 8 with no record; a record holding the default map; an LFO stored without a depth | kUnsupportedVersion; kCorruptManifest; kCorruptManifest; kCorruptManifest | none |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `LfoSpec`, `EgSpec`, `EqBand`, `ModMap` in `Region`, `has_mod_extension`, `validate_mod`.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 8`; minor 8 appends the modulation table after the minor 7 table.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp`, `formatlib/README.md` -- opcode table (95 entries), `eq_opcode`/`lfo_opcode`/`eg_opcode`, `apply_mod`, EQ entries through the 13.5 CC machinery, default frequencies, curves/set_cc pruning.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- `SampleModDesc` and its mapping.
- `engine/core/include/velora/core/{sample_bank,sample_engine,sample_filter}.hpp`, `src/sample_engine.cpp` -- `ModVoice`, `init_mod`, `lfo_next`, pitch step correction, `env_next(mod)`, smoothed cutoff, `make_sample_eq`, `sample_filter_tick_smooth`, `Config::modulated_voices`, `kSampleModCostUnits`.
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/tests/sample_suite_test.cpp`, `tools/offline-render/CMakeLists.txt` -- seven sfizz null cases and the `strip139` negatives.
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/{sample_features,parallel_render,rtsan_sample_features}_test.cpp`, `engine/core/tests/sample_bench.cpp`, `engine/host/tests/sample_bundle_loader_test.cpp`, `engine/host/macos/main.cpp` (declares `modulated_voices`).
- Docs: `_bmad-output/planning-artifacts/sfz-modulation-design.md` (new), `sfz-scope-decision.md` (13.9 rows and an update section), `epics.md` (status note), `formatlib/README.md`, `engine/core/README.md`, the suite README.

## Tasks & Acceptance

**Execution:**
- design note -- fix the matrix, LFO/EG/EQ rules from sfizz, smoothing, sum rules, determinism, cost -- before any code.
- `formatlib` model, bundle, writer, reader -- modulation table, minor 8 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes below, reject the rest with a reason -- importer.
- engine ports, loader, sample engine -- per-voice LFO/EG/EQ, cutoff modulation, cost declaration -- playback.
- tests, sfizz fixtures and goldens, bench, READMEs -- coverage and docs.

**Opcodes that land:** `eq1`..`eq3` `_freq`, `_bw`, `_gain`, `_type` (peak, lshelf, hshelf), `_vel2freq`/`_veltofreq`, `_vel2gain`/`_veltogain`, and `_freq|_bw|_gain` + `_oncc<N>`/`_cc<N>`/`cc<N>`/`_curvecc<N>` (controllers 0..127 and the 13.5 pseudo-CCs, at most 4 per target and band); `amplfo_`, `pitchlfo_`, `fillfo_` `depth` (dB / cents), `freq`, `delay`, `fade`, `wave` 0..7; `pitcheg_`, `fileg_` `delay`, `start`, `attack`, `hold`, `decay`, `sustain`, `release`, `depth`, `vel2depth`/`veltodepth`. Everything else is rejected (see `deferred`).

**Acceptance Criteria:**
- Given the tier, when the story starts, then the design note and this spec record the scope decision on measured need and the evidence (done before the code; the decision record's 13.9 rows say so).
- Given regions with these opcodes, when they play, then modulation runs on the render thread without allocation or locks, deterministically, 1 versus 3 partitions are bit-identical, regions without the opcodes are bit-identical, extremes stay finite (tests below).
- Given the sfizz oracle, when each family is rendered, then it passes within the recorded tolerance (-60 dB; -63.9 to -67.4 dB measured) and the negatives fail, or the difference is documented per opcode (README).
- Given the bundle format, when the table is added, then minor 8 is written only when needed, minors 0..8 read, existing bundles are byte-identical, plain and protected.
- Given the corpus job, when Sonatina and the Karoryfer libraries run, then they import and pass the null test within recorded tolerances, or the remaining differences are documented per opcode. **NOT MET.** No library imports; the differences and the blockers are documented, which is not what this criterion asks for.
- Epic 13's AC "kamoepiano301 and Headroom Piano import and pass" is also **NOT MET** by this story (neither imports; Headroom Piano is blocked by `amp_veltrack_oncc73`, `width_oncc*` and FLAC samples). Both Epic 13 corpus ACs are NOT MET.

## Spec Change Log

Entries below are decisions taken while implementing, not review loopbacks.
- Scope cut to the dedicated LFOs/EGs and the EQ; the generic `lfoNN`/`egNN` matrix is deferred (CC-following per block, random S&H, the largest piece).
- Added `Config::modulated_voices` / `kSampleModCostUnits` (18) after measuring (first estimate 12 was low).
- The pitch/filter EGs use `env_next(mod)` (exact sfizz decay) instead of the amplitude EG's ramp; a cutoff that moves uses `sample_filter_tick_smooth` (sfizz's per-term coefficient timing). Both were needed to pass the null tests (-21 and -42 dB before).
- `parallel_render_test`'s block-loop case keeps its original (unmodulated) cost declaration: its 270-voice budget assumes the old figure.

- Review follow-ups: a trailing or inert EQ band, an LFO/EG that cannot affect the sound and a type/curve-only opcode are no longer stored (minor 8 only for a sound that changes); `validate_region` rejects a trailing default band; EQ validation is tightened to what the DSP honours (bw 0.01..4 octaves, gain -96..+60 dB; the importer clamps and logs); a mod EG released during its delay goes idle; the mod EGs are released again after `env_next`; `undeclared_extended_voices()` counts modulated regions; `mod_sane` rejects an LFO/EG that is on without a depth. A mod EG whose sustain is at or below the free-running threshold releases itself while the key is held: this is what sfizz does too (the same `ADSREnvelope` class: `freeRunning` is set in `reset` for all three EGs), confirmed by the `pitcheg` null fixture (key 65, `pitcheg_sustain=0`, -63.9 dB), so the engine keeps the switch.

## Review Triage Log

No review pass has run on this story yet (status in-progress).

## Design Notes

EQ entries reuse the Story 13.5/13.8 controller machinery (`FilterMod`, `cc_norm`, `cc_curve`, the pruned instrument setup) rather than new `CcTarget` values, for the reason 13.8 gave: the older readers validate targets. The minor 8 table is separate for the same reason. The pitch step correction (`ratio * (pitch_mul[f] - pitch_mul[f-1])` at frame f) is the sfizz `jump[0] = 0` + `cumsum` indexing; without it a one-octave EG attack ends a whole sample off.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: all pass (timing flakes: re-run alone)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh eq_bands eq_velocity eq_cc amplfo pitchlfo pitcheg fillfo_fileg` -- expected: goldens written by the local sfizz oracle
- `build/engine/core/sample_bench` -- expected: per-voice figures with the modulation lines
- `python3 tools/corpus/corpus.py run --out build/corpus139` -- expected: reports which libraries import
- Linux Docker -- not run (broken at HEAD by unrelated LVGL drift).

## Auto Run Result

Status: implemented, awaiting review (spec status left `in-progress` as instructed). macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 764 tests, 762 pass, 2 skipped (`librarian_demo_acceptance`, `sample_demo_null_test`: need local demo libraries); no timing flake this run (the full run takes about 3 minutes on this machine). RealtimeSanitizer variants are not built here (clang < 20); `sample_features_traffic_test` runs the same traffic. Linux Docker not run (LVGL drift).

Landed: the opcodes listed under Tasks; format minor 8 (modulation table, written only for a region with a `ModMap`, reader 0..8, older bundles byte-identical, tested plain and protected, corruption and bit-flip tests); `eq1`..`eq3` bands (peak, low and high shelf; velocity and controller entries read at voice start; sfizz's positional default frequencies); `amplfo`/`pitchlfo`/`fillfo` (waves 0..7, delay, fade, parabolic sine as sfizz) and `pitcheg`/`fileg` (the ADSR, depth and vel2depth, released with the amplitude EG), per-voice state, voice-local clocks, 1 versus 3 partitions bit-identical (`parallel_render_tests`), regions without the opcodes bit-identical, extremes finite. `Config::modulated_voices`, `kSampleModCostUnits` = 18 (29 units for the worst-case voice; measured 26.4 to 27.3).

sfizz null (tolerance -60 dB, local oracle 1.2.3): `eq_bands` -66.8, `eq_velocity` -67.4, `eq_cc` -67.3, `amplfo` -64.9, `pitchlfo` -64.7, `pitcheg` -63.9, `fillfo_fileg` -67.0 dB; the `strip139` negatives fail as required (-6.6, -6.7, -8.2, -7.6, +2.5, +2.8, -4.2 dB). Per-opcode differences from sfizz that the fixtures avoid are in `engine/core/README.md` ("Differences from sfizz", Story 13.9) and the `deferred` list.

Corpus (`python3 tools/corpus/corpus.py run --out build/corpus139`, 12 libraries, local caches): VSCO2 still passes (75 of 75) and Manthey Klaviano still passes, so unchanged regions are unchanged; every other library still fails import, as before. What this story changed in their histograms (`build/corpus138/all` to `build/corpus139`): `eq1_*`/`eq2_*` are gone from Headroom Piano, Sonatina (86 files) and the Karoryfer libraries; `amplfo_*` (256 files), `fileg_*`, `fillfo_*` are gone from Sonatina; `pitcheg_depth_oncc`/`pitcheg_delay_oncc116` (CC-driven, deferred) now show in Bear Sax. What still blocks, honestly:
- Headroom Piano (4 files): `amp_veltrack_oncc73`, `width_oncc76`/`79` and `width_curvecc76`/`79`; and, found with a scratch copy that has those removed, its samples are FLAC (`unsupported audio: not a WAV file`), so it cannot import until the importer reads FLAC.
- Sonatina (747 files): 746 of them are `damaged`, not `unsupported_features`: the sample paths use `..` (`../Samples-looped/...`, 744 files; 190 of the 747 are `includes/*.sfz` fragments the corpus job lists as files) and the importer refuses a path that leaves the SFZ's directory. The 13.8 report already showed this (746 damaged) but its opcode histogram hid it. Behind that: `delay` (536 lines), `offset_random`, `delay_random`, `lfo01_*` (86 files, generic LFO, deferred), `xfin_lovel` and friends, `width`, `group_tune`, `pitch_veltrack`.
- Meatbass (39), Bear Sax (48), Weresax (12 of 13): `damaged` for the same `..` path reason, plus `lfoNN_*`, `eg06_*` (flex EG, deferred), `offset_random`, `pitch_keytrack`, `var01_mod`.
- kamoepiano301, Osiris, Salamander, Steel Drum, Scarypiano: unchanged (Story 13.8's list).
So the Epic 13 ACs "Sonatina and the Karoryfer libraries import and pass the null test, or the remaining differences are documented per opcode" and "kamoepiano301 and Headroom Piano import and pass" are NOT MET; the per-opcode differences and the remaining blockers are documented here. `tools/corpus/manifest.json` is unchanged (its `skip_files` admits only out-of-scope tiers).

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patches sent: pitch-modulation headroom test with streaming/integrity, parallel test compares modulated_voices false vs true, mod-EG release in delay/choke/steal, extended-declaration accounting, canonical form (no sound-neutral bands/entries, trailing default bands rejected), EQ range agreement, weaker assertions tightened, deferred entries and NOT MET status.
- Note: the implementer's fix-round report did not reach the orchestrator; the orchestrator verified the working tree (build + full ctest) and spot-checked that the integrity test and parallel-test changes landed, but did not read each fix.
- NOT MET: both Epic 13 corpus ACs (no library imports; real blockers are '..' sample paths, FLAC samples, delay, offset_random, lfoNN/egNN, amp_veltrack_oncc73, width_oncc).
- Verification: macOS ctest 764 tests pass (librarian_crash_campaign timing flake passes alone); sfizz null residuals -63.9..-67.4 dB vs -60 dB; RTSan and Linux Docker unbuilt.

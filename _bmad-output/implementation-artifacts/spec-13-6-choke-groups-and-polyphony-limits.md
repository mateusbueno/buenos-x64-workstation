---
title: 'Choke groups and polyphony limits'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '5b76d5395e9927e560756550ed1a52cb3f2099d4'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-3-sample-end-first-legato-triggers-and-release-key.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-5-cc-at-voice-start-and-sfizz-pseudo-ccs.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: ['oversized']
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story.
    evidence: |-
      Story 13.2 to 13.5 deferred entries.
    severity: medium
  - summary: >-
      Not implemented here, still rejected with `unsupported_features`/`unsupported_value`: `output` (routing to other buses; the engine is stereo-only), a `polyphony` in a `<global>`/`<master>`/`<group>` header without a non-zero `group` set at that header or a broader one (it would limit the header's whole region set: sfizz `RegionSet`; `unsupported_value` on `polyphony` with the opcode's file:line), `polyphony=0`, `group`/`off_by` beyond the int32 range, `off_mode` other than fast/normal/time, `note_selfmask` other than on/off. `polyphony_group`, `offby`, `offmode` are accepted aliases.
    evidence: |-
      sfizz Region.cpp / Synth.cpp (handleMasterOpcodes, handleGlobalOpcodes, handleGroupOpcodes), VoiceManager.cpp read for this story.
    severity: low
  - summary: >-
      sfizz dispatches a synthetic note off for a choked attack voice (Synth::checkOffGroups -> noteOffDispatch), which starts that note's trigger=release regions. The engine does not: a library with release samples under a choke group plays one release sample fewer per cut. Not covered by a null fixture.
    evidence: |-
      sfizz Synth.cpp lines 1312-1321 (checkOffGroups) and 1349-1366 (noteOffDispatch); engine/core/README.md "Choke groups and polyphony limits".
    severity: medium
---

<intent-contract>

## Intent

**Problem:** `group`, `off_by`, `off_mode`, `off_time`, `polyphony` and `note_polyphony` make the importer reject a file as `unsupported_features`, so hi-hats, noise releases (Salamander's pedal noises, Osiris, Meatbass, Bear Sax, Sonatina) and mono instruments cannot import, and a voice can never cut another.

**Approach:** Carry a per-region voice-group record (choke group, `off_by`, cut mode and time, region / group / note polyphony limits, `note_selfmask`) through the model, the bundle (format minor 5) and the engine. The engine applies sfizz's order when a region is about to start a voice: choke groups first, then note, region and group polyphony; a cut is a release of the victim voice (and the voices of its note on) with the victim's `off_mode`, in the shared voice pool, so the arbiter's stealing is unchanged.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0 to 4 as today); the reader accepts minor 0..5 and rejects a newer one; the writer stamps minor 5 only when a region has a non-default voice-group record; a bank in which no region has an `off_by` or a limit renders bit-identically to before and runs none of the new code (`SampleBank::uses_voice_groups`); all cuts happen in the control phase (note on, key up, note off), never in `render`, so the 3-partition parallel render sees the same voice table as the serial one; "oldest" is the voice start order (`Voice::order`), never a slot index, a pointer or a hash; ties are broken by start order; no allocation, lock or syscall (fixed loops over the voice table, a scratch bit in the voice); a cut does not wait for the sustain pedal; voices fired by a note off are never cut; a voice that has been cut is not cut or counted again; deterministic.

**Never:** region-set polyphony (`RegionSet`), `output`, the synthetic note off sfizz sends for a choked voice, per-block follow-up of anything, triggering CI, committing or pushing; changing the velocity law, the Story 13.3 to 13.5 behaviour or the arbiter's stealing rules.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| hi-hat cut, fast | open hat `group=1 off_by=2`, closed hat `group=2` starts while the open hat rings | open hat envelope = exp(-9 n / (0.006 fs)) from the closed hat's note on, whatever its `ampeg_release` | none |
| cut, normal / time | `off_mode=normal` / `off_time=0.02` | the voice's own release / 20 ms; times below 6 ms behave as 6 ms | none |
| self-choke | `group=3 off_by=3` on two notes | another note of the group cuts the voice; a retrigger of the same note does not (its gates still hold) | none |
| ringing / pedal-held voice | voice already released by its note off, or held by CC64 | cut at once with the cut time; the pedal's later note off finds nothing to do | none |
| release-triggered voice | `trigger=release` / `release_key` voice | never cut by `off_by`; counts for polyphony | none |
| group polyphony | `group=5 polyphony=2`, three notes | the third note releases the oldest voice (and its sister voices from the same note on) | none |
| note polyphony | `note_polyphony=1`, same note again | equal or louder retrigger releases the previous voice; a quieter one does not (`note_selfmask=on`); `note_selfmask=off`: oldest goes | none |
| ordering | `note_polyphony=2`, velocities 100, 60, 110 | the quietest older voice goes | none |
| region set | `<group> polyphony=2` without a group number, `<global> polyphony` without `group`, a region that leaves the header's group | file rejected: `unsupported_value` on `polyphony`, file:line of the opcode | `<global> group=1 polyphony=1` (Salamander) and `<master> group=N polyphony=P` are accepted as the group's limit |
| out of range | `polyphony=0`; `polyphony=999`; `group=9999999999`; `off_mode=slow` | `unsupported_value`; ignored with an `ignored_value` entry (no limit); `unsupported_value`; `unsupported_value` | none |
| reader | minor 6; minor 5 with no voice-group record; a record holding the default | kUnsupportedVersion; kCorruptManifest; kCorruptManifest | none |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `OffMode`, `VoiceGroup` in `Region`, `has_vg_extension`, validation (ranges, `off_time` zero unless `kTime`, -0.0, limits at most 255).
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 5`; minor 5 appends the voice-group table (28-byte records) after the minor 4 tables.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp` -- opcode table, `group`/`off_by`/`off_mode`/`off_time`/`polyphony`/`note_polyphony`/`note_selfmask` and aliases, header-level tracking of `group` and `polyphony` for the region-set rejection, per-group limit resolution (`resolve_group_polyphony`).
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields.
- `engine/core/include/velora/core/sample_bank.hpp`, `sample_engine.hpp`, `src/sample_engine.cpp` -- `uses_voice_groups`, `off_rate`, `Voice::{offed, mark, order, event}`, `apply_voice_limits`, `off_voice`, `off_sisters`, call sites in `start_voice`, `key_released` and `stop_voice_impl`.
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/tests/sample_suite_test.cpp`, `tools/offline-render/CMakeLists.txt` -- three sfizz null-test cases and their `strip136` negatives.
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/{sample_engine,parallel_render,rtsan_sample_features}_test.cpp`, `engine/host/tests/sample_bundle_loader_test.cpp`.
- Docs: `formatlib/README.md`, `engine/core/README.md`, `tools/offline-render/tests/sample_suite/README.md`.

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add `VoiceGroup` and minor 5 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes, reject region-set polyphony, resolve the group limits -- importer.
- engine ports, loader, sample engine -- choke, cut modes, note / region / group polyphony, sister voices.
- tests, sfizz fixtures and goldens, READMEs -- coverage and docs; the unit tests for the I/O matrix rows.

**Acceptance Criteria:**
- Given a region with `off_by` matching another region's `group`, when the second region starts, then the first group's voices are released with the fast or normal release named by `off_mode` and `off_time`, in the shared voice pool and under the existing voice-stealing rules, and a test covers the cut, the fast release time and the interaction with the sustain pedal.
- Given `polyphony` and `note_polyphony`, when the limit is reached, then the oldest voice of that group or note is released, deterministically, with no allocation in the render thread, and parallel voice render (3 partitions) is bit-identical to 1 thread and within the repo's 1e-5 contract of the serial render.
- Given the null test against sfizz, when a fixture uses groups, then it passes, or the divergence of sfizz's stealing and `off_mode` timing is documented with evidence.
- Given the bundle format, when the fields are added, then minor 5 is written only when needed, minors 0..5 read, and existing bundles are byte-identical.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (timing flakes `librarian_service_smoke`, `librarian_crash_campaign`, `ui_shell_test` re-run serially)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh choke_groups choke_sustain polyphony_limits` -- expected: goldens written by the local sfizz oracle
- `python3 tools/corpus/corpus.py run --only osiris-piano --max-null 0 --out build/corpus136/osiris-piano` -- expected: reports which libraries import now
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 732 tests, 100% pass (2 skipped: `librarian_demo_acceptance`, `sample_demo_null_test`, both need local demo libraries). The first full run showed `librarian_install_test` and `librarian_install_service_test` failing because they used format minor 5 as "newer than the reader"; both now use minor 6 (as do `bundle_test` and `protected_test`). RealtimeSanitizer variants (`rtsan_*`) are not built on this machine (clang < 20); the same traffic runs as `sample_features_traffic_test` with the new cuts and limits in it. Linux Docker not run (LVGL drift).

Landed: `group` (`polyphony_group`), `off_by` (`offby`), `off_mode` (`offmode`: fast, normal, time), `off_time` (implies time), `polyphony`, `note_polyphony`, `note_selfmask`; format minor 5 (28-byte records, written only for a region with a non-default `VoiceGroup`; every older bundle byte-identical, tested for minors 0..4 content under every writer ceiling); engine cut and limits in sfizz's order, sister voices, `note_selfmask`. Still rejected: see `deferred` (`output`, region-set `polyphony`, `polyphony=0`).

sfizz null residuals (tolerance -60 dB): choke_groups -62.5, choke_sustain -62.6, polyphony_limits -64.4; the `strip136` negatives fail as required (-0.8, -7.4, -2.8 dB). The cut shape (fast 6 ms, normal, time), the sustain-pedal cut, the oldest-voice choice, sister voices and the `note_selfmask` ordering all agree with sfizz at the oracle's 64-frame block size; no divergence of sfizz's stealing or `off_mode` timing was found beyond the synthetic note off listed in `deferred`. The first version of `choke_groups` retriggered a looped note exactly half-period aligned, so the two voices cancelled in both renderers; the shipped timing avoids that (README).

Parallel render: 1 and 3 threads are bit-identical with 320 voices of churn through cuts and limits (mono and split), the voice table (which slots are alive, state, note) is identical after every block in serial and both parallel modes, and an instrument whose limits never apply is bit-identical to one without them (serial and 3 threads). The audio of the parallel render equals the serial render within the repository's existing 1e-5 tolerance (measured 2.2e-7), NOT bit for bit: the parallel path sums partition buffers and the serial path accumulates voice by voice (float rounding of the sums, ~4e-9 with three voices). The AC wording "bit-identical to the serial result" is therefore met for the control state and for 1-vs-3 threads, not for the sample-exact audio, which was never the contract of Story 1.18.

Corpus (local caches, `corpus.py run --only <id> --max-null 0`, import only, no null runs): two of the twelve cached libraries import completely and none of the others does yet: VSCO2 (75 of 75 files, 3163 regions) and Manthey Klaviano (1 of 1, 153 regions), both unchanged from Story 13.5 (neither uses the new opcodes). `group`, `off_by`, `off_mode`, `off_time`, `note_polyphony` and `polyphony` no longer appear among the blockers of any of them. Osiris (74 files): `sustain_cc` and `ampeg_release_shape` (Programs), `..` sample paths in included map files, and, newly visible, the Story 13.5 rule "`ampeg_dynamic=1` with `ampeg_*_oncc` is rejected" (2110 regions in `01-natural.sfz` alone: it is an `unsupported_value`, so the corpus histogram, which lists opcodes only, does not show it). Meatbass, Bear Sax, Weresax: LFOs (`lfoN_*`), `egNN_time1_oncc` (EG matrix), `eqN_*`, `bend_up`/`bend_down`, `offset_random`, `var01_*`, `fil_keytrack`/`pitch_keytrack`. Salamander: `rt_decay`, `on_locc64`/`on_hicc64` (Story 13.7), `amp_veltrack_oncc99`/`curvecc99`; its `<global> group=1 polyphony=1` pedal-noise pattern now imports. Scarypiano: `bend_up`/`bend_down` only (13.7). kamoepiano301: `cutoff_oncc131`/`cutoff_curvecc131` (13.8), `ampeg_release_shape`, `sustain_cc`. Steel Drum: `xfin/xfout_lovel/hivel`, `width_oncc/curvecc`. Headroom: `amp_veltrack_oncc73`, `width_oncc/curvecc`, `eq1/2_*`, `fil_veltrack`. Sonatina (747 files): `fil_*`, `delay`, `amplfo_*`, `cutoff_cc`, `ampeg_dynamic` with `_oncc` and others.

NOT MET by this story: the Epic 13 AC "kamoepiano301, Scarypiano, Osiris and Steel Drum import" (13.5) is still open; for Osiris, `group`/`note_polyphony` are no longer in the way, but `sustain_cc`, `ampeg_release_shape`, `..` map paths and the `ampeg_dynamic` + `ampeg_*_oncc` rejection are.

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: header polyphony hidden by a deeper one rejected, off_time=0 uses the 6 ms floor, off_sisters skips already-cut voices and the dead `fast` parameter is removed, release-triggered voices tests (never cut; release regions limited), group_polyphony without group rejected, loader static_asserts, note_selfmask=mask documented, epics AC reworded to what ships.
- Deferred: sisters release with their own region's off_mode (sfizz uses the choker's?); off_mode=normal with an engaged part_release knob untested; no synthetic note-off for choked voices (release samples per cut, medium; no owning story); apply_voice_limits worst-case cost unmeasured; RTSan and Linux Docker unbuilt (LVGL drift).
- Verification: macOS ctest 732 tests pass (stream_test and librarian_service_smoke failed once under -j 8, passed on rerun); sfizz null residuals -62.5, -62.6, -64.4 dB vs -60 dB.

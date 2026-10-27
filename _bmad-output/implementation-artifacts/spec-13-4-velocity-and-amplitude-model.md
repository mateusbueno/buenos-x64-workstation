---
title: 'Velocity and amplitude model'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '17825c70620844f9179552e5ecc1f216d6c2f334'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-2-importer-value-rules-and-inert-opcodes.md', '{project-root}/_bmad-output/implementation-artifacts/spec-13-3-sample-end-first-legato-triggers-and-release-key.md', '{project-root}/formatlib/README.md', '{project-root}/engine/core/README.md']
warnings: []
deferred:
  - summary: >-
      Linux Docker gcc build is broken at HEAD by LVGL drift in ui/src/adapters/drm_adapter.cpp (unrelated, left alone by instruction); Linux is not run for this story.
    evidence: |-
      Story 13.2 and 13.3 deferred entries.
    severity: medium
  - summary: >-
      Opcodes of the same families that this story does not implement: `amp_veltrack_oncc<N>`, `amp_veltrack_curvecc<N>`, every other `*_oncc`/`*_curvecc` form (Story 13.5, CC at voice start); `amp_random`, `ampeg_vel2delay`, `ampeg_vel2hold`, `ampeg_vel2depth`, the SFZ 1 aliases `ampeg_veltoattack`/`veltodecay`/`veltosustain`/`veltorelease`, velocity crossfades `xfin_lovel`/`xfin_hivel`/`xfout_lovel`/`xfout_hivel` and `xf_velcurve`, `offset_random`, `delay`, `rt_decay` (tier 2 of the scope decision names offset/delay/velocity crossfades, but the story's own opcode list does not; they stay rejected with `unsupported_features` until a story takes them).
    evidence: |-
      Story 13.4 text lists amp_veltrack, amp_velcurve_N, amplitude, group_volume, master_volume, ampeg_vel2attack/decay/sustain/release, amp_keytrack/amp_keycenter. sfz-scope-decision.md tier 2 and tier 4.
    severity: low
---

<intent-contract>

## Intent

**Problem:** The importer rejects `amp_veltrack`, `amp_velcurve_N`, `amplitude`, `group_volume`, `master_volume`, `ampeg_vel2*` and `amp_keytrack`/`amp_keycenter`, and the sample engine has one fixed touch response (gain = velocity squared), so libraries such as Meatbass and Bear Sax (`amp_velcurve_N`) and Sonatina (`group_volume`) cannot import or play as authored.

**Approach:** Carry a per-region amplitude map (veltrack, sparse velocity curve, amplitude, key track, four `ampeg_vel2*` scalars) through the model, the bundle (format minor 3, additive amp table) and the engine; evaluate it once at note-on, as sfizz does (`noteGain`/`velocityCurve`, `EGDescription::get*`). `group_volume`/`master_volume`/`global_volume` fold into the region's `volume_db` and the `*_amplitude` opcodes into `amplitude` at import (sfizz sums them the same way), so they cost no new bundle fields.

## Boundaries & Constraints

**Always:** a bundle without the new fields is byte-identical to before (minor 0, 1 or 2 as today); the reader accepts minor <= 3 (N, N-1 and older), rejects newer; a region without any of the new opcodes renders bit-identically (the old `1 - (1 - v*v)` expression and the old envelope numbers are used as they were); the mapping is evaluated at note-on only; no allocation, lock or syscall in the render thread (the velocity curve is a 128-entry table built in `prepare`); deterministic (no RNG draw added).

**Never:** CC modulation of any of these (`*_oncc`, Story 13.5); `amp_random`, `ampeg_vel2delay/hold/depth`, velocity crossfades, `offset_random`, `delay` (see `deferred`); change the default velocity-squared law; trigger CI; commit or push.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| default touch | no opcode | gain = (vel/127)^2, as before, bit for bit | none |
| veltrack 0 | `amp_veltrack=0` | gain 1 at every velocity | none |
| veltrack negative | `amp_veltrack=-100` | gain = 1 - (vel/127)^2: loud at low velocity | none |
| velcurve points | `amp_velcurve_1=0.2 amp_velcurve_127=1 amp_velcurve_64=0.9` | table of 128 entries, ends default to (0,0) and (127,1), gaps linearly filled, linear interpolation at `vel` (sfizz) | none |
| velcurve value out of 0..1 | `amp_velcurve_64=1.5` | clamped to 1, recorded in `ignored_values` | none |
| velcurve N above 127 | `amp_velcurve_200=1` | ignored (sfizz drops it), recorded in `ignored_values` | none |
| more than 32 curve points | 33 distinct N | `unsupported_value` on `amp_velcurve_<N>` | file rejected as unsupported_features |
| amplitude | `amplitude=50` | gain x 0.5 (`*_amplitude` multiply) | none |
| volumes sum | `volume=-3 group_volume=-3 master_volume=+1` | `volume_db` = -5 (sum clamped to -144..48) | none |
| vel2 scalars | `ampeg_vel2attack=0.1` at vel 127 | attack = ampeg_attack + 1.0 * 0.1; vel2sustain in percent, result clamped 0..100 % | none |
| amp_keytrack | `amp_keytrack=1 amp_keycenter=60`, note 72 | + 12 dB | none |
| reader, newer minor | version word minor 4 | kUnsupportedVersion | kUnsupportedVersion |
| minor 3 without a record | version word minor 3, empty amp tables | kCorruptManifest (one encoding per content) | kCorruptManifest |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/instrument_model.hpp`, `formatlib/src/instrument_model.cpp` -- `AmpMap` (veltrack, amplitude, keytrack, keycenter, sparse curve, vel2 scalars) in `Region`, `has_amp_extension`, validation.
- `formatlib/include/velora/format/bundle.hpp`, `formatlib/src/layout.hpp`, `writer.cpp`, `reader.cpp` -- `kFormatMinor = 3`; minor 3 appends a variable-length amp table after the minor 2 trigger/end table.
- `formatlib/src/sfz_import.cpp`, `formatlib/include/velora/format/sfz_import.hpp` -- the opcodes, folding of group/master/global volume and amplitude, opcode table, `ampeg_dynamic` note.
- `engine/ports/include/velora/ports/sample_source.hpp`, `engine/host/src/sample_bundle_loader.cpp` -- descriptor fields and mapping.
- `engine/core/include/velora/core/sample_bank.hpp`, `sample_engine.hpp`, `src/sample_engine.cpp` -- per-region constants and the 128-entry curve table at `prepare`; note-on evaluation in `start_region`; per-voice envelope constants (`Env::decay_rate/sustain/free_running`).
- `tools/offline-render/tests/sample_suite/{generate_suite.py,cases.txt,sfz,midi,golden,README.md}`, `tools/offline-render/CMakeLists.txt` -- one sfizz null-test case per opcode family (goldens rendered by the local sfizz oracle).
- Tests: `formatlib/tests/{sfz,bundle,protected}_test.cpp`, `engine/core/tests/sample_engine_test.cpp`, `engine/host/tests/sample_bundle_loader_test.cpp`, `librarian/tests/*`.
- Docs: `formatlib/README.md`, `engine/core/README.md`.

## Tasks & Acceptance

**Execution:**
- `formatlib` model, bundle, writer, reader -- add `AmpMap` and minor 3 -- storage.
- `formatlib/src/sfz_import.cpp` -- accept the opcodes, fold volumes and amplitudes -- importer.
- engine ports, loader, sample engine -- velocity-to-gain mapping, key track, vel2 envelope scalars at note-on.
- tests, sfizz fixtures and goldens, READMEs -- coverage and docs.

**Acceptance Criteria:**
- Given regions using `amp_veltrack`, `amp_velcurve_N`, `amplitude`, `group_volume`, `master_volume`, when they are imported, then each region carries a velocity-to-gain mapping evaluated at note-on that replaces the fixed velocity-squared gain only where the opcodes ask for it, and a region without the opcodes renders bit-identically to before.
- Given `ampeg_vel2attack`, `vel2decay`, `vel2sustain`, `vel2release` and `amp_keytrack`/`amp_keycenter`, when a note starts, then the scalar is applied at note-on, deterministically and without allocation or locks in the render thread, and a null test against sfizz passes for a fixture using each opcode.
- Given the bundle format, when the fields are added, then `format_version` minor is bumped to 3, minors 3 and 2 (and older) are readable, and every existing demo bundle opens and renders bit-identically.

## Verification

**Commands:**
- `cmake --build build -j 8` -- expected: success
- `ctest --test-dir build -j 8` -- expected: 100% pass (two tests skip)
- `tools/offline-render/tests/sample_suite/regenerate_goldens.sh <new cases>` -- expected: goldens written by the local sfizz oracle
- Linux Docker (`tools/ci/docker-check.sh`) -- not run: broken at HEAD by unrelated LVGL drift (see deferred).

## Auto Run Result

Status: implemented, awaiting review. macOS `cmake --build build -j 8` and `ctest --test-dir build -j 8`: 713 tests (708 + five `sample_null_amp*` cases; the five `sample_null_neg_strip134_*` negatives are registered on top), all pass except `librarian_crash_campaign`, which is timing-flaky (a statistical check on where SIGKILLs land: it failed twice with "svc_tally.none >= 2" and passed on the third run, unrelated to this story); two skipped (librarian_demo_acceptance, sample_demo_null_test). Linux Docker not run (LVGL drift, see deferred).

Oracle: sfizz 1.2.3 was available locally (`tools/oracle/.work`), so the goldens were rendered by it, not derived analytically. Null residuals against sfizz: amp_veltrack -68.0 dB, amp_velcurve -67.9, amp_gain -67.4, ampeg_vel2 -64.5, amp_keytrack -68.1 (tolerance -60 dB, oracle floor about -68 dB).

Format: minor 3 = minor 2 layout (tables present, possibly empty) + variable-length amp table. Bundles without amp fields keep minor 0/1/2 and their bytes (existing goldens unchanged and passing). The reader accepts minor 0..3, minor 4 is `kUnsupportedVersion`.

Notes: `ampeg_dynamic` stays allow-listed: with `ampeg_vel2*` alone the flag changes nothing (trigger velocity is constant), shown by the `ampeg_vel2` null fixture (one region sets `ampeg_dynamic=1`); the earlier comment and the Story 13.2 AC text in `epics.md` (which said vel2* must remove the allow-listing) are superseded by that evidence, `epics.md` itself was not edited. Tests that used `amp_veltrack` as their example of an unsupported opcode now use `amp_random`. Limits: at most 32 `amp_velcurve_N` points per region (more is `unsupported_value`); `amp_keytrack`, vel2 scalars and curve values out of sfizz's range are clamped and recorded in `ignored_values`; `global_*`/`*_amplitude`/`global_volume` were added with the same mechanism although the story lists only group/master volume.

## Review Triage Log (orchestrator)

### 2026-10-27 — Review pass
- Three layers (blind, edge-case, verification-gap). Patched: finite-gain clamps, part attack/release override tests, protected minor-3 round trip, loader field mapping test, zero-padded amp_velcurve_N rejection, inherited amp_keycenter kept on bad value, -0.0 normalisation, spec/README/epics text, generate_suite args.
- Deferred: amp curve table dedupe per region; release velocity for trigger=release voices; dead fractional interpolation in vel table; 32-point curve cap vs sfizz 128; Linux Docker unbuilt (LVGL drift); global_volume/global_amplitude not oracle-tested.
- Verification: macOS ctest 718 tests; librarian_service_smoke and librarian_crash_campaign failed once under -j 8 and passed serially (timing flakes); sfizz null residuals -64.5..-68.1 dB vs -60 dB tolerance.

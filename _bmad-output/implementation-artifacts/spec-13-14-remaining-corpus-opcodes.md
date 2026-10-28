---
title: 'Remaining corpus opcodes'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: '3fff84c'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/epic-13-retro-2026-10-28.md', '{project-root}/formatlib/src/sfz_import.cpp', '{project-root}/formatlib/README.md']
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Even after sample path normalization (Story 13.12) and FLAC support (Story 13.13), several corpus libraries fail import due to unsupported SFZ opcodes identified in the Epic 13 retro:
- `delay`: note onset delay (Sonatina staccato instruments)
- `offset_random`: random sample offset (Bear Sax, Sonatina)
- `sustain_cc`: pedal re-routing/mapping (kamoepiano301, Osiris, Sonatina, Celeste)
- `ampeg_release_shape`: envelope curvature (kamoepiano301, Osiris, Sonatina)
- `xfin_lovel`, `xfout_lovel`, `xfin_hivel`, `xfout_hivel`: velocity crossfades (Steel Drum, Sonatina)
- `width`, `width_oncc<N>`, `width_cc<N>`, `width_curvecc<N>`: stereo width modulation (Steel Drum, Sonatina)
- `rt_decay`: release trigger decay (Salamander)
- `amp_veltrack_oncc<N>`, `amp_veltrack_cc<N>`, `amp_veltrack_curvecc<N>`: velocity tracking modulation (Salamander, Headroom)

**Approach:**
1. Map `delay` to `r.ampeg.delay` in `apply(...)` (or handle as onset delay; `delay_random` allow-listed).
2. Allow-list `offset_random` in `sfz_opcode_table()` (accepted and counted; non-deterministic sample offset).
3. Allow-list `sustain_cc` in `sfz_opcode_table()` (sustain pedal routing).
4. Allow-list `ampeg_release_shape`, `ampeg_attack_shape`, `ampeg_decay_shape` in `sfz_opcode_table()` (envelope shapes).
5. Allow-list velocity crossfade opcodes `xfin_lovel`, `xfin_hivel`, `xfout_lovel`, `xfout_hivel` in `sfz_opcode_table()`.
6. Allow-list `width`, `width_oncc<N>`, `width_cc<N>`, `width_curvecc<N>` in `sfz_opcode_table()`.
7. Allow-list `rt_decay` in `sfz_opcode_table()` (release trigger decay rate).
8. Allow-list `amp_veltrack_oncc<N>`, `amp_veltrack_cc<N>`, `amp_veltrack_curvecc<N>` in `sfz_opcode_table()`.
9. Synchronize `formatlib/README.md` opcode table to keep `format_sfz_test` passing.
10. Add unit tests for these opcodes in `formatlib/tests/sfz_test.cpp`.

## Boundaries & Constraints

**Always:** Opcode table in `formatlib/README.md` must stay in exact 1:1 synchronization with `sfz_opcode_table()`.
**Never:** Regress existing SFZ parsing behavior or break existing bundle tests.

</intent-contract>

## Code Map

- `formatlib/src/sfz_import.cpp` -- update `sfz_opcode_table()` and `apply()`.
- `formatlib/README.md` -- update opcode documentation table.
- `formatlib/tests/sfz_test.cpp` -- test opcode acceptance and classification.

## Auto Run Results

- `format_sfz_test`: PASS (100% table synchronization with `formatlib/README.md`).
- `librarian_test`: PASS.
- Full format & librarian suite (40 tests): 100% PASS.
- Salamander Grand Piano V3: 100% of regions imported into 693MB native bundle; null-tested against sfizz oracle.
- Steel Drum: imported successfully into native bundle and null-tested.

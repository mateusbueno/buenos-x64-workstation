---
title: 'Importer value rules and inert opcodes'
type: 'feature'
created: '2026-10-27'
status: 'done'
baseline_revision: '4cc1fb11f53712e5838e926751ff309d11f38348'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/epic-5-context.md', '{project-root}/formatlib/README.md']
warnings: []
deferred:
  - summary: >-
      Linux Docker gcc build fails in ui/src/adapters/drm_adapter.cpp (lv_linux_drm_set_file signature drift in the fetched LVGL); pre-existing, unrelated to SFZ.
    evidence: |-
      docker-check.sh gcc stops at drm_adapter.cpp:14 after missing includes in sysex tests and ui screens were fixed in this story.
    severity: medium
  - summary: >-
      Orphan samples used only by hikey=-1 regions stay in the bundle; other parse_sfz_file callers do not drop never-note regions; the report omits the ignored value text.
    evidence: |-
      Edge-case and blind reviewers; resolved when Story 13.7 adds controller-triggered voices.
    severity: low
---

<intent-contract>

## Intent

**Problem:** The SFZ importer rejects a whole library on harmless values: `lokey=-1`, `lokey=c-2` (out of the 0..127 note range), `<curve>` headers, and it must keep `ampeg_dynamic` inert only while nothing consumes it.

**Approach:** Follow sfizz in `formatlib/src/sfz_import.cpp`: an unparseable or out-of-range note value falls back to the opcode default and is logged as an ignored value; `hikey=-1` marks a region that never triggers from a note; `<curve>` blocks are parsed and stored inert.

## Boundaries & Constraints

**Always:** a region without these cases produces a bit-identical region table and bundle; ignored values are reported (a new `ignored_values` list on the parse result, surfaced like `allowlisted`); a curve used through an unsupported opcode still skips the file with that opcode named; `ampeg_dynamic` stays allow-listed with a comment stating it is inert only while no `ampeg_*_oncc`/`ampeg_vel2*` is supported and that Story 13.5 revisits it.

**Never:** change engine or bundle format in this story (a note-never-triggered region is flagged on the parse result and is not written to the bundle, with an ignored-value report line, until Story 13.7 adds controller-triggered voices); invent opcode support beyond the three cases; trigger CI.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| lokey out of range | `lokey=-1` | key_lo = default 0; ignored value logged | none, file imports |
| lokey name out of range | `lokey=c-2` | key_lo = default 0; logged | none |
| hikey out of range (not -1) | `hikey=128` | key_hi = default 127; logged | none |
| hikey -1 | `hikey=-1` | region flagged never-note-triggered, not written to bundle, logged | none |
| unparseable note | `lokey=zz` | default; logged | none |
| key out of range | `key=-1` | lo/hi/center keep defaults; logged | none |
| curve header | `<curve> curve_index=1 v000=0 v127=1` | stored inert on result | none |
| curve + unsupported opcode | region uses `amp_velcurve_1`-style unsupported opcode | file skipped, opcode named | unsupported_features |
| ampeg_dynamic | `ampeg_dynamic=1` alone | inert, accepted | none |

</intent-contract>

## Code Map

- `formatlib/src/sfz_import.cpp` -- `parse_note` (~191), `apply` lokey/hikey/key (~389), `onParseHeader` (~230, add `curve`), `sfz_opcode_table` (~38), region flush (~500-570).
- `formatlib/include/velora/format/sfz_import.hpp` -- `SfzParseResult` (add `ignored_values`, `curves`, parallel never-note flag).
- `formatlib/tests/sfz_test.cpp` -- unit tests (add one per matrix row, each asserting the region table against the sfizz behaviour named in the matrix).
- `formatlib/README.md` and `tools/librarian` import report -- document/surface ignored values if the report lists allowlisted opcodes.

## Tasks & Acceptance

**Execution:**
- `formatlib/include/velora/format/sfz_import.hpp` -- add result fields -- carries ignored values, curves, never-note flag.
- `formatlib/src/sfz_import.cpp` -- implement fallback, hikey=-1 flag, curve header parsing, ampeg_dynamic comment -- the three cases.
- `formatlib/tests/sfz_test.cpp` -- test every matrix row -- coverage.
- Import report path (find where `allowlisted` is consumed) -- surface ignored values in the report -- AC "report logs the ignored value".

**Acceptance Criteria:**
- Given `lokey=-1` or `lokey=c-2`, when imported, then key_lo is the default and the report logs the ignored value.
- Given `hikey=-1`, when imported, then the region is flagged as never note-triggered.
- Given a `<curve>` header, when parsed, then it is stored and inert; unsupported opcodes using it still skip the file.
- Given `ampeg_dynamic` alone, when imported, then it stays inert.
- Existing demo bundles still import bit-identically.

## Verification

**Commands:**
- `cmake --build build -j && ctest --test-dir build -R "format|sfz|corpus" --output-on-failure` -- expected: pass
- `tools/ci/docker-check.sh` -- expected: pass (local Linux validation; never trigger CI)

## Review Triage Log

### 2026-10-27 — Review pass
- verdicts: ~30 findings across four layers; patched: key clears never_note, curve bounds and report_ gating, librarian never-note tests, README curve text, hikey=-1 message; deferred: orphan samples, other callers, ignored value text, preflight ignored_values row, ampeg_dynamic guard test, Linux LVGL drift; rejected: sysex include note (needed for Linux build).

## Auto Run Result

Status: done. Importer falls back to defaults on bad lokey/hikey/key, flags hikey=-1 regions (dropped before bundle until 13.7), stores inert <curve> blocks, ignored values in batch JSON. macOS ctest 708/708 pass. Docker gcc: fixed missing includes in 4 files; still blocked by pre-existing LVGL drift (deferred). followup_review_recommended: false.

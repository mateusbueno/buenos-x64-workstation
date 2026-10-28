---
title: 'Allow relative sample paths within the library root'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: 'dbd51fbdfe0f27950c406d15a5198031d27918a5'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/epic-13-retro-2026-10-28.md', '{project-root}/formatlib/README.md']
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** SFZ libraries that organize their instruments in subdirectories (such as `Programs/Piano.sfz` with samples in `Samples/` referenced as `..\Samples\*.wav`, e.g. Sonatina, Meatbass, Bear Sax, Weresax, Scarypiano, Osiris) fail import because `check_path` strictly rejects any path containing `..` relative to the SFZ file's own directory.

**Approach:**
1. Extend `parse_sfz_file(path, library_root = {})` in `formatlib` to accept an optional `library_root`.
2. Inside `Importer::check_path`, validate that normalized sample and `default_path` targets stay within `root_` (`library_root` if supplied, else `dir_`). Reject with `path leaves the library root` if it escapes above `root_`, or `path leaves the SFZ directory` if `library_root` was not supplied.
3. Construct `SfzSample::name` relative to `root_` (so sample paths do not contain `../`).
4. Wire `base_dir` in `librarian/src/import.cpp` and `root` in `librarian/src/preflight.cpp` to `parse_sfz_file`.

## Boundaries & Constraints

**Always:** An escape outside `library_root` is strictly rejected with `kInvalidValue`. When `library_root` is empty, fallback to the SFZ file's directory `dir_` preserving existing strict behavior.
**Never:** Allow escaping outside the library root or filesystem traversal.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Relative sample with `..` inside root | `sample=../Samples/a.wav`, root set | Accepted, sample stored with clean relative name | None |
| Relative sample with `..` escaping root | `sample=../../outside.wav`, root set | Issue `kInvalidValue` | "leaves the library root" |
| Relative sample with `..` without root | `sample=../Samples/a.wav`, no root | Issue `kInvalidValue` | "leaves the SFZ directory" |
| Default path with `..` inside root | `default_path=../Samples/`, root set | Accepted | None |
| Default path with `..` escaping root | `default_path=../../outside/`, root set | Issue `kInvalidValue` | "leaves the library root" |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/sfz_import.hpp` -- add `library_root` parameter to `parse_sfz_file`.
- `formatlib/src/sfz_import.cpp` -- update `Importer` constructor, `check_path`, sample name construction, and `parse_sfz_file`.
- `librarian/src/import.cpp` -- pass `base` to `parse_sfz_file`.
- `librarian/src/preflight.cpp` -- pass `root` to `parse_sfz_file` in `analyze_sfz`.
- `formatlib/tests/sfz_test.cpp` -- unit tests for `..` inside and outside library root.
- `librarian/tests/librarian_test.cpp` -- end-to-end import test with subfolder SFZ and `..` samples.

## Tasks & Acceptance

- [x] 1. Extend `parse_sfz_file` API in `formatlib/include/velora/format/sfz_import.hpp` with optional `library_root`.
- [x] 2. Implement root-boundary path validation in `formatlib/src/sfz_import.cpp` using canonical path comparison.
- [x] 3. Construct clean sample relative names without `../` when root is set.
- [x] 4. Wire `base` in `librarian/src/import.cpp` and `root` in `librarian/src/preflight.cpp` to `parse_sfz_file`.
- [x] 5. Add unit and integration tests in `formatlib/tests/sfz_test.cpp` and `librarian/tests/librarian_test.cpp`.
- [x] 6. Verify end-to-end against real corpus library (Karoryfer Scarypiano).

## Review Triage Log

- Layer 1 (Sanity & Contract): Paths escaping the declared library root are strictly rejected with `kInvalidValue`. macOS symlinks (`/var` vs `/private/var`) are handled consistently via `weakly_canonical`.
- Layer 2 (Regressions): Backward compatibility preserved for calls without `library_root`.
- Layer 3 (Tests & Verification): `format_sfz_test`, `librarian_test`, and ctest suite pass.

## Auto Run Result

Status: done
Verification:
- `format_sfz_test`: PASS
- `librarian_test`: PASS
- Karoryfer Scarypiano full import test: 429 samples, 429 regions published successfully.


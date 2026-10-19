---
title: 'Story 1.13: Import the demo SFZ library subset into bundles'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: 'c0b128e13bd62b117d021352cd199b436fa55ad2'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/_bmad-output/business/11-12-sfizz-license-check.md'
  - '{project-root}/formatlib/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** There is no way to turn an SFZ library into a bundle. The investor demo needs real sample libraries (Salamander Grand Piano, a VSCO2 instrument) published atomically under `/data/lib` with an instrument model that preserves SFZ key/velocity/round-robin/loop/envelope mapping.

**Approach:** (1) Vendor the sfizz 1.2.3 SFZ **parser** (exactly the file closure and conditions in `_bmad-output/business/11-12-sfizz-license-check.md`) into `formatlib/third_party/` and wrap it in a small formatlib SFZ-to-instrument-model layer. (2) Extend the bundle format with a per-instrument region table (bump `kFormatVersion` to 2; the reader accepts 1 and 2; v1 bundles read with empty region tables). (3) Add a `librarian` CLI (`librarian import <file.sfz> --root <data root> [--category ...]`, host and board) that parses, maps the demo opcode subset, stages the bundle in a temp dir on the same filesystem, decode-verifies every sample bit-exact, fsyncs, renames to `<root>/lib/<library_id>/<bundle_id>/` and atomically flips `<root>/lib/<library_id>/current`.

## Boundaries & Constraints

**Always:** Vendor only the files listed in the 11-12 closure table (or a subset), unmodified with SPDX/copyright headers intact, record any modification in the file header, replace `ghc::filesystem` with `std::filesystem` if practical, and keep `THIRD_PARTY_NOTICES.md`, `tools/ci/third_party_manifest.json` and `tools/ci/check_third_party_notices.py` consistent (the check script must still pass); the abseil file-level list produced by vendoring must be added to the 11-12 document before finishing; exclude GPL/LGPL; sfizz's synth/renderer, st_audiofile and dr_libs are NOT vendored (WAV reading uses formatlib's own loader; extend it to read samples referenced by SFZ only in the formats already supported: 16/24-bit PCM and 32-bit float WAV); the importer never resamples or requantizes; supported opcode subset exactly: headers `<global> <master> <group> <region> <control>`, `lokey/hikey/key`, `lovel/hivel`, `pitch_keycenter`, `tune`/`transpose`, `volume`/`pan`, `offset`, `seq_length`/`seq_position`, `lorand`/`hirand`, `loop_mode`/`loop_start`/`loop_end`, `ampeg_*`, `trigger` (attack/release), `sample`, `#define`, `#include`, `default_path`; any opcode outside the subset is reported per file as unsupported and **the file is skipped with that reason** (no partial bundle); crash safety: killing the importer at any point leaves either the previous `current` or the new one, never a partial bundle under `current` (stage -> verify -> fsync files and directories -> rename -> flip `current` with symlink-rename); `library_id` for user imports is the UUIDv5 of a source fingerprint minted by formatlib; `instrument_key` is the normalized `.sfz` relative path.

**Never:** Implement the full Epic 5 import UX, categories UI, SF2, crypto, streaming, or engine playback of regions (that is Story 1.15); weaken bit-exactness; vendor files outside the closure without extending the 11-12 table first; commit large sample libraries (tests use small synthetic fixtures generated in code or tiny committed ones); download Salamander/VSCO2 (real-library acceptance is an env-gated manual test, see tasks).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Happy import | synthetic multi-region SFZ with velocity layers, round-robin, loop, release trigger, ampeg, defines, include, default_path | bundle published; region table matches the parsed region table field by field | none |
| Unsupported opcode | region uses e.g. `cutoff`, `lfo01_freq` | file skipped, reason lists the unsupported opcodes with file:line, nothing published, previous `current` intact | exit non-zero |
| Missing sample | `sample=` points to a missing file | import fails with the path, nothing published | exit non-zero |
| Re-import identical | same SFZ twice | same `library_id` and `bundle_id`; `current` unchanged or re-flipped to the identical bundle; no duplicate garbage | none |
| Changed content | one sample changed | new `bundle_id`, same `library_id` (same source fingerprint rule documented), `current` flips atomically; old bundle dir retained | none |
| Killed mid-import | importer killed at each stage (test via a fault-injection hook that aborts after stage/verify/fsync/rename/before flip) | `current` is either old or new, never partial; staging leftovers do not appear under `lib/<id>/` | recovery on next run |
| Bit-exact verify | corrupt a staged sample before verify (fault injection) | importer detects mismatch, aborts, publishes nothing | exit non-zero |
| Unsupported audio | 8-bit/odd-rate/compressed sample | skipped with reason | exit non-zero |
| Include cycles / missing include | recursive `#include`, missing file | typed error, no crash | exit non-zero |
| Filesystem | staging on the same filesystem as the destination | rename is atomic; if the staging dir is on another filesystem the importer refuses | exit non-zero |

</intent-contract>

## Code Map

- `formatlib/` -- existing bundle writer/reader/WAV loader (read `README.md`, `include/velora/format/bundle.hpp`); extend manifest with a per-instrument region table; `kFormatVersion` 1 -> 2 with reader accepting 1 and 2 (the version tests currently produce version-0 and N+1 via `WriterOptions::format_version`; update them)
- `formatlib/third_party/sfizz/` (new) -- vendored parser closure per the 11-12 table; fetch from https://github.com/sfztools/sfizz tag 1.2.3 (commit 4e70dc0b…) and its pinned submodules (abseil c2435f83, ghc 8a2edd6d, simde 2bf459d4) into a scratch directory, copy only the closure; this download is authorized for this story
- `THIRD_PARTY_NOTICES.md`, `tools/ci/third_party_manifest.json`, `tools/ci/check_third_party_notices.py`, `_bmad-output/business/11-12-sfizz-license-check.md` -- licensing records from Story 11.12; keep consistent
- `engine/host/include/velora/host/library_resolver.hpp`, `tools/library-gen/` -- the `<root>/lib/<library_id>/current -> <bundle_id>/bundle.vlb` layout and constants shared with the importer; the resolver must still pass its tests with bundles written by the new importer
- `tools/ci/docker-check.sh` -- local Linux validation (gcc, clang-20 RTSan); CI is manual-only
- `_bmad-output/planning-artifacts/epics.md` Story 1.13 (~line 1121) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `formatlib/third_party/sfizz/` + notices/manifest/doc updates -- vendor the parser closure, build it as `velora_sfizz_parser` with no heavy dependencies, add abseil file-level list to the 11-12 doc -- parser dependency
- `formatlib/include/velora/format/instrument_model.hpp` + `src/` -- `Region` model (sample ref, key lo/hi/center, vel lo/hi, tune/transpose/volume/pan, offset, seq length/position, lorand/hirand, loop mode/start/end, ampeg params, trigger) and instrument; serialization into the bundle manifest region table; reader accessors -- instrument model in the format
- `formatlib/src/sfz_import.cpp` + header -- SFZ file -> instrument model using the vendored parser, with `#define`, `#include`, `default_path`, header inheritance (global/master/group/region), unsupported-opcode reporting with file:line -- mapping
- `librarian/` (new top-level dir per the Structural Seed) -- `librarian import` CLI and library: parse, load/verify samples, write bundle, stage/verify/fsync/rename/flip with fault-injection hooks for tests, startup reconcile of leftover staging dirs under `<root>/lib/.staging/` -- importer
- `librarian/tests/` and `formatlib/tests/` -- tests for every matrix row (synthetic SFZ + generated WAVs), field-by-field comparison of parsed region table vs bundle instrument table, kill-at-every-stage fault injection, resolver can open the published bundle, version 1 and 2 reading -- coverage
- an env-gated acceptance test `VELORA_SFZ_DEMO_DIR` (path containing Salamander Grand Piano and/or a VSCO2 instrument): when set, imports them and checks region counts equal the SFZ source region counts; skipped (ctest skip code) when unset; document in the READMEs -- real-library acceptance, performed manually by the founder
- READMEs, CMake wiring, `.github` untouched except nothing (CI is manual-only) -- docs

**Acceptance Criteria:**
- Given a synthetic SFZ within the opcode subset, when imported, then a bundle is published and its region table equals the parsed region table
- Given an unsupported opcode, then the file is skipped with a per-file reason and nothing is published
- Given the importer is killed at any stage, then `current` is either the previous or the new bundle and never partial
- Given every sample, then it is decode-verified bit-exact before publish
- Given the notices check script, then it still passes; given macOS and Linux Docker, then all tests pass

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~50 findings — patched 9 groups, rest deferred
- findings:
  - `[medium]` `patch` half-specified loops (loop_start only) gave a misleading error; SFZ defaults not applied — loop points resolved once the sample length is known, inverted range is a clear error
  - `[medium]` `patch` opcode values validated per flushed region (duplicates; bad values at region-less levels never reported); parser warnings dropped; sample/default_path could escape the SFZ directory — validated once at read, deduped, warnings become issues, escapes rejected
  - `[medium]` `patch` crash-injection env hook shipped in the production CLI; mistyped --root silently created; category written after publish (wrong failure report) — hook only under a test define, root must exist, category now part of the staged atomic publish
  - `[medium]` `patch` validate_region accepted values the importer forbids; set_source_id("") collapsed identities — aligned ranges, empty id is an error
  - `[medium]` `patch` region table untested under truncation/bit-flip; --base untested; demo acceptance counting flawed; third-party closure not enforced — sweeps, base test, fixed counting, notices script now enforces the 11-12 closure table; golden region-record literal added
  - `[low]` `defer` library_id depends on the SFZ's absolute path (re-import on another machine gets a new id); old bundle dirs never pruned; whole library held in RAM during import (multi-GB needs streaming); smpl-chunk loops in WAV ignored; Abseil file list derived on macOS only; frozen real v1 bundle fixture
  - `[intent gap noted]` Real Salamander/VSCO2 libraries were not available in this environment: AC "each produces a bundle with all regions mapped" is covered with synthetic and Salamander-style fixtures plus an env-gated acceptance test (`VELORA_SFZ_DEMO_DIR`), not yet run on the real libraries. The strict opcode subset (no `amp_veltrack`, no `ampeg_*_oncc`/vel2 variants, no `../` in default_path) will probably skip real SFZs until the founder decides to widen it.

## Auto Run Result

Status: done (real-library acceptance pending the founder's manual run). Vendored the sfizz 1.2.3 parser closure (+abseil, simde, ghc `fs_std.hpp` switched to `std::filesystem`) with licensing records, bundle format v2 with a per-instrument region table (reader accepts v1 and v2), SFZ-to-instrument-model mapping for the demo opcode subset with per-file unsupported-opcode reporting, and the `librarian import` CLI with stage → bit-exact verify → fsync → rename → atomic `current` flip, fault injection and real fork/kill tests at all stages. Verification: macOS ctest all pass (demo acceptance skipped without `VELORA_SFZ_DEMO_DIR`), Docker gcc and clang-20 RTSan ok, purity and third-party notices checks pass. Follow-up review recommended: false.

---
title: 'Story 5.8: Import execution survives drive removal and power loss'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: 'f3e379a4a4b6a0ecfc46b73e4a458cc43b746914'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-5-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 5.8 "Import execution survives drive removal and power loss" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~3727; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Build on Stories 5.6 (publish/catalog/reconcile/crash campaign) and 5.7 (preflight, ImportAction). Read librarian/README.md, proto/README.md, librarian/src/import_worker.*, library_service.*, library_reconcile.*, library_index.*, tests/crash_campaign_test.cpp.

Constraints:
- Checkpointed publishing: during a multi-timbre import of one library, publish a new immutable bundle containing all finished timbres at a checkpoint interval (configurable; default every 10 timbres or 20 GB, whichever first, plus at job end), reusing verified sample data from the previous bundle by hard links (fall back to copy only when link fails, e.g. cross-device; document) so published bundles stay immutable and `current` flips atomically (existing atomic publish mechanism). Catalog rows reflect each published checkpoint. Interval is config (service options + CLI/service flag), tested with small values.
- Drive removal: detect I/O errors (EIO/ENXIO/ENODEV/ENOENT-on-source-root, short reads) or an explicit unmount signal (an injectable `source_gone` probe: source root no longer stat-able / device id changed) at any point in the item pipeline; then publish timbres finished so far, discard the in-progress timbre whole (no partial files left in staging), end the job with new terminal status `DRIVE_REMOVED` (additive enum value) with imported vs total counts in the final event, leave earlier libraries untouched. Mac test: use an injectable fault (hook that makes the source unreadable mid-timbre) AND, if feasible without root, a hdiutil-created disk image detached mid-import in an env-gated test that skips cleanly when unavailable.
- Import again (ImportAction IMPORT on already_imported): re-converts the timbre, publishes a new bundle where `current` points at it with the re-converted timbre, `library_id` and `instrument_key` unchanged, and a Live Set referencing that timbre resolves it without edits (test through engine/host LibraryResolver / LiveSetLoader as existing tests do; replace a changed timbre in place within the new bundle; other timbres of the library are retained via hard links).
- Crash campaign: 50 kills of the librarian process at random points of an import (SIGKILL of a real child process running the CLI or service import with a seeded random delay/fault point; deterministic seeds, no timing assertions: use fault-point hooks/env-selected kill points enumerated across the pipeline so each kill lands at a distinct named point, repeated with seeds to reach 50), restart each time, reconcile pass leaves only fully verified published bundles and matching catalog rows; the final state after the last restart + a completed re-run equals a clean import's content hash. Extend the 5.6 crash_campaign_test rather than duplicating.
- Keep librarian crypto-free; update READMEs and proto README; additive proto only.
- Tests for each Given/When/Then plus checkpoint boundaries (interval 1, interval N with remainder, GB threshold with injected sizes), failure to hard-link fallback, drive removal before first checkpoint (nothing published, job DRIVE_REMOVED imported=0), after checkpoint (earlier timbres visible), removal during Import again (old bundle stays current and intact).


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 5.8 (~line 3727) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 5.8 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: superseded checkpoint bundles pruned in-run and by reconcile (marker file); kIo confirmed by a stronger probe before ending DRIVE_REMOVED, otherwise item fails and job continues; partial-library contract (`ImportItemResult.partial`, docs); single-timbre checkpoint naming; unattempted items reported SKIPPED/drive_removed and bytes_done fixed; lazy timbres_total; removal conversion only before rename; checkpoint catalog failure surfaced and retried; old-client gating of DRIVE_REMOVED; tests (multi-sample discard, kIo/short read, double removal + preflight, service flags, shipped binary ignores kill vars, reconcile pruning, campaign tallies).
Deviations accepted: no hard links (a bundle is one file; checkpoints rewrite from held encoded blocks, no format change); checkpoints SF2-only; removal publishes everything finished.
Deferred (README Known limitations): block store/real hard links, streaming finalize (RAM ~2x library), board default of 20 GiB checkpoint threshold to lower at integration, LiveSetLoader-level Import-again test.

## Auto Run Result

Status: done. docker-check ok (gcc 493/493, clang-20 RTSan 508/508); macOS ctest 500/500; disk-image test passed with VELORA_TEST_DISK_IMAGE=1.

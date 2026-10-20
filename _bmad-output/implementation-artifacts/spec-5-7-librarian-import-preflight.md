---
title: 'Story 5.7: Import preflight: batch scan, free space and duplicates'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: 'a209f9ba03607443595b70816b740cb8e1618d75'
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

**Problem and approach:** Story 5.7 "Import preflight: batch scan, free space and duplicates" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~3707; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Build on Story 5.6 (librarian service: `librarian/include/velora/librarian/library_service.hpp`, `library_index.*`, `import_worker.*`, `proto/library_api.proto`, catalog schema 2) and the importers (SFZ/SF2 in `formatlib`/`librarian/src/import.cpp`) plus the category suggestion from Story 5.5.

Constraints:
- Add a Library API preflight request/reply to `proto/library_api.proto` (additive only): scan a source path (a mounted volume or folder) for `.sfz`/`.sf2` without converting; group into libraries by top-level folder (root files form one library named after the volume label; on the Mac tests use a folder plus an explicit label argument); one row per timbre with name, size, format, suggested category, status in a closed enum (`ready`, `needs_category`, `skipped_unsupported_features`, `skipped_damaged`, `already_imported`).
- Duplicate detection: use the same `library_id` minting from the source fingerprint as the importer (do not duplicate the logic; share the function) and look up the catalog; `already_imported` rows default to action Skip; the import request must accept Import again per row (extend the import item with a per-row action if it does not already allow it).
- Space: preflight computes required bytes (worst-case uncompressed PCM size plus staging plus a fixed reserve constant, documented) and free bytes on the data dir filesystem (statvfs; injectable for tests); an import request whose required space exceeds free space is refused with a new typed error code carrying both numbers in details (closed enum, additive).
- Reads headers only (SF2 sample header chunk, SFZ text parse for sample references with stat, no sample data read beyond what size and damage checks need); damage = unreadable/truncated/missing referenced samples; unsupported features = what the importer already classifies.
- Preflight must run off the service thread (use the worker) and be cancellable/bounded: cap files scanned (e.g. 20000) with a typed truncation flag; never follow symlinks outside the source root; sanitise non-UTF-8 names like 5.6 does.
- 1,000-file synthetic test volume: preflight completes, test asserts correctness and counts (no timing assertions); the duration is reported in the reply, README labels host figures as not representative.
- librarian stays crypto-free (tools/ci/check_librarian_no_crypto.py must pass); update librarian/README.md and proto/README.md.
- Tests: each Given/When/Then, all five statuses, root-label library, already-imported default Skip then Import-again succeeds, space refusal with both numbers, symlink escape, truncation cap, headers-only (a test that a huge sample file with only header bytes valid is not fully read is not required; instead assert via a counting fs hook if easy).


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 5.7 (~line 3707) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 5.7 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: unconfined space estimate (skipped files charged their own size, never 0); reserved bytes across queued/running jobs; FIFO-safe opens; estimate cache keyed on sfz+includes+samples; preflight shutdown race; bounded service-thread work (50 cache-miss estimates/request, one catalog query); row cap 50000; alias dedupe and no out-of-source text in detail; unknown ImportAction rejected; free_known flag; `._` filter; per-client preflight limit; many test gaps (importer parity table, space refusals on cache miss, nested/depth/unreadable dirs, mid-scan cancel, QUEUE_FULL, paging/budget, partial presence, header-only SF2 bound, library_id golden vectors).
Rejected/adjusted: `../` sample paths are rejected by the SFZ parser, so the unconfined test uses an outside-source symlink; the symlink-escaping sample stays skipped_damaged in preflight (story forbids following it) while the importer follows it, asserted as a known divergence.
Deferred: library_id derives from canonical path (re-mounted path = new library); non-UTF-8 names not importable via sanitised path; non-UTF-8 test only runs on Linux; #include of a FIFO via formatlib parser not hardened.

## Auto Run Result

Status: done. docker-check ok (gcc 492/492, clang-20 RTSan 507/507); macOS ctest 499/499 (-j6).

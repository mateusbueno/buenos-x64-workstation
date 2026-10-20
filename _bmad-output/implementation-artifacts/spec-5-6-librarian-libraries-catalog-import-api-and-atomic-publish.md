---
title: 'Story 5.6: Librarian libraries catalog, import API and atomic publish'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: 'fbf1be6eff0204b0c300c7d01663d8a946828831'
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

**Problem and approach:** Story 5.6 "Librarian libraries catalog, import API and atomic publish" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~3676; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** extend the Story 4.11 `librarian-service` (`librarian/src/library_service.*`, `catalog.*`, `proto/library_api.proto`, single-writer flock, SQLite WAL catalog `schema_version` 1 with only Live Set favorites/recent) with the Epic 1/5 import pipeline (`librarian/src/import.cpp` `import_sfz`/`import_sf2`/batch, formatlib bundles, category model from Story 5.5, suggestion module). Read `librarian/README.md`, `proto/README.md`, `engine/host/README.md`, AD-9/10/11/15 and the AR28 notes (stage → bit-exact verify → fsync → rename → flip `current`, **then** catalog rows, in that order). **Catalog:** bump `schema_version` to 2 (migration v1→v2 forward-only, keeping favorites/recent rows; an unknown newer version is refused untouched as in 4.11) and add only the tables this story needs: `libraries(library_id TEXT PRIMARY KEY, name TEXT, source_kind TEXT (sfz|sf2), source_path_fingerprint/source info TEXT, bundle_id TEXT (current), instrument_count INTEGER, imported_seq INTEGER)` and `instruments(library_id, instrument_key, name TEXT, category INTEGER (1..17, never 0/none: CHECK constraint), sample_count/region_count/bytes, PRIMARY KEY(library_id, instrument_key))` with foreign key/cascade on library removal; no search tables yet (5.16). **Library API (additive messages in `library_api.proto`):** `ImportRequest{source: path to an SFZ/SF2 file or folder, items[] {file path, category (required per instrument), optional name}, options {checkpoint settings later}}` → synchronous ack with a job id, `ImportProgress` push events (job id, state queued/running/published/failed/cancelled..., counts imported vs total, current item, reason codes from the 5.1 report, bytes), `ListLibraries`, `ListInstruments{library_id}` (+ replies), `CancelImport{job}`; **an import request with any instrument lacking a category (or `none`) is rejected with a typed error (`CATEGORY_REQUIRED`, closed enum, additive) before anything is written**; only `librarian` writes under `<data>/lib` and the catalog (document in README; the service owns the `<data>/lib` tree and takes a second flock `lib.lock` or reuses the catalog lock to refuse a second writer, including the `librarian import` CLI which must go through the same lock when run against a data dir that has a service running (document: the CLI keeps working standalone when no service holds the lock, and refuses with a clear message otherwise)). **Import pipeline in the service:** a worker thread runs jobs serially (one import job at a time, FIFO queue, bounded); each instrument/library is converted, staged, verified by bit-exact decode round-trip, fsynced, renamed and `current` flipped (reuse `import_sfz`/`import_sf2` stage/publish helpers; do NOT duplicate logic), and only then the catalog rows are written, in that order; a failure at any point writes no catalog rows; **the bundle manifest records each instrument's name and category** (the category already stored in the bundle dir's `category` file by 5.5 for SFZ; for multi-instrument SF2 bundles each instrument needs its own category and name: extend the librarian's bundle-side metadata additively — e.g. a `catalog.meta` file in the bundle dir (protobuf, `format_version`) listing per instrument key: display name and category id, written inside the staged bundle directory before the rename so it is part of the atomic publish and the immutable bundle — so the catalog can be **rebuilt from bundles alone**; document why not in the binary manifest (no format bump)); **startup reconcile pass** (run before serving): delete staging leftovers (`<data>/lib/.staging/*`, orphan `.current.tmp.*`), remove catalog rows whose library has no `current` link, and recreate missing catalog rows from the `catalog.meta` of bundles under `current` (a bundle without `catalog.meta` — e.g. Epic 1 demo bundles — is listed with name from the manifest instrument keys and category taken from the legacy `category` file via `read_category_file`, or treated as "uncategorised" and NOT cataloged if no valid category: document and test), so that catalog and `<data>/lib` always agree and no partial library is listed; expose a `Reconcile`/`rebuild` internal API and test idempotence. **No encryption:** the bundle carries no encryption and the librarian links no encryption code path: add a CI check (symbol check script `tools/ci/check_librarian_no_crypto.py` run in ctest: inspect the built `librarian-service` and `librarian` binaries/libs with `nm`/`otool` (portable fallback: scan linked static libs' undefined/defined symbols) for AES/OpenSSL/mbedtls/libsodium/OP-TEE symbols and for known crypto function names; also check CMake target link lines for crypto libs) that fails if any such symbol/link is present; skip with a clear message if no `nm` is available). **Crash campaign:** "the librarian is killed at random points during 200 scripted imports; on restart the reconcile pass leaves catalog and `<data>/lib` agreeing and no partial library is listed": implement a test harness that runs the service (in-process thread or subprocess) with a deterministic fault-injection hook at every stage boundary (stage-created, samples-written, verified, fsynced, renamed, link-flipped, catalog-row-written, catalog-committed, job-finished) selected pseudo-randomly from a seed for each of 200 scripted imports (mix of SFZ and SF2, single and multi-instrument, re-imports, failures), kill via `_exit` in a forked child (like earlier crash tests), restart, run reconcile, and assert the invariants (a) every catalog library has a valid `current` bundle that verifies (`verify_all`) and matching instrument rows, (b) every `current` bundle has catalog rows, (c) no staging leftovers, (d) a re-run of the same script converges; the 200-import campaign must be deterministic (seeded) and run in reasonable time in the default ctest (cap per-import size tiny; document the runtime; if it exceeds ~60 s, mark `slow` but still part of the default run). Other tests: catalog migration v1→v2 keeping favorites/recent, constraints (category 0 rejected), import API over a real socket (success, category-required rejection with nothing written, duplicate import → `already imported`/unchanged semantics (reuse 5.1's `unchanged`), cancel, progress events monotone and ending with a terminal state, unreadable source, unsupported_features/damaged skip reports propagated per item), list APIs, single-writer enforcement (second service refused, CLI refused while service runs), concurrent clients (import + list), sandbox-agnostic. Backward compat: existing `librarian` CLI behaviour/tests unchanged.

**Never:** preflight scanning (5.7), checkpoint publish/drive removal (5.8), sandboxing (5.9), UI, search/favorites for instruments (5.16/5.19), encryption/protected bundles (Epic 6), or change bundle bytes of existing bundles.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 5.6 (~line 3676) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 5.6 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: reconcile no longer deletes rows on transient I/O errors; describe_bundle bounds check; unreadable catalog.meta keeps row; CLI/service single-writer lock now always created and shared/exclusive; stop() race (SHUTTING_DOWN, queued jobs cancelled); category presence (optional field, missing/none/unknown); ListInstruments ordering; unknown override keys; per-item report caps and UTF-8 sanitising; request cap 1000 items; no-crypto script fails on nm failure/unresolved link names; new tests (catalog-dir split, subscriber pushes, partial cancel, paging/budget, stop mid-job, CLI-vs-service lock, injected I/O errors, migrate hook).
Deferred: item validation still runs on the service thread (bounded by 1000-item cap); corrupt-manifest bounds check covered by review only; imported-target exemption in the no-crypto link walk.

## Auto Run Result

Status: done. Validation: docker-check ok (gcc 491/491, clang-20 RTSan 506/506); macOS ctest 498/498 with -j6 --timeout 3000 (serial run starved by a foreign process).

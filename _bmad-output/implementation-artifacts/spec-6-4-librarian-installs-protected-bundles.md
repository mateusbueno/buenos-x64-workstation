---
title: 'Story 6.4: Librarian verifies and installs protected bundles'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: 'e89d99c3dcef4e642bdefc14b9d091211e59c07b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-6-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 6.4 "Librarian verifies and installs protected bundles" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4209; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Builds on 5.6-5.8 (librarian service, catalog, atomic publish, reconcile, Library API in proto/library_api.proto), 6.1 (trusted key headers embedded per variant), 6.2 (velora_format_verify: signature + hash verification, NO decrypt) and 6.3 (content-pack output: bundle.vlrp, content-key.vault, wrapped-key.<kid>.bin, catalog.meta, pack.json). ACs: epics.md ~4209 (FR31 install, FR32 librarian holds no keys, AR28 partial, NFR25).

Constraints:
- Add an install path to the Library API (additive proto): `InstallProtectedRequest{source_dir or bundle path + wrapped-key blob path (or inline bytes with size cap), optional expected library_id}` with progress/terminal events like imports (reuse the job/worker machinery and the single writer, terminal states, cancel, DRIVE_REMOVED semantics where applicable) and a CLI `librarian install-protected <dir>` equivalent. Installing runs on the worker: verify manifest signature against the embedded trusted content-signing keys (variant: use the dev variant header on the Mac with a test trust list; trusted keys must be injectable for tests), verify the index hash, hash-list root and EVERY block hash (ciphertext), reading the file in bounded memory (stream, not whole-file load), then stage, fsync, rename to `/data/lib/<vendor library_id>/<bundle_id>/`, flip `current` via the existing atomic mechanism, store the wrapped-key blob next to the bundle (`wrapped-key.bin` in the bundle dir, fsynced; validate it is a well-formed wrapped_key_v1 blob for this library_id by structure only: size 105, version byte, NOT unwrapping), and write catalog rows marked Factory (catalog schema: add a `kind` column/field Factory vs User with an additive migration to schema 3; Library API LibraryInfo gets `kind`; user imports stay `User`). Protected libraries' instruments: catalog rows are produced from the manifest's instrument names/categories WITHOUT decrypting (the manifest/catalog.meta from the pack carries names+categories; verify catalog.meta signature coverage: it must be derived from the signed manifest or be hash-bound to it: decide, document, and test tamper of catalog.meta).
- The librarian never decrypts and never handles an unwrapped content key: its build links only velora_format_verify (no velora_format_decrypt, no protected writer); update tools/ci/check_librarian_no_crypto.py/check_format_no_decrypt.py usage so the real librarian binaries are scanned and still pass with the verify symbols allowed; add a test that the librarian binaries contain no cipher symbols.
- Rejection: a bundle with one flipped bit ANYWHERE (loop over a sample of byte positions covering header, manifest, index, seal, blocks, wrapped-key blob, catalog.meta) or signed by an untrusted key, wrong library_id vs destination, already installed same bundle_id (idempotent: report unchanged), a downgrade (older format) etc. is rejected with a typed reason (additive error codes/enum: signature_invalid, untrusted_key, block_hash_mismatch, hash_list_mismatch, corrupt_bundle, wrapped_key_invalid, already_installed), nothing published, and reconcile leaves no staging files (test via crash campaign hooks: add install kill points to the existing crash campaign and assert the same invariants, 20+ kills).
- Reconcile/lease semantics for protected bundles identical to user bundles; Import again of a Factory library_id by the user import path must be refused (typed `factory_library_protected`); user-imported bundles still written unencrypted; protected and user libraries coexist in ListLibraries with kind shown.
- Space check like 5.7 (required = bundle size + reserve) with INSUFFICIENT_SPACE; preflight (5.7) marks nothing special.
- Keep librarian crypto-free of cipher; update READMEs (librarian, proto); all existing tests pass.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.4 (~line 4209) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.4 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: size caps and seal-first verification before allocation (bad_alloc handled); FIFO-safe opens; signed pack_revision in the catalog.meta seal (VCM2) with replay/downgrade rejection; pruning of old protected bundles; repair of damaged same-id dirs; factory protection via any */bundle.vlrp and early importer check; rebuild keeps Factory rows without trust; wrapped-key recipient-id check, redelivery replace, modes; seal-signer==bundle-signer in describe; dev-trust variant refused in release builds and --trust-list compiled out of non-dev; measured bounded memory; exact reasons per flip class and per-iteration nothing-published checks; mid-copy crash kills via real service restart; negative-control scanner test; e2e content-pack -> install test.
Deferred/documented: wrapped-key payload bytes can't be validated without unwrapping (DoS only); install source path oracle on the local socket; no inline-bytes variant; a kill during pruning leaves extra old dirs until next install; fsync-failure-after-rename path untested.

## Auto Run Result

Status: done. docker-check ok (gcc 516, RTSan 531); macOS ctest 523/523.

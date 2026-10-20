---
title: 'Story 6.2: Protected bundle format in formatlib'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: '8c1be3af5160d8fd72b9c911b961cb282db1cf9b'
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

**Problem and approach:** Story 6.2 "Protected bundle format in formatlib" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4150; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Builds on Story 6.1 (tools/keys/KEY-HIERARCHY.md: Ed25519 manifest signing key, key id = first 8 bytes SHA-256(pub), wrapped_key_v1 / vault_entry_v1, Ed25519 verifier strictness: S<L, canonical R/A, reject small-order A, cofactorless; embedded trusted keys headers velora_trusted_keys_<variant>.hpp) and the Epic 1 bundle (formatlib bundle.hpp, writer.cpp, reader, index, hashes, FLAC blocks; formatlib/README.md). Also read Story 6.2 ACs (epics.md ~4150) and AR27/NFR13/NFR44 in the architecture.

Constraints:
- Protected variant of the block-indexed bundle: each coded block encrypted with AES-256-CTR under the per-library content key, counter block derived from a per-bundle nonce and the global block index (document exact layout: e.g. 96-bit nonce prefix || 32-bit... choose so that no counter block is reused within a key even for large blocks: counter = nonce(64-bit) || global_block_index(32-bit) || intra-block counter(32-bit) and cap block size so the intra-block counter cannot overflow; document and assert). The per-block hash of the CIPHERTEXT is stored in the index; manifest carries vendor library_id, bundle_id, format_version, index hash, hash-list root and an Ed25519 signature + signing key id. Bump/extend the format version additively (existing unprotected bundles stay readable; reader rejects unknown versions with typed error). Specify the new fields in formatlib docs byte-exactly.
- Crypto implementation: use OpenSSL libcrypto (EVP) if available via find_package(OpenSSL) on macOS (Homebrew) and Linux (libssl-dev in the Docker image: update tools/ci/docker/Dockerfile and rebuild image tag), OR a small reviewed vendored implementation; pick one and document (licence notices in THIRD_PARTY_NOTICES.md must stay passing). Ed25519 verification MUST follow the strictness in KEY-HIERARCHY.md section 6 (if the library is more permissive, add the extra checks yourself and test with small-order/non-canonical vectors). Sign: provide a signing function used by the content-pack tool (Story 6.3) in a SEPARATE target (e.g. formatlib_protected_writer) so shipped code does not need signing.
- Build targets/layers: (a) `formatlib` core + `velora_format_verify` (signature verification and ciphertext block-hash verification, no decrypt) always available including the desktop build target; (b) `velora_format_decrypt` (AES-CTR decrypt + reader that returns decoded PCM) available only to the engine/test/offline-render targets; (c) writer/sign target for tools. A CI symbol check (new tools/ci/check_format_no_decrypt.py, wired into ctest, docker-check.sh and ci.yml purity job, with --self-test) proves the desktop-verify target (and the librarian binaries) contain no decrypt path: define the check by symbols (e.g. nm shows no EVP_aes*_ctr/EVP_Decrypt*/EVP_CIPHER_CTX decrypt-init and none of the project's decrypt symbols) and keep the existing librarian no-crypto check consistent: the librarian will need verification (hash+Ed25519) in Story 6.4, so update check_librarian_no_crypto.py now to allow the verify symbols but still forbid cipher/decrypt symbols (and keep all its existing guarantees otherwise, with tests).
- Reader API: open(path, TrustedKeys, ContentKey) verifies manifest signature against trusted keys (typed error `signature_invalid`, `untrusted_key`), index hash, hash-list root; read_frames(...) verifies the block hash, decrypts and decodes only needed blocks; never returns plaintext from a bad block (typed `block_hash_mismatch`); opening with the wrong content key must not return garbage silently: the decoded block fails its FLAC/decoder integrity (add a per-block plaintext check value or rely on FLAC frame CRC and a manifest key-check value: add a key-check field = truncated HMAC/hash of a fixed label under the key, documented) -> typed `wrong_content_key`.
- Tamper tests: flip each byte class (header, manifest, index, block data, signature, key id), untrusted key, truncated file, nonce change; all give typed errors, never plaintext. 10,000 random seeks on a test bundle decode bit-identical to the source PCM (deterministic seed, no timing). Reproducibility: same source+key+nonce => identical bytes.
- Unprotected bundles unchanged: existing tests, goldens and bundle ids stay passing.
- Update formatlib/README.md and THIRD_PARTY_NOTICES.md; add the RTSan note: decrypt runs on non-RT threads only (reader APIs documented not RT-safe).


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.2 (~line 4150) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.2 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). No exploitable memory-safety/verify-then-use defect found. Patched: no-decrypt scanner denylist + exact allowlist and @VERSION stripping; non-vacuous "detects decrypt" test (--expect-flagged); librarian scanner tested on the real verify artifacts; strict Ed25519 rejects small-order R and tests use vectors a lax verifier accepts; OpenSSL notice text taken from the real headers; exact error codes in the tamper matrix, unknown-version and precedence cases; plaintext-integrity, good-bad-good, >256-block, pinned-vector and unprotected-golden tests; key/plaintext wiping, nonce precondition (zero nonce rejected, derive_nonce overload), safe file write, duplicate key-id handling.
Deferred/documented: float blocks checked by size only; "no decrypt path" means no cipher-API call (shared libcrypto contains AES); counter_block stays in core (layout only); S+L non-vacuity not demonstrable with OpenSSL 3.6; ci.yml edits unrun (CI manual).

## Auto Run Result

Status: done. docker-check ok (gcc 497+, clang-20 RTSan 520); macOS ctest 512/512.

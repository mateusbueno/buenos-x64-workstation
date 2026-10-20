---
title: 'Story 6.1: Signing and content-key hierarchy with dev roots'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: '3594fa8bebef68d2d26435c6790a5d9a3bc9ac5a'
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

**Problem and approach:** Story 6.1 "Signing and content-key hierarchy with dev roots" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4124; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Epic 6 start. There is no epic-6-context.md; read Epic 6's intro and Stories 6.1–6.5 in epics.md (~lines 4120–4265), the architecture spine (_bmad-output/planning-artifacts, search for AR27, AR34, NFR23, NFR25), tools/ci/*, and the Story 1.6 boot-signing material if present in the repo (grep for secure boot / signing in docs; 1.6 is not implemented yet so document the intended trust relation rather than inventing files).

Constraints:
- Deliver `tools/keys/KEY-HIERARCHY.md` defining, per root (dev, factory-test, production): boot-image signing key, RAUC bundle signing key, content-manifest signing key (Ed25519), content-key vault KEK; who holds each, storage, rotation, which image variants trust which public keys; production private keys only in an offline HSM (ceremony = Story 6.10).
- Dev key tool in `tools/keys/` (Python 3 stdlib + one well-known crypto dep is NOT allowed to be assumed on CI; prefer a small C++ tool under tools/keys using the crypto library you choose for Story 6.2 later, OR a Python script using only `cryptography` if you gate tests when missing — choose and document; the key formats must be what 6.2/6.3/6.5 will consume: Ed25519 seed/public key raw 32 bytes files with key-id = first 8 bytes of SHA-256(public), a 32-byte dev KEK, a dev device key pair (X25519 or Ed25519+wrap scheme: document the wrap format used by the vault in 6.8/6.5: define it now in KEY-HIERARCHY.md as `wrapped_key_v1` = ephemeral X25519 + HKDF-SHA256 + AES-256-GCM of the content key, or justify another)). Output to a directory OUTSIDE the repo (default `$XDG_DATA_HOME/velora-dev-keys` or `~/.velora-dev-keys`), every file labelled dev-only (header/sidecar JSON with root=dev), file mode 0600, refuse to write inside a git work tree.
- CI check script `tools/ci/check_no_private_keys.py` fails if private key material (PEM PRIVATE KEY blocks, raw 32-byte seed files with the dev-key naming, files under the dev key dir name patterns, base64 seed patterns in `*.json` sidecars flagged private) is committed or present in the git index/working tree; has a --self-test with a violating fixture and a clean tree; wire into docker-check.sh and the CI workflow manual job and ctest if the repo does that for other checks.
- Trusted-key embedding: a manifest `tools/keys/trust/<root>.pub-list` (public keys only, committed) plus a script/CMake function `velora_embed_trusted_keys(<variant>)` generating a header with trusted public keys for a variant; dev images trust only dev keys; the build REFUSES (CMake FATAL_ERROR / script exit non-zero) to embed a dev public key into a production variant, and a production variant with no production key list is also refused; tests for both refusals (cmake script-mode test or python test).
- Tests under ctest (Python via add_test like other tools checks); keep them hermetic (temp dirs, no network). README for tools/keys. Update THIRD_PARTY_NOTICES.md if you add a dependency, keep check_third_party_notices.py passing.
- Do not create or commit any private key material; test keys are generated into temp dirs at test time.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.1 (~line 4124) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.1 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: key tool symlink/overwrite/chmod/atomicity hardening; Ed25519 public-key validation (canonical, on-curve, not small order) at embed; dev/fixture denylist for non-dev variants; stale header deletion on refusal; per-variant namespaces + kIsProduction; configure dependencies; private-key scanner coverage (chunked scan, JWK, lists, names, markers, 32-byte files, allowlist, kind-asserting self-test); KEY-HIERARCHY per-key rotation rows, normative wrapped_key_v1/vault_entry_v1 with pinned vectors and round-trip/tamper tests, verifier strictness for 6.2/6.3, sign command and RFC 8032 cross-check.
Deferred: dev boot-image key (Story 1.6) and RAUC keys (6.13) not generated here; no key versioning for vault entries; wrapped_key_v1 has no sender authentication (DoS only); Python Ed25519/X25519/AES-GCM are dev-tool/test-only, non-constant-time, checked against RFC/NIST vectors only; production header compile only checked via Python test.
Note: one transient setlist_controller_tests failure under load, passed on rerun (known flake class).

## Auto Run Result

Status: done. docker-check ok (gcc 497, clang-20 RTSan 512); macOS ctest 504/504.

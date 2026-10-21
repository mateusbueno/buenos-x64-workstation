---
title: 'Story 6.8: Device registry and content-key vault'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: 'ba0e4f0a309a6786188cb8ba5f87a815c875b501'
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

**Problem and approach:** Story 6.8 "Device registry and content-key vault" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4320; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Builds on 6.1 (KEY-HIERARCHY.md: roots, KEK, vault_entry_v1, wrapped_key_v1, station certs under the factory-test root), 6.3 (tools/content-pack: crypto.{hpp,cpp} vault_entry_v1 open + wrapped_key_v1 seal, vendor ids, `content-key.vault` outputs) and the dev tool velora_keys.py. Read the Story 6.8 ACs (epics.md ~4320) and 6.16/6.17 (4545-4604) to see how stations and repair mode will consume the API (do not implement those).

Constraints:
- New `tools/registry/` vendor-side service (NEVER linked into shipped unit targets; extend check_content_pack_not_linked.py-style check to the registry), C++20 using OpenSSL (TLS 1.3, mutual TLS) + SQLite, reusing the content-pack crypto code by moving shared vault/wrap code into a small shared library target (e.g. tools/common/vault_crypto) used by both content-pack and registry without behaviour change (existing content-pack tests must stay green). Minimal HTTP/1.1 + JSON API (documented in tools/registry/README.md and an OpenAPI-ish API.md): POST /v1/devices (register), GET/POST for wrapped keys, POST /v1/devices/{id}/revoke (admin cert), GET /v1/devices/{id}, with typed JSON errors.
- Data model: per unit device_id, device_public_key (X25519), som_id, serial_number, tier, provisioning_date, station_id, status (active|revoked); unique constraints: no duplicate ACTIVE device_id or serial (a revoked record may be superseded: SoM swap flow stores a new device row with a link `replaces`); append-only audit log of every write (and every refused call) in SQLite with a hash chain (each entry includes SHA-256 of the previous entry) and triggers preventing UPDATE/DELETE on the audit table; a `verify-audit` command checks the chain.
- Auth: mutual TLS; station certificates issued under the factory-test root CA (provide a dev CA generator in tools/registry/tools or tools/keys: python script using stdlib+openssl CLI is acceptable only if hermetic tests skip cleanly without it; prefer generating test CAs/certs inside the C++ test with OpenSSL APIs). Roles from cert subject (station vs admin). Unauthenticated clients or revoked devices: refuse (TLS handshake failure or 401/403 as appropriate) and log in the audit table (handshake failures logged too where the cert is not available). Rate limit / request size caps / timeouts.
- Wrapped keys: client requests library_ids for a device_id; the registry looks up the vaulted content key (vault directory configured, `content-key.vault` per library as produced by content-pack, opened with the KEK of the configured root — KEK loaded from a file with 0600 check, mlocked, never logged), unwraps ONLY inside the wrap operation, produces wrapped_key_v1 for the device public key, wipes plaintext immediately; never logs or stores plaintext keys; responses contain only wrapped blobs. Revoked or unknown device => refused. Requesting an unknown library => typed error. Audit every issuance (device, library, time, station) but not the key.
- Backup: `registry backup` writes an encrypted backup (SQLite online backup API snapshot, encrypted with AES-256-GCM under a backup key derived from a separate backup key file via HKDF; file format documented) to a second location directory; `registry restore` + a restore test that recovers the DB byte-equivalent (rows and audit chain verify). A nightly job example (systemd timer unit file + cron line in README, untested on a server).
- Tests (ctest): in-process or spawned server on localhost ephemeral port with test CA; happy path register + wrapped-key issuance where the returned blob unwraps with the device private key (reuse the dev provider / content-pack unwrap) to the original content key; duplicates rejected; revoked device refused; unauthenticated (no cert), wrong CA cert, expired cert, admin-only revoke by a station refused; audit chain verify and tamper detection (modify a row => verify-audit fails); append-only triggers; backup/restore round trip and wrong key fails; no plaintext key in logs/DB/backup (scan); input fuzz sanity on malformed JSON/oversized bodies; concurrency (two parallel registrations of the same serial => one wins).
- Keep everything hermetic, no network beyond localhost; update THIRD_PARTY_NOTICES.md only if you add a dependency; keep all existing checks passing (core deps, no_private_keys with the generated test certs/keys only in temp dirs, librarian no-crypto, content-pack-not-linked).


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.8 (~line 4320) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.8 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: absolute per-connection deadline incl. handshake, per-IP connection cap, worker exception boundary; throttled refusal auditing; audit integrity (append-time consistency checks, startup verify, trigger SQL text compare, restore entry, HMAC-signed anchor files with --anchor-dir); restore validates before swap and never damages the existing DB; public-key validation (non-canonical/small-order), id token rules and case-insensitive serials; revoke/issue TOCTOU closed inside the transaction; supersede rule, per-station library allowlist and registration cap; CR/LF header rejection, certificate serial deny-list, CA/KEK expectations for non-dev roots, mlock-or-refuse and non-dumpable KEK; extensive test additions (TLS refusal matrix with exact reasons, independent unwrap vs pinned vector, whole-tree encoded secret scan with negative control, backup/restore matrix, append-only attacks, races, exit codes, API.md error-code coverage).
Deferred/documented: CRL/OCSP (deny-list file instead); proof of possession of the device private key; vault directory not covered by the DB backup; chain is unkeyed (anchors mitigate); DROP/ALTER on a live DB cannot be prevented, only detected; systemd units/cron untested on a server.

## Auto Run Result

Status: done. docker-check ok (gcc 549, RTSan job passed); macOS ctest 538/538; registry_test stable over 12 runs.

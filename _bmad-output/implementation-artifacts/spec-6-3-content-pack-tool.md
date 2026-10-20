---
title: 'Story 6.3: Content-pack tool'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: '06cd81e46f653fce2c1de34502c03f45d7a6cbae'
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

**Problem and approach:** Story 6.3 "Content-pack tool" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4180; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Builds on 6.1 (tools/keys, KEY-HIERARCHY.md: vault_entry_v1 under the KEK, wrapped_key_v1, dev keys outside the repo), 6.2 (formatlib protected writer: finalize_protected / finalize_protected_to_file, derive_nonce, ProtectedBundleReader, velora_format_protected_writer target) and the SFZ importer (librarian import path / formatlib) plus categories (Story 5.5 pack description per-instrument categories). Story ACs: epics.md ~4180 (AR31, FR30, FR31, FR34 partial, FR24).

Constraints:
- New `tools/content-pack` C++ CLI (not linked into shipped binaries; add to the tool no-decrypt checks as an allowed signing/encrypting target; update check scripts so shipped targets stay clean). Input: pack description file (JSON; define schema in tools/content-pack/README.md: vendor library_id (UUID), name/localized names as the importer supports, source path, per-instrument category (Story 5.5 closed enum) and overrides, root = dev|factory-test|production selecting KEK/signing key paths), signing key (Ed25519 seed file from tools/keys), KEK file, output dir.
- Imports through the SAME importer as user imports (call the librarian/formatlib import code path, not a copy; SFZ/WAV and SF2 as supported), but with the vendor library_id from the description instead of the path-derived one (add an explicit library_id override option to the importer API if needed, additive, without changing user import behaviour).
- Generates a fresh random 256-bit content key (from the OS CSPRNG) unless `--content-key-file`/test hook supplies one for reproducibility; writes a protected bundle (6.2) with signed manifest; writes the content key ONLY as vault_entry_v1 (AES-256-GCM under the root's KEK per KEY-HIERARCHY.md) and optionally a wrapped_key_v1 blob for a given device public key (`--device-pub`); never plaintext key on disk (also not in temp files, logs, argv; the key is not accepted via argv; wipe memory). A filesystem-write test hook/monitor asserts no plaintext key or plaintext block hits disk (scan all files written in the output and temp dirs for the key bytes and for plaintext PCM of the source).
- Reproducibility: same source + same content key + same nonce derivation + same signing key => identical bundle bytes (Ed25519 deterministic) and identical vault entry? (vault entry uses random nonce: allow `--deterministic` test mode or document that the vault entry may differ while bundle bytes must be identical).
- Vendor ID register `tools/content-pack/vendor-ids` (committed text file: library_id, name, owner, date): refuse with an error if the pack description's library_id is already assigned to a different library name in the register (or any entry of a different source); a new id must be appended by an explicit `--register` step; test with temp register files; ship the initial register containing only a documented placeholder/dev range if needed.
- Verification: a test that decrypts the packed bundle with the vaulted key (unwrap in the test via the dev KEK) on the Mac using ProtectedBundleReader and renders with the offline renderer (tools/offline-render) a short MIDI suite: output bit-identical to the render of the same library imported unencrypted. Provide a small deterministic test library (generate WAV+SFZ in the test).
- Typed, documented exit codes/errors; README for the tool; THIRD_PARTY_NOTICES/ checks stay passing; keep all existing tests.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.3 (~line 4180) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.3 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). Patched: non-dev root guard now requires sidecar with matching root/key_id and refuses dev-listed keys; register lock held across check+publish+append with rollback, dev-range/v4 enforcement, normalized names, fsyncs; --overwrite keeps unrecoverable old vault dirs; absolute source path removed from catalog.meta/pack.json; --content-key-file dev-only and non-zero; secret wiping and fewer plaintext copies; vendor-id used for `current` lookup in non-sink SF2 import; independent write monitor over output and temp dirs; real non-vacuity scan checks; CLI reproducibility test; honest render evidence (rebuilt == importer finalize == published, negative control); extra exit-code tests; UUID vault/wrapped vectors cross-pinned with the Python reference; no-shipped-target-links-content-pack check.
Deferred/documented: render from the protected bundle directly is Story 6.5; SF2 render comparison not covered; register has no source column; Unicode confusables not folded; race test timing-dependent only on mutants; no real large library packed; SFZ sample paths escaping the SFZ dir is old importer behaviour.

## Auto Run Result

Status: done. docker-check ok (RTSan 509, gcc 524); macOS ctest 516/516.

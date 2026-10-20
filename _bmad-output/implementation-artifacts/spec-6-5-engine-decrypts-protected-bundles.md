---
title: 'Story 6.5: Engine decrypts protected bundles on I/O edge threads'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_revision: '88c3c5d170abf53ec07f786899d389dfe9432daf'
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

**Problem and approach:** Story 6.5 "Engine decrypts protected bundles on I/O edge threads" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~4234; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

Builds on 6.2 (velora_format_decrypt: ProtectedBundleReader; velora_format_verify), 6.4 (installed protected bundles at lib/<library_id>/current/bundle.vlrp + wrapped-key.bin, Factory kind), the Epic 1 engine host (LibraryResolver, sample streaming/head loader, AD-23 generations with lifetime, FsPort), the voice arbiter/SampleEngine, and tools/offline-render. ACs: epics.md ~4234 (FR30/31/32, AR19/AR20, NFR17, NFR4 partial). Read engine/core/README.md, engine/host/README.md, the architecture spine on AR19/AR20/AR23, and the existing code for instrument load, head loader and I/O streaming threads before designing.

Constraints:
- Key-provider port in `engine/ports` (pure interface, no crypto deps in engine/core; engine/core purity check must stay passing): `KeyProvider::unwrap_content_key(library_id, wrapped_blob) -> Result<ContentKeyHandle>`; handle owns key memory: mlocked, madvise(DONTDUMP)/non-dumpable on Linux (PR_SET_DUMPABLE is process-wide: document; use MADV_DONTDUMP + setrlimit core 0 in the host), zeroized on release (explicit_bzero/OPENSSL_cleanse via a small wipe helper). Dev implementation in `engine/host` that unwraps wrapped_key_v1 with the dev device private key (from tools/keys dev dir; path injected, never default-embedded in shipped code: dev-only target/flag), implementing KEY-HIERARCHY wrapped_key_v1 exactly (X25519 + HKDF-SHA256 + AES-256-GCM via OpenSSL) and reusing velora_format_decrypt/protected_writer test vectors.
- Generation integration (AD-23): when a generation that references a protected library is prepared, the control thread obtains the content key through the port, attaches it to the generation (refcounted with the generation's lifetime), and when the generation is freed after its last voice ends the key memory is zeroized (test: observe zeroization via a test hook that records the handle's wipe, plus a use-after-free-safe design; no key copies elsewhere).
- Protected instruments stream like unprotected: block-hash check and AES-CTR decrypt happen ONLY on non-RT threads (head loader and I/O edge threads); RT threads read decoded PCM from the same ring/stream buffers as for unprotected libraries; the SampleEngine/RT code is unchanged except where it must signal integrity events; RTSan render test over a protected instrument passes under clang-20 RTSan in docker-check (the RT callback must not allocate/lock/decrypt).
- Integrity: a block whose hash mismatches during streaming => the voice outputs silence with a short fade for that block, a `content_integrity` counter increments (exposed via the existing engine counters/telemetry path and the control plane status snapshot, additive), and the library is reported damaged (event to control plane / host API, additive); at instrument load the same failure refuses the load with a typed error (reuse the existing load error path; add typed error `content_integrity`/`wrong_content_key`/`signature_invalid` mapping).
- Verification at load: engine host verifies the bundle signature with the engine's trusted key set (injectable; dev variant), uses ProtectedBundleReader open (signature + key check) before accepting.
- Offline renderer: tools/offline-render gets protected-bundle support through the dev key provider (flags `--dev-device-key` and wrapped key path, dev-only build option) so a protected bundle and an unprotected bundle of the same library render the same MIDI suite BIT-IDENTICALLY (test: use tools/content-pack output or the formatlib test writer on the deterministic test library; compare WAV hashes); this replaces the 6.3 substitute (update content-pack README: now covered).
- No plaintext block or key is written to disk on any path: a filesystem write monitor test (FsPort/LD hook or tracing all writes in the process during the test via a write-hooking FsPort + scanning temp dirs) verifies no plaintext PCM/key bytes land in any file.
- Do not break: unprotected paths, goldens, null tests, RTSan tests, purity (engine/core must not link OpenSSL; only engine/host and tools may). Update engine/host README, engine/core README (RT note) and docs.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 6.5 (~line 4234) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 6.5 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

Two reviewers (edge-case-hunter, verification-gap). No RT-safety violation or key UAF found. Patched: key-schedule/shared-secret wiping and single locked page for unwrap temporaries; protected_downgrade refusal for bundle.vlb next to protected files; dev provider default OFF outside Debug, refused in RelWithDebInfo/production, artifact scan for dev symbols; scrub holds instrument weakly so the key is freed with the generation; damage reported once, clear_library on reload, io_errors surfaced; RT look-ahead gain cap and shift-based block index with 340-voice zero-allocation test; decrypt thread-identity test; tightened RTSan expectations plus damage-report negative control; typed load-time refusal of Live Sets (bundle_error); stronger write-monitor needles and Live Set render under ulimit -f 0; more sample formats and the full 17-case sample suite protected-vs-unprotected bit-identical; trust-list parser, smaps (lo/dd) and rlimit checks.
Deferred/documented: no streamer until Story 1.17 (scrub-driven substitute; read-triggered detection and refill failures not covered); 8-bit/int32 are not bundle formats; SF2 suite not rendered protected-vs-unprotected; ulimit -f 0 covers only the offline-render subprocess runs; clean reload clears damaged flag even if an older damaged generation still plays; engine-host smoke test macOS only.

## Auto Run Result

Status: done. docker-check ok (gcc 528, RTSan 547); macOS ctest 536/536.

---
title: 'Story 8.2: Production release signing and per-root update bundles'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - '_bmad-output/planning-artifacts/architecture.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Production units must trust only the `prod` cryptographic root (`/etc/rauc/keyring.pem`) and reject update bundles signed under `dev` or `factory-test`, while the release build environment must never touch the production private key (which lives exclusively in the offline HSM ceremony from Story 6.10). Every shipped release must also record the exact image hash, bundle hash, signing key fingerprint, and root CA fingerprint per variant (`dev`, `factory-test`, `prod`).

**Approach:**
1. Implement `tools/release-sign/velora_release_sign.py` with `prepare-request` (produces an unsigned bundle payload + `signing-request.json` and fails closed if any private key file or env var is present), `attach-signature` (verifies detached Ed25519 signature from the offline HSM against the target root's public key, refuses cross-root signatures, emits the signed `.raucb` bundle, and updates `release-record.json`), `verify-bundle` (enforces single-root trust), and `trace-unit` (traces any field unit's installed version or bundle hash to its signing event).
2. Add `BridgeOptions::trusted_update_root` and per-root bundle verification in `BridgeServer::handle_update` (`sysd/src/bridge_server.cpp`), rejecting `dev` and `factory-test` bundles on a `prod` unit with `ERROR_CODE_UPDATE_BAD_SIGNATURE` (`reason = "wrong_root"`) before modifying any slot.

## Boundaries & Constraints

**Always:**
- Keep production private keys strictly outside the repository and build environment (`check_no_private_keys.py` and `yocto/ci/static_checks.py` must pass).
- Reject cross-root bundles before marking any update ready or touching A/B slots.

**Ask First:** None.

**Never:**
- Commit `.pem`, `.key`, `.seed`, or `.priv` files into the repository.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error / Fallback |
|----------|---------------|----------------------------|------------------|
| Production release build | `prepare-request --root prod` without private keys | Writes `unsigned.payload` + `signing-request.json` | Fails with `private_key_rejected` if any private key env/file is present |
| Offline HSM signature attached | `attach-signature` with valid `prod` signature | Writes signed `.raucb` and updates `release-record.json` | Fails with `cross_root_rejected` or `bad_signature` on mismatch |
| Cross-root bundle on `prod` unit | `dev` or `factory-test` bundle staged to `prod` `BridgeServer` | Rejected with `ERROR_CODE_UPDATE_BAD_SIGNATURE` (`reason = "wrong_root"`); staging file removed | Neither slot modified; `on_firmware_ready` not invoked |

</frozen-after-approval>

## Code Map

- `tools/release-sign/velora_release_sign.py` — Offline signing request generator, signature attacher, per-root verifier, and release record tracer.
- `tools/release-sign/README.md` — CLI documentation for Story 8.2 workflow.
- `sysd/include/velora/sysd/bridge_server.hpp` & `sysd/src/bridge_server.cpp` — `BridgeOptions::trusted_update_root` and per-root rejection in `BridgeServer::handle_update`.
- `sysd/tests/bridge_test.cpp` — Unit tests verifying a `prod` unit rejects `dev` and `factory-test` bundles and accepts `prod` bundles.
- `CMakeLists.txt` — Registers `release_sign_selftest`.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Implement `prepare-request` and `attach-signature` in `tools/release-sign/velora_release_sign.py` so production builds produce an unsigned bundle + signing request without private keys and attach the verified offline HSM signature.
- [x] **Task 2 (AC2):** Enforce per-root bundle verification in `velora_release_sign.py verify-bundle` and `BridgeServer::handle_update` so a `prod` unit rejects `dev` and `factory-test` bundles with `ERROR_CODE_UPDATE_BAD_SIGNATURE` (`wrong_root`).
- [x] **Task 3 (AC3):** Generate and validate `release-record.json` listing `image_sha256`, `bundle_sha256`, `signing_key_fingerprint`, and `root_ca_fingerprint` across `dev`, `factory-test`, and `prod`, with `trace-unit` lookup.

## Verification

- `ctest --test-dir build --output-on-failure -R "sysd_bridge_tests|release_sign_selftest|no_private_keys"`
- `python3 yocto/ci/static_checks.py`

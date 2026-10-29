---
title: 'Story 8.4: MCU firmware manifest in the A/B image and boot-time version check'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - '_bmad-output/planning-artifacts/architecture.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Each A/B SoC slot ships paired firmware binaries for the keybed and panel MCUs under `/usr/lib/velora/mcu/`. At build time, `/usr/lib/velora/mcu/manifest.json` must be verified against the shipped binaries' SHA-256 hashes. At boot time, when `sysd` receives `HELLO` from each MCU, if its `fw_version` differs from the manifest (whether due to an update or a rollback to an older SoC slot), `sysd` must show `"UPDATING CONTROLS"` with progress, flash the MCU from the booted slot's image (excluding flash time from the 10 s normal-boot target), retry once on failure while the dual-bank bootloader preserves the previous firmware bank, and display `"Keyboard controller update failed · Contact support"` on a second failure.

**Approach:**
1. Implement `tools/mcu-manifest/verify_mcu_manifest.py` to generate and verify `/usr/lib/velora/mcu/manifest.json` against the `keybed.bin` and `panel.bin` SHA-256 digests and sizes at build time.
2. Implement `McuManifest` and `McuBootChecker` in `sysd/include/velora/sysd/mcu_boot_checker.hpp` and `sysd/src/mcu_boot_checker.cpp` to verify the manifest, compare each MCU's `HELLO` `fw_version` against the slot's manifest, drive the `"UPDATING CONTROLS"` progress updates, retry once per boot on failure, preserve the active bank, and emit `"Keyboard controller update failed · Contact support"` on repeated failure.

## Boundaries & Constraints

**Always:**
- Verify MCU binary SHA-256 against `manifest.json` both at build time and before boot-time reflashing.
- Reflash on any version difference (`!=`), covering both forward updates and A/B slot rollbacks.

**Ask First:** None.

**Never:**
- Include MCU reflash duration in the 10 s normal-boot target.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error / Fallback |
|----------|---------------|----------------------------|------------------|
| Build-time manifest check | `verify_mcu_manifest.py` | Passes when `keybed.bin` and `panel.bin` match `manifest.json` SHA-256 & size | Exits non-zero on SHA-256 or size mismatch |
| Boot `HELLO` matches manifest | `reported_fw_version == entry.fw_version` | `Action::kUpToDate`, no flash, `flash_time_excluded_from_boot_target = false` | N/A |
| Boot `HELLO` differs (upgrade or rollback) | `reported_fw_version != entry.fw_version` | Shows `"UPDATING CONTROLS"` (`0..100%`), flashes binary, `flash_time_excluded_from_boot_target = true` | Retries once on transient failure |
| Second flash failure | Both attempts fail | Dual-bank bootloader runs previous bank (`active_bank_preserved = true`); displays `"Keyboard controller update failed · Contact support"` | `Action::kFailedContactSupport` |

</frozen-after-approval>

## Code Map

- `tools/mcu-manifest/verify_mcu_manifest.py` — Build-time generator and SHA-256 verifier for `/usr/lib/velora/mcu/manifest.json`.
- `sysd/include/velora/sysd/mcu_boot_checker.hpp` & `sysd/src/mcu_boot_checker.cpp` — `McuManifest` and `McuBootChecker`.
- `sysd/tests/mcu_boot_checker_test.cpp` — Unit tests for manifest SHA-256 verification, version match, upgrade, rollback downgrade, single-boot retry, and second-failure `"Keyboard controller update failed · Contact support"`.
- `sysd/CMakeLists.txt` & `CMakeLists.txt` — Registers `sysd_mcu_boot_checker_test` and `mcu_manifest_selftest`.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Build-time verifier `tools/mcu-manifest/verify_mcu_manifest.py` checks `/usr/lib/velora/mcu/manifest.json` versions and SHA-256 hashes against the MCU binaries and fails on any mismatch.
- [x] **Task 2 (AC2):** `McuBootChecker::on_mcu_hello` compares `HELLO` version against `manifest.json`, does nothing when equal, and shows `"UPDATING CONTROLS"` with progress while flashing on upgrade or rollback (excluded from the 10 s normal-boot target).
- [x] **Task 3 (AC3):** On flash interruption/failure, the dual-bank bootloader preserves the previous bank, `McuBootChecker` retries once, and on a second failure shows `"Keyboard controller update failed · Contact support"`.

## Verification

- `ctest --test-dir build --output-on-failure -R "sysd_mcu_boot_checker_test|mcu_manifest_selftest"`

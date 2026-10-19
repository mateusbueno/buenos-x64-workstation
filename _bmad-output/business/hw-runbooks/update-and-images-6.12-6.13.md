# DRAFT FOR FOUNDER — does not satisfy the listed stories' acceptance criteria

Runbook for Stories 6.12 (RAUC A/B slots with rollback) and 6.13 (dev and factory-test image variants with per-root keyrings). Prepared 2026-10-19. Nothing here has been run. Bootloader backend, slot device paths, `rauc` configuration keys and bundle-build commands are not in the repo; they are "TBD from vendor docs" (or from the Story 1.6 report), and no commands for the unnamed bootloader are given.

Path shorthand (relative to `_bmad-output/`): EPICS = `planning-artifacts/epics.md`; SPINE, HWI = files in `planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/`.

## 1. Acceptance criteria

Story 6.12 (EPICS:4434-4463; covers FR66, AR36 partial, AR2 partial; runs on build host/ARM64 VM for the bundle, dev board for install and rollback, EPICS:4440-4442)
- 6.12-AC1 meta-rauc (`wrynose`, RAUC >=1.15.2) and Story 6.11 layout: RAUC system config declares slots `rootfs.0` / `rootfs.1` with the bootloader backend of the chain chosen in Story 1.6; CI produces a RAUC bundle signed with the dev signing key beside the image; dev image carries only the dev keyring (4446-4449)
- 6.12-AC2 Running slot A, `rauc install` of a valid dev-signed bundle from the dev shell: written to slot B, boot order switches to B with a boot-attempt counter (3 attempts), after reboot runs from B; a systemd unit marks the slot good only after `engine`, `ui` and `sysd` reached ready, not before (4451-4454)
- 6.12-AC3 Bundle whose kernel panics, or whose `engine` fails to reach ready: after the counter is exhausted the bootloader boots the previous slot, RAUC reports it booted with the failed slot marked bad; `sysd` records the rollback (from-version, to-version) in a state file under `/data/config` (4456-4459)
- 6.12-AC4 Bundle signed with a key outside the dev keyring, or with one byte flipped: RAUC rejects it before writing any slot, running slot unchanged (4461-4463)

Story 6.13 (EPICS:4465-4489; covers AR3 partial, NFR23 partial, AR2 partial, AR34 partial; runs on build host/VM + CI, dev board and prototype unfused, EPICS:4471-4473)
- 6.13-AC1 CI release builds two images and two RAUC bundles from one layer set: `dev` (dev-signed, SSH and debug UART on) and `factory-test` (factory-test-signed, SSH off, factory test suite installed), layer set structured so production is a third variant; each image ships only its own root's RAUC keyring, so a dev bundle is rejected by `rauc install` on a factory-test unit and vice versa (4477-4480)
- 6.13-AC2 factory-test variant on a prototype runs a test runner, one test per available subsystem (MCU links, keybed scan, panel controls and LEDs, codec loopback, display and touch, NVMe read/write speed), each pass/fail, on-screen summary and a machine-readable report on the factory station link; missing hardware reports `not present` instead of crashing (4482-4485)
- 6.13-AC3 `/etc/os-release` states the variant in either image (4487-4489)

Context: AD-18 read-only rootfs, RAUC A/B with rollback, data on a journaled partition (SPINE:257-265); AD-26 signed RAUC bundles only via desktop app or USB-A stick, `sysd` sole installer, library updates are never part of a RAUC bundle (SPINE:343-349); keyring/key hierarchy per root in Story 6.1 (EPICS:4124-4140); layout from Story 6.11 (EPICS:4403-4433); NFR24 / hardware factory-test expectations HWI:131-137.

## 2. Preconditions and equipment

Entry gates:
1. Story 1.6 done: bootloader chain chosen (barebox or U-Boot + Rockchip tools; EPICS:924-958) and its report committed (`tools/spikes/secure-boot/REPORT.md`; confirm it exists - it is not in the repo at preparation time). Chain selected: ____
2. Story 6.11 done: wks layout with boot area, two root slots (only A populated), `/data`, read-only `/`.
3. Story 6.1 done: dev keys generated outside the repo (`tools/keys/`; not in the repo at preparation time), plus separate factory-test keys for 6.13. Never place any key in the repo or in a shared folder.
4. Dev board running the dev image (Story 1.4/1.5); for 6.13-AC2 a prototype from Story 3.2 with the MCU stories (3.3, 3.8) as far as available.
5. For 6.12-AC3: the `sysd`, `engine` and `ui` ready-state signals exist (they come from other stories); if not, steps 8-11 stay blocked.

Equipment: Linux build host or ARM64 Linux VM with Yocto (Yocto 6.0, RAUC >=1.15.2, SPINE:428-440); CI runner; dev board with serial console and NVMe; a second NVMe/dev board for restoring a broken slot if possible (recovery method TBD from the Story 1.6 report); the factory station link as built by the partner (TBD; Story 6.16 defines the station); a hex editor or script to flip one byte; spare unfused prototype for 6.13.

## 3. Ordered steps

### 6.12

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 1 | Build the dev image with meta-rauc `wrynose`; inspect the RAUC system config in the image | Slots `rootfs.0` and `rootfs.1` declared; bootloader backend equals the chain from Story 1.6 (value TBD from the report) | AC1 |
| 2 | Inspect keyrings in the image | Only the dev keyring is present | AC1 |
| 3 | Have CI build the release and fetch the bundle | A bundle signed with the dev key exists beside the image | AC1 |
| 4 | Boot the dev board from slot A; run RAUC status | Booted slot A, slot B inactive | AC2 |
| 5 | From the dev shell run `rauc install` of a valid dev-signed bundle | Bundle written to slot B; boot order shows B first with 3 attempts | AC2 |
| 6 | Reboot | System runs from B (confirm via RAUC status and the version string) | AC2 |
| 7 | Check the mark-good unit before and after `engine`, `ui`, `sysd` reach ready (delay one service on purpose in a test build) | Slot is marked good only after all three are ready, never earlier | AC2 |
| 8 | Build a bundle whose kernel deliberately panics; install; reboot | Boot attempt counter decrements per failed boot (3 attempts) | AC3 |
| 9 | After the counter is exhausted | Bootloader boots the previous slot; RAUC reports it booted and the failed slot marked bad | AC3 |
| 10 | Build a bundle whose `engine` never reaches ready; repeat 8-9 | Same rollback outcome | AC3 |
| 11 | Inspect `/data/config` rollback state file written by `sysd` | Contains from-version and to-version of the rollback (file name TBD by implementation) | AC3 |
| 12 | Try a bundle signed with a key outside the dev keyring | `rauc install` rejects it before writing any slot; running slot and slot contents unchanged (compare hashes) | AC4 |
| 13 | Try a valid bundle with one byte flipped | Same rejection | AC4 |

### 6.13

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 14 | Run the CI release build | Two images and two bundles from the same layer set; `dev` has SSH and debug UART on; `factory-test` has SSH off and the test suite installed | AC1 |
| 15 | Inspect layer config | Production can be added as a third variant without restructuring (reviewer note) | AC1 |
| 16 | Inspect keyrings of each image | Each has only its own root's keyring | AC1 |
| 17 | Install the dev bundle on a factory-test unit and the factory-test bundle on a dev unit | Both rejected by `rauc install`, running slot unchanged | AC1 |
| 18 | Boot the factory-test image on a prototype; start the runner | One test per subsystem listed; each shows pass/fail; on-screen summary appears | AC2 |
| 19 | Read the machine-readable report at the factory station link | Report present with a result per subsystem | AC2 |
| 20 | Repeat with a subsystem disconnected (for example unplug the keybed or panel harness) | The related test shows `not present`; runner does not crash | AC2 |
| 21 | Read `/etc/os-release` on each image | States variant `dev` or `factory-test` (field name TBD by implementation) | AC3 |

## 4. Risks and irreversible steps

- Nothing in this runbook is fuse-related. Keep both variants on unfused boards; Story 6.13 states "prototype (unfused)" (EPICS:4473). Fusing is only in the 6.14-6.17 runbook.
- Deliberate bad bundles (kernel panic, engine never ready) can strand a board if the rollback does not work. Gate: do steps 8-10 first on the dev board that has a known recovery path (serial console plus ability to reflash the boot medium; method TBD from the Story 1.6 report), not on the only prototype.
- Test keys: generate dev and factory-test keys outside the repository (Story 6.1 AC2, EPICS:4124-4150: a CI check fails on committed private key material). Keep factory-test private key off developer laptops if policy requires (decision for founder; Story 6.1 hierarchy document is the source).
- A unit with a key mismatch in its keyring cannot accept bundles until reflashed; do not install a wrong-root bundle by force.
- Engine/UI/sysd "ready" semantics come from other stories; do not weaken them to make the mark-good test pass.

## 5. Entry / exit criteria mapped to ACs

Entry: section 2 gates 1-5.
Exit (for the founder to judge): 6.12 needs evidence for steps 1-13; 6.13 for 14-21. Failure or "not yet testable" is recorded, not hidden.

| AC | Steps | Evidence |
|---|---|---|
| 6.12-AC1 | 1-3 | RAUC config extract, keyring listing, CI bundle artifact |
| 6.12-AC2 | 4-7 | RAUC status before/after, mark-good unit log |
| 6.12-AC3 | 8-11 | Serial logs of failed boots, RAUC status, state file |
| 6.12-AC4 | 12-13 | Rejection messages, slot hashes before/after |
| 6.13-AC1 | 14-17 | CI log, image feature diff, rejection messages |
| 6.13-AC2 | 18-20 | Screen photo, report file, not-present run |
| 6.13-AC3 | 21 | os-release text |

## 6. Evidence table (blank)

| Step | AC | Date | Operator | Board / serial | Image + bundle version | Result | Key observation | File / log reference | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 1-3 | 6.12-AC1 | | | | | | | | |
| 4-7 | 6.12-AC2 | | | | | | | | |
| 8-11 | 6.12-AC3 | | | | | | | | |
| 12 | 6.12-AC4 | | | | | | | | |
| 13 | 6.12-AC4 | | | | | | | | |
| 14-16 | 6.13-AC1 | | | | | | | | |
| 17 | 6.13-AC1 | | | | | | | | |
| 18-19 | 6.13-AC2 | | | | | | | | |
| 20 | 6.13-AC2 | | | | | | | | |
| 21 | 6.13-AC3 | | | | | | | | |

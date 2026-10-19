# DRAFT FOR FOUNDER — does not satisfy the listed stories' acceptance criteria

Runbook for Stories 6.14 (production image, secure boot, debug lockdown), 6.15 (OTP anti-rollback), 6.16 (factory provisioning station) and 6.17 (SoM swap re-provisioning). Prepared 2026-10-19. Nothing here has been run. The repo names no fuse map, OTP register, fuse address, OTP capacity, key size, registry API, programming fixture, rkdeveloptool-style command or maskrom procedure. Those are all "TBD from vendor docs" or "TBD from the Story 1.6/1.7 reports". Do not copy fuse values from the web into this runbook without a vendor citation.

Path shorthand (relative to `_bmad-output/`): EPICS = `planning-artifacts/epics.md`; SPINE, HWI = files in `planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/`.

## 0. HARD GATE: sacrificial unit first (applies to every step marked IRREVERSIBLE)

Fuses and OTP bits cannot be undone (EPICS:960-966, "fusing is irreversible"). Before any irreversible step on a unit that is not marked sacrificial:
1. The same procedure was executed end to end on a unit physically labelled SACRIFICIAL, with the evidence attached.
2. The result of that run was reviewed and signed off by the founder (name/date in the evidence table).
3. The exact fuse values were derived from the production public-key hash produced in the Story 6.10 ceremony (EPICS:4378-4401), checked against the vendor documentation for the fuse layout (TBD), and the second person confirmed the hash on screen.
4. The unit's serial is recorded and is not the only unit available for the story.
If any item is false: stop.

## 1. Acceptance criteria

Story 6.14 (EPICS:4491-4518; covers FR33, NFR23, NFR24 partial, AR3 partial, AR8 production use; runs on prototype, sacrificial units for fusing tests, EPICS:4497-4499)
- 6.14-AC1 Production variant built as the third variant of the 6.13 layer set, using Story 1.6 chain and Story 6.10 public keys: every boot stage signed with the production boot key; no SSH; no getty on any UART; no debug UART output after the loader; kernel `console=` empty; marked production in `/etc/os-release`; ships only the production RAUC keyring (4503-4505)
- 6.14-AC2 Prototype fused with the production root key hash and SoC JTAG disabled by fuse: boots the production image; an unsigned image, a dev-signed image or an image with one flipped byte does not boot; a JTAG probe cannot attach; bootloader offers no interactive console or alternative boot source (4507-4510)
- 6.14-AC3 USB-A drive with a bootable OS, or USB-C host trying to enter maskrom download: unit does not boot from it and maskrom loading refuses unsigned loaders (4512-4514)
- 6.14-AC4 Probed from outside: USB-C exposes only UAC2 + MIDI + NCM (or nothing if the gadget is not yet enabled); NCM accepts no SSH; no UART emits a login prompt or kernel log (4516-4518)

Story 6.15 (EPICS:4520-4543; covers AR35, NFR23 anti-rollback; runs on fused dev board (sacrificial) or prototype, EPICS:4528)
- 6.15-AC1 Security version field in signed image metadata and bootloader check against the OTP counter: image below the counter does not boot, and A/B slot logic falls back to the other slot if valid (4532-4534)
- 6.15-AC2 A new security release, after it booted and its boot-confirmed hook (called from the Story 6.12 mark-good unit) ran: OTP counter bumped to the image's security version, never before a successful boot; non-security releases never bump (4536-4539)
- 6.15-AC3 Procedure documented in `tools/keys/ANTI-ROLLBACK.md`: OTP capacity (number of possible bumps), release policy for using one, and test results on the sacrificial board (4541-4543)

Story 6.16 (EPICS:4545-4574; covers AR33, FR34 pre-installed wrapped, FR30, NFR23; runs on factory station (Linux PC with partner fixture) + prototype units; blocked by 11.7, EPICS:4553-4555)
- 6.16-AC1 Unit passed the factory-test checks (6.13) unfused; station connected to the registry; provisioning run reads serial from label/EEPROM and SoM ID, fuses production root key hash and debug lockdown, flashes the production image, starts the single-purpose provisioning agent (production-signed, no shell, refuses to run once marked provisioned); each step idempotent or refuses safely; station log records every step per serial (4559-4562)
- 6.16-AC2 Agent asks the content-key TA for device public key and ID; station registers device ID, public key, SoM ID, serial and tier; receives factory content keys wrapped for the device; installs factory release bundles and wrapped-key blobs through the librarian install path (4564-4566)
- 6.16-AC3 Final check: unit boots production, a factory instrument renders a test note matching the expected checksum on the unit, provisioning agent disabled for good, registry status active (4568-4570)
- 6.16-AC4 On any failure (fuse error, registry unreachable, install error): station stops, shows step and reason, never marks the unit active, and the unit can be resumed or quarantined (4572-4574)

Story 6.17 (EPICS:4576-4602; covers AR33 SoM swap; runs on factory station repair mode + prototype, EPICS:4582-4584)
- 6.17-AC1 Unit identified by serial, SoM replaced, repair mode: fuses and provisions the new SoM as in 6.16; registry links serial to new SoM ID and device key and marks the old device ID revoked with reason and date (4588-4590)
- 6.17-AC2 Registry wraps the factory content keys for the new device key; station installs new wrapped-key blobs; existing encrypted bundles on the NVMe reused without copying; factory library plays (4592-4594)
- 6.17-AC3 User data on the NVMe (setlists, Live Sets, imported libraries) preserved and opens unchanged (4596-4598)
- 6.17-AC4 Revoked old SoM placed in another unit with the original wrapped-key blobs: registry refuses any further wrap for it; blobs wrapped for the new SoM do not unwrap on it (4600-4602)

Architecture context: AD-14 content keys wrapped to the OP-TEE device key, never on disk (SPINE:215-227); AD-25 provisioning sequence and anti-rollback (SPINE:329-341); AD-26 only signed RAUC bundles, `sysd` sole installer (SPINE:343-349); HWI:131-137 (factory fuses RK3588 OTP secure-boot keys, JTAG/SWD disabled, debug UART internal only, serial readable on label and EEPROM, registry links serial to SoM ID); NFR23/24 (EPICS:273-274); OP-TEE >=4.9.0 (SPINE:434-440).

## 2. Preconditions and equipment

Entry gates (stop if any is false):
1. Story 1.6 report: chain selected, fuse procedure for the dev root documented in Story 1.7 (EPICS:960-975: exact fusing steps, HUK source, blockers). Both reports are not in the repo at preparation time (`tools/spikes/secure-boot/REPORT.md`); this runbook cannot proceed without them. Chain: ____ Fusing procedure ref: ____
2. Story 1.7 proven on a sacrificial dev board: boots signed image, rejects unsigned/different-key image, derives the same key across reboots and a different one on a second board.
3. Story 6.10 ceremony done: production public keys and their hashes available (EPICS:4378-4401). Private keys never leave the offline HSM; the production image is signed on the offline laptop only.
4. Stories 6.12 and 6.13 done (RAUC A/B with mark-good unit; layer set with variants).
5. Story 11.7 delivered: partner programming fixture, SoM ID and serial EEPROM location, tier-ID strap (EPICS:4555; HWI:139-148). Fixture: ____ (TBD from vendor docs).
6. Registry service exists (AR34, EPICS:373; interface TBD) and the factory content keys exist, wrapped per device (AD-14/25). Registry test instance separate from production registry: ____
7. Sacrificial units: how many? Quantity and serials: ____ At least one for 6.14/6.15/6.16 and a second for the SoM-swap path in 6.17.

Equipment (categories; models TBD): offline signing laptop + HSM (not for use at the station); factory station Linux PC with the partner's programming fixture; JTAG/SWD probe supported by the SoC (model TBD from vendor docs) for the attach test; USB-A drive with a bootable OS image; USB-C host computer; UART adapters for the debug header; serial console logger; a second SoM and a second carrier (for 6.17), with the means to swap a SoM (TBD from vendor docs); reference NVMe test data (setlist, Live Set, imported library) for 6.17-AC3.

## 3. Ordered steps

Steps marked IRREVERSIBLE are behind the section 0 gate.

### 6.14 Production image and lockdown

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 1 | Build the production variant as the third variant of the 6.13 layer set, signed with production keys on the offline laptop only | Image artifacts; every boot stage signed (loader, TF-A, OP-TEE, bootloader, kernel FIT per EPICS:938) | 6.14-AC1 |
| 2 | Static inspection: services list, getty units, kernel command line, os-release, RAUC keyrings | No SSH, no getty on any UART, `console=` empty, os-release says production, only the production RAUC keyring | 6.14-AC1 |
| 3 | Dry run on an UNFUSED sacrificial unit: boot the production image | Boots; no debug output after the loader; no console login | 6.14-AC1 |
| 4 | IRREVERSIBLE: on a sacrificial unit only, program the production root key hash and the SoC JTAG-disable fuse by the Story 1.7 procedure (fuse fields TBD from vendor docs) | Procedure completes; fuse readback matches the intended values | 6.14-AC2 |
| 5 | Power the fused sacrificial unit | Boots the production image | 6.14-AC2 |
| 6 | Try, in turn: an unsigned image, a dev-signed image, the production image with one flipped byte | None boots | 6.14-AC2 |
| 7 | Connect a JTAG probe; check the bootloader for any interactive console or alternative boot source | Probe cannot attach; no console, no alternative boot source | 6.14-AC2 |
| 8 | Insert a USB-A drive with a bootable OS; reset | Unit does not boot from it | 6.14-AC3 |
| 9 | Connect the USB-C to a host and attempt to enter maskrom download with an unsigned loader (entry method TBD from vendor docs) | Unit does not boot from it; maskrom loading refuses unsigned loaders | 6.14-AC3 |
| 10 | Probe from outside: enumerate USB-C descriptors on a host; port-scan the NCM interface for SSH; listen on every UART | Only UAC2 + MIDI + NCM (or nothing if gadget not yet enabled); no SSH; no login prompt or kernel log on any UART | 6.14-AC4 |

### 6.15 Anti-rollback (sacrificial only)

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 11 | Read from the vendor docs how many OTP bits are available for the counter and how the bootloader reads them (TBD from vendor docs); write the capacity number into `tools/keys/ANTI-ROLLBACK.md` | Capacity (number of possible bumps) documented | 6.15-AC3 |
| 12 | Build signed images carrying security versions N and N+1 (field in signed metadata) | Metadata present and covered by the signature | 6.15-AC1 |
| 13 | Boot a sacrificial unit on version N at counter value N | Boots | 6.15-AC1 |
| 14 | Install version N+1 as a security release into the inactive slot; reboot; wait until the mark-good unit runs | Counter still N until the boot-confirmed hook runs; then the counter becomes N+1 | 6.15-AC2 |
| 15 | IRREVERSIBLE: the counter bump in step 14 burns an OTP bit. Run it only on the sacrificial unit | Readback shows the new counter value | 6.15-AC2 |
| 16 | Present version N (below the counter) | Does not boot; A/B logic falls back to the other slot if valid | 6.15-AC1 |
| 17 | Install a non-security release (same security version) and boot it to mark-good | Counter unchanged | 6.15-AC2 |
| 18 | Fail a security release before mark-good (for example a panic build) | Counter not bumped | 6.15-AC2 |
| 19 | Write results and the release policy for using one bump into `tools/keys/ANTI-ROLLBACK.md` | Document contains capacity, policy, sacrificial-board results | 6.15-AC3 |

### 6.16 Factory provisioning (per unit, on the station)

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 20 | Confirm the unit passed the factory-test image checks (6.13) while still unfused | Test report on record | 6.16-AC1 |
| 21 | Start the provisioning run: station reads serial (label/EEPROM) and SoM ID | Both values logged against the serial | 6.16-AC1 |
| 22 | IRREVERSIBLE: station fuses the production root key hash and debug lockdown (section 0 gate; first run per fixture on a sacrificial unit) | Fuse readback verified; station log entry | 6.16-AC1 |
| 23 | Station flashes the production image (6.14) | Image flashed, verified | 6.16-AC1 |
| 24 | Start the provisioning agent (production-signed, no shell) | Agent runs; trying to run it again on a provisioned unit is refused | 6.16-AC1 |
| 25 | Repeat any completed step (power-cycle the station mid-run, rerun) | Step is idempotent or refuses safely; log shows it | 6.16-AC1 |
| 26 | Agent asks the content-key TA for device public key and ID | Public key and ID returned; no private key leaves OP-TEE | 6.16-AC2 |
| 27 | Station registers device ID, public key, SoM ID, serial, tier | Registry entry created (not yet active) | 6.16-AC2 |
| 28 | Receive factory content keys wrapped for the device; install factory release bundles and wrapped-key blobs through the librarian install path | Bundles and blobs installed | 6.16-AC2 |
| 29 | Final check: boot production; render the test note on a factory instrument | Checksum matches the expected value (value TBD, from the content team) | 6.16-AC3 |
| 30 | Disable the provisioning agent for good; set registry status to active | Agent refuses to run; registry active | 6.16-AC3 |
| 31 | Failure injection: unplug the registry link; force a fuse error on a sacrificial unit if possible; fail an install | Station stops, shows step and reason, does not mark the unit active; unit can be resumed or quarantined | 6.16-AC4 |

### 6.17 SoM swap (repair mode)

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 32 | Take a provisioned sacrificial unit with user data on the NVMe (setlists, Live Sets, imported library); record checksums | Baseline recorded | 6.17-AC3 |
| 33 | Replace the SoM with a new, unfused SoM (method TBD from vendor docs); identify the unit by serial | Unit identified by serial in the station | 6.17-AC1 |
| 34 | Run the station in repair mode | New SoM fused and provisioned as in steps 21-24 | 6.17-AC1 |
| 35 | Check the registry | Serial linked to new SoM ID and device key; old device ID revoked with reason and date | 6.17-AC1 |
| 36 | Registry wraps the factory content keys for the new device key; station installs the new blobs; confirm bundles on NVMe were not copied (same files/inodes, checksums) | Existing encrypted bundles reused; factory library plays | 6.17-AC2 |
| 37 | Open the user data | Preserved and opens unchanged vs step 32 | 6.17-AC3 |
| 38 | Place the revoked old SoM in another sacrificial unit with the original wrapped-key blobs; also try the new-SoM blobs on it | Registry refuses any further wrap for the old SoM; new-SoM blobs do not unwrap on it | 6.17-AC4 |

## 4. Risks and irreversible steps (highlighted)

- IRREVERSIBLE: secure-boot root key hash fuse (steps 4, 22, 34). A wrong hash bricks the unit permanently for any image not signed by the matching key. Gate: section 0.
- IRREVERSIBLE: SoC JTAG-disable fuse (steps 4, 22). After this no debug on that unit; field diagnostics rely on logs only. Gate: section 0; confirm with the founder that debug lockdown is intended for the unit.
- IRREVERSIBLE: OTP anti-rollback counter bumps (steps 14-15). Capacity is finite (documented in step 11). Do not exhaust bits during testing; budget the number of bumps on the sacrificial unit before starting.
- IRREVERSIBLE in effect: MCU SWD/JTAG option bytes (Story 3.4) belong to the MCU runbook, but the station fixture may program them; the same sacrificial-first rule applies (TBD from vendor docs).
- Production private keys: never on the station or a developer laptop (EPICS:4378-4401, NFR23). Signing happens only on the offline laptop with the HSM.
- Registry: use a test registry for the sacrificial runs; revoking a device ID in the production registry (step 35) cannot be undone cleanly (decision for the founder).
- Factory content keys wrapped for a device should only be requested for units that completed fuse and production flash; wrapping before that would create keys for devices that never provision.
- Failure semantics (6.16-AC4): never force a failing unit to "active" to move the line.
- Station and fixture details (programming fixture protocol, USB boot entry) are unknown here; do not improvise commands.

## 5. Entry / exit criteria mapped to ACs

Entry: section 2 gates 1-7 and section 0 for any irreversible step.
Exit (for the founder to judge): evidence for every AC row below on at least one sacrificial unit; production-unit runs only after the sacrificial run is signed off.

| AC | Steps | Evidence |
|---|---|---|
| 6.14-AC1 | 1-3 | Signing log, service/getty/cmdline listing, os-release, keyring listing |
| 6.14-AC2 | 4-7 | Fuse readback, four boot attempts, JTAG attach failure |
| 6.14-AC3 | 8-9 | Boot logs/photos, maskrom refusal |
| 6.14-AC4 | 10 | USB descriptors, port scan, UART captures |
| 6.15-AC1 | 12-13, 16 | Boot refusal log, fallback log |
| 6.15-AC2 | 14-15, 17-18 | Counter readbacks before/after each case |
| 6.15-AC3 | 11, 19 | `tools/keys/ANTI-ROLLBACK.md` content |
| 6.16-AC1 | 20-25 | Station log per serial |
| 6.16-AC2 | 26-28 | Registry record, install log |
| 6.16-AC3 | 29-30 | Checksum match, agent refusal, registry status |
| 6.16-AC4 | 31 | Station error screens, registry state |
| 6.17-AC1 | 32-35 | Registry before/after |
| 6.17-AC2 | 36 | Install log, file identity proof, play test |
| 6.17-AC3 | 37 | Checksums before/after |
| 6.17-AC4 | 38 | Registry refusal, failed unwrap log |

## 6. Evidence table (blank)

| Step | AC | Date | Operator | Unit serial / SoM ID | Sacrificial? (Y/N) | Image + key version | Result | Readback / measured value | File / log reference | Founder sign-off (name/date) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1-3 | 6.14-AC1 | | | | | | | | | |
| 4-5 | 6.14-AC2 | | | | | | | | | |
| 6 | 6.14-AC2 | | | | | | | | | |
| 7 | 6.14-AC2 | | | | | | | | | |
| 8 | 6.14-AC3 | | | | | | | | | |
| 9 | 6.14-AC3 | | | | | | | | | |
| 10 | 6.14-AC4 | | | | | | | | | |
| 11 | 6.15-AC3 | | | | | | | | | |
| 13, 16 | 6.15-AC1 | | | | | | | | | |
| 14-15 | 6.15-AC2 | | | | | | | | | |
| 17-18 | 6.15-AC2 | | | | | | | | | |
| 19 | 6.15-AC3 | | | | | | | | | |
| 20-25 | 6.16-AC1 | | | | | | | | | |
| 26-28 | 6.16-AC2 | | | | | | | | | |
| 29-30 | 6.16-AC3 | | | | | | | | | |
| 31 | 6.16-AC4 | | | | | | | | | |
| 32-35 | 6.17-AC1 | | | | | | | | | |
| 36 | 6.17-AC2 | | | | | | | | | |
| 37 | 6.17-AC3 | | | | | | | | | |
| 38 | 6.17-AC4 | | | | | | | | | |

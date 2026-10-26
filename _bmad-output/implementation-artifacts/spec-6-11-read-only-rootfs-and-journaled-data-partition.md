---
title: 'Story 6.11: Read-only root filesystem and journaled data partition (config and Docker validation)'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '5cf996cd3f9b0cfe9f49c1744ba4afa0cbd3960d'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/yocto/README.md']
warnings: []
deferred:
  - summary: >-
      Not verified (needs full image build / dev board): booting the wic, / mounted ro with EROFS on /usr and /etc, tmpfs mounts, users and data directories on the booted system, rootfs fitting 2 GiB, the 20-hard-power-removal forced-fsck test (fsck passno 0 needs fsck.mode=force).
    evidence: |-
      Validation is wks parse, recipe/variable and do_install-level (81 checks in Docker). See yocto/README.md Open points.
    severity: high
  - summary: >-
      Open design points: NVMe/PCIe/EFI_PARTITION built into the kernel for root=PARTLABEL without initramfs; which stage reads the loader/uboot_env partitions on NVMe; /data growth to the real NVMe size; OP-TEE secure storage must not land on tmpfs /var/lib; no separate boot partition (loader partitions + /boot in slot A).
    evidence: |-
      Recorded in the README.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** The dev image has no committed disk layout and its root is writable, so a power cut can corrupt the OS and nothing separates persistent user data (AR36 partial, NFR15 partial, NFR26 partial).

**Approach:** Extend the `yocto/` layer (Story 1.4) with: a committed `wks` file defining the NVMe layout (boot area, two root slots sized for A/B with only slot A populated, one `/data` partition on ext4 `data=ordered` with the reason recorded in `yocto/README.md`); read-only root (`read-only-rootfs`) with tmpfs for `/var/volatile`, `/tmp`, `/run` and volatile bounded journald (already present, keep consistent); a `/data` mount with `nodev,nosuid`; per-service data directories `/data/lib`, `/data/catalog`, `/data/livesets`, `/data/config` created at boot (tmpfiles.d / mount-time) and owned by their service users (engine control thread, librarian, sysd per the AD-9 single-writer table in the architecture docs); and Docker validation of all of it without an image build.

## Boundaries & Constraints

**Always:** The layout lives in one `wks` file under `yocto/meta-velora` (wic) that every image build uses. Parse the `wks` in the Docker validation with wic's own kickstart parser (from the pinned openembedded-core checkout) and check: boot partition, two root partitions of identical size, one data partition, ext4 with `data=ordered` for data (via fstab/mount options or `--fsoptions`), root slots labelled for A/B (`rootfs.0`/`rootfs.1` naming used later by RAUC; slot B left unpopulated), `/data` options include `nodev,nosuid`. Read-only root via IMAGE_FEATURES `read-only-rootfs` for the dev image; `/etc` and `/usr` stay read-only (document that the dev image therefore needs no writable /etc; machine-id and similar writable state go to tmpfs or /data/config, state which). Service users (e.g. `velora-engine`, `velora-librarian`, `velora-sysd`) are created through `useradd` in recipes (extrausers or useradd class), directory ownership/modes per the AD-9 table (read it in _bmad-output/planning-artifacts/architecture, find AD-9 and the single-writer table and cite it in a README table). Static + bitbake -e checks added to the existing validation (`yocto/ci/`) and the validation must still pass end to end. Keep Docker disk use small.

**Never:** No full image build, no boot, no fsck / power-removal test (dev board), no RAUC (Story 6.12), no changes outside `yocto/`, `.github/workflows/`, docs. Do not invent NVMe sizes silently: choose explicit sizes, state them and the reasoning in the README (assumption: 1 TB library lives on a separate SSD library volume or on /data? read the architecture notes on library placement and follow them; if unclear, size /data to hold Live Sets/config/catalog only and record the question under unresolved).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Layout | committed wks | parses; boot + 2 equal root slots + 1 ext4 data (data=ordered) | check fails otherwise |
| Slot B | wks | slot B partition exists, unpopulated | check fails otherwise |
| Read-only | image config | read-only-rootfs on, tmpfs for /var/volatile, /tmp, /run | check fails otherwise |
| Data mount | fstab | /data nodev,nosuid | check fails otherwise |
| Data dirs | tmpfiles/recipe | /data/lib,/catalog,/livesets,/config exist with owner per AD-9 | check fails otherwise |
| Journald | config | volatile, bounded | check fails otherwise |

## Code Map

- `yocto/meta-velora/**` (image recipe, velora-system-config recipe, distro conf), `yocto/kas/velora-dev.yml`, `yocto/ci/static_checks.py`, `validate-inner.sh`, `validate.sh`, `yocto/README.md` -- from Story 1.4.
- openembedded-core wic: `scripts/lib/wic/ksparser.py`, `meta/classes-recipe/image_types_wic.bbclass`, `WKS_FILE` -- layout mechanism.
- `_bmad-output/planning-artifacts/architecture/**` (AD-9 single-writer table, /data layout, NFR15) -- ownership and paths.

## Tasks & Acceptance

**Execution:**
- `yocto/meta-velora/wic/velora-nvme.wks` (or equivalent path) + image recipe WKS_FILE -- the layout
- fstab / mounts / read-only rootfs / tmpfs / service users / data dirs in the layer
- validation -- wic parse checks, static checks, bitbake -e checks (IMAGE_FEATURES, WKS_FILE), integrated in `validate.sh`
- `yocto/README.md` -- layout table with sizes, ext4 data=ordered reason, AD-9 table, EROFS expectation, what is NOT verified (boot, EROFS, 20-power-cut fsck test on the dev board)

**Acceptance Criteria:**
- Given the layer, then a committed wks defines boot + two root slots + /data (ext4 data=ordered, nodev,nosuid) and the Docker validation parses and checks it.
- Given the image config, then root is read-only, runtime paths are tmpfs and journald is volatile and bounded.
- Given the data directories, then they exist per the AD-9 table with service-user ownership.
- Boot-time EROFS behaviour and the 20-power-removal fsck test are recorded as not done (dev board).

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass (single focused reviewer; config work)
- findings:
  - `[high]` `patch` bootargs/wks tie, rootwait/rootfstype, wic in IMAGE_FSTYPES, boot-area offsets, static GIDs, produced-artefact checks (do_install), wic fetched via bitbake, README overstatements — fixed (validation 55 -> 81 checks)
  - `[medium]` `defer` kernel NVMe built-in audit, OP-TEE storage path, fsck passno/forced fsck, fstab device naming, nofail

## Verification

**Commands:**
- `bash yocto/ci/validate.sh` -- expected: all checks pass (previous 44 plus the new ones)

## Auto Run Result

Summary: velora-nvme.wks (loader area, rootfs.0 populated 2 GiB, rootfs.1 empty, /data 4 GiB ext4 data=ordered nodev,nosuid), read-only root + tmpfs, service users with static UID/GID, AD-9 tmpfiles, bootargs for root=PARTLABEL, wic-based layout checks.
Verification: bash yocto/ci/validate.sh 81/81 (re-run by the orchestrator).

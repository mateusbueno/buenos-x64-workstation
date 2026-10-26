# yocto/: Velora dev and factory-test images (Stories 1.4, 6.13)

Reproducible, licence-clean dev image for the RK3588 dev board on Yocto 6.0 "Wrynose" LTS (systemd, Linux 6.18 LTS mainline with PREEMPT_RT, no vendor kernel; see `KERNEL-LTS-PLAN.md`).

## Status

| Item | State |
|---|---|
| Layer and recipe parse (`bitbake -p`), image recipe resolves (`bitbake -g` provider resolution) | validated in Docker (`ci/validate.sh`) |
| Licence policy (GPL-3.0/LGPL-3.0/AGPL-3.0 blocked, GPLv2 and GCC-exception runtimes allowed) | policy values validated in Docker at recipe level (dry run); the image-level `do_rootfs` enforcement is **NOT** exercised (needs a build) |
| flac / metaflac / libsndfile absent from the image | recipe-level only: absent from the `bitbake -g` dependency graph (`pn-buildlist` and `task-depends.dot`); `PACKAGE_EXCLUDE` (also libflac, libflac++) is the package-level guard and is NOT exercised without a build |
| Dev markers, no swap, no core dumps, SSH, debug UART | statically checked and checked on the resolved bitbake configuration |
| Disk layout (`files/wic/velora-nvme.wks`): boot area, `rootfs.0`/`rootfs.1`, `/data` | parsed in Docker with wic's own kickstart parser and checked (Story 6.11, `ci/check_wks.py`); the `.wic` itself is **NOT** produced |
| Read-only root, tmpfs runtime paths, `/data` options, service users, data directories | statically checked and checked on the resolved bitbake configuration (Story 6.11); runtime behaviour **NOT** verified (needs a build and a boot) |
| RAUC A/B slots, dev keyring only, dev-signed bundle recipe, mark-good unit, rollback recorder, U-Boot A/B script (Story 6.12) | config and recipes parse/resolve in Docker; bundle signing, signature/tamper rejection, recorder and boot-script logic **tested in Docker** (`ci/rauc/`); **install, boot-counter switching and rollback at boot are NOT done** (need the dev board) |
| Image variants: shared base, dev and factory-test images, per-variant bundle recipes and keyrings, factory test runner, `/etc/os-release` variant (Story 6.13) | recipes parse/resolve in Docker; the factory-test variant is checked on the resolved bitbake configuration (no SSH, runner installed, own keyring only); cross-root rejection (both directions), own-root verification and the runner with simulated hardware **tested in Docker** (`ci/rauc/`, `ci/factory-test/`); **boot, the on-screen display and every real hardware probe are NOT done** |
| **Full image build** | **NOT done** (needs a Linux host with >= 100 GB free disk) |
| **Boot on the dev board** | **NOT done** |
| **1 h cyclictest under the Story 1.1 load profile** | **NOT done** |
| Kernel config audit (the `.cfg` fragments only apply at kernel build) | **NOT done** |
| `.github/workflows/yocto.yml` | written, **never run** |

## Unresolved dependencies and open points

- **TODO(Story 1.1)**: `meta-velora/recipes-kernel/linux/files/preempt-rt.cfg` and the `isolcpus`/`nohz_full`/`rcu_nocbs` values in `meta-velora/conf/distro/include/velora-bootargs.inc` are PLACEHOLDERS. Story 1.1 (PREEMPT_RT latency spike) is not done; its validated fragment and boot arguments must replace them. Cores 5-7 are an assumption from the epic text.
- **TODO(Story 1.2)**: the audio configuration (I2S/TDM, PCM3168A, UAC2 gadget, ALSA settings) is not present. `linux-yocto_%.bbappend` carries the marker where the fragment goes.
- **RK3588 BSP / board**: the BSP layer exists for Wrynose (`meta-rockchip`, branch `wrynose`, pinned in `kas/velora-dev.yml`), so no fallback machine definition was needed. It has `rock-5b` but no `rock-5b-plus` machine: the dev image uses `rock-5b` as the closest board (the epic says "ROCK 5B+ class"). Whether `rock-5b` boots a ROCK 5B+ is unverified. `meta-rockchip` ships a closed DDR/TPL blob (rkbin, proprietary licence); this is a licence/provenance point for the factory image and must be reviewed before production (not blocked by `INCOMPATIBLE_LICENSE`, which only matches GPL-3.0/LGPL-3.0/AGPL-3.0).
- **poky**: the `poky` repository has no `wrynose` branch (Yocto now publishes `openembedded-core` + `bitbake` + `meta-yocto`). The kas file therefore pins `bitbake` (2.18), `openembedded-core`, `meta-openembedded`, `meta-arm`, `meta-rockchip` directly. `meta-yocto` is not used: `velora.conf` builds on `conf/distro/defaultsetup.conf`.
- No SSH key is committed. Root login is by key only; pass a public key with `VELORA_DEV_AUTHORIZED_KEYS_FILE` (see below). The dev image enables systemd-networkd with DHCP on ethernet so SSH is reachable. **The debug UART login path is still undecided**: root has no password, so the serial console shows a getty that nobody can log into until a decision is made (open point for the board bring-up).
- Dev and factory-test images exist (Story 6.13). The production image, secure boot and production keys (Story 6.14, 8.x/AR8) are out of scope and NOT built.
- **TODO(Story 1.6)**: the RAUC bootloader backend (`bootloader=uboot`), `fw_env.config`, and `files/uboot/velora-ab-boot.cmd` are PLACEHOLDERS for the boot chain chosen in Story 1.6 (barebox or U-Boot with Rockchip tools).

## Layout

```
yocto/
  kas/velora-dev.yml            kas project: machine, distro, target, layers pinned to commits
  kas/velora-factory-test.yml   factory-test variant: includes the dev file (same pins), sets target and VELORA_VARIANT
  meta-velora/                  our layer
    conf/distro/velora.conf     distro: systemd, INCOMPATIBLE_LICENSE, os-release dev marker, debug UART
    conf/distro/include/velora-bootargs.inc   bootargs placeholder (Story 1.1)
    recipes-core/images/velora-image-base.inc            shared base of every image variant
    recipes-core/images/velora-image-dev.bb              dev variant (SSH, authorized keys)
    recipes-core/images/velora-image-factory-test.bb     factory-test variant (no SSH, runner)
    recipes-core/velora-factory-test/                    factory test runner (Python) and its systemd unit
    recipes-core/velora-system-config/        sysctl (no swap, no cores), coredump.conf, limits, journald
    recipes-kernel/linux/       linux-yocto bbappend + preempt-rt.cfg (placeholder) + velora-dev.cfg (no swap)
  meta-velora/files/wic/velora-nvme.wks           the one disk layout (Story 6.11)
    recipes-core/velora-service-users/        useradd: velora-engine, velora-librarian, velora-sysd
    recipes-core/base-files/                  fstab additions (tmpfs /tmp, /data mount point)
  meta-velora/recipes-core/rauc/            RAUC (Story 6.12): rauc-conf/rauc bbappends, system.conf, fw_env.config, mark-good unit and ready targets, rollback recorder, U-Boot A/B script recipe
  meta-velora/recipes-core/bundles/                        velora-bundle-base.inc (shared), velora-bundle-dev.bb, velora-bundle-factory-test.bb
  meta-velora/files/uboot/velora-ab-boot.cmd              U-Boot A/B slot selection (BOOT_ORDER / BOOT_x_LEFT, 3 attempts)
  ci/rauc/                      gen-dev-root.sh (--variant dev|factory-test), Dockerfile (RAUC 1.15.2), test-bundles.sh, test_rollback_record.py, test_boot_script.sh, run-tests.sh
  ci/factory-test/test_runner.py   factory test runner tests (run inside the RAUC test container)
  ci/extract_bb_func.py         pulls one shell function out of a recipe so the Docker tests run the recipe's own code
  ci/check_wks.py               wks layout check (wic kickstart parser)
  ci/validate.sh                Docker validation entry point (no image build)
  ci/validate-inner.sh          runs inside the container
  ci/static_checks.py           static checks of the committed files
  ci/kas-test-layer.yml         validation-only kas overlay adding the test layer
  ci/make-ssh-overlay.sh        generates the kas overlay that installs a dev public key (used by CI and validation)
  ci/test-layer/meta-velora-test/   GPL-3.0/LGPL-3.0/AGPL-3.0/GPL-2.0 test recipes and test images (never in the product build)
  docker/Dockerfile             Ubuntu 24.04 + Yocto host packages + kas 4.7
```

## Validate (no image build, Docker only)

```
bash yocto/ci/validate.sh          # first run clones the pinned layers into the Docker volume velora-yocto-work (~1.5 GB) and parses
bash yocto/ci/validate.sh --clean  # delete that volume
```

The checks (recipe-level graph only; no package-level or `do_rootfs` validation): static config checks; kas checkout at pinned commits; `bitbake -p` over all layers; `bitbake -g velora-image-dev` (full provider resolution); `bitbake -g` package set (forbidden recipes absent, required ones present, self-test with a deliberately bad image); resolved variables via `bitbake -e`; licence policy (test images with a GPL-3.0, LGPL-3.0 or AGPL-3.0 recipe must fail with "incompatible license", the GPL-2.0 test image must resolve, and BusyBox, linux-yocto, U-Boot, libgcc and gcc-runtime must stay in the resolved image with the policy applied to them).

## Disk layout (Story 6.11)

Defined once in `meta-velora/files/wic/velora-nvme.wks` (`WKS_FILE = "velora-nvme.wks"` in the image recipe, which overrides the meta-rockchip default `rockchip.wks`). GPT on the NVMe (`nvme0n1`).

| Partition (GPT name) | Size | Filesystem | Role |
|---|---|---|---|
| `loader1`, `v_storage`, `reserved`, `reserved1`, `uboot_env`, `reserved2`, `loader2`, `atf` | ~16 MiB total, offsets from meta-rockchip's `rockchip.wks` | none (raw) | boot area: RK3588 loader, U-Boot env, U-Boot, ATF/OP-TEE |
| `rootfs.0` (slot A) | 2 GiB fixed | ext4, mounted `ro` | populated from the image; holds `/boot` (kernel, device tree, extlinux) |
| `rootfs.1` (slot B) | 2 GiB fixed, same size as A | none (raw, no filesystem) | left **unpopulated**; RAUC (Story 6.12) writes it. Slot B has no filesystem, so it has no label: only its GPT partition name `rootfs.1` is real |
| `data` | 4 GiB fixed | ext4 with journal, `defaults,nodev,nosuid,noatime,data=ordered` | `/data`, the only persistent writable filesystem |

Sizes are **assumptions**, not measurements: a 2 GiB slot leaves room for a minimal systemd image plus kernel and modules (a real build must confirm the rootfs fits; wic fails the build if it does not); the boot area keeps the meta-rockchip offsets (copied from `rockchip.wks` and checked against a fixed table in `ci/check_wks.py`). **Open question**: the RK3588 boot ROM does not boot from NVMe, so which stage reads the `loader1`/`loader2`/`uboot_env`/`atf` partitions on the NVMe (U-Boot from SPI flash, an SD card, eMMC) is not decided here; the offsets are inherited from the SD-card layout and are unverified for NVMe. **Deviation from the spec text** ("boot area"): the boot area is the raw loader partitions above plus `/boot` inside slot A; there is no separate boot partition. Kernel and extlinux config live in `/boot` of the active slot, so an A/B update switches kernel and rootfs together. The `rootfs.0`/`rootfs.1` names are GPT partition names (slot A's ext4 label is also `rootfs.0`); the kernel finds slot A with `root=PARTLABEL=rootfs.0` (set in `velora-bootargs.inc`, together with `ro` instead of meta-rockchip's `rw`). No swap partition.

**Library placement and `/data` size (unresolved).** The architecture puts library bundles under `/data/lib` (AD-9, AD-10; FR2 allows up to 1 TB of on-device libraries), so `/data` is the partition that must grow to fill the NVMe. The 4 GiB in the wks is only the dev image size: wic cannot grow a partition to fill a disk. **Open question**: how `/data` is grown to the real NVMe capacity (first-boot `systemd-repart` plus `resize2fs`, or a factory provisioning step), and whether the NVMe capacity is large enough for 1 TB of libraries plus both slots. Not decided here; Story 6.12 (RAUC) and the factory flow must settle it.

**Why ext4 `data=ordered`.** `/data` holds the SQLite catalog (WAL), Live Sets and sample bundles published by atomic rename (AD-9, AD-11). ext4 with its journal keeps the filesystem metadata consistent across a power cut (NFR15 partial); `data=ordered` writes file data before the metadata that references it, so a renamed or newly extended file never exposes stale blocks, at lower cost than `data=journal` (every write twice, bad for sample streaming) and with stronger guarantees than `writeback`. The journal is requested explicitly (`-O has_journal`) so it cannot be lost by a changed mke2fs default. `nodev,nosuid` because nothing on `/data` may be a device node or setuid binary; `noatime` avoids read-induced writes during shows.

### Read-only root

- `IMAGE_FEATURES` has `read-only-rootfs` (checked) and the kernel arguments carry `ro` (checked); oe-core also rewrites the fstab `/dev/root` entry to `ro` (**from oe-core source, unverified**), so `/etc` and `/usr` are read-only. The dev image therefore needs no writable `/etc`.
- tmpfs: `/run` and `/var/volatile` (stock base-files fstab), `/tmp` (explicit line from `base-files_%.bbappend`, `nodev,nosuid`, 64 MiB). oe-core `volatile-binds` maps `/var/lib`, `/var/cache`, `/var/spool`, `/srv` to `/var/volatile`, so they are RAM too (the package is checked to be in the image; the mapping is **from oe-core source, unverified**).
- Journald: volatile and bounded (`Storage=volatile`, `RuntimeMaxUse=32M`, `RuntimeMaxFileSize=8M`), unchanged from Story 1.4. Logs are lost on reboot.
- Writable state with no persistent home: `/etc/machine-id` is an empty file in the image, so systemd should mount a transient machine-id on tmpfs at every boot (a different ID each boot; **from oe-core source, unverified**); sshd host keys are generated into tmpfs as oe-core's read-only hook arranges (**from oe-core source, unverified**) (`/etc/ssh/sshd_config_readonly`), so the dev board's SSH host key changes every boot (open point: move host keys and a stable machine-id to `/data/config` if that is not acceptable). Anything that must persist goes to `/data/config` owned by `velora-sysd`.
- "EROFS" in the epic is the errno a write to a read-only filesystem returns (`EROFS`), not the EROFS filesystem type: the slots are ext4.

### Service users and data directories (AD-9)

Users come from `recipes-core/velora-service-users` (useradd class, static IDs 900-902, no login, no home). Directories are created at boot by `systemd-tmpfiles` from `/etc/tmpfiles.d/10-velora-data.conf` (the root filesystem is read-only, so this cannot be baked into `/data`, which is a separate filesystem). Source: `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md`, AD-9 "One writer per persistent dataset" (and AD-31 for Live Sets).

| AD-9 dataset | Sole writer (user) | Directory | Mode | Readers |
|---|---|---|---|---|
| Library bundles | librarian (`velora-librarian`) | `/data/lib` | 0755 | engine, read-only (AD-10) |
| Catalog (SQLite WAL) | librarian (`velora-librarian`) | `/data/catalog` | 0750 | ui and desktop through the librarian API only |
| Setlists, Live Sets, ASSIGN maps, setlist tags (AD-31) | engine control thread (`velora-engine`) | `/data/livesets` | 0750 | ui and desktop through published state |
| System config | sysd (`velora-sysd`) | `/data/config` | 0755 | others, at start and on change |
| Device and content keys | OP-TEE | not on `/data` | n/a | engine on request |

`/data` itself is `root:root 0755`. The librarian's staging directory must be on the same filesystem as `/data/lib` (epic 3, atomic rename): it has to live under `/data/lib` or `/data/catalog`; the librarian service unit (a later story) owns that choice. Process sandboxing (`ProtectSystem=strict`, `ReadWritePaths`) belongs to the service units, not to this story.

### Open points and deferred (Story 6.11)

- **Kernel config audit**: root is found with `root=PARTLABEL=rootfs.0` and no initramfs, so NVMe, PCIe and `CONFIG_EFI_PARTITION` (GPT) must be built into the kernel, not modules. The `.cfg` fragments do not set them yet; audit at the first kernel build.
- **fsck**: oe-core's `read_only_rootfs_hook` sets the root fstab pass number to 0 and the `/data` line from wic has none, so no automatic fsck is configured. The 20-power-removal test must force it (`fsck.mode=force`) or run `e2fsck` explicitly.
- **fstab fragility**: wic writes the `/data` fstab entry by device node (`/dev/nvme0n1pN`), which breaks if the partition numbers or the NVMe enumeration change. Decide `--use-label` or `--use-uuid` for `/data`.
- **No `nofail` on `/data`**: if `/data` cannot mount, boot drops to emergency mode. This is intended (nothing may write elsewhere), not tested.
- **OP-TEE secure storage**: `/var/lib` is a tmpfs bind, so OP-TEE secure storage must not land there. A persistent path (`/data/...`) or RPMB must be reserved in the OP-TEE story.
- **RAM bounds**: `/var/volatile` (and so `/var/lib`, `/var/cache`, `/var/tmp`) is tmpfs with no size limit (default half of RAM, shared); only `/tmp` (64 MiB) and the journal (32 MiB) are bounded.

### Not verified (Story 6.11)

All of the above is validated as configuration and parser output only. **NOT done**: building the `.wic`; booting; confirming that `/` really mounts `ro` and that writes under `/usr` and `/etc` fail with EROFS; that tmpfs mounts, users and directories exist with the right owners on a running system; that the rootfs fits a 2 GiB slot; and the acceptance test of forcing `fsck` after 20 hard power removals at idle (`/data` mounts cleanly every time, root slot unchanged against its build hash). Those need the dev board and are NOT done.

## RAUC: A/B slots, signed bundles, rollback (Story 6.12)

**What exists.** meta-rauc is pinned in `kas/velora-dev.yml` (`wrynose` branch, commit verified with `git ls-remote` on 2026-10-25; the layer declares `LAYERSERIES_COMPAT = wrynose` and builds RAUC 1.15.2). The dev image installs `rauc` (D-Bus service, json, no network/streaming/bundle creation), `rauc-conf`, `velora-rauc-mark-good`, `velora-rollback-record` and `velora-uboot-ab-script`, and a kernel fragment (`velora-rauc.cfg`: loop, squashfs, device-mapper, dm-verity; plain bundles need loop and squashfs, dm-verity is kept for a possible later switch; the first real build must audit it).

**Slots.** `files/system.conf`: `[slot.rootfs.0]` and `[slot.rootfs.1]` addressed by GPT name (`/dev/disk/by-partlabel/rootfs.N`, matching the Story 6.11 wks), ext4, bootnames `A` and `B`; compatible string `Velora RK3588` (equal to `RAUC_BUNDLE_COMPATIBLE`, checked by `ci/static_checks.py`); plain bundles only (`bundle-formats=-verity -crypt`: RAUC verifies the signature over the whole payload hash before it mounts or writes anything, so one flipped byte anywhere is rejected before any slot write; see the caveat below); RAUC status in `/data/rauc` (the root is read-only). Bootloader backend `uboot` is the placeholder for Story 1.6 (`TODO(Story 1.6)`; if barebox wins, switch to the barebox backend and drop `fw_env.config` and the U-Boot script). Wrynose has no `u-boot-fw-utils`: `fw_printenv`/`fw_setenv` come from `libubootenv-bin`; `fw_env.config` points at the `uboot_env` partition with an ASSUMED 0x8000 environment size.

**Flow.** `rauc install` (from `sysd`, Story 8.x) writes the bundle to the inactive slot and sets `BOOT_ORDER` to put it first with `BOOT_x_LEFT=3`. `files/uboot/velora-ab-boot.cmd` (compiled to `/boot/boot.scr`) takes the first slot of `BOOT_ORDER` with attempts left, decrements its counter, saves the environment and boots it. It does not use extlinux: slot B holds the same image as slot A, whose `extlinux.conf` says `root=PARTLABEL=rootfs.0`, so the script builds `root=PARTLABEL=rootfs.<n> rauc.slot=<A|B>` itself, finding the partition number by GPT name (`part number`). `rauc install` sets the counter of its target slot to 3 and puts that slot first in `BOOT_ORDER`; `rauc status mark-good` resets the booted slot's counter to 3; the script's exhaustion rule (below) is the only other place a counter is set. A slot that is never marked good boots 3 times, then the other slot boots (the rollback), and RAUC reports the failed slot bad. If every slot of `BOOT_ORDER` is out of attempts, only the LAST slot of `BOOT_ORDER` (the fallback: the previously good slot after an install put the new one first) gets 3 fresh attempts and the board resets; a slot at 0 that RAUC marked bad is never given attempts again by the script, so a fallback that also fails is rebooted forever and the known-bad slot is not resurrected. This is fail-stuck, not a guarantee against a brick: a board whose fallback slot is itself broken keeps rebooting it.

**Mark good.** `velora-rauc-mark-good.service` runs `rauc status mark-good` and has `Requires=` and `After=` on `velora-engine-ready.target`, `velora-ui-ready.target` and `velora-sysd-ready.target`. Contract: each service is `Type=notify`, sends `READY=1` only when it can do its job, and declares `RequiredBy=`/`Before=` on its target; the unit is never wanted by `default.target` and has no timer or "after boot" shortcut (its only `[Install]` entry is `multi-user.target`, which merely starts it while `Requires=`/`After=` hold it back). The services do not exist, so each target carries a PLACEHOLDER `Requires=velora-<name>.service` (marked `TODO`) that makes the target fail: until the real services land, nothing marks a slot good (fail closed). The meta-rauc stock mark-good unit (wanted by multi-user.target, waits only for `boot-complete.target`) is excluded (`BAD_RECOMMENDATIONS`, `RRECOMMENDS:remove`).

**Rollback record.** `velora-rollback-record` (Python 3, stand-in for `sysd`, run by `velora-rollback-record.service` at boot) reads `rauc status --detailed --output-format=json`. The attempted slot is the one with the newest `installed.timestamp`; if the booted slot is another one, it writes `/data/config/rollback.json` (`from_version` = attempted/failed version, `to_version` = booted version, `timestamp`, `reason` = `boot-failed` when RAUC reports the attempted slot's `boot_status` bad, else `slot-mismatch`, plus slot names and the failed slot's install timestamp). Atomic: temp file in `/data/config`, `fsync`, rename, directory `fsync`; a failed write leaves the previous file and no temp file. It does not rewrite an identical record (keeps the first timestamp), writes nothing when no rollback happened, and does not clear an old record (the UI or `sysd` does).

**Keys.** Nothing secret is committed. `ci/rauc/gen-dev-root.sh` creates the dev root (ECDSA P-256 CA, signing certificate chained to it) in `$XDG_DATA_HOME/velora-dev-keys/rauc` (else `~/.velora-dev-keys/rauc`, next to `velora_keys.py`'s directory; it refuses any directory inside the repository and never overwrites without `--force`). Only `ca.cert.pem` (public) goes into the image, through `VELORA_RAUC_KEYRING_FILE`; the signing key stays on the developer machine and is read by path (`VELORA_RAUC_SIGN_DIR`) by the bundle recipe. Without the variables the layer still parses; `rauc-conf` `do_install` fails with "VELORA_RAUC_KEYRING_FILE is not set" (or "missing or empty", or "contains a private key"), and the bundle recipe fails with "VELORA_RAUC_SIGN_DIR is not set". The keyring must hold exactly one certificate and its subject must carry the `not for production` marker `gen-dev-root.sh` writes, so a production or foreign CA cannot be baked into the dev image by mistake (`rauc-conf` `do_install` fails otherwise); the bundle recipe additionally checks that the signing certificate verifies against the keyring and that the key matches the certificate, and fails when `VELORA_RAUC_KEYRING_FILE` is unset. `system.conf` sets `use-bundle-signing-time=true` (the certificate chain is judged at the time recorded in the bundle, not the device clock): a DEV-ONLY trade-off, also noted in `tools/keys/KEY-HIERARCHY.md`, to revisit in Story 6.13 before factory-test and production keyrings exist. The certificate has no `extendedKeyUsage` on purpose: RAUC verifies with the S/MIME-signing purpose and rejects a code-signing-only certificate.

```
bash yocto/ci/rauc/gen-dev-root.sh
# kas overlay or local.conf:
#   VELORA_RAUC_KEYRING_FILE = "/home/me/.velora-dev-keys/rauc/ca.cert.pem"
#   VELORA_RAUC_SIGN_DIR     = "/home/me/.velora-dev-keys/rauc"
kas build yocto/kas/velora-dev.yml --target velora-bundle-dev   # after the real image build prerequisites
```

**Tests (Docker, `bash yocto/ci/rauc/run-tests.sh`, part of `ci/validate.sh`).** Ubuntu's `rauc` is older than 1.15.2, so the test image (`ci/rauc/Dockerfile`, 301 MB as measured by `docker images`, remove with `validate.sh --clean`) builds RAUC 1.15.2 from the release tarball with the sha256 meta-rauc pins; no service, no bootloader access. The test generates throw-away dev and foreign roots inside the container and checks: the generator's own rules; the committed `system.conf` is accepted by RAUC; a dev-signed plain bundle builds (with the keyring check the recipe uses), `rauc info` verifies it against the dev keyring; a bundle signed by a key outside the keyring is rejected (`rauc info`, `rauc install`, and bundle creation with `--keyring`); one flipped byte is rejected by `rauc info` at five offsets (signature, signature-length trailer, start/middle/end of the payload); `rauc install` of each rejected bundle is checked in its LOG: it ends with the specific verification failure and never reaches the stages after verification (mount, compatible check, slot writes), with the dev-signed bundle's install log as positive control. "Slot images byte-identical" is also asserted but proves nothing on its own, because this container cannot mount a bundle and would leave the slots untouched even if verification had passed; the bundle recipe's signing-file check (own shell function, stub `bbfatal`); the recorder (unit tests on real `rauc status` JSON, atomicity under injected fsync/rename/permission failures); the U-Boot script (the real file against stub U-Boot commands: boot A, boot B after install with `root=PARTLABEL=rootfs.1`, three attempts then fallback, both exhausted, reset). Bitbake side (`ci/validate-inner.sh` 5d): keyring handling (missing, empty, private key, valid), bundle recipe variables, `do_install` of `rauc-conf`, the units, the recorder and `boot.scr`.

**What the tests do NOT prove, and findings.**
- **Plain format: time-of-check/time-of-use.** RAUC verifies a plain bundle completely, then mounts and reads it again during install. If an attacker can rewrite the bundle file between verification and install (for example a writable USB stick or a bundle in a world-writable directory), the second read is not covered. Mitigations are for Story 8.x: `sysd` copies the bundle into a root-only directory on `/data` before `rauc install`, or passes `--require-manifest-hash`/the bundle over a descriptor it owns. Plain bundles also have to be read once in full before install starts (slower than verity).
**Open points (RAUC, found in review, not solved here).**
- A stale `rollback.json` stays after a later successful update; `sysd` must supersede or clear it (this tool never removes it).
- `rollback.json` is written by a root unit into a `sysd`-owned directory (`/data/config`, `velora-sysd`), so it is `root:root` 0644; the owner changes when `sysd` takes the writer over.
- A power cut during or right after an update can produce a `slot-mismatch` record without any boot failure (the newest installed slot is simply not the booted one); only `boot-failed` means RAUC marked the attempted slot bad.
- The boot path can fail open: if the board's distro boot finds an extlinux entry (meta-rockchip's, `root=PARTLABEL=rootfs.0`) before the rootfs-resident `boot.scr`, `BOOT_ORDER` is ignored and slot A always boots. Story 1.6 must make `boot.scr` the first boot target.
- `fw_env.config` offset and size against U-Boot's `CONFIG_ENV_OFFSET`/`CONFIG_ENV_SIZE`/redundant environment are unverified (assumed device `uboot_env`, offset 0, size 0x8000).
- The `uboot_env` partition is created raw (`RK_UBOOT_ENV` rawcopy in the wks): whether flashing leaves it with a valid environment, or erased, or overwritten at each reflash (which would reset `BOOT_ORDER`), is not checked.
- The script uses the number returned by `part number` in `nvme 0:<n>`; whether `part number` returns decimal or hex and how `load` parses it must be confirmed on the board.

- **NOT done (dev board needed):** `rauc install` of a bundle to a slot (the Docker test stops at the loop mount: a container is not root with loop; compatible-string rejection also happens after the mount); U-Boot environment access (`fw_env.config`, the environment size) and the boot counter in real U-Boot (the script runs in bash against stubs: `hush` differences, for example the quoted `for` idiom, are covered only by the RAUC-documented form, and the `part number`, `load`, `booti` commands, the kernel/dtb file names under `/boot` and the load addresses are unverified); rollback after a real failed boot; `boot.scr` discovery by the board's distro boot next to the extlinux entry meta-rockchip creates; the mark-good and recorder units under a real systemd (the targets fail by design until the services exist); the kernel options of `velora-rauc.cfg` in the final `.config`; whether the ext4 file from the image (smaller than the 2 GiB slot, not grown after install) is acceptable.
- Wrynose's `rauc` pulls `libubootenv-bin` and `python3` into the image through the recorder; whether the image stays GPLv3-free is checked only at recipe level (`bitbake -g`), like the rest of the licence policy.
- `.github/workflows/yocto.yml` was not changed and never ran; the RAUC tests are part of `ci/validate.sh`, which is run locally.

## Image variants (Story 6.13)

One source tree, three planned variants. Each is a separate image recipe that `require`s `recipes-core/images/velora-image-base.inc` (licence policy, read-only root, wks, RAUC, service users, `/data`) and adds only what is variant specific:

| Variant | `VELORA_VARIANT` / `VARIANT_ID` | Image recipe, bundle recipe | SSH | Debug UART | RAUC root and keyring | Status |
|---|---|---|---|---|---|---|
| dev | `dev` ("Development") | `velora-image-dev`, `velora-bundle-dev` | `ssh-server-openssh`, key login, `VELORA_DEV_AUTHORIZED_KEYS_FILE` | serial getty on `ttyS2` (no login possible: root has no password, open point above) | dev root, `Velora dev RAUC root` | built (recipe level) |
| factory-test | `factory-test` ("Factory test") | `velora-image-factory-test`, `velora-bundle-factory-test` | none: no feature, packages excluded, rootfs asserted free of sshd/units/`.ssh`, build fails if an authorized-keys file is configured | kernel console only; the serial getty is unlinked and masked | factory-test root, `Velora factory-test RAUC root` | built (recipe level) |
| production | `prod` (reserved in `velora.conf`) | **NOT defined, NOT built** (Story 6.14) | none | locked down (6.14) | production root, HSM only | structure only |

**How a variant is selected.** One build is one variant: `kas/velora-<variant>.yml` sets `VELORA_VARIANT` (the dev file leaves the distro default `dev`; the factory-test file includes it, so layers and pins are identical, and overrides the target and the variant). `VELORA_VARIANT` becomes `VARIANT_ID` in `/etc/os-release`, picks the keyring marker `rauc-conf` checks, and each image recipe checks `VELORA_IMAGE_VARIANT` against it in a `do_rootfs` prefunc (building `velora-image-factory-test` with the dev configuration fails before the rootfs is assembled instead of producing an image whose os-release lies). A production variant would add `velora-image-production.bb` (`VELORA_IMAGE_VARIANT = "prod"`) requiring the same base, a `velora-bundle-production.bb` requiring `velora-bundle-base.inc`, a `kas/velora-production.yml`, and a production RAUC keyring generated in the HSM; none of that exists, and `gen-dev-root.sh` refuses to create such a root.

**Keyrings per root.** Each image ships exactly one keyring file, `/usr/lib/rauc/ca.cert.pem`, the public certificate of its own variant's root (`VELORA_RAUC_KEYRING_FILE`, supplied from outside the repository). `rauc-conf` fails the build unless the file holds exactly one PEM block (`grep -c '^-----BEGIN '` is 1, so no second block of any type such as `X509 CERTIFICATE` or `TRUSTED CERTIFICATE`), that block is a plain `CERTIFICATE`, there is no private key, the certificate is a self-signed root, and its whole subject (RFC 2253) equals exactly `CN=Velora <variant> RAUC root (not for production),O=Velora` (the format `gen-dev-root.sh` writes; a marker hidden in `O=`/`OU=`, a prefix or suffix, or a certificate issued by another CA all fail). A dev keyring in a factory-test build (and the reverse) is therefore a build error, not a runtime surprise. `VELORA_VARIANT` must be `dev`, `factory-test` or `prod`; the `not for production` subject rule is applied to dev and factory-test only, and for `prod` both `rauc-conf` and the bundle base fail closed with a message that **the production keyring rule is supplied by Story 6.14** (no production keyring or rule is defined here). The bundle recipes repeat the keyring checks, require that the signing certificate chains to it and the key matches, and refuse a bundle variant that differs from `VELORA_VARIANT`. `gen-dev-root.sh` refuses `--variant` together with `--name`, and refuses the reserved names `dev`, `factory-test`, `prod` and `production` for `--name` (which exists only to label foreign test roots). `system.conf` is the same file for both variants (the keyring path is the only variant-dependent thing, and it is the same path); the compatible string `Velora RK3588` is shared, so isolation rests on the signature roots, not on the compatible string.

```
# dev root (default directory ~/.velora-dev-keys/rauc) and factory-test root (on the factory-test signing host; --out is mandatory)
bash yocto/ci/rauc/gen-dev-root.sh --variant dev
bash yocto/ci/rauc/gen-dev-root.sh --variant factory-test --out /secure/factory-test-keys
# factory-test build (kas overlay or local.conf):
#   VELORA_RAUC_KEYRING_FILE = "/secure/factory-test-keys/ca.cert.pem"
#   VELORA_RAUC_SIGN_DIR     = "/secure/factory-test-keys"        (bundle only)
kas build yocto/kas/velora-factory-test.yml                          # image
kas build yocto/kas/velora-factory-test.yml --target velora-bundle-factory-test
```

Decision recorded for `use-bundle-signing-time=true` (KEY-HIERARCHY asked Story 6.13 to revisit it): kept for both variants because the units have no trustworthy clock yet and a device-clock check would reject valid bundles. **The 365-day validity of the factory-test root does NOT bound this option.** With `use-bundle-signing-time=true` expiry is judged at the signing time written inside the bundle, which the signer asserts: whoever holds a stolen factory-test signing key can backdate the bundle and it validates against units already flashed with that keyring, whatever the certificate's `notAfter`. Only newly built images drop an old keyring (a root regenerated per factory-test release protects units flashed after the rotation, not earlier ones). The dev root lives 3650 days. This stays a documented decision to revisit (a trustworthy clock, or a revocation/anti-rollback mechanism, would be needed); production is Story 6.14's decision.

**Debug UART policy for factory-test.** Both variants assert at `do_rootfs` that root's `/etc/shadow` password field is `*` or `!...` (locked; oe-core's `zap_empty_root_password` makes it `*` because `empty-root-password` is never a feature), so neither has an empty root password. The kernel console stays on (boot messages are useful when a unit fails on the line), but no login getty runs on it and root has no password: the runner needs no console input and reports through the on-screen summary and the JSON file the station link reads, so a getty would only be attack surface. The dev image keeps its getty (still no usable login, see above). If the line later needs a serial diagnostic channel it must be a purpose-built, factory-test-signed service, not a login shell.

**Factory test runner** (`recipes-core/velora-factory-test`, Python 3 standard library, installed as `/usr/libexec/velora/velora-factory-test`, run once per boot by `velora-factory-test.service`). One test per subsystem available now: MCU links, keybed scan, panel controls and LEDs, codec loopback, display and touch, NVMe read/write speed. Each reports `pass`, `fail` or `not present`; the runner decides pass/fail itself against its own constants (a probe that expects 0 keys, a different key count, 0 LEDs or another control count fails with `probe expects ...`); the runner never stops for a missing device: every probe runs in an isolated thread with a per-test timeout (30 s, NVMe 60 s) and any exception, including `SystemExit`, becomes `fail` with a `crash:` or `timeout` detail. Hardware is reached only through a `Hardware` probe layer that returns `None` for absent hardware and plain measurements otherwise; the runner, not the probe, decides pass or fail (limits are constants at the top of the file, marked as placeholders until measured on the prototype). `SysfsHardware` is the real layer: display and touch (`/sys/class/drm`, `/sys/class/input`), NVMe (a 64 MiB write, fsync and read-back on `/data/factory-test`, page cache dropped by `posix_fadvise`), MCU link device nodes and unit identifiers (device-tree serial, machine id) are best-effort real; the MCU test expects the links `velora-mcu-keybed` and `velora-mcu-panel` (`EXPECTED_LINKS`): none of the nodes present is `not present`, any one missing or not opening is a down link (`fail`), and the detail says `presence only, no protocol (TODO Stories 3.x)`; the NVMe test writes a per-chunk distinct pattern, reads it back chunk by chunk and fails on any mismatch (`data corruption`); display and touch is presence only (no pixel or touch-event test): no DRM card at all is `not present` (the display subsystem is absent, like any other missing interface), a DRM card with no connected connector is `fail` (the unit has the subsystem and shows nothing), a connected display without a touch device is `fail`; the keybed scan, panel/LED test and codec loopback are stubs that report `not present` when the device node or sound card is absent and otherwise `fail` with `NotImplementedError`, never a false `pass` (`TODO(Stories 3.x)` for the MCU link library that lives engine side, `TODO(Story 1.2)` for the audio configuration).

Outputs, **console and file only** (the on-screen requirement is met only as plain text on the console/journal and in a file; nothing renders it on the display, and the factory station link has no transport yet: the report is a file at a known path): a plain-text summary (one line per subsystem plus counts) to stdout, the journal and `/data/factory-test/summary.txt`, and a JSON report `/data/factory-test/report.json` with schema `velora-factory-test-report/2`. Files are 0644, written atomically (temp file, fsync, rename, directory fsync); the previous report and summary are deleted at the start of every run so a stale one is never mistaken for this run's; summary and report are written independently; if `/data/factory-test` cannot be written, both go to the fallback directory `/run/velora-factory-test` (exit code 3). Fields: `variant` and `version` (from `/etc/os-release`), `timestamp` (UTC), `boot_id` (`/proc/sys/kernel/random/boot_id`), `unit` (identifiers if available), `tests[]` of `{name, status, detail, duration_ms}`, and `summary` `{pass, fail, not_present, total, overall}` where `overall` is `fail` if any test failed, else `incomplete` if any was not present, else `pass`. Exit code 0, 1 or 2 accordingly (3: the report could not be written); the unit is `After=` the udev trigger, sound.target, tmpfiles and `data.mount` (`Wants=data.mount`, deliberately not `RequiresMountsFor=/data`: that would turn a failed `/data` mount into no report at all, which contradicts the `/run` fallback), has `TimeoutStartSec=300`, `ReadWritePaths=-/data/factory-test` and `RuntimeDirectory=velora-factory-test`, and treats exit codes 1 and 2 as results, not as a service failure (3 is a unit failure).

**Verified here vs not.**
- Verified in Docker (`bash yocto/ci/validate.sh`): the layer parses and both images resolve; the factory-test configuration has no SSH feature, excludes the SSH packages, installs the runner, has the factory-test os-release (`VARIANT_ID=factory-test`) while dev keeps `dev`; `rauc-conf` installs only the matching keyring and rejects the other root's; a dev-signed bundle is rejected against the factory-test keyring and the reverse (`rauc info` and `rauc install`, at verification, with the install log checked and the slots byte-identical), each bundle verifies against its own keyring, plus positive controls; the generator's rules (variants, production refusal, key modes, validity); the image's SSH assertions and getty masking (the recipe's own functions on a fake rootfs); the runner with simulated hardware (pass, fail, missing, crashing and hanging probes) and the report schema.
- Also verified: the factory-test no-SSH assertion also runs a `find` over the whole rootfs for `authorized_keys*`, `sshd*`, `dropbear*`, `ssh*.socket` and `ssh*.service` (tested on a fake rootfs per pattern; unrelated names such as `ssh-keygen` and `scp` pass), and the root-lock assertion (accepts `*`, `!`, `!!`; rejects empty and real hashes and a missing shadow). Risk to confirm on the first real build: the `find` patterns may also match files shipped by other packages (for example systemd's own `ssh*` units or `/etc/pam.d/sshd`); if so the pattern list is the thing to tune, not the assertion dropped.
- **NOT done:** a full image build, so `do_rootfs` post-processing, the real package set, `/etc/os-release` content and the absence of sshd in a built rootfs are not observed; boot on a prototype or the dev board; the on-screen display (the text exists, nothing renders it); every real hardware probe (nothing has run on hardware: the MCU, keybed, panel, codec probes are stubs, display, touch and NVMe probes are untested on a real board); the factory-test signing host, the station link and the factory flashing flow; installing a factory-test bundle.

**Deferred (Story 6.13 review, not solved here).**
- **A6, shared compatible string and slot names:** both variants share `compatible=Velora RK3588` and the slot names `rootfs.0`/`rootfs.1`, so cross-variant isolation rests entirely on the signature roots. A per-variant compatible string would add a second, independent barrier; decide with Story 6.14.
- **B6, `/data` is on the NVMe under test:** the runner's NVMe speed test and its report live on the device it tests; a failing NVMe can take the report with it (the `/run` fallback only covers a missing `/data`). Needs a factory-test `nofail` fstab entry for `/data` and a boot test on the board.
- **B8, unit identity:** `machine_id` is volatile on a read-only root (regenerated at boot unless provisioned), `/proc/device-tree/serial-number` may be empty on the RK3588 board, and the clock is untrusted (`timestamp` is informational); a trustworthy unit identity must come from the factory station or the registry (Story 6.8).
- **B9, runner dependencies:** `RDEPENDS` `python3-core python3-json python3-threading` are assumed to be the right Wrynose sub-packages (the runner also imports `argparse`, `hashlib`, `tempfile`, `datetime`, `glob`); confirm on the first real build that the image can import every module.
- **B7, board confirmation:** the unit's ordering (`systemd-udev-trigger.service`, `sound.target`, `data.mount`), its sandboxing (`ProtectSystem=strict`, `RuntimeDirectory`) and the `/run` fallback are unverified without a boot.

## Licence policy: how it is enforced and what is not tested

- The values are in `conf/distro/velora.conf` (`VELORA_INCOMPATIBLE_LICENSE`); every image variant applies them (in `velora-image-base.inc`) as `INCOMPATIBLE_LICENSE`, so `do_rootfs` fails ("Some packages cannot be installed into the image because they have incompatible licenses") if any installed package is GPL-3.0/LGPL-3.0/AGPL-3.0. The policy is per image on purpose: a global `INCOMPATIBLE_LICENSE` was tried and makes the dev image unresolvable, because oe-core recipes (systemd, opkg-utils, python3, ...) name GPLv3 packages (bash, tar, coreutils, ...) in RDEPENDS of sub-packages that are never installed.
- Test coverage here is recipe level: the test layer applies the same policy values to the test recipes, and bitbake skips an incompatible recipe so the test image no longer resolves. The real image-level failure at `do_rootfs` needs a full build and is **NOT** exercised; the first real build must also confirm that the dev image's actual package set is GPLv3-free (the dependency graph can contain GPLv3 recipes whose packages are not installed, which is allowed).
- oe-core expands `GPL-3.0*` only to `GPL-3.0-only` and `GPL-3.0-or-later`, so `GPL-3.0-with-GCC-exception` (libgcc, libstdc++) and GPL-2.0-only (kernel, BusyBox, U-Boot) pass without any `INCOMPATIBLE_LICENSE_EXCEPTIONS` entry.
- `systemd` itself RDEPENDS on `util-linux-swaponoff`/`mkswap` (binaries only; nothing activates swap).

## Real build (Linux host with Docker, >= 100 GB free)

The build runs in the same Docker image as the validation (it has the Yocto host packages). Clones and output go to `.yocto-work/` (git-ignored); `KAS_BUILD_DIR` must be set as below, otherwise kas builds into an unignored `./build`.

```
docker build -t velora-yocto-build yocto/docker
mkdir -p .yocto-work && chmod 777 .yocto-work
echo "ssh-ed25519 AAAA... you@host" > .yocto-work/authorized_keys             # your PUBLIC key (optional)
bash yocto/ci/make-ssh-overlay.sh /work /work/.yocto-work/authorized_keys .yocto-work/kas-ssh.yml
docker run --rm -u root -e KAS_WORK_DIR=/work -e KAS_BUILD_DIR=/work/.yocto-work/build -v "$PWD":/work -w /work velora-yocto-build \
  bash -c "chown -R builder:builder /work/.yocto-work && su builder -c 'cd /work && kas build .yocto-work/kas-ssh.yml'"   # or yocto/kas/velora-dev.yml without a key
```

Output is under `.yocto-work/build/tmp/deploy/`: the `.wic` image under `images/rock-5b/` and `licenses/**/license.manifest`. CI does this through `.github/workflows/yocto.yml` (manual dispatch, needs a runner with >= 100 GB).

## Updating pins

`git ls-remote --heads <url> wrynose` for each repo in `kas/velora-dev.yml`, update `commit:`, rerun `ci/validate.sh`. Verified at authoring time (2026-10-25): bitbake branch `2.18`, `openembedded-core`, `meta-openembedded`, `meta-arm`, `meta-rockchip`, `meta-rauc` (all branch `wrynose`).

## Choices

- Mainline `linux-yocto` 6.18 with an out-of-tree-free RT fragment: the kernel plan forbids vendor kernels and out-of-tree RT patches.
- Licence policy values live in the distro, applied per image (see above). Native tools are not restricted (they are not shipped).
- No `debug-tweaks`: it would give an empty root password and open SSH login.
- Swap is excluded three ways (the first real build must audit the final kernel `.config` for `# CONFIG_SWAP is not set`; `velora-dev.cfg` sets `CONFIG_EXPERT=y` itself because that option is only configurable under EXPERT; individual `.swap` units and generators are covered by the kernel having no swap): `vm.swappiness=0`, `CONFIG_SWAP` off in the kernel fragment, `swap.target` masked; no image recipe or wks adds a swap partition.
- Core dumps are excluded three ways: `kernel.core_pattern=|/bin/false`, systemd `Storage=none`/`ProcessSizeMax=0`, `DefaultLimitCORE=0`.
- `ptest` is removed from `DISTRO_FEATURES` and the extensible SDK class is removed from the image (`IMGCLASSES:remove`): both pull GPL-3.0 build/test tools (make, bash) into the dependency graph and are not wanted on a product image. No SDK is produced by this image.
- The journal is volatile and bounded (`Storage=volatile`, `RuntimeMaxUse=32M`, `RuntimeMaxFileSize=8M`): logs are lost on every reboot.
- Dev access: `VELORA_DEV_AUTHORIZED_KEYS_FILE` must point to a non-empty file or the build fails (`bbfatal`); a changed key file re-runs `do_rootfs`.

## Validation run record

| Date | Command | Result |
|---|---|---|
| 2026-10-25 | `bash yocto/ci/validate.sh` | 41 passed, 0 failed (before the review fixes); 44 passed, 0 failed (after the review fixes: networkd, journal bounds, tighter licence assertions, kas dump of the SSH overlay) |
| 2026-10-25 | `bash yocto/ci/validate.sh` (Story 6.11: wks parse with wic, read-only root, data dirs) | 55 passed, 0 failed (before the review fixes) |
| 2026-10-25 | `bash yocto/ci/validate.sh` (after the Story 6.11 review fixes: artefact checks, wic via bitbake, wks variants) | 81 passed, 0 failed |
| 2026-10-25 | `bash yocto/ci/validate.sh` (Story 6.12: RAUC config, keyring handling, bundle recipe, units, boot.scr, plus `ci/rauc` Docker tests) | 115 passed, 0 failed (Yocto part), RAUC tests 33 passed, 0 failed (before the review fixes) |
| 2026-10-25 | `bash yocto/ci/validate.sh` (after the Story 6.12 review fixes: plain bundles, log-checked rejections, recorder for unversioned slots, boot-script fallback rule, keyring/bundle checks) | 119 passed, 0 failed (Yocto part), RAUC tests 35 passed, 0 failed |

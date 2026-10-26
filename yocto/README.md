# yocto/: Velora dev image (Story 1.4)

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
- This is the dev image only. Production images, RAUC, secure boot (Story 8.x/AR8) are out of scope.

## Layout

```
yocto/
  kas/velora-dev.yml            kas project: machine, distro, target, layers pinned to commits
  meta-velora/                  our layer
    conf/distro/velora.conf     distro: systemd, INCOMPATIBLE_LICENSE, os-release dev marker, debug UART
    conf/distro/include/velora-bootargs.inc   bootargs placeholder (Story 1.1)
    recipes-core/images/velora-image-dev.bb   the dev image
    recipes-core/velora-system-config/        sysctl (no swap, no cores), coredump.conf, limits, journald
    recipes-kernel/linux/       linux-yocto bbappend + preempt-rt.cfg (placeholder) + velora-dev.cfg (no swap)
  meta-velora/files/wic/velora-nvme.wks           the one disk layout (Story 6.11)
    recipes-core/velora-service-users/        useradd: velora-engine, velora-librarian, velora-sysd
    recipes-core/base-files/                  fstab additions (tmpfs /tmp, /data mount point)
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

## Licence policy: how it is enforced and what is not tested

- The values are in `conf/distro/velora.conf` (`VELORA_INCOMPATIBLE_LICENSE`); `velora-image-dev` applies them as `INCOMPATIBLE_LICENSE`, so `do_rootfs` fails ("Some packages cannot be installed into the image because they have incompatible licenses") if any installed package is GPL-3.0/LGPL-3.0/AGPL-3.0. The policy is per image on purpose: a global `INCOMPATIBLE_LICENSE` was tried and makes the dev image unresolvable, because oe-core recipes (systemd, opkg-utils, python3, ...) name GPLv3 packages (bash, tar, coreutils, ...) in RDEPENDS of sub-packages that are never installed.
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

`git ls-remote --heads <url> wrynose` for each repo in `kas/velora-dev.yml`, update `commit:`, rerun `ci/validate.sh`. Verified at authoring time (2026-10-25): bitbake branch `2.18`, `openembedded-core`, `meta-openembedded`, `meta-arm`, `meta-rockchip` branch `wrynose`.

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

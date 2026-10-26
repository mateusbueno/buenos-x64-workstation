# yocto/: Velora dev image (Story 1.4)

Reproducible, licence-clean dev image for the RK3588 dev board on Yocto 6.0 "Wrynose" LTS (systemd, Linux 6.18 LTS mainline with PREEMPT_RT, no vendor kernel; see `KERNEL-LTS-PLAN.md`).

## Status

| Item | State |
|---|---|
| Layer and recipe parse (`bitbake -p`), image recipe resolves (`bitbake -g` provider resolution) | validated in Docker (`ci/validate.sh`) |
| Licence policy (GPL-3.0/LGPL-3.0/AGPL-3.0 blocked, GPLv2 and GCC-exception runtimes allowed) | policy values validated in Docker at recipe level (dry run); the image-level `do_rootfs` enforcement is **NOT** exercised (needs a build) |
| flac / metaflac / libsndfile absent from the image | recipe-level only: absent from the `bitbake -g` dependency graph (`pn-buildlist` and `task-depends.dot`); `PACKAGE_EXCLUDE` (also libflac, libflac++) is the package-level guard and is NOT exercised without a build |
| Dev markers, no swap, no core dumps, SSH, debug UART | statically checked and checked on the resolved bitbake configuration |
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

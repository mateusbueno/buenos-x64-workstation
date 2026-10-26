---
title: 'Story 1.4: Minimal Yocto dev image for the dev board (layer, license policy and parse validation)'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '0c9dd6b25b1701c441fcbdc54b61501d02a0cb4f'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/yocto/KERNEL-LTS-PLAN.md']
warnings: []
deferred:
  - summary: >-
      Full image build, boot on the dev board, 1 h cyclictest, kernel .config audit (CONFIG_PREEMPT_RT, CONFIG_SWAP off), do_rootfs licence enforcement and package-level exclusion of flac/libsndfile are NOT validated; the GitHub workflow has never run.
    evidence: |-
      Validation is recipe/graph/variable level in Docker (44 checks). Needs a >=100 GB Linux host and the board.
    severity: high
  - summary: >-
      Unresolved dependencies: Story 1.1 kernel RT fragment and boot args and Story 1.2 audio config are placeholders (TODO markers); rock-5b stands in for the ROCK 5B+; UART login path undecided; rkbin proprietary blob needs provenance review; poky has no wrynose branch so bitbake + openembedded-core are pinned directly.
    evidence: |-
      Listed in yocto/README.md.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** There is no Yocto layer in the repo, so the dev board runs a hand-configured system. Story 1.4 asks for a reproducible, license-clean dev image on Yocto 6.0 "Wrynose" LTS (systemd, Linux 6.18 PREEMPT_RT, SSH, debug UART, dev-marked os-release, no swap, no core dumps, INCOMPATIBLE_LICENSE for GPL-3.0/LGPL-3.0, no flac/metaflac/libsndfile) built by CI with a license manifest (AR3 partial, AR4, AR2 partial, NFR10, NFR17 partial, NFR27, NFR37).

**Approach:** Author the committed `yocto/` tree (kas project file pinning layer revisions, a distro/policy layer, a dev image recipe, kernel config fragment and bootargs, os-release/dev marker, systemd/sysctl settings for no swap and no core dumps, SSH + debug UART config, an INCOMPATIBLE_LICENSE policy, a GitHub Actions workflow for main-branch builds and artifact publishing, and a README) and validate it WITHOUT a full image build, inside Docker: layer/recipe parse with bitbake (`bitbake -p` or kas `--update` + parse-only), license-policy checks (a test recipe with a GPL-3.0 license must fail with INCOMPATIBLE_LICENSE; GPLv2 kernel/BusyBox and GCC-exception runtimes must be allowed; flac/metaflac/libsndfile must not be in the image's package set, checked via `bitbake -g` dependency graph or package listing), and static checks of the config files. A full image build and the boot/cyclictest acceptance need the dev board and ≥100 GB of disk and are explicitly NOT done here.

## Boundaries & Constraints

**Always:** Pin every layer to a branch/commit that exists (check with `git ls-remote` before pinning: poky Wrynose release branch, meta-openembedded, a Rockchip RK3588 BSP layer, and mark clearly if the RK3588 BSP for Wrynose does not exist yet, then fall back to a documented mainline-kernel machine definition in the repo's own layer rather than inventing upstream layers). Put licence policy in the distro conf (`INCOMPATIBLE_LICENSE` covering GPL-3.0*, LGPL-3.0*, AGPL-3.0* with the exceptions the story names, and the per-package exceptions needed for GPLv2 kernel/BusyBox and GCC-exception runtimes). `INIT_MANAGER = "systemd"`. The kernel config fragment and boot arguments come from Story 1.1 and the audio configuration from Story 1.2, which are not done: create clearly named placeholder files (`preempt-rt.cfg` with `CONFIG_PREEMPT_RT=y` and the isolcpus/nohz/rcu bootargs placeholder) marked `TODO(Story 1.1)` / `TODO(Story 1.2)` in file and README, and list them in the result as unresolved dependencies. Dev marking in `/etc/os-release` (e.g. `VARIANT_ID=dev`). No swap (no swap in fstab, `vm.swappiness`/no swap unit), core dumps disabled (`kernel.core_pattern`/`LimitCORE=0`/`Storage=none`). SSH and debug UART (serial-getty on the console) enabled in the dev image only. Keep all Docker work reproducible through a committed script or Dockerfile under `yocto/` (e.g. `yocto/docker/Dockerfile` + `yocto/ci/validate.sh`) so CI and the user can rerun it. Do not leave large clones in the repo (use a git-ignored work dir; add to .gitignore).

**Never:** No full `bitbake core-image` build (disk: the host has ~21 GB free, an image build needs ≥100 GB), no claims that the image boots or meets cyclictest, no fabricated upstream layer URLs or branches, no secrets/keys, no changes outside `yocto/`, `.github/workflows/`, `.gitignore` and docs. Do not touch C++ code.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Layer parse | kas checkout + bitbake -p of the dev image in Docker | parse succeeds, image recipe resolves | failure reported with log tail |
| License policy | test recipe with GPL-3.0 added to the image | INCOMPATIBLE_LICENSE failure | policy test fails if build does NOT fail |
| Allowed licenses | GPLv2 kernel/BusyBox, GCC-exception runtimes | not blocked | policy test fails if blocked |
| Forbidden packages | flac, metaflac, libsndfile | absent from the image package set | check fails if present |
| Dev markers | image config | os-release dev marker, no swap, core dumps disabled, ssh + debug uart enabled | static check fails otherwise |
| Missing upstream | RK3588 BSP layer for Wrynose absent | documented fallback, listed as unresolved | explicit note, no invented URL |

## Code Map

- `yocto/KERNEL-LTS-PLAN.md` -- existing (Story 8.14): kernel lifecycle facts, 6.18 LTS, Yocto 6.0 "Wrynose".
- `_bmad-output/planning-artifacts/epics.md` Story 1.4 (line ~868), 1.1 (kernel RT spike), 1.2 (audio), 6.11-6.13 (later consumers of this image) -- requirements context.
- `tools/ci/docker/Dockerfile`, `tools/ci/docker-check.sh` -- existing Docker conventions for CI checks.
- `.github/workflows/` (check what exists; CI is manual-only to save minutes -- see below) -- where the Yocto job goes.
- New: `yocto/kas/velora-dev.yml` (or similar), `yocto/meta-velora/` (conf/layer.conf, conf/distro/velora.conf, recipes-core/images/velora-image-dev.bb, recipes-kernel/linux/ fragment + bbappend, recipes-core/ systemd/sysctl/os-release bbappends), `yocto/docker/Dockerfile`, `yocto/ci/validate.sh`, `yocto/README.md`, workflow file.

## Tasks & Acceptance

**Execution:**
- `yocto/meta-velora/**` + `yocto/kas/*` -- layer, distro (systemd, INCOMPATIBLE_LICENSE), dev image, kernel fragment/bootargs placeholders, no swap/core dumps/ssh/uart/os-release
- `yocto/docker/Dockerfile`, `yocto/ci/validate.sh` -- containerised validation: layer parse, license policy tests (GPL-3.0 must fail, GPLv2/GCC-exception allowed), forbidden-package check, static config checks
- `.github/workflows/yocto.yml` -- workflow that builds the dev image and publishes the image + license manifest (workflow_dispatch only, matching the project's manual-only CI policy; no automatic triggers)
- `yocto/README.md` -- layout, how to run the validation in Docker, how to do the real build on a ≥100 GB Linux host, unresolved dependencies (Story 1.1 fragment/bootargs, Story 1.2 audio config, RK3588 BSP), reasons for choices
- run the validation in the user's Docker and record the output summary

**Acceptance Criteria:**
- Given `yocto/`, then a distro on Yocto 6.0 "Wrynose" with systemd parses in Docker and the dev image recipe resolves.
- Given a GPL-3.0 recipe added to the image, then the policy check fails through INCOMPATIBLE_LICENSE, while GPLv2 kernel/BusyBox and GCC-exception runtimes are allowed, and flac/metaflac/libsndfile are not in the image.
- Given the committed files, then os-release is dev-marked, there is no swap, core dumps are disabled, SSH and the debug UART are enabled.
- The full image build, boot on the board and the 1 h cyclictest are recorded as NOT done.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~45 findings — patches 7 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` build-dir mismatch between workflow/README/validation; runs-on/runner input; artifact upload hardening; authorized-keys checksum + fatal check; CONFIG_EXPERT dependency of the swap fragment; unbounded journal; static-check destination ties and precedence bug; weak licence assertions; forbidden-package graph coverage — fixed (validation 41 -> 44 checks)
  - `[medium]` `defer` do_rootfs-level licence/package enforcement, kernel config audit, UART login, rkbin licence flags, boot-arg consumption
  - `[low]` `reject` Docker digest pinning, sstate caching, SBOM, tautological busybox/u-boot allow-list nit (kept as documented sanity checks)

## Verification

**Commands:**
- `bash yocto/ci/validate.sh` -- expected: all checks pass (runs in Docker; first run clones layers)

## Auto Run Result

Summary: yocto/ layer (kas pins, meta-velora distro, dev image, system config for no swap/no core dumps/bounded journal/networkd, kernel fragments), Docker validation (44 checks), manual-only workflow, README.
Verification: bash yocto/ci/validate.sh passes 44/44 (re-run by the orchestrator in Docker).
Residual risks: see deferred. Docker volume velora-yocto-work (~1.5 GB) remains; remove with bash yocto/ci/validate.sh --clean.

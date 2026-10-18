# Review — Versions & Technology Reality Check

- **Target:** `../ARCHITECTURE-SPINE.md` (ARM64 Stage Workstation, draft 2026-10-17)
- **Review date:** 2026-10-17
- **Lens:** Was every committed decision (Stack table, named technologies in ADs) web-checked against the current state of the world, or asserted from memory?
- **Method:** Web search and direct fetch of upstream release pages, kernel.org, Yocto release notes, layer repos, mailing-list archives. Each claim below is tagged:
  - **CONFIRMED** — current upstream source agrees with the spine
  - **OUTDATED** — the technology is real but a newer version exists or the pin should change
  - **RISK** — real, but a fact found during the check undermines the decision or leaves it unproven
  - **UNVERIFIED** — no authoritative source found; still an assertion

## Verdict

Most of the stack exists and fits: Linux 6.18 LTS, mainline PREEMPT_RT on arm64, Yocto 6.0 "Wrynose" LTS, meta-rauc on Wrynose, LVGL's DRM driver, the USB gadget functions, PCM3168A, and RK3588 DSI/USB/I2S in mainline all check out. The spine still does not record evidence for any of its pins, and several are stale. One named dependency, sfizz, has been **archived upstream** (read-only since 2026-06-21). The content-protection chain in AD-14 (secure boot, OTP device key, OP-TEE) is the least proven part of the design on mainline RK3588.

## Summary table

| # | Item (spine) | Spine says | Found (2026-10-17) | Status |
|---|---|---|---|---|
| 1 | Linux kernel | 6.18 LTS, PREEMPT_RT mainline, ALSA UMP | 6.18 is LTS (designated 2025-12-03), latest 6.18.57 (2026-10-15), projected EOL **Dec 2027**. PREEMPT_RT mainline since 6.12 for arm64/x86/riscv. ALSA UMP since 6.5 | CONFIRMED, with a lifetime RISK |
| 2 | Yocto | 6.0 "Wrynose" LTS | Released 2026-05-13, LTS to April 2030, ships linux-yocto 6.18, GCC 15.2. Point releases 6.0.1–6.0.3 exist | CONFIRMED |
| 3 | RAUC | ≥ 1.14 | Latest **1.15.2** (2026-03-27, fixes CVE-2026-34155). meta-rauc has a `wrynose` branch and ships rauc 1.15.2 | OUTDATED (raise the floor) |
| 4 | LVGL | 9.5 (DRM/KMS) | 9.5.0 was released 2026-02-18. **9.6.0** (2026-09-16) is now the latest. The DRM driver (`LV_USE_LINUX_DRM`, optional EGL/GBM) is present and documented | OUTDATED (minor) |
| 5 | Tauri | 2.x (2.11 line) | 2.11.x ended at 2.11.5 (2026-07-01). Latest is **2.12.3** (2026-10-10) | OUTDATED |
| 6 | sfizz | 1.2.3 | 1.2.3 is the last release. The license is BSD-2-Clause. **The repo was archived 2026-06-21**, as was sfizz-ui. No successor has been named | RISK (high) |
| 7 | libFLAC | 1.5.0 | 1.5.0 (2025-02-11) is still the latest. The library uses Xiph BSD-like terms. The `flac`/`metaflac` CLI tools are GPL | CONFIRMED (licence caveat) |
| 8 | Protocol Buffers | "version shipped in Yocto 6.0" | meta-oe wrynose: protobuf **6.33.6** (per the layer index search snippet). Upstream has moved to v36.x (36.0 Aug, 36.1 Sep 2026) | UNPINNED; no exact version recorded |
| 9 | SQLite | "version shipped in Yocto 6.0" | Wrynose oe-core: **3.51.3**. Upstream latest 3.53.3 (2026-06-26). 3.52.0 was withdrawn | UNPINNED; no exact version recorded |
| 10 | OP-TEE | "version from the RK3588 SoM BSP" | Upstream optee_os has `platform_rk3588.c`. meta-arm recipe was at 4.7.0 with 2 RK3588 test failures, fixes pending for 4.9.0 (Jan 2026). Field reports of TF-A/OP-TEE ABI mismatches (Aug 2026) | RISK; unpinned, and conflicts with the mainline kernel choice |
| 11 | RK3588 PREEMPT_RT | implied by AD-5 | Mainline RT works on arm64. **No published mainline RK3588 latency data found.** Only vendor 6.1 SDK RT results (Forlinx), with no numbers | UNVERIFIED |
| 12 | RK3588 MIPI DSI | implied (UI display) | DSI2 glue `dw-mipi-dsi2-rockchip` merged for 6.14 (`CONFIG_ROCKCHIP_DW_MIPI_DSI2`). Samsung DC-PHY driver v8 March 2025; exact merge version not confirmed | CONFIRMED (PHY merge version unverified) |
| 13 | RK3588 USB3 OTG device mode | AD-13, AD-17 (gadget) | USBDP combo PHY merged 6.10. Dual-role DWC3 works in peripheral mode only if the board DT enables the USBDP PHY and sets the role (July 2026 ROC-PC patch shows UDC missing otherwise) | CONFIRMED, with a board-DT RISK |
| 14 | RK3588 I2S/TDM | AD-17 | `rockchip,rk3588-i2s-tdm` supported since 2022/2023 (Frattaroli, Ciocaltea). **March 2026 patch:** BL31 can leave the I2S MCLK-out GRF gates closed, so the codec gets no MCLK (merge status unknown). Multichannel TDM slave-mode reports not found | CONFIRMED, with a RISK |
| 15 | PCM3168A ASoC | HARDWARE-INTERFACES | Mainline `sound/soc/codecs/pcm3168a*.c` (I2C/SPI), with TDM modes and per-direction slot configuration | CONFIRMED |
| 16 | USB gadget UAC2 | AD-17 | `f_uac2` with configfs, async with feedback endpoint (needed for Windows/macOS hosts), `c_sync` adaptive option. History of reverts including a NULL dereference on dwc3 in adaptive mode | CONFIRMED, with a RISK |
| 17 | USB gadget MIDI 2.0 | AD-8, CAP-10 | `f_midi2` merged in 6.6 (configfs, UMP, MIDI 1.0 fallback) | CONFIRMED |
| 18 | USB gadget CDC-NCM | AD-13 | `f_ncm` is a long-standing mainline function. Composite UAC2+MIDI2+NCM on dwc3 not specifically found | CONFIRMED (composite UNVERIFIED) |
| 19 | io_uring | AD-3 / streaming I/O | Mainline and maintained (selftests still active in 2026). `kernel.io_uring_disabled` sysctl (0/1/2) exists for hardening. No public RT-audio data found | CONFIRMED (fit is UNVERIFIED) |
| 20 | RK3588 secure boot / OTP | AD-14 | BootROM verifies an RSA-2048/4096 key against a hash in OTP. Fuse burning goes through the OP-TEE `rk_secure_boot` PTA. On the open side, only **barebox** (`rksecure`, `rkimage`) supports this; **U-Boot cannot call the PTA yet**. The vendor path uses the closed `rk_sign_tool`/rkbin. **One root key, not revocable or rotatable** | RISK (high) |
| 21 | Per-device key in OTP / OP-TEE | AD-14 | **No upstream RK3588 HUK (`tee_otp_get_hw_unique_key`) override found.** OP-TEE's default returns a static key | UNVERIFIED / RISK (high) |
| 22 | Init system | AD-4 "all under systemd" | Wrynose: systemd is the default for non-Poky distros, but **Poky keeps sysvinit** | Note: the distro config must say this explicitly |
| 23 | C++20 toolchain | Yocto 6.0 toolchain | GCC 15.2 / LLVM 22.1 | CONFIRMED |

## Findings

### F1 — sfizz is archived upstream [HIGH]

- **Evidence:** github.com/sfztools/sfizz shows "archived by the owner on Jun 21, 2026. It is now read-only." sfizz-ui is archived too. The last release, 1.2.3, has no announced successor. The license is BSD-2-Clause, so AD-15 holds. Optional dependencies are LGPL-2.1 (libsndfile, JACK). The default dr_libs backend is MIT-0.
- **Impact:** The spine names sfizz as the SFZ parser/reference renderer (Stack table, AD-15). If it is linked into the shipped `librarian`/`formatlib`, the import path depends on unmaintained code. That includes security fixes for an attack surface that reads untrusted user files (CAP-8).
- **Fix:**
  - Pin sfizz 1.2.3 by commit hash.
  - Either vendor it as an owned fork in-tree with a fuzzing harness, or limit it to an unshipped test oracle and write an in-house SFZ parser for `librarian`.
  - Record the decision in AD-15. Do not enable libsndfile, and keep the dr_libs backend.

### F2 — The AD-14 content-protection chain is not proven on mainline RK3588 [HIGH]

- **Evidence:**
  - The Pengutronix blog (2026-06-19) describes the BootROM flow. The public key hash in OTP is immutable, and a single root key cannot be revoked or rotated. Only RSA is supported. Open-source fuse programming exists only in barebox (`rksecure`) through the OP-TEE `rk_secure_boot` PTA. U-Boot "cannot currently call this PTA".
  - A barebox RFC (Jan 2026) confirms that the secure-boot OTP is reachable only from the secure world.
  - Upstream optee_os has `platform_rk3588.c`, but no RK3588 HUK/OTP override was found, and the default `tee_otp_get_hw_unique_key` returns a static key.
  - A Yocto patch (Jan 2026) reports 2 failing OP-TEE tests on RK3588 with meta-arm 4.7.0. An August 2026 barebox thread shows TF-A/OP-TEE dynamic-SHM and UID mismatches that were not resolved.
- **Impact:**
  - "Per-device key held in RK3588 OTP / OP-TEE" may not be implementable on upstream OP-TEE without new driver work.
  - "OP-TEE version from the RK3588 SoM BSP" most likely means a vendor (rkbin/6.1-era) stack. That conflicts with the mainline 6.18 kernel choice and leaves the TF-A/OP-TEE/kernel ABI unpinned.
  - Choosing a bootloader becomes an architectural decision: barebox, or U-Boot plus new PTA client work.
- **Fix:**
  - Add an explicit AD, or a Deferred item with a deadline, covering the bootloader (barebox vs U-Boot), the TF-A source (upstream vs rkbin BL31), and the OP-TEE version (pin, e.g. ≥ 4.9.0).
  - Run a spike on a dev board: burn the key hash, boot signed, and derive a per-device key from OTP in OP-TEE.
  - Document the single-root-key risk: HSM custody and an escrow procedure.
  - If no HUK exists, specify how the per-device key is provisioned (generated at factory and stored in OP-TEE secure storage bound to an OTP-derived key) before AD-14 is final.

### F3 — Stale or unpinned versions in the Stack table [MEDIUM]

- **RAUC:** `≥ 1.14` → **`≥ 1.15.2`**. 1.15.2 fixes CVE-2026-34155 and is what meta-rauc `wrynose` ships. Note the Wrynose migration: `rauc-conf` now installs `system.conf` and the keyring into `/usr/lib/rauc`.
- **Tauri:** `2.11 line` → **`2.12` (2.12.3, 2026-10-10)**. 2.11.1 already contained an IPC ACL security fix, so state a minimum patch policy.
- **LVGL:** `9.5` → **`9.6` (9.6.0, 2026-09-16)**, or keep 9.5 deliberately and record why. The DRM API changed between 9.4 and 9.5 (`lv_linux_drm_set_file` now returns `lv_result_t`), so pin the minor version.
- **Protobuf:** state **6.33.6** (meta-oe wrynose; layer index search snippet, not fetched directly) instead of "version shipped". Upstream is at v36.x. Confirm in-tree with `bitbake -e protobuf | grep ^PV`. The desktop app (Tauri/Rust) must use a compatible protobuf runtime, such as prost or a pinned protoc, built from the same `.proto` files.
- **SQLite:** state **3.51.3** (Wrynose oe-core) instead of "version shipped". Upstream is at 3.53.3. The desktop app must not bundle a different SQLite that it then uses to write the device catalog. AD-9 already forbids that, but the version should still be recorded.
- **Linux:** add the point-release policy (6.18.y, currently 6.18.57).

### F4 — Kernel support window is shorter than the Yocto LTS and the product life [MEDIUM]

- **Evidence:** kernel.org/Phoronix list 6.18 LTS through **December 2027**. Wrynose is supported until **April 2030**. CIP SLTS series (August 2026 post) end at 6.12, and no 6.18 SLTS decision was found.
- **Impact:** Units sold in 2027–2028 would run an EOL kernel unless the project migrates, which on RK3588 means re-validating RT, DSI, USB gadget and I2S.
- **Fix:** Record a kernel lifecycle rule. Either track the next LTS (late-2026 LTS) through RAUC updates and plan re-validation, or watch for an extended 6.18 EOL or a CIP SLTS pick. Add this to Deferred with "revisit at 6.18 EOL announcement / Dec 2026 LTS designation".

### F5 — RK3588 mainline peripherals: present, but board-level gaps unproven [MEDIUM]

- **PREEMPT_RT:** No mainline RK3588 cyclictest data was found. AD-5 (3 isolated A76 cores, 64-frame blocks of 1.33 ms) assumes a latency budget nobody has measured. **Fix:** make an RT latency spike, with cyclictest and rtla under stress-ng plus NVMe plus GPU load on 6.18-rt, a Milestone-1 exit criterion.
- **USB OTG device mode:** USBDP PHY mainline since 6.10, but peripheral mode needs board DT (`usbdp_phy` enabled, `role-switch-default-mode`/`dr_mode = "peripheral"` or otg with Type-C). **Fix:** call this out in HARDWARE-INTERFACES as a DT requirement.
- **I2S/TDM:** The driver is in mainline. A March 2026 patch series ("clk: rockchip: rk3588: add I2S MCLK output gate clocks") shows the firmware can leave MCLK-out gated. AD-17 makes the codec the clock master from its own crystal, so MCLK-out from the SoC may not be needed. Still verify the TDM slave-mode, 8-channel configuration with PCM3168A on real hardware.
- **UAC2 gadget:** async plus feedback is required for macOS/Windows. The adaptive mode has a dwc3 crash history. **Fix:** commit to async mode with explicit feedback in AD-17, and include a composite (UAC2 + f_midi2 + NCM) enumeration test on macOS, Windows and Linux in the acceptance tests.
- **DSI:** DSI2 controller in 6.14. Confirm the DC-PHY driver merge version in the 6.18 tree before panel selection.

### F6 — Minor notes [LOW]

- **libFLAC:** The library is BSD-like (Xiph), but the `flac`/`metaflac` CLI tools are GPL. With AD-15, make sure the image builds only libFLAC, with no CLI packages, or that the GPLv2 CLI is consciously allowed. It is GPLv2+, not GPL-3, so it is not blocked by `INCOMPATIBLE_LICENSE`, but it is probably not wanted.
- **systemd:** Wrynose Poky defaults to sysvinit. The product distro must set `INIT_MANAGER = "systemd"`.
- **io_uring:** It fits for the non-RT streaming threads (NVMe reads, O_DIRECT, registered buffers). Set `kernel.io_uring_disabled=1` with a dedicated group so that only `engine` can create rings, which reduces attack surface on a device with a network port. No public RT-audio benchmarks were found, so the I/O thread design should be measured in the RT spike.
- **Yocto host:** Wrynose build hosts need 32 GB RAM and 140 GB disk. Size the CI runners accordingly.

## What was confirmed without caveat

- 6.18 is an LTS kernel. PREEMPT_RT is mainline on arm64.
- Yocto 6.0 "Wrynose" is a real LTS, released 2026-05-13.
- meta-rauc has a `wrynose` branch.
- LVGL provides a Linux DRM/KMS driver.
- sfizz is BSD-2-Clause.
- libFLAC 1.5.0 is the latest release.
- `f_midi2` (6.6+), `f_uac2` and `f_ncm` exist in mainline.
- PCM3168A has a mainline ASoC driver with TDM.
- `rockchip,rk3588-i2s-tdm` is mainline.
- The RK3588 DSI2 controller is mainline (6.14).
- The RK3588 USBDP PHY is mainline (6.10).
- Upstream OP-TEE has an RK3588 platform.

## Sources

- Linux 6.18 LTS: https://www.phoronix.com/news/Linux-6.18-LTS-Kernel ; https://linuxiac.com/linux-6-18-is-now-listed-as-the-new-long-term-support-kernel/ ; https://www.kernel.org/ (6.18.57, 2026-10-15; mainline 7.3-rc7)
- PREEMPT_RT mainline: https://www.phoronix.com/news/Linux-6.12-Does-Real-Time ; https://heise.de/-10061458
- Yocto 6.0 Wrynose: https://www.cnx-software.com/2026/05/14/yocto-project-6-0-wrynose-released-with-linux-6-18-lts/ ; https://docs.yoctoproject.org/migration-guides/release-notes-6.0.html
- RAUC: https://github.com/rauc/rauc/releases ; https://packages.fedoraproject.org/pkgs/rauc/rauc-doc/fedora-44-updates.html (CVE-2026-34155) ; https://lore.ptxdist.org/ptxdist/adjnVv8SaWVE6SjV@pengutronix.de/t/
- meta-rauc wrynose: https://github.com/rauc/meta-rauc ; https://github.com/rauc/meta-rauc/tree/wrynose/recipes-core/rauc
- LVGL: https://github.com/lvgl/lvgl/releases ; https://lvgl.io/blog/release-v9-5 ; https://docs.lvgl.io/9.5/integration/embedded_linux/drivers/drm.html
- Tauri: https://v2.tauri.app/release/ ; https://tauri.app/release/core/
- sfizz: https://github.com/sfztools/sfizz ; https://github.com/sfztools/sfizz/releases ; https://github.com/sfztools
- libFLAC: https://github.com/xiph/flac/releases ; https://github.com/xiph/flac
- Protobuf: https://protobuf.dev/news/v34 ; https://tracker.debian.org/media/packages/p/protobuf/changelog-36.0rc2-1 ; OE layer index (wrynose protobuf 6.33.6, from search snippet; direct fetch returned 403)
- SQLite: https://sqlite.org/ ; https://versionlog.com/sqlite/ ; https://sqlite.org/releaselog/3_52_0.html
- RK3588 DSI: https://lkml.iu.edu/hypermail/linux/kernel/2412.0/04470.html ; https://www.cateee.net/lkddb/web-lkddb/ROCKCHIP_DW_MIPI_DSI2.html ; https://lists.infradead.org/pipermail/linux-rockchip/2025-March/056892.html
- RK3588 USB: https://www.cateee.net/lkddb/web-lkddb/PHY_ROCKCHIP_USBDP.html ; https://lists.infradead.org/pipermail/linux-arm-kernel/2023-July/851554.html ; https://lists.infradead.org/pipermail/linux-rockchip/2026-July/073191.html
- RK3588 I2S/TDM: https://lists.infradead.org/pipermail/linux-riscv/2023-March/028786.html ; https://lkml.iu.edu/hypermail/linux/kernel/2210.3/04110.html ; https://lkml.iu.edu/2603.2/07875.html
- PCM3168A: https://ti.com/tool/PCM3168ASW-LINUX ; https://codebrowser.dev/linux/linux/sound/soc/codecs/pcm3168a.c.html
- USB gadget MIDI 2.0: https://www.phoronix.com/news/Linux-6.6-USB ; https://lwn.net/Articles/939185/
- USB gadget UAC2: https://lore.kernel.org/lkml/YFnXfUr21JrweUoU@kroah.com/T/ ; https://lore.kernel.org/all/87r1ei5at8.fsf@kernel.org/t/
- io_uring: https://cos.googlesource.com/third_party/kernel/+/74081e0354558f6449ebd9a6e02c734542a7cd9e%5E%21/ ; https://lkml.iu.edu/hypermail/linux/kernel/2603.2/08334.html
- RK3588 RT data: https://www.forlinx.net/industrial-news/rk3588-development-board-preempt-rt-patch-testing-810.html
- OP-TEE RK3588: https://github.com/OP-TEE/optee_os/tree/master/core/arch/arm/plat-rockchip ; https://patchwork.yoctoproject.org/project/yocto/cover/20260126-optee-os-v1-0-874261a77dad@cherry.de ; https://lore.barebox.org/barebox/19d25c21-a937-4e63-abd5-67afafc6abd9@pengutronix.de/T/
- RK3588 secure boot/OTP: https://www.pengutronix.de/en/blog/2026-06-19-rk3588-secure-boot.html ; https://lists.infradead.org/pipermail/barebox/2026-January/054315.html ; https://forum.radxa.com/t/secure-boot-on-rock-5b/14498
- HUK background: https://wiki.st.com/stm32mpu/wiki/Hardware_Unique_Key_overview
- CIP SLTS: https://cip-project.org/2026/08

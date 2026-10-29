# Kernel LTS migration plan (Story 8.14, AR5)

- **Status:** plan written (documentation part of Story 8.14). Execution (Phases 1 to 5) is NOT started and needs the dev board and prototype.
- **Written:** 2026-10-19
- **Current kernel:** Linux 6.18 LTS, mainline, `CONFIG_PREEMPT_RT`, no out-of-tree RT patches, in Yocto 6.0 "Wrynose" (architecture memlog: kernel lifecycle decision, stack decision).
- **Owner:** founder. **Reviewer:** firmware lead (to confirm).

Machine-readable plan state (read by the CI check in section 8):

```
PLAN_START_DATE=2027-06-01
PLAN_STARTED=no
EOL_BASELINE=2027-12 (story baseline; see section 1 for the conflicting 2028-12 projection)
```

## 1. Lifecycle facts and the EOL discrepancy

| Fact | Value | Source |
|---|---|---|
| 6.18 became a longterm series | early Dec 2025 (kernel.org table lists release 2025-11-30) | S1, S2 |
| 6.18 projected EOL, original | Dec 2027 | S1, S3 |
| 6.18 projected EOL, after Feb 2026 extension | **Dec 2028** (6.12 also Dec 2028; 6.6 moved to Dec 2027; 6.1 stays Dec 2027) | S4, S5, S6 |
| Latest mainline stable seen by the search tool | 7.2, released 2026-08-16 | S7 |
| Yocto 6.0 Wrynose | LTS, ships Linux 6.18, supported until at least April 2030 | S8, existing review (R1) |
| Next Yocto releases | 6.1 in Q4 2026 (kernel version unknown); next LTS (7.0) sometime in 2028 | S8 |

**Discrepancy to resolve.** Story 8.14 and the architecture say 6.18 EOL is about Dec 2027. Search results dated after Feb 2026 say kernel.org now projects Dec 2028. kernel.org itself could not be fetched in this session (proxy refused), so the live value is **to confirm** on https://www.kernel.org/category/releases.html. kernel.org gives only a month and calls the dates projections, so either value can move.

**Decision for this plan.** Keep the story's conservative baseline (EOL Dec 2027). The start date stays **2027-06-01** (6 months before 2027-12-01; derived from the story, not a sourced date). If kernel.org confirms Dec 2028, the founder may re-baseline the start to 2028-06-01 by editing the header block above in a reviewed commit; the CI check then follows the new date. Until that decision is recorded, 2027-06-01 applies. Rationale: units sold in 2027 must not depend on an extension that can be reversed, and the Phase 1 to 3 work is cheap to start early.

**Candidate next LTS.** No next LTS is designated yet (as of 2026-10-19, none found). Kernel LTS has so far been the last release of a calendar year, but that is a pattern, not a rule, and one commenter guessed 7.3 or 7.4 (S7). The candidate is therefore "the kernel that Greg Kroah-Hartman designates as the first 7.x longterm series, **to confirm**; expected from the last release of 2026 (7.3 or 7.4) , to confirm". Do not pin a version until kernel.org lists it as longterm. Also check what kernel Yocto 6.1 (Q4 2026) and later releases ship, since an LTS kernel not in a supported Yocto release means carrying `linux-yocto` or a custom recipe ourselves (to confirm).

## 2. Vendor kernel and Rockchip status

The product runs mainline, not the Rockchip vendor kernel (architecture: mainline PREEMPT_RT, upstream OP-TEE direction). Status found:

- Vendor 6.1 is still the default for several RK3588 distro images (DietPi moved RK3588 boards to the vendor 6.1 kernel in 2024) and its source lives on Rockchip's GitLab (S9). No official end-of-life or support window from Rockchip or Radxa was found: **to confirm**.
- Mainline RK3588 has advanced steadily (network 6.7, USB3 6.8, Mali GPU 6.10, CPU freq 6.11 per Collabora's Dec 2024 review; decoders upstream by FOSDEM 2026) (S10, S11). The Collabora page is dated and may have moved: **to confirm** for 7.x.
- The existing versions review records the relevant mainline pieces for this product: DSI2 glue in 6.14, USBDP PHY in 6.10, `rockchip,rk3588-i2s-tdm` in mainline, ALSA UMP in 6.5, `f_midi2` in 6.6 (R1, sources there, not re-verified here).
- A 2026 forum thread argues RK3588 users should move to mainline for CVE coverage over vendor 6.1 (S9). Treated as opinion.
- **Policy:** the vendor kernel is not a fallback for this migration. If the new mainline LTS fails a gate, the fallback is staying on the last good 6.18.y (while it is supported) and filing upstream, not switching to a vendor tree. A vendor-kernel fallback would need a new ADR (it would break the OP-TEE, RT and RAUC assumptions).
- **No published mainline RK3588 PREEMPT_RT latency data was found (R1 row 11).** Story 1.1 produces our own baseline; the migration compares against it.

## 3. Timeline (relative to PLAN_START_DATE = 2027-06-01)

All offsets are planned. Calendar dates after the start date are derived from the story's 6-month window and are proposals, **to confirm** at the Phase 0 gate.

| Phase | Window | Where | Entry criteria | Exit criteria |
|---|---|---|---|---|
| 0. Watch and prepare | now to 2027-05-31 | desk | This plan merged | New LTS designated on kernel.org and its version pinned in section 1 (or "none yet" recorded with date); kernel.org EOL for 6.18 re-checked and recorded; Yocto release carrying the candidate identified; CI check (section 8) merged; Story 1.1, 1.22, 8.10, 8.13 and the USB matrix baselines for 6.18 exist and are archived with the image version |
| 1. Config port | start + 0 to 4 weeks | desk + dev board | PLAN_STARTED=yes committed; baseline artifacts above available | Candidate kernel builds with the section 4 config fragment ported (diff of `olddefconfig` warnings reviewed, no unknown symbols); boots on dev board; `/sys/kernel/realtime` reads 1 |
| 2. Dependency re-validation | start + 4 to 10 weeks | dev board | Phase 1 exit | Every row of section 4 marked pass in the validation table, with evidence links |
| 3. Acceptance gates | start + 10 to 18 weeks | dev board, then prototype | Phase 2 exit | Every gate in section 5 passes on the dev board; prototype re-run of 8.10, 8.13 and USB matrix passes (when the prototype exists) |
| 4. Release as RAUC update | start + 18 to 22 weeks | CI + prototype | Phase 3 exit | Signed bundle installs on a unit running the old kernel, boots, is marked good, and the rollback drill (section 6) passes |
| 5. Roll-out | before EOL | field | Phase 4 exit | Update available to all shipped units through the desktop app and USB-A intake; 6.18 retired from the build matrix after the support window agreed in section 6 |

Buffer: Phases 1 to 4 total about 22 weeks of the 26 weeks between the start date and a Dec 2027 EOL, leaving about 4 weeks. The week counts are planning estimates with no benchmark behind them; revise after Phase 1.

## 4. Inventory of kernel dependencies to re-validate

| # | Dependency | What to check on the candidate kernel | Evidence of pass | Pending |
|---|---|---|---|---|
| 1 | PREEMPT_RT config fragment (`tools/spikes/rt-latency/`) | `CONFIG_PREEMPT_RT` still selectable on arm64 with our options; symbols renamed or removed; `isolcpus`, `nohz_full`, `rcu_nocbs` boot args still honored on the 3 isolated A76 cores; governor and DVFS settings unchanged; no new RT-incompatible default (for example new locks or debug options) | Diff of fragment; `/sys/kernel/realtime` = 1; Story 1.1 spike rerun | Dev board |
| 2 | Device tree for MIPI DSI | DSI2 controller and DC-PHY nodes (`dw-mipi-dsi2-rockchip`), panel and touch bindings unchanged or migrated; panel lights at native mode | Boot log, DRM modetest output, photo | Dev board |
| 3 | I2S/TDM machine driver | `rockchip,rk3588-i2s-tdm` plus PCM3168A ASoC (TDM slots per direction); MCLK gating behavior (codec is clock master per AD-17); machine driver still compiles against ASoC API; 64-frame period at 48 kHz opens | `aplay` and `arecord` loopback; 1 h zero-xrun run as in Story 1.2 | Dev board |
| 4 | USB gadget functions `f_uac2`, `f_midi2`, `f_ncm` | configfs composite still enumerates with exactly three functions; UAC2 async with feedback still correct; dwc3 peripheral mode and USBDP PHY DT unchanged; no new function appears | USB enumeration matrix (below) | Dev board + Mac, Windows, Linux hosts |
| 5 | RAUC bootloader backend | Bootloader (barebox or U-Boot, per the secure-boot spike outcome) still reads and writes slot state; kernel command line, DT and rootfs slot selection unchanged; RAUC version compatible with the Yocto release carrying the kernel (floor 1.15.2 per R1) | Slot-switch and mark-good test; `rauc status` | Dev board |
| 6 | OP-TEE interface | `tee` subsystem driver and `optee` driver ABI with the pinned OP-TEE OS (>= 4.9.0 per R1) and TF-A; SMC and shared-memory behavior (R1 notes dynamic SHM mismatches on RK3588); TA calls from `engine` and `librarian` work | TA unwrap and verify test; xtest suite result | Dev board |
| 7 | Other items found in the existing docs | NVMe and `io_uring` behavior with `kernel.io_uring_disabled=1` plus group; sysctls; driver set for the IRQ affinity plan from Story 1.1 | Short checklist run | Dev board |

If the candidate needs patches to pass any row, record each patch with its upstream status; out-of-tree RT patches remain forbidden (Story 1.1 constraint).

## 5. Acceptance gates

All gates must pass on the candidate kernel with the same hardware class, load profiles and thresholds as the 6.18 baseline. A gate that passed before and fails now blocks the release.

| Gate | Source story | Pass condition (as stated in the stories) | Notes for the migration |
|---|---|---|---|
| Latency threshold | Story 1.1 | `cyclictest` and `rtla timerlat` for at least 12 h under the full load profile; max latency on every isolated core <= 100 microseconds; outliers above 50 microseconds traced and attributed | Compare against the 6.18 baseline, and flag regressions in max and p99.99 even if still under the limit |
| 24 h soak | Story 1.22 | 24 h at 256 voices, SSD >= 90% full; zero xruns and zero stream underruns; any non-zero counter fails | Run on the release-candidate image with the new kernel |
| Boot time | Story 8.10 | Power-on to first audible note <= 10 s, 20 runs per Boot-into option, 1 TB library | New kernel size, driver probe time and initrd changes can add boot time |
| Power-cut campaigns | Story 8.13 | 100 cuts during update and 100 during import; always boots into previous or new firmware; no partial library | Re-run on the prototype or dev board with the relay rig; includes the kernel-update install window |
| USB enumeration matrix | Story 9.1 (`sysd/USB-MATRIX.md`) | macOS (current and previous major), Windows 11, current Ubuntu LTS load in-box class drivers; MIDI port appears; no storage exposed | Add a new column or dated rows for the new kernel |

## 6. Rollback approach

- The new kernel ships as a normal signed RAUC bundle, installed to the inactive slot by `sysd` (the sole installer), through the desktop app or a USB-A stick (architecture update delivery decision).
- A failed boot falls back to the previous slot by RAUC A/B: the unit boots the old kernel and rootfs, and the new slot is not marked good. This is covered by the Story 8.13 campaigns.
- **Rollback drill (planned, Phase 4):** (a) build a bundle whose kernel panics at boot, install, confirm automatic return to the old slot; (b) build a bundle that boots but fails the application health check, confirm it is not marked good and rolls back; (c) record slot states after each step. Pass: old firmware running, slot states consistent, no data loss in `/data`.
- Data compatibility: persisted formats are versioned and readers accept N and N-1 (architecture versioning decision), so rolling back the kernel does not touch library or catalog formats.
- Open question, **to confirm**: whether bootloader or OTP anti-rollback counters (review-rubric mentions the interaction) can block an intentional downgrade to 6.18.y; answer comes from the secure-boot spike.
- Keep 6.18.y buildable in CI until the new kernel has been in the field long enough for the founder to retire it (period to be set at Phase 5; not decided).

## 7. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| No LTS designated in time, or Yocto lacks it | Cannot adopt on schedule | Phase 0 watch; carry kernel recipe ourselves, or rely on 6.18 extension if confirmed |
| RT latency regression on the new kernel | Gate 1.1 fails | Bisect on mainline; keep 6.18.y until fixed; report upstream |
| DSI, I2S, USB or OP-TEE regressions | Phase 2 blocks | Per-row evidence; patch or pin; no vendor tree |
| EOL moves again | Plan date wrong | Re-check kernel.org at each phase gate |
| Dev board not available or prototype delayed | Gates cannot run | Keep the board reserved from the start date; prototype re-runs are additive |

## 8. Start-date enforcement (calendar reminder and CI check)

Both are required by the story and are committed in the repository:

- **Calendar reminder:** `yocto/kernel-lts-plan-reminders.ics` defines two iCalendar events: 2027-05-01 ("Velora Kernel LTS Migration Plan starts in 30 days") and 2027-06-01 ("Start Velora Kernel LTS Migration Plan execution"), linked to this file.
- **CI check:** `tools/ci/check_kernel_lts_plan.py` (registered in CTest as `kernel_lts_plan_check` and `kernel_lts_plan_check_selftest`), run on every merge to the main branch. It reads `PLAN_START_DATE` and `PLAN_STARTED` from the header block of this file and verifies `yocto/kernel-lts-plan-reminders.ics`. If today (UTC) is after `PLAN_START_DATE` and `PLAN_STARTED` is not `yes`, the job fails with the message `"Kernel LTS migration plan not started: see yocto/KERNEL-LTS-PLAN.md"`. Setting `PLAN_STARTED=yes` requires the Phase 1 branch to exist. Test: `check_kernel_lts_plan.py --self-test` verifies with faked dates that the check passes before and on the date, fails after it when `PLAN_STARTED=no`, and passes once `PLAN_STARTED=yes`.
- Re-baselining the date requires a reviewed change to the header block; the check is the audit trail.

## 9. Execution status tracker (to fill in during execution)

| Phase / gate | Status | Date | Evidence |
|---|---|---|---|
| Phase 0 | not started | | |
| Phase 1 | not started | | |
| Phase 2 (rows 1 to 7) | not started | | |
| Phase 3 (5 gates) | not started | | |
| Phase 4 (RAUC bundle + rollback drill) | not started | | |
| Phase 5 | not started | | |
| Calendar reminder set | done | 2026-10-29 | `yocto/kernel-lts-plan-reminders.ics` |
| CI check merged | done | 2026-10-29 | `tools/ci/check_kernel_lts_plan.py` (`kernel_lts_plan_check`) |

## 10. Sources

Access date for all web sources: **2026-10-19**. In this session pages could not be opened directly (the egress proxy refused kernel.org, Phoronix and similar hosts); facts come from search-result summaries of these pages and should be re-read before relying on them. Items marked "to confirm" above stay open until then.

- S1 Linuxiac, "Linux 6.18 Is Now Listed as the New Long-Term Support Kernel": https://linuxiac.com/linux-6-18-is-now-listed-as-the-new-long-term-support-kernel/
- S2 Thomas-Krenn wiki, Linux longterm kernel versions (lists 6.18 released 30.11.2025): https://www.thomas-krenn.com/en/wiki/Linux_longterm_kernel_versions
- S3 The New Stack, "Linux 6.18: all about the new long term support Linux kernel": https://thenewstack.io/linux-6-18-all-about-the-new-long-term-support-linux-kernel/
- S4 Phoronix, "Linux 6.18 LTS / 6.12 LTS / 6.6 LTS Support Periods Extended": https://www.phoronix.com/news/Linux-6.18-LTS-6.12-6.6-Extend
- S5 GamingOnLinux, "Longterm supported Linux kernels get a longer life" (Feb 2026, quotes kernel.org table): https://www.gamingonlinux.com/2026/02/longterm-supported-linux-kernels-get-a-longer-life/
- S6 Linuxiac, "Linux Kernel LTS Support Extended for Multiple Releases": https://linuxiac.com/linux-kernel-lts-support-extended-for-multiple-releases/ ; also It's FOSS https://itsfoss.com/news/linux-lts-kernel-support-extension/ and LWN https://lwn.net/Articles/1060271/
- S7 endoflife.date Linux and endoflife.ai (7.2 released 2026-08-16; no next-LTS announcement found); LWN comment on 7.3 or 7.4 as first 7.x LTS: https://endoflife.date/linux ; https://endoflife.ai/article-linux-kernel-510-515-eol ; https://lwn.net/Articles/1060721/
- S8 CNX Software, "Yocto Project 6.0 Wrynose released with Linux 6.18 LTS" (2026-05-14); Yocto release info: https://www.cnx-software.com/2026/05/14/yocto-project-6-0-wrynose-released-with-linux-6-18-lts/ ; https://docs.yoctoproject.org/migration-guides/release-notes-6.0.html ; https://endoflife.date/yocto
- S9 DietPi forum threads on vendor 6.1 and mainline migration: https://dietpi.com/forum/t/proper-way-to-move-from-vendor-to-mainline-kernel/25192 ; https://dietpi.com/forum/t/lots-of-nanopis-now-have-kernel-6-1-from-friendlyelec/18274/4
- S10 Collabora, "Rockchip RK3588 upstream support progress and future plans": https://www.collabora.com/news-and-blog/news-and-events/rockchip-rk3588-upstream-support-progress-future-plans.html
- S11 CNX Software, "Rockchip RK3588 mainline Linux support, status and future work for 2025" (2024-12-21); FOSDEM 2026 slides "No line like mainline Rockchip": https://cnx-software.com/2024/12/21/rockchip-rk3588-mainline-linux-support-current-status-and-future-work-for-2025/ ; https://fosdem.org/2026/events/attachments/KLFW73-no-line-like-mainline-rockchip/slides/267550/no-line-l_vrjwxmj.pdf
- R1 Internal: `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/reviews/review-versions.md` (findings F4, F5, table rows 1, 11 to 14, 18, 20) and `.memlog.md` (kernel lifecycle decision); `_bmad-output/planning-artifacts/epics.md` (Stories 1.1, 1.22, 8.10, 8.13, 8.14, 9.1; AR5).

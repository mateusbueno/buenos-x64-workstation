---
title: 'Story 8.14: Kernel LTS migration plan'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - 'yocto/KERNEL-LTS-PLAN.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Linux 6.18 LTS reaches its baseline projected end-of-life around December 2027, so units sold in 2027 need a documented, scheduled migration plan to the next mainline LTS kernel at least 6 months before EOL (`2027-06-01`), enforced by a calendar reminder and a CI check.

**Approach:**
1. Complete `yocto/KERNEL-LTS-PLAN.md` covering the candidate LTS kernel selection, dependency re-validation inventory (`PREEMPT_RT` fragment, MIPI DSI DT, I2S/TDM machine driver, USB gadget `f_uac2`/`f_midi2`/`f_ncm`, RAUC bootloader backend, OP-TEE interface), acceptance gates (latency threshold, 24 h soak, boot time, 100+100 power-cut campaigns, USB enumeration matrix), RAUC A/B rollout and rollback approach, and start-date enforcement.
2. Commit `yocto/kernel-lts-plan-reminders.ics` (2027-05-01 and 2027-06-01 reminders) and `tools/ci/check_kernel_lts_plan.py` (registered in CTest as `kernel_lts_plan_check` and `kernel_lts_plan_check_selftest`).

## Boundaries & Constraints

**Always:**
- Enforce `PLAN_START_DATE` and `PLAN_STARTED` in CI so any unstarted plan past `2027-06-01` fails the build.

**Ask First:** None.

**Never:**
- Adopt out-of-tree RT patches or switch to the vendor kernel without a new ADR.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error / Fallback |
|----------|---------------|----------------------------|------------------|
| Date before or on `PLAN_START_DATE` | `today <= 2027-06-01`, `PLAN_STARTED=no` | `check_kernel_lts_plan.py` exits 0 | N/A |
| Date after `PLAN_START_DATE`, unstarted | `today > 2027-06-01`, `PLAN_STARTED=no` | Fails with `"Kernel LTS migration plan not started: see yocto/KERNEL-LTS-PLAN.md"` | Exits 1 |
| Date after `PLAN_START_DATE`, started | `today > 2027-06-01`, `PLAN_STARTED=yes` | `check_kernel_lts_plan.py` exits 0 | N/A |

</frozen-after-approval>

## Code Map

- `yocto/KERNEL-LTS-PLAN.md` — Kernel LTS migration plan document and machine-readable state header.
- `yocto/kernel-lts-plan-reminders.ics` — Calendar reminders for `2027-05-01` and `2027-06-01`.
- `tools/ci/check_kernel_lts_plan.py` — CI check script and self-test.
- `CMakeLists.txt` — Registers `kernel_lts_plan_check` and `kernel_lts_plan_check_selftest`.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Document candidate LTS kernel, dependency inventory, acceptance gates, and RAUC rollback approach in `yocto/KERNEL-LTS-PLAN.md`.
- [x] **Task 2 (AC1):** Add `yocto/kernel-lts-plan-reminders.ics` and `tools/ci/check_kernel_lts_plan.py` enforcing the 6-month pre-EOL start date (`2027-06-01`).

## Verification

- `ctest --test-dir build --output-on-failure -R "kernel_lts_plan_check"`
- `python3 yocto/ci/static_checks.py`

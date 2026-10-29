#!/usr/bin/env python3
"""Story 8.14: CI enforcement check for `yocto/KERNEL-LTS-PLAN.md`.

Verifies:
  1. `yocto/KERNEL-LTS-PLAN.md` exists and contains the required sections covering candidate
     LTS kernel version, driver/DT compatibility matrix, 72 h RT latency re-qualification,
     100+100 power-cut re-qualification, and RAUC A/B test-unit rollout with rollback drill.
  2. The calendar reminder file `yocto/kernel-lts-plan-reminders.ics` exists and schedules
     the 2027-05-01 (30-day advance) and 2027-06-01 (start execution) events.
  3. Reads `PLAN_START_DATE` and `PLAN_STARTED` from `yocto/KERNEL-LTS-PLAN.md`. If today (UTC,
     or `--today YYYY-MM-DD`) is after `PLAN_START_DATE` and `PLAN_STARTED` is not `yes`, fails
     with `"Kernel LTS migration plan not started: see yocto/KERNEL-LTS-PLAN.md"`.
"""
import argparse
import datetime
import os
import re
import sys
import tempfile

DATE_RE = re.compile(r"^PLAN_START_DATE=(\d{4}-\d{2}-\d{2})\s*$", re.M)
STARTED_RE = re.compile(r"^PLAN_STARTED=(yes|no)\s*$", re.M)
REQUIRED_HEADINGS = (
    "## 1. Lifecycle facts",
    "## 3. Timeline",
    "## 4. Inventory of kernel dependencies",
    "## 5. Acceptance gates",
    "## 6. Rollback approach",
    "## 8. Start-date enforcement",
)


def check_plan(plan_path, ics_path=None, today_str=None):
    if not os.path.isfile(plan_path):
        return False, f"missing kernel LTS plan: {plan_path}"
    with open(plan_path, "r", encoding="utf-8") as f:
        text = f.read()

    for heading in REQUIRED_HEADINGS:
        if heading not in text:
            return False, f"missing required section '{heading}' in {plan_path}"

    m_date = DATE_RE.search(text)
    if not m_date:
        return False, f"missing PLAN_START_DATE=YYYY-MM-DD in {plan_path}"
    m_started = STARTED_RE.search(text)
    if not m_started:
        return False, f"missing PLAN_STARTED=yes|no in {plan_path}"

    start_date = datetime.date.fromisoformat(m_date.group(1))
    started = m_started.group(1) == "yes"

    if ics_path is not None:
        if not os.path.isfile(ics_path):
            return False, f"missing calendar reminder file: {ics_path}"
        with open(ics_path, "r", encoding="utf-8") as f:
            ics_text = f.read()
        if "BEGIN:VCALENDAR" not in ics_text or "20270601" not in ics_text:
            return False, f"calendar reminder file {ics_path} missing 2027-06-01 event"

    if today_str:
        today = datetime.date.fromisoformat(today_str)
    else:
        today = datetime.datetime.now(datetime.timezone.utc).date()

    if today > start_date and not started:
        return False, "Kernel LTS migration plan not started: see yocto/KERNEL-LTS-PLAN.md"

    return True, f"ok (today={today.isoformat()}, start={start_date.isoformat()}, started={m_started.group(1)})"


def self_test():
    repo_root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    real_plan = os.path.join(repo_root, "yocto", "KERNEL-LTS-PLAN.md")
    real_ics = os.path.join(repo_root, "yocto", "kernel-lts-plan-reminders.ics")

    # 1. Before start date (e.g. 2027-05-15) -> passes
    ok, msg = check_plan(real_plan, real_ics, today_str="2027-05-15")
    assert ok, msg

    # 2. On exact start date (2027-06-01) -> passes
    ok, msg = check_plan(real_plan, real_ics, today_str="2027-06-01")
    assert ok, msg

    # 3. After start date (2027-06-02) with PLAN_STARTED=no -> fails with exact message
    ok, msg = check_plan(real_plan, real_ics, today_str="2027-06-02")
    assert not ok, "expected failure after start date when PLAN_STARTED=no"
    assert msg == "Kernel LTS migration plan not started: see yocto/KERNEL-LTS-PLAN.md", msg

    # 4. After start date with PLAN_STARTED=yes -> passes
    with tempfile.TemporaryDirectory() as tmp:
        with open(real_plan, "r", encoding="utf-8") as f:
            started_text = f.read().replace("PLAN_STARTED=no", "PLAN_STARTED=yes")
        tmp_plan = os.path.join(tmp, "KERNEL-LTS-PLAN.md")
        with open(tmp_plan, "w", encoding="utf-8") as f:
            f.write(started_text)
        ok, msg = check_plan(tmp_plan, real_ics, today_str="2027-06-02")
        assert ok, msg

    print("check_kernel_lts_plan: self-test ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="CI check for yocto/KERNEL-LTS-PLAN.md (Story 8.14)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--plan", default=None)
    ap.add_argument("--ics", default=None)
    ap.add_argument("--today", default=None, help="override UTC date (YYYY-MM-DD)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    repo_root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    plan_path = args.plan or os.path.join(repo_root, "yocto", "KERNEL-LTS-PLAN.md")
    ics_path = args.ics or os.path.join(repo_root, "yocto", "kernel-lts-plan-reminders.ics")

    ok, msg = check_plan(plan_path, ics_path, today_str=args.today)
    if not ok:
        print(f"check_kernel_lts_plan: FAIL: {msg}", file=sys.stderr)
        return 1
    print(f"check_kernel_lts_plan: {msg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""SSD qualification runner (Story 11.11). Python 3 stdlib only.

  run_ssd_qual.py plan  --target /mnt/ssd/streamfile --size 900G --hours 12 --ssd-label modelA [--dry-run]
  run_ssd_qual.py summarize --fio-json out/stream.json [--thresholds thresholds.json]
  run_ssd_qual.py smart --device /dev/nvme0 --out out/smart_start.txt [--dry-run]
  run_ssd_qual.py --self-test

--dry-run prints every command and runs nothing (no fio/root needed).
Thresholds come from thresholds.json (null = not defined, reported as TODO, never pass/fail).
"""
import argparse, json, os, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PCTS = [("p50", "50.000000"), ("p99", "99.000000"), ("p99.9", "99.900000"),
        ("p99.99", "99.990000"), ("max", None)]

def load_thresholds(path):
    with open(path) as f:
        return json.load(f)

def run_or_print(cmd, dry, env=None):
    print("$ " + " ".join(f"{k}={shlex.quote(v)}" for k, v in (env or {}).items()) + " " + " ".join(shlex.quote(c) for c in cmd))
    if dry:
        return 0
    e = dict(os.environ); e.update(env or {})
    return subprocess.call(cmd, env=e)

def smart_cmds(device):
    """Endurance/SMART logging procedure; run at start, at end, and (optionally) periodically."""
    return [
        ["nvme", "smart-log", device],
        ["nvme", "smart-log", device, "-o", "json"],
        ["nvme", "id-ctrl", device],            # fw version (fr), model (mn), vwc (flush behaviour)
        ["smartctl", "-a", device],
        ["smartctl", "-x", device],
    ]

def cmd_plan(a):
    d = a.dry_run
    secs = int(float(a.hours) * 3600)
    os.makedirs(a.outdir, exist_ok=True) if not d else None
    rc = 0
    print("# 1. SMART baseline (before)")
    for c in smart_cmds(a.device):
        run_or_print(c, d)
    print("# 2. Fill to >=90% (epics.md:6418)")
    run_or_print(["fio", os.path.join(HERE, "jobs/fill.fio")], d, {"FIO_FILE": a.target, "FIO_SIZE": a.size})
    print(f"# 3. Streaming read load for {a.hours} h (min 12 h per thresholds.json)")
    out = os.path.join(a.outdir, f"{a.ssd_label}_stream.json")
    run_or_print(["fio", "--output-format=json", f"--output={out}", os.path.join(HERE, "jobs/stream_randread.fio")], d,
                 {"FIO_FILE": a.target, "FIO_RUNTIME": str(secs), "FIO_LOGPREFIX": os.path.join(a.outdir, a.ssd_label)})
    print("# 3b. In parallel, log drive temperature (e.g. every 10 s) with thermal/thermal_logger.py (hwmon incl. nvme)")
    print("# 4. SMART after")
    for c in smart_cmds(a.device):
        run_or_print(c, d)
    print(f"# 5. Summarize: run_ssd_qual.py summarize --fio-json {out}")
    return rc

def extract(job):
    """Return {label: microseconds} from a fio JSON job's read clat_ns percentiles."""
    r = job["read"]
    pc = r.get("clat_ns", {}).get("percentile", {})
    res = {}
    for label, key in PCTS:
        if key is None:
            res[label] = r.get("clat_ns", {}).get("max", 0) / 1000.0
        else:
            res[label] = pc.get(key, float("nan")) / 1000.0
    return res

def judge(res, th):
    keymap = {"p99": "p99_us_max", "p99.9": "p99_9_us_max", "p99.99": "p99_99_us_max", "max": "max_us_max"}
    verdict = {}
    for lab, k in keymap.items():
        lim = th.get(k, {}).get("value")
        if lim is None:
            verdict[lab] = "TODO (no threshold in repo)"
        else:
            verdict[lab] = "PASS" if res[lab] <= lim else "FAIL"
    return verdict

def cmd_summarize(a):
    with open(a.fio_json) as f:
        data = json.load(f)
    th = load_thresholds(a.thresholds)
    res = extract(data["jobs"][0])
    v = judge(res, th)
    print(f"{'metric':8} {'latency_us':>12}  verdict")
    for lab, _ in PCTS:
        print(f"{lab:8} {res[lab]:12.1f}  {v.get(lab, '-')}")
    return 0

def cmd_smart(a):
    for c in smart_cmds(a.device):
        run_or_print(c, a.dry_run)
    return 0

def self_test():
    fake = {"jobs": [{"read": {"clat_ns": {"max": 9_000_000, "percentile": {
        "50.000000": 100_000, "99.000000": 400_000, "99.900000": 900_000, "99.990000": 3_000_000}}}}]}
    th = load_thresholds(os.path.join(HERE, "thresholds.json"))
    res = extract(fake["jobs"][0])
    assert res["p50"] == 100.0 and res["p99.99"] == 3000.0 and res["max"] == 9000.0, res
    assert all("TODO" in x for x in judge(res, th).values())
    th2 = {"p99_us_max": {"value": 300}, "p99_9_us_max": {"value": 1000}, "p99_99_us_max": {"value": None}, "max_us_max": {"value": 1}}
    v = judge(res, th2)
    assert v["p99"] == "FAIL" and v["p99.9"] == "PASS" and v["max"] == "FAIL" and "TODO" in v["p99.99"], v
    for k in ("min_test_hours", "min_fill_percent", "voices"):
        assert th[k]["value"] is not None and th[k]["source"]
    for k, e in th.items():
        if isinstance(e, dict) and e.get("value") is None:
            assert "TODO" in e["source"], k
    print("self-test OK")
    return 0

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true")
    sub = p.add_subparsers(dest="cmd")
    pl = sub.add_parser("plan"); pl.add_argument("--target", required=True); pl.add_argument("--size", required=True)
    pl.add_argument("--hours", default="12"); pl.add_argument("--ssd-label", required=True)
    pl.add_argument("--device", default="/dev/nvme0"); pl.add_argument("--outdir", default="out")
    pl.add_argument("--dry-run", action="store_true")
    s = sub.add_parser("summarize"); s.add_argument("--fio-json", required=True)
    s.add_argument("--thresholds", default=os.path.join(HERE, "thresholds.json"))
    sm = sub.add_parser("smart"); sm.add_argument("--device", default="/dev/nvme0"); sm.add_argument("--dry-run", action="store_true")
    a = p.parse_args()
    if a.self_test:
        return self_test()
    return {"plan": cmd_plan, "summarize": cmd_summarize, "smart": cmd_smart}.get(a.cmd, lambda _: p.print_help() or 2)(a)

if __name__ == "__main__":
    sys.exit(main())

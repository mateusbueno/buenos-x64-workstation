#!/usr/bin/env python3
"""Power-fail timing analysis (Story 11.9 AC1, NFR18). Stdlib only. No hardware needed.

Input CSV (one row per cut, times from scope/logic analyzer on the same time base, in milliseconds):
  cut,t_input_loss_ms,t_power_fail_ms,t_rail_drop_ms
Computes per cut: input_loss -> POWER_FAIL, POWER_FAIL -> rail drop (lead), input_loss -> rail drop (hold-up).
Reports min/max over cuts and checks hold-up >= 150 ms, lead >= 100 ms (thresholds.json, from repo).

  powerfail_analyze.py cuts.csv [--expect-cuts 20]
  powerfail_analyze.py --self-test
"""
import csv, json, os, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))

def analyze(path, expect=None):
    th = json.load(open(os.path.join(HERE, "thresholds.json")))
    hold_min, lead_min = th["holdup_ms_min"]["value"], th["power_fail_lead_ms_min"]["value"]
    rows = list(csv.DictReader(open(path)))
    m = {"loss_to_pf": [], "pf_to_drop": [], "holdup": []}
    bad = []
    for r in rows:
        l, p, d = (float(r[k]) for k in ("t_input_loss_ms", "t_power_fail_ms", "t_rail_drop_ms"))
        m["loss_to_pf"].append(p - l); m["pf_to_drop"].append(d - p); m["holdup"].append(d - l)
        if d - l < hold_min or d - p < lead_min:
            bad.append(r["cut"])
    for k, v in m.items():
        print(f"{k:11} min={min(v):8.2f} ms  max={max(v):8.2f} ms")
    ok = not bad and (expect is None or len(rows) == expect)
    if expect is not None and len(rows) != expect:
        print(f"WARNING: {len(rows)} cuts, expected {expect}")
    print(f"cuts={len(rows)} failing={bad or 'none'} -> {'PASS' if ok else 'FAIL (report must name the redesign)'}")
    return 0 if ok else 1

def self_test():
    d = tempfile.mkdtemp(); p = os.path.join(d, "c.csv")
    with open(p, "w") as f:
        f.write("cut,t_input_loss_ms,t_power_fail_ms,t_rail_drop_ms\n")
        for i in range(20):
            f.write(f"{i},0,{5+i*0.1},{200+i}\n")
    assert analyze(p, 20) == 0
    with open(p, "a") as f:
        f.write("20,0,10,120\n")
    assert analyze(p) == 1
    print("self-test OK"); return 0

if __name__ == "__main__":
    if "--self-test" in sys.argv: sys.exit(self_test())
    if len(sys.argv) < 2: print(__doc__); sys.exit(2)
    exp = int(sys.argv[sys.argv.index("--expect-cuts") + 1]) if "--expect-cuts" in sys.argv else None
    sys.exit(analyze(sys.argv[1], exp))

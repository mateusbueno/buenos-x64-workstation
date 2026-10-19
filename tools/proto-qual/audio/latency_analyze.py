#!/usr/bin/env python3
"""Story 3.19 key-to-sound latency analyzer (stdlib only).

Input CSV columns (microseconds, same clock or pre-aligned by the rig):
  note,key_ts_us,onset_ts_us[,scan_transport_us,time_sync_us,sched_us,render_us,codec_us]
key_ts_us = contact closing (rig sense); onset_ts_us = analog onset on Main out.
Verdicts use thresholds.json; null thresholds give TODO, never PASS/FAIL.
"""
import argparse, csv, json, math, os, sys, tempfile, random

HERE = os.path.dirname(os.path.abspath(__file__))
BREAK = ["scan_transport_us", "time_sync_us", "sched_us", "render_us", "codec_us"]

def load_thr(path=None):
    with open(path or os.path.join(HERE, "thresholds.json")) as f:
        return {k: v["value"] for k, v in json.load(f).items()}

def pct(sorted_vals, p):
    if not sorted_vals: return None
    k = (len(sorted_vals) - 1) * p / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)

def stats(vals):
    s = sorted(vals)
    return {"n": len(s), "min": s[0], "median": pct(s, 50), "p99": pct(s, 99), "max": s[-1]}

def analyze(rows, thr, label="run"):
    lat = []
    br = {b: [] for b in BREAK}
    for r in rows:
        lat.append((float(r["onset_ts_us"]) - float(r["key_ts_us"])) / 1000.0)
        for b in BREAK:
            if r.get(b) not in (None, ""): br[b].append(float(r[b]) / 1000.0)
    out = {"label": label, "latency_ms": stats(lat), "breakdown_ms": {b: stats(v) for b, v in br.items() if v}, "verdicts": {}}
    mx = thr["latency_contact_to_analog_ms_max"]
    out["verdicts"]["every_note_max_ms"] = "PASS" if out["latency_ms"]["max"] <= mx else "FAIL"
    out["verdicts"]["notes_count"] = "PASS" if len(lat) >= thr["latency_notes_required"] else "FAIL"
    st = out["breakdown_ms"].get("scan_transport_us")
    lim = thr["scan_transport_ms_max_exclusive"]
    out["verdicts"]["scan_transport_lt_1ms"] = ("TODO (no scan_transport column)" if not st else
        "PASS" if st["max"] < lim else "FAIL")
    out["verdicts"]["p99_gate"] = "TODO" if thr.get("p99_latency_ms_max") is None else (
        "PASS" if out["latency_ms"]["p99"] <= thr["p99_latency_ms_max"] else "FAIL")
    out["verdicts"]["background_voices"] = "TODO (operator attests 256 voices in meta.txt)"
    return out

def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

def report(res):
    l = res["latency_ms"]
    print("== %s ==" % res["label"])
    print("latency ms: n=%d min=%.3f median=%.3f p99=%.3f max=%.3f" % (l["n"], l["min"], l["median"], l["p99"], l["max"]))
    for b, s in res["breakdown_ms"].items():
        print("  %-18s min=%.3f median=%.3f p99=%.3f max=%.3f" % (b, s["min"], s["median"], s["p99"], s["max"]))
    for k, v in res["verdicts"].items(): print("  %-24s %s" % (k, v))

def synth_rows(n, seed, bad=False):
    rnd = random.Random(seed); rows = []
    for i in range(n):
        sc = rnd.uniform(150, 400); ts = rnd.uniform(20, 60); sch = rnd.uniform(100, 600)
        rd = rnd.uniform(300, 1200); cd = rnd.uniform(1000, 1500)
        tot = sc + ts + sch + rd + cd + (4000 if bad and i == 7 else 0)
        k = i * 100000.0
        rows.append({"note": i, "key_ts_us": k, "onset_ts_us": k + tot, "scan_transport_us": sc,
                     "time_sync_us": ts, "sched_us": sch, "render_us": rd, "codec_us": cd})
    return rows

def self_test():
    thr = load_thr()
    assert abs(pct([1, 2, 3, 4], 50) - 2.5) < 1e-9
    good = analyze(synth_rows(1000, 1), thr)
    assert good["verdicts"]["every_note_max_ms"] == "PASS" and good["verdicts"]["notes_count"] == "PASS", good
    assert good["verdicts"]["scan_transport_lt_1ms"] == "PASS"
    assert good["verdicts"]["p99_gate"] == "TODO"
    bad = analyze(synth_rows(1000, 1, bad=True), thr)
    assert bad["verdicts"]["every_note_max_ms"] == "FAIL"
    assert analyze(synth_rows(10, 2), thr)["verdicts"]["notes_count"] == "FAIL"
    with tempfile.TemporaryDirectory() as d:  # CSV round trip
        p = os.path.join(d, "l.csv"); rows = synth_rows(50, 3)
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
        assert analyze(read_csv(p), thr)["latency_ms"]["n"] == 50
    print("latency_analyze self-test OK (synthetic data only)")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?"); ap.add_argument("--label", default="run")
    ap.add_argument("--thresholds"); ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="validate the CSV and thresholds, print nothing verdict-like")
    a = ap.parse_args()
    if a.self_test: return self_test()
    if not a.csv: ap.error("csv required")
    thr = load_thr(a.thresholds); rows = read_csv(a.csv)
    miss = {"key_ts_us", "onset_ts_us"} - set(rows[0] if rows else [])
    if miss or not rows: sys.exit("bad CSV: missing %s or empty" % sorted(miss))
    if a.dry_run:
        print("dry-run: %d rows, columns ok, thresholds loaded (%d keys)" % (len(rows), len(thr))); return
    res = analyze(rows, thr, a.label)
    print(json.dumps(res, indent=1)) if a.json else report(res)
    sys.exit(1 if "FAIL" in res["verdicts"].values() else 0)

if __name__ == "__main__": main()

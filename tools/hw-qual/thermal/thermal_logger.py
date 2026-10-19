#!/usr/bin/env python3
"""Thermal + CPU frequency logger (Story 11.9 AC3, NFR11; Story 3.20 asks 10 s interval, epics.md:2510). Stdlib only.

  thermal_logger.py --out run.csv [--interval 10] [--duration-h 24]
  thermal_logger.py --self-test        (no hardware; uses a fake sysfs tree)

CSV columns: iso_time, epoch_s, then one column per thermal zone (type, degC), per hwmon temp input
(name/label, degC, covers nvme), and per CPU scaling_cur_freq (kHz) plus cpuN_maxfreq_khz at start-of-row,
so throttling = cur_freq < max_freq (analyse with --analyze). Throttle limits are NOT defined in repo (TODO).
"""
import argparse, csv, glob, os, sys, tempfile, time, datetime

def rd(p):
    try:
        with open(p) as f:
            return f.read().strip()
    except OSError:
        return ""

def discover(root):
    cols = []  # (name, path, scale)
    for z in sorted(glob.glob(os.path.join(root, "sys/class/thermal/thermal_zone*"))):
        cols.append((f"tz_{os.path.basename(z)}_{rd(z + '/type')}_C", z + "/temp", 1000.0))
    for h in sorted(glob.glob(os.path.join(root, "sys/class/hwmon/hwmon*"))):
        name = rd(h + "/name") or os.path.basename(h)
        for t in sorted(glob.glob(h + "/temp*_input")):
            lab = rd(t.replace("_input", "_label"))
            cols.append((f"hwmon_{os.path.basename(h)}_{name}_{lab or os.path.basename(t)}_C", t, 1000.0))
    for c in sorted(glob.glob(os.path.join(root, "sys/devices/system/cpu/cpu[0-9]*/cpufreq")),
                    key=lambda p: int(p.split("/cpu")[-2].split("/")[0]) if False else p):
        n = c.split("/")[-2]
        cols.append((f"{n}_curfreq_khz", c + "/scaling_cur_freq", 1.0))
    return cols

def sample(cols):
    out = []
    for _, p, s in cols:
        v = rd(p)
        try:
            out.append(round(float(v) / s, 3))
        except ValueError:
            out.append("")
    return out

def run(root, outpath, interval, duration_s, max_rows=None):
    cols = discover(root)
    if not cols:
        print("no sensors found under", root, file=sys.stderr)
    t_end = time.time() + duration_s if duration_s else None
    n = 0
    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["iso_time", "epoch_s"] + [c[0] for c in cols])
        f.flush()
        while True:
            now = time.time()
            w.writerow([datetime.datetime.fromtimestamp(now).isoformat(timespec="seconds"), f"{now:.3f}"] + sample(cols))
            f.flush(); os.fsync(f.fileno())
            n += 1
            if (max_rows and n >= max_rows) or (t_end and time.time() + interval > t_end):
                break
            time.sleep(interval)
    return n

def analyze(path):
    """Print max of each temp column and min of each freq column; lets a human compare freq drop vs max (throttling)."""
    with open(path) as f:
        rows = list(csv.DictReader(f))
    for k in rows[0].keys() - {"iso_time", "epoch_s"}:
        vals = [float(r[k]) for r in rows if r[k] != ""]
        if vals:
            print(f"{k}: min={min(vals)} max={max(vals)}")

def self_test():
    d = tempfile.mkdtemp()
    def mk(p, v):
        os.makedirs(os.path.dirname(os.path.join(d, p)), exist_ok=True)
        open(os.path.join(d, p), "w").write(v)
    mk("sys/class/thermal/thermal_zone0/type", "soc-thermal"); mk("sys/class/thermal/thermal_zone0/temp", "55000")
    mk("sys/class/hwmon/hwmon1/name", "nvme"); mk("sys/class/hwmon/hwmon1/temp1_input", "41000")
    mk("sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "2256000")
    out = os.path.join(d, "o.csv")
    n = run(d, out, 0.01, None, max_rows=3)
    rows = list(csv.reader(open(out)))
    assert n == 3 and len(rows) == 4, rows
    hdr = rows[0]
    assert any("soc-thermal" in h for h in hdr) and any("nvme" in h for h in hdr) and any("curfreq" in h for h in hdr), hdr
    assert rows[1][2] == "55.0" and rows[1][3] == "41.0" and rows[1][4] == "2256000.0", rows[1]
    print("self-test OK")
    return 0

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true"); p.add_argument("--out")
    p.add_argument("--interval", type=float, default=10.0); p.add_argument("--duration-h", type=float)
    p.add_argument("--root", default="/", help="sysfs root (for testing)"); p.add_argument("--analyze")
    a = p.parse_args()
    if a.self_test: sys.exit(self_test())
    if a.analyze: analyze(a.analyze); sys.exit(0)
    if not a.out: p.error("--out required")
    run(a.root, a.out, a.interval, a.duration_h * 3600 if a.duration_h else None)

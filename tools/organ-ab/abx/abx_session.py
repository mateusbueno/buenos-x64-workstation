#!/usr/bin/env python3
"""ABX session tool for Story 2.14 (NFR21) with inputs from Story 11.14 (EXT7).

Stdlib only. Subcommands:
  plan     blinded randomized trial plan from a seed -> key.csv (hidden) + one sheet per listener
  analyze  answers CSV(s) + key.csv -> per-listener and pooled one-sided binomial p-values, pass/fail
  --self-test

No acceptance threshold is built in: --alpha, --min-trials and --rating-threshold are required
by `analyze`. The repo values are cited in README.md (epics.md:1789-1797, Story 2.14:
>=16 trials/listener, p < 0.05, mean rating >= 4/5).
"""
import argparse, csv, math, os, random, sys, tempfile

SOURCES = ("organ", "reference")


def binom_sf(c, n, p=0.5):
    """One-sided exact P(X >= c) for X~Binomial(n,p). p=0.5 uses exact integer arithmetic."""
    if c <= 0:
        return 1.0
    if c > n:
        return 0.0
    if p == 0.5:
        return sum(math.comb(n, k) for k in range(c, n + 1)) / 2 ** n
    return sum(math.comb(n, k) * p ** k * (1 - p) ** (n - k) for k in range(c, n + 1))


def min_correct(n, alpha):
    """Smallest c with P(X>=c) < alpha (strict), or None if impossible."""
    for c in range(n + 1):
        if binom_sf(c, n) < alpha:
            return c
    return None


def make_plan(seed, listeners, trials, excerpts):
    """Return key rows. Per listener: each excerpt used equally often (cycled, shuffled),
    A/B roles (which of organ/reference is A) and X (A or B) are balanced then shuffled."""
    rng = random.Random(seed)
    rows = []
    for li in range(1, listeners + 1):
        lid = "L%02d" % li
        ex = [excerpts[i % len(excerpts)] for i in range(trials)]
        a_org = [i % 2 == 0 for i in range(trials)]      # A is organ?
        x_is_a = [(i // 2) % 2 == 0 for i in range(trials)]
        rng.shuffle(ex); rng.shuffle(a_org); rng.shuffle(x_is_a)
        for t in range(trials):
            a, b = ("organ", "reference") if a_org[t] else ("reference", "organ")
            x = a if x_is_a[t] else b
            rows.append(dict(listener=lid, trial=t + 1, excerpt=ex[t], A=a, B=b, X=x,
                             correct="A" if x_is_a[t] else "B"))
    return rows


def write_csv(path, fields, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def cmd_plan(a):
    excerpts = [e.strip() for e in a.excerpts.split(",") if e.strip()]
    if not excerpts:
        sys.exit("no excerpts given")
    os.makedirs(a.out_dir, exist_ok=True)
    key = make_plan(a.seed, a.listeners, a.trials, excerpts)
    write_csv(os.path.join(a.out_dir, "key.csv"),
              ["listener", "trial", "excerpt", "A", "B", "X", "correct"], key)
    for li in sorted({r["listener"] for r in key}):
        sheet = [dict(listener=r["listener"], trial=r["trial"], excerpt=r["excerpt"],
                      answer="", rating_A="", rating_B="") for r in key if r["listener"] == li]
        write_csv(os.path.join(a.out_dir, "sheet_%s.csv" % li),
                  ["listener", "trial", "excerpt", "answer", "rating_A", "rating_B"], sheet)
    print("wrote key.csv (KEEP SEPARATE from listeners) and %d sheets in %s (seed=%s)"
          % (a.listeners, a.out_dir, a.seed))


def load_answers(paths, key):
    ans, errs = {}, []
    for p in paths:
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                k = (r["listener"].strip(), int(r["trial"]))
                if k not in key:
                    errs.append("unknown trial %s in %s" % (k, p)); continue
                if k in ans:
                    errs.append("duplicate answer %s" % (k,)); continue
                ch = r["answer"].strip().upper()
                if ch not in ("A", "B"):
                    errs.append("bad/blank answer %s in %s" % (k, p)); continue
                rt = {}
                for side in ("A", "B"):
                    v = r.get("rating_" + side, "").strip()
                    if v:
                        try:
                            fv = float(v)
                            if not 1 <= fv <= 5: raise ValueError
                            rt[side] = fv
                        except ValueError:
                            errs.append("bad rating %s %s" % (side, k))
                ans[k] = (ch, rt)
    return ans, errs


def analyze(key_rows, answer_paths, alpha, min_trials, rating_threshold):
    key = {(r["listener"], int(r["trial"])): r for r in key_rows}
    ans, errs = load_answers(answer_paths, key)
    per, organ_ratings = {}, []
    for (lid, t), (ch, rt) in sorted(ans.items()):
        d = per.setdefault(lid, [0, 0])
        d[1] += 1
        d[0] += ch == key[(lid, t)]["correct"]
        for side, v in rt.items():
            if key[(lid, t)][side] == "organ":
                organ_ratings.append(v)
    out = dict(listeners={}, errors=errs, alpha=alpha, min_trials=min_trials,
               rating_threshold=rating_threshold)
    tc = tn = 0
    for lid in sorted({k[0] for k in key}):
        c, n = per.get(lid, [0, 0])
        tc += c; tn += n
        total = sum(1 for k in key if k[0] == lid)
        out["listeners"][lid] = dict(correct=c, n=n, planned=total, rate=c / n if n else None,
                                     p=binom_sf(c, n) if n else None,
                                     complete=n == total, enough=n >= min_trials,
                                     distinguishes=bool(n) and binom_sf(c, n) < alpha)
    out["pooled"] = dict(correct=tc, n=tn, p=binom_sf(tc, tn) if tn else None)
    out["mean_organ_rating"] = (sum(organ_ratings) / len(organ_ratings)) if organ_ratings else None
    out["n_organ_ratings"] = len(organ_ratings)
    L = out["listeners"].values()
    valid = not errs and all(l["complete"] and l["enough"] for l in L)
    abx_ok = valid and not any(l["distinguishes"] for l in L)
    rating_ok = (out["mean_organ_rating"] is not None and out["mean_organ_rating"] >= rating_threshold)
    out.update(valid=valid, abx_pass=abx_ok, rating_pass=rating_ok,
               verdict="PASS" if valid and (abx_ok or rating_ok) else ("FAIL" if valid else "INVALID"))
    return out


def report(o):
    print("ABX report  alpha=%g  min_trials=%d  rating_threshold=%g"
          % (o["alpha"], o["min_trials"], o["rating_threshold"]))
    for lid, l in o["listeners"].items():
        p = "n/a" if l["p"] is None else "%.4f" % l["p"]
        flag = []
        if not l["complete"]: flag.append("INCOMPLETE")
        if not l["enough"]: flag.append("<min_trials")
        if l["distinguishes"]: flag.append("CAN-TELL(p<alpha)")
        print("  %s %d/%d correct  one-sided p=%s  %s" % (lid, l["correct"], l["n"], p, " ".join(flag)))
    pp = o["pooled"]
    print("  pooled %d/%d  p=%s (informational; verdict uses per-listener rule)"
          % (pp["correct"], pp["n"], "n/a" if pp["p"] is None else "%.4f" % pp["p"]))
    mr = o["mean_organ_rating"]
    print("  mean organ rating: %s (n=%d)" % ("n/a" if mr is None else "%.2f" % mr, o["n_organ_ratings"]))
    for e in o["errors"]: print("  ERROR:", e)
    print("  criterion 1 (no listener p<alpha): %s" % ("met" if o["abx_pass"] else "not met"))
    print("  criterion 2 (mean organ rating>=threshold): %s" % ("met" if o["rating_pass"] else "not met"))
    print("VERDICT: %s" % o["verdict"])


def cmd_analyze(a):
    with open(a.key, newline="") as f:
        key_rows = list(csv.DictReader(f))
    o = analyze(key_rows, a.answers, a.alpha, a.min_trials, a.rating_threshold)
    report(o)
    return 0 if o["verdict"] == "PASS" else 1


def self_test():
    assert abs(binom_sf(12, 16) - 2517 / 65536) < 1e-12
    assert abs(binom_sf(11, 16) - 6885 / 65536) < 1e-12
    assert min_correct(16, 0.05) == 12
    assert binom_sf(0, 5) == 1.0 and binom_sf(6, 5) == 0.0
    a = make_plan(7, 4, 16, ["e1", "e2", "e3", "e4"])
    assert a == make_plan(7, 4, 16, ["e1", "e2", "e3", "e4"]) and a != make_plan(8, 4, 16, ["e1", "e2", "e3", "e4"])
    for li in range(1, 5):
        rs = [r for r in a if r["listener"] == "L%02d" % li]
        assert len(rs) == 16 and sum(r["A"] == "organ" for r in rs) == 8
        assert sum(r["correct"] == "A" for r in rs) == 8
        assert all(r["X"] == (r["A"] if r["correct"] == "A" else r["B"]) for r in rs)
    d = tempfile.mkdtemp()
    ns = argparse.Namespace(seed=7, listeners=3, trials=16, excerpts="e1,e2", out_dir=d)
    cmd_plan(ns)
    sheets = open(os.path.join(d, "sheet_L01.csv")).read()
    assert "organ" not in sheets and "reference" not in sheets and "correct" not in sheets
    key = list(csv.DictReader(open(os.path.join(d, "key.csv"))))
    kd = {(r["listener"], int(r["trial"])): r for r in key}

    def fake(lid, ncorrect, rating_organ):
        rows = []
        for t in range(1, 17):
            r = kd[(lid, t)]
            ch = r["correct"] if t <= ncorrect else ("B" if r["correct"] == "A" else "A")
            ra = rating_organ if r["A"] == "organ" else 5 - rating_organ + 1
            rb = rating_organ if r["B"] == "organ" else 5 - rating_organ + 1
            rows.append(dict(listener=lid, trial=t, excerpt=r["excerpt"], answer=ch, rating_A=ra, rating_B=rb))
        return rows
    paths = []
    for lid, nc, rt in (("L01", 9, 3), ("L02", 8, 3), ("L03", 13, 3)):
        p = os.path.join(d, "ans_%s.csv" % lid); paths.append(p)
        write_csv(p, ["listener", "trial", "excerpt", "answer", "rating_A", "rating_B"], fake(lid, nc, rt))
    o = analyze(key, paths, 0.05, 16, 4.0)
    assert o["listeners"]["L03"]["distinguishes"] and not o["listeners"]["L01"]["distinguishes"]
    assert o["verdict"] == "FAIL" and abs(o["mean_organ_rating"] - 3) < 1e-9
    assert analyze(key, paths, 0.05, 16, 3.0)["verdict"] == "PASS"       # rating criterion rescues
    assert analyze(key, paths[:2], 0.05, 16, 4.0)["verdict"] == "INVALID"  # L03 missing -> incomplete
    assert analyze(key, paths, 0.05, 20, 4.0)["verdict"] == "INVALID"    # below min_trials
    write_csv(paths[2], ["listener", "trial", "excerpt", "answer", "rating_A", "rating_B"], fake("L03", 10, 3))
    o = analyze(key, paths, 0.05, 16, 4.0)
    assert o["verdict"] == "PASS" and o["abx_pass"]
    print("self-test OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sp = ap.add_subparsers(dest="cmd")
    p = sp.add_parser("plan")
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--listeners", type=int, required=True)
    p.add_argument("--trials", type=int, required=True, help="trials per listener")
    p.add_argument("--excerpts", required=True, help="comma-separated excerpt ids")
    p.add_argument("--out-dir", required=True)
    q = sp.add_parser("analyze")
    q.add_argument("--key", required=True)
    q.add_argument("--answers", nargs="+", required=True, help="completed listener CSVs")
    q.add_argument("--alpha", type=float, required=True)
    q.add_argument("--min-trials", type=int, required=True)
    q.add_argument("--rating-threshold", type=float, required=True)
    a = ap.parse_args()
    if a.self_test:
        self_test(); return 0
    if a.cmd == "plan":
        cmd_plan(a); return 0
    if a.cmd == "analyze":
        return cmd_analyze(a)
    ap.print_help(); return 2


if __name__ == "__main__":
    sys.exit(main())

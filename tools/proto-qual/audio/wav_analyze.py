#!/usr/bin/env python3
"""WAV analyzer for Stories 3.13-3.16 (stdlib: wave, math, struct only).

Subcommands (per channel, 16/24/32-bit PCM, any channel count):
  level   FILE            peak / RMS dBFS per channel (use for level, balance)
  noise   FILE            RMS dBFS per channel of a silence capture (noise floor)
  thdn    FILE --freq HZ  THD+N: residual after removing fitted fundamental, dB re total
  xtalk   FILE --drive N --victim M --freq HZ   tone on channel N only, level at M vs N (dB)
  pop     FILE --noise-floor-file F2 | --noise-dbfs X   transient detection
Verdicts compare to thresholds.json; null threshold => TODO.
"""
import argparse, json, math, os, struct, sys, tempfile, wave

HERE = os.path.dirname(os.path.abspath(__file__))
def load_thr():
    with open(os.path.join(HERE, "thresholds.json")) as f:
        return {k: v["value"] for k, v in json.load(f).items()}

def read_wav(path):
    with wave.open(path, "rb") as w:
        n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    if sw == 2: vals = [x / 32768.0 for x in struct.unpack("<%dh" % (n * ch), raw)]
    elif sw == 3:
        vals = []
        for i in range(0, len(raw), 3):
            v = int.from_bytes(raw[i:i + 3], "little", signed=True); vals.append(v / 8388608.0)
    elif sw == 4: vals = [x / 2147483648.0 for x in struct.unpack("<%di" % (n * ch), raw)]
    else: raise ValueError("unsupported sample width %d" % sw)
    return sr, [vals[c::ch] for c in range(ch)]

def write_wav(path, sr, chans, sw=3):
    n = len(chans[0]); out = bytearray()
    for i in range(n):
        for c in chans:
            v = max(-1.0, min(1.0, c[i])); iv = int(round(v * (2 ** (8 * sw - 1) - 1)))
            out += iv.to_bytes(sw, "little", signed=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(len(chans)); w.setsampwidth(sw); w.setframerate(sr); w.writeframes(bytes(out))

def db(x): return -300.0 if x <= 1e-15 else 20 * math.log10(x)
def rms(v): return math.sqrt(sum(x * x for x in v) / len(v)) if v else 0.0

def thdn(v, sr, f):
    """Least-squares fit of sin/cos at f (+DC); THD+N = residual rms / total rms (dB)."""
    n = len(v); w = 2 * math.pi * f / sr
    s = c = 0.0; m = sum(v) / n
    for i, x in enumerate(v):
        s += (x - m) * math.sin(w * i); c += (x - m) * math.cos(w * i)
    a, b = 2 * s / n, 2 * c / n
    res = [x - m - a * math.sin(w * i) - b * math.cos(w * i) for i, x in enumerate(v)]
    return db(rms(res) / max(rms([x - m for x in v]), 1e-15)), math.hypot(a, b)

def pops(v, sr, floor_rms, win_ms=1.0, margin_db=None):
    """Return list of (time_s, level_dBFS) windows whose RMS exceeds floor+margin."""
    if margin_db is None: return None
    n = max(1, int(sr * win_ms / 1000)); lim = floor_rms * 10 ** (margin_db / 20); hits = []
    for i in range(0, len(v) - n + 1, n):
        r = rms(v[i:i + n])
        if r > lim: hits.append((i / sr, db(r)))
    return hits

def cmd(a):
    thr = load_thr(); sr, ch = read_wav(a.file)
    if a.cmd in ("level", "noise"):
        for i, v in enumerate(ch, 1):
            print("ch%d peak=%.2f dBFS rms=%.2f dBFS" % (i, db(max(abs(x) for x in v)), db(rms(v))))
        k = "noise_floor_dbfs_max"
        print("noise-floor verdict: %s" % ("TODO (no threshold in repo)" if a.cmd == "noise" and thr[k] is None else "n/a"))
    elif a.cmd == "thdn":
        for i, v in enumerate(ch, 1):
            r, amp = thdn(v, sr, a.freq); print("ch%d fundamental=%.2f dBFS THD+N=%.2f dB (%s)" % (i, db(amp / math.sqrt(2)), r, "TODO" if thr["thd_n_db_max"] is None else ("PASS" if r <= thr["thd_n_db_max"] else "FAIL")))
    elif a.cmd == "xtalk":
        d, vv = ch[a.drive - 1], ch[a.victim - 1]
        _, ad = thdn(d, sr, a.freq); _, av = thdn(vv, sr, a.freq)
        x = db(av / max(ad, 1e-15)); t = thr["crosstalk_main_aux_db_max"]
        print("crosstalk ch%d->ch%d = %.2f dB (%s)" % (a.drive, a.victim, x, "TODO" if t is None else ("PASS" if x <= t else "FAIL")))
    elif a.cmd == "pop":
        floor = rms(read_wav(a.noise_floor_file)[1][0]) if a.noise_floor_file else 10 ** (a.noise_dbfs / 20)
        m = thr["pop_margin_db"] if a.margin_db is None else a.margin_db
        for i, v in enumerate(ch, 1):
            h = pops(v, sr, floor, margin_db=m)
            if h is None: print("ch%d pop verdict: TODO (pop_margin_db not defined in repo; pass --margin-db for exploratory use). max window rms %.2f dBFS vs floor %.2f dBFS" % (i, max(db(rms(v[j:j+48])) for j in range(0, len(v)-48, 48)), db(floor)))
            else: print("ch%d %s: %d transient window(s) %s" % (i, "FAIL" if h else "PASS", len(h), h[:3]))

def self_test():
    sr = 48000; n = sr
    import random; rnd = random.Random(7)
    def noise(a): return [rnd.gauss(0, a) for _ in range(n)]
    tone = [0.5 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(n)]
    # level
    assert abs(db(rms(tone)) - (-9.03)) < 0.05
    # THD+N: add 1% 3rd harmonic => ~ -40 dB
    h3 = [t + 0.005 * math.sin(2 * math.pi * 3000 * i / sr) for i, t in enumerate(tone)]
    r, amp = thdn(h3, sr, 1000); assert abs(r - (-40.0)) < 0.5 and abs(amp - 0.5) < 0.01, (r, amp)
    # noise floor: sigma 1e-4 => -80 dBFS
    nz = noise(1e-4); assert abs(db(rms(nz)) - (-80)) < 0.5
    # crosstalk: ch2 gets tone * 10^(-60/20)
    ct = [t * 1e-3 for t in tone]
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.wav"); write_wav(p, sr, [tone, ct, nz]); sr2, c = read_wav(p)
        assert sr2 == sr and len(c) == 3 and abs(c[0][100] - tone[100]) < 1e-5
        _, a1 = thdn(c[0], sr, 1000); _, a2 = thdn(c[1], sr, 1000)
        assert abs(db(a2 / a1) - (-60)) < 0.5
        # pop: silence + noise with a 2 ms click at 0.5 s
        sil = noise(1e-4); clean = pops(sil, sr, 1e-4, margin_db=6); assert clean == [], clean
        for i in range(24000, 24096): sil[i] += 0.05
        hit = pops(sil, sr, 1e-4, margin_db=6); assert hit and abs(hit[0][0] - 0.5) < 0.002, hit
        assert pops(sil, sr, 1e-4, margin_db=None) is None   # no repo margin => TODO
        write_wav(os.path.join(d, "s16.wav"), sr, [tone[:100]], sw=2); assert len(read_wav(os.path.join(d, "s16.wav"))[1][0]) == 100
    print("wav_analyze self-test OK (synthetic WAV only)")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sp = ap.add_subparsers(dest="cmd")
    for n in ("level", "noise", "thdn", "xtalk", "pop"):
        p = sp.add_parser(n); p.add_argument("file")
        p.add_argument("--dry-run", action="store_true", help="only open the WAV and print its format")
        if n in ("thdn", "xtalk"): p.add_argument("--freq", type=float, default=1000.0)
        if n == "xtalk": p.add_argument("--drive", type=int, required=True); p.add_argument("--victim", type=int, required=True)
        if n == "pop":
            p.add_argument("--noise-floor-file"); p.add_argument("--noise-dbfs", type=float, default=-90.0)
            p.add_argument("--margin-db", type=float)
    a = ap.parse_args()
    if a.self_test: return self_test()
    if not a.cmd: ap.error("subcommand required")
    if a.dry_run:
        sr, ch = read_wav(a.file); print("dry-run: %d Hz, %d ch, %d frames" % (sr, len(ch), len(ch[0]))); return
    cmd(a)

if __name__ == "__main__": main()

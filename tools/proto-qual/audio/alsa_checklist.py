#!/usr/bin/env python3
"""Story 3.13/3.15 ALSA config checklist (stdlib only).

Checks captured text of `aplay -D <dev> --dump-hw-params /dev/zero` style output or
/proc/asound/cardN/pcmXp/sub0/hw_params ("rate: 48000 (48000/1)", "channels: 6",
"period_size: 64") against thresholds.json. Items that cannot be read from ALSA
(clock master, PLL, xruns, DLL lock) are listed MANUAL with the evidence to attach.
  alsa_checklist.py --playback FILE --capture FILE
  alsa_checklist.py --dry-run     (print the checklist, read nothing)
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
def load_thr():
    with open(os.path.join(HERE, "thresholds.json")) as f:
        return {k: v["value"] for k, v in json.load(f).items()}

def field(text, name):
    m = re.search(r"^\s*%s\s*[:=]\s*\[?\s*(\d+)" % name, text, re.M | re.I)
    return int(m.group(1)) if m else None

MANUAL = [
 ("PCM3168A is BCLK/LRCK master on its 24.576 MHz crystal, SoC I2S/TDM slave", "DT snippet (bitclock-master/frame-master), scope BCLK/LRCK at codec pins, `cat /sys/kernel/debug/clk/clk_summary`"),
 ("SoC PLL generates no audio clock", "clk_summary showing MCLK/I2S clocks not derived from the audio PLL"),
 ("Mainline rockchip-i2s-tdm + pcm3168a drivers in use", "`lsmod`/`dmesg | grep -i -E 'pcm3168|i2s'`, kernel config"),
 ("Engine 1 h, zero xruns", "engine xrun counter export (FR120) + ALSA /proc/asound/cardN/pcm0p/sub0/status"),
 ("DLL (Story 1.20) locks to codec clock", "engine DLL lock log"),
 ("DAC->ADC loopback latency + noise vs partner reference figures", "wav_analyze.py output + partner figures (TODO: figures not in repo)"),
]

def check(play, cap, thr):
    res = []
    def cmp(label, got, want):
        res.append((label, "TODO (not found in input)" if got is None else ("PASS" if got == want else "FAIL"), "got=%s want=%s" % (got, want)))
    for tag, txt, chk in (("playback", play, thr["playback_channels"]), ("capture", cap, thr["capture_channels"])):
        if txt is None: res.append((tag + " hw_params", "TODO (no file)", "")); continue
        cmp(tag + " channels", field(txt, "channels"), chk)
        cmp(tag + " rate", field(txt, "rate"), thr["sample_rate_hz"])
        cmp(tag + " period_size", field(txt, "period_size"), thr["period_frames"])
    return res

def show(res):
    for l, v, d in res: print("%-8s %-42s %s" % (v.split(" ")[0], l, d))
    for l, ev in MANUAL: print("%-8s %-42s evidence: %s" % ("MANUAL", l[:42], ev))

def self_test():
    thr = load_thr()
    good_p = "access: RW_INTERLEAVED\nrate: 48000 (48000/1)\nchannels: 6\nperiod_size: 64\n"
    good_c = "rate: 48000 (48000/1)\nchannels: 2\nperiod_size: 64\n"
    assert all(v == "PASS" for _, v, _ in check(good_p, good_c, thr))
    r = check(good_p.replace("channels: 6", "channels: 8"), good_c, thr)
    assert [v for l, v, _ in r if l == "playback channels"] == ["FAIL"]
    assert check(None, good_c, thr)[0][1].startswith("TODO")
    assert field("period_size = 64 frames", "period_size") == 64
    print("alsa_checklist self-test OK (synthetic text only)")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--playback"); ap.add_argument("--capture")
    ap.add_argument("--self-test", action="store_true"); ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.self_test: return self_test()
    thr = load_thr()
    if a.dry_run:
        print("expected: playback %d ch, capture %d ch, %d Hz, period %d frames" % (thr["playback_channels"], thr["capture_channels"], thr["sample_rate_hz"], thr["period_frames"]))
        show([]); return
    rd = lambda p: open(p).read() if p else None
    res = check(rd(a.playback), rd(a.capture), thr); show(res)
    sys.exit(1 if any(v == "FAIL" for _, v, _ in res) else 0)

if __name__ == "__main__": main()

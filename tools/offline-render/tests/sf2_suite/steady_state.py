#!/usr/bin/env python3
"""Steady-state check of an SF2 render against its FluidSynth golden (Story 5.3), stdlib only.

FluidSynth's output is a constant fraction of a frame late compared with the engine's (its always-on 2-pole
output filter delays by about 0.2 frame), which an integer-frame null test cannot absorb. This script
aligns a window of the golden with the candidate at a fractional lag (4-point Catmull-Rom, the same kernel
as the engine) and prints the best lag, the residual there and the residual after fitting a gain.

  steady_state.py golden.wav candidate.wav FIRST_FRAME LAST_FRAME

The candidate is what `sf2_suite_test <case>` leaves in <build>/tools/offline-render/sf2_suite/<case>/candidate.wav
(32-bit float, engine MIDI already delayed by the oracle's one block). Pick a window inside a held note.
"""
import math
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    pos, fmt = 12, None
    while pos < len(d):
        cid, size = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
        if cid == b'data':
            tag, ch, _, _, _, bits = fmt
            body = d[pos + 8:pos + 8 + size]
            n = len(body) // (bits // 8)
            if tag == 3:
                v = list(struct.unpack('<%df' % n, body))
            elif bits == 16:
                v = [x / 32768.0 for x in struct.unpack('<%dh' % n, body)]
            else:
                raise SystemExit('unsupported WAV format')
            return v[0::ch]
        pos += 8 + size + (size & 1)
    raise SystemExit('no data chunk')


def interp(v, p):
    i = int(math.floor(p))
    f = p - i
    p0, p1, p2, p3 = v[i - 1], v[i], v[i + 1], v[i + 2]
    return p1 + 0.5 * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)))


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    ref, cand = load(sys.argv[1]), load(sys.argv[2])
    lo, hi = int(sys.argv[3]), int(sys.argv[4])
    best = None
    lag = -1.0
    while lag <= 1.0001:
        err = sum((interp(ref, i + lag) - cand[i]) ** 2 for i in range(lo, hi))
        if best is None or err < best[0]:
            best = (err, lag)
        lag += 0.05
    energy = sum(cand[i] ** 2 for i in range(lo, hi))
    lag = best[1]
    num = sum(interp(ref, i + lag) * cand[i] for i in range(lo, hi))
    gain = num / energy
    fit = sum((interp(ref, i + lag) - gain * cand[i]) ** 2 for i in range(lo, hi))
    print('best fractional lag %.2f frame, residual %.1f dB; with a fitted gain of %.4f dB: %.1f dB' %
          (lag, 10 * math.log10(best[0] / energy), 20 * math.log10(gain), 10 * math.log10(fit / energy)))


if __name__ == '__main__':
    main()

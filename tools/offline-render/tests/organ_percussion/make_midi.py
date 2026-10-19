#!/usr/bin/env python3
"""Writes organ_percussion.mid (Story 2.3): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second).

It exercises, on organ part 0 (MIDI channel 1), through the mapped controllers only: percussion on/off (CC 80), harmonic
(CC 84: <64 second, >=64 third), decay (CC 85: <64 fast, >=64 slow), volume (CC 86: <64 soft, >=64 normal) and key click
(CC 82), with staccato and legato playing. Run:  python3 make_midi.py organ_percussion.mid
"""
import struct
import sys

TPS = 960  # ticks per second at 120 bpm, 480 ppq
ev = []  # (tick, order, bytes)


def cc(t, ctl, val):
    ev.append((int(t * TPS), 0, bytes([0xB0, ctl, val])))


def note(t_on, t_off, n, vel=100):
    ev.append((int(t_on * TPS), 2, bytes([0x90, n, vel])))
    ev.append((int(t_off * TPS), 1, bytes([0x80, n, 0])))


def setup(t, perc, harmonic, decay, volume):  # harmonic/decay/volume: 0 or 127
    cc(t, 80, 127 if perc else 0)
    cc(t, 84, harmonic)
    cc(t, 85, decay)
    cc(t, 86, volume)


# 1. third / fast / soft: legato chord first note alone, then staccato single notes (each retriggers)
setup(0.00, True, 127, 0, 0)
note(0.00, 0.30, 60)
note(0.40, 0.50, 64)
note(0.60, 0.70, 67)
# 2. same setting, legato: only the first key carries the percussion
note(0.80, 1.30, 60)
note(0.95, 1.30, 64)
note(1.10, 1.30, 67)
# 3. second / slow / normal: staccato then legato
setup(1.40, True, 0, 127, 127)
note(1.40, 1.80, 60)
note(1.90, 2.30, 55)
note(2.05, 2.30, 62)
# 4. the remaining combinations, one note each, so that all eight harmonic/decay/volume combinations are heard:
#    second/fast/normal, third/slow/soft, third/fast/normal, second/slow/soft, third/slow/normal, second/fast/soft
#    (third/fast/soft is section 1, second/slow/normal section 3)
setup(2.40, True, 0, 0, 127)
note(2.40, 2.60, 67)
setup(2.70, True, 127, 127, 0)
note(2.70, 2.95, 72)
setup(3.00, True, 127, 0, 127)
note(3.00, 3.20, 64)
setup(3.30, True, 0, 127, 0)
note(3.30, 3.55, 69)
setup(3.60, True, 127, 127, 127)
note(3.60, 3.80, 62)
setup(3.85, True, 0, 0, 0)
note(3.85, 4.05, 74)
# 5. key click on its own: percussion off, click raised to full; the same key twice, then another key
setup(4.10, False, 127, 0, 0)
cc(4.10, 82, 127)
note(4.15, 4.25, 48)
note(4.30, 4.40, 48)
note(4.45, 4.55, 72)

ev.sort(key=lambda e: (e[0], e[1]))
trk = bytearray()
trk += b"\x00\xff\x51\x03" + (500000).to_bytes(3, "big")
last = 0
for tick, _, data in ev:
    d = tick - last
    last = tick
    vlq = [d & 0x7F]
    d >>= 7
    while d:
        vlq.append((d & 0x7F) | 0x80)
        d >>= 7
    trk += bytes(reversed(vlq)) + data
trk += b"\x00\xff\x2f\x00"
out = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(trk)) + bytes(trk)
with open(sys.argv[1], "wb") as f:
    f.write(out)

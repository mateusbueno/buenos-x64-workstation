#!/usr/bin/env python3
"""Writes organ_scanner.mid (Story 2.4): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second).

A six-note chord on organ part 0 (MIDI channel 1) is held for the whole file while the controllers cycle the scanner:
vibrato/chorus off, then V1, V2, V3, C1, C2, C3 (CC 81 on/off, CC 87 type: value round(index * 127 / 5)), then off again,
then leakage raised from 0 to 1 (CC 83) in two steps. A second, short chord on channel 2 (part 1) stays on the default
setting to show the parts are independent. Run:  python3 make_midi.py organ_scanner.mid
"""
import struct
import sys

TPS = 960
ev = []


def cc(t, ctl, val, ch=0):
    ev.append((int(t * TPS), 0, bytes([0xB0 | ch, ctl, val])))


def note(t_on, t_off, n, vel=100, ch=0):
    ev.append((int(t_on * TPS), 2, bytes([0x90 | ch, n, vel])))
    ev.append((int(t_off * TPS), 1, bytes([0x80 | ch, n, 0])))


END = 4.4
for n in (48, 52, 55, 60, 64, 67):
    note(0.0, END, n)
for i in range(6):  # V1 V2 V3 C1 C2 C3, 0.4 s each starting at 0.4 s
    t = 0.4 + 0.4 * i
    cc(t, 87, round(i * 127 / 5))
    cc(t, 81, 127)
cc(2.8, 81, 0)      # back to off
cc(3.2, 83, 64)     # leakage half
cc(3.6, 83, 127)    # leakage full
cc(3.6, 87, 127)
cc(3.8, 81, 127)    # C3 with full leakage
for n in (40, 47):  # part 1: untouched scanner (registry default: off), short notes
    note(1.0, 1.6, n, ch=1)

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

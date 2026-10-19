#!/usr/bin/env python3
"""Writes organ_rotary.mid (Story 2.6): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second).

A six-note chord on organ part 0 (MIDI channel 1) is held for 5 s while the controllers drive the rotary speaker: CC 89
switches the stage on at 0.3 s (from off, a 5 ms crossfade), CC 79 goes FAST at 1.0 s, STOP at 2.4 s and SLOW at 4.2 s (CC 79
values 127 / 0 / 64: fast, stop, slow). A short second chord on channel 2 (part 1) stays on the registry default (rotary off) to
show the parts are independent. Run:  python3 make_midi.py organ_rotary.mid
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


for n in (48, 52, 55, 60, 64, 67):
    note(0.0, 5.0, n)
cc(0.3, 89, 127)    # rotary on (slow)
cc(1.0, 79, 127)    # fast
cc(2.4, 79, 0)      # stop
cc(4.2, 79, 64)     # slow
for n in (40, 47):  # part 1: rotary left off
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

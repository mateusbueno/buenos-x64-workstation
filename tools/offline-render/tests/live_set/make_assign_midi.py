#!/usr/bin/env python3
"""Writes assign.mid (Story 4.10): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second), a held organ chord for assign.liveset played with --assign-mode
and encoder turns (K1 = drawbar 4', K2 = reverb send, K3 empty) while the chord, a melody note and a bass note sound.

  chord  C3 G3 E4   held from 0.2 to 5.0 (across every encoder turn)
  melody C5         2.2 .. 5.6 (starts after the first turns, ends after the last)
  bass   E1         4.0 .. 5.0
Run:  python3 make_assign_midi.py assign.mid
"""
import struct
import sys

TPS = 960
ev = []


def note(t_on, t_off, n, vel=100, ch=0):
    ev.append((int(round(t_on * TPS)), 2, bytes([0x90 | ch, n, vel])))
    ev.append((int(round(t_off * TPS)), 1, bytes([0x80 | ch, n, 0])))


for n in (48, 55, 64):
    note(0.20, 5.00, n, 100)
note(2.20, 5.60, 72, 90)
note(4.00, 5.00, 28, 110)

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

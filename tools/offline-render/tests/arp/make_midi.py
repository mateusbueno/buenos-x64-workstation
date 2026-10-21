#!/usr/bin/env python3
"""Writes arp.mid (Story 7.2): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second).

Exercises simultaneous multi-part arpeggiation across Part 0 (channel 0) and Part 1 (channel 1):
- Channel 0: a three-note chord (60, 67, 64) struck in non-ascending order so As-Played and Up produce distinct pitch orders,
  followed by a second chord (62, 65, 69, 72) after a brief release gap.
- Channel 1: a bass triad (48, 55, 52) struck at t = 0 and a two-note interval (43, 50) in the second phrase.
Run: python3 make_midi.py arp.mid
"""
import struct
import sys

TPS = 960  # ticks per second at 120 bpm, 480 ppq
ev = []  # (tick, order, bytes)


def note_ticks(t_on_ticks, t_off_ticks, n, vel=100, ch=0, order_bias=0):
    ev.append((t_on_ticks, 2 + order_bias, bytes([0x90 | ch, n, vel])))
    ev.append((t_off_ticks, 1, bytes([0x80 | ch, n, 0])))


# Phrase 1 (ticks 0..1440 = 0.0 s..1.5 s at 120 bpm):
# Channel 0: C4 (60), G4 (67), E4 (64) all at tick 0 in non-sorted order (60, 67, 64)
note_ticks(0, 1440, 60, 100, ch=0, order_bias=0)
note_ticks(0, 1440, 67, 85, ch=0, order_bias=1)
note_ticks(0, 1440, 64, 115, ch=0, order_bias=2)

# Channel 1: C3 (48), G3 (55), E3 (52) at tick 0 in non-sorted order
note_ticks(0, 1440, 48, 95, ch=1, order_bias=0)
note_ticks(0, 1440, 55, 80, ch=1, order_bias=1)
note_ticks(0, 1440, 52, 110, ch=1, order_bias=2)

# Phrase 2 (ticks 1560..2400 = 1.625 s..2.5 s at 120 bpm):
note_ticks(1560, 2400, 62, 105, ch=0, order_bias=0)
note_ticks(1560, 2400, 69, 90, ch=0, order_bias=1)
note_ticks(1560, 2400, 65, 110, ch=0, order_bias=2)

note_ticks(1560, 2400, 43, 100, ch=1, order_bias=0)
note_ticks(1560, 2400, 50, 90, ch=1, order_bias=1)

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

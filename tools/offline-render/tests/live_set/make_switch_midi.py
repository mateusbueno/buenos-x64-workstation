#!/usr/bin/env python3
"""Writes switch.mid (Story 4.5): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second), for the seamless switch golden
switch/switch_a.liveset -> switch/switch_b.liveset, rendered with --switch-at 3.0:switch/switch_b.liveset --switch-load-blocks 100
(the new Live Set takes over about 0.13 s after the jump).

Notes cross the swap in every way a note can:
  held        (A) a piano chord 52 + 55 + 59 from 0.5 to 4.5, released well after the swap
  releasing   (A) key 50 ends at 2.5; the pad's 0.8 s release is still sounding when the swap happens at 3.0
  sustained   (A) keys 64 and 40 are released at 2.9 / 3.1 under the pedal (down at 2.6, up at 4.0); the pedal-up releases them on Live Set B's watch
  new         (B) keys 72, 41, 84 and 65 after the swap (72 under the pedal that is still down), the organ zone above C4 included
Run:  python3 make_switch_midi.py switch.mid
"""
import struct
import sys

TPS = 960
ev = []


def note(t_on, t_off, n, vel=100, ch=0):
    ev.append((int(round(t_on * TPS)), 2, bytes([0x90 | ch, n, vel])))
    ev.append((int(round(t_off * TPS)), 1, bytes([0x80 | ch, n, 0])))


def pedal(t, down, ch=0):
    ev.append((int(round(t * TPS)), 0, bytes([0xB0 | ch, 64, 127 if down else 0])))


# Live Set A
note(0.20, 0.70, 36, 110)
note(0.50, 4.50, 52, 90)       # held chord across the swap
note(0.52, 4.52, 55, 85)
note(0.54, 4.46, 59, 80)
note(0.80, 1.20, 60, 100)
note(1.40, 2.00, 38, 100)
note(2.00, 2.50, 50, 100)      # releasing: the pad tail is still sounding at the swap
pedal(2.60, True)
note(2.70, 2.90, 64, 100)      # sustained by the pedal until 4.0
note(2.80, 3.10, 40, 95)       # released at 3.1, after the jump, still under the pedal
# the swap: --switch-at 3.0, the new generation is published ~0.13 s later
note(3.50, 3.90, 72, 100)      # Live Set B: under the pedal that is still down
pedal(4.00, False)
note(4.20, 4.80, 41, 100)
note(4.40, 5.50, 84, 90)
note(4.60, 5.20, 65, 100)

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

#!/usr/bin/env python3
"""Writes scenes.mid (Story 4.4): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second), for scenes.liveset played with
--scene-at 0:0 --scene-at 2.0:1 --scene-at 4.5:2 (verse, chorus, outro).

Notes are placed so that held, pedal-sustained and releasing notes cross every recall:
  verse  (0.0 .. 2.0): bass notes, a piano note held from 1.0 to 5.2 (across both recalls), a note that ends in the chorus.
  chorus (2.0 .. 4.5): the pad is on the whole keyboard and the organ plays; the pedal holds a note over the recall into the outro; a note that
                       started in the chorus is still held in the outro.
  outro  (4.5 .. 6.0): the piano alone; a key with no part (the bass zone with the bass off).
Run:  python3 make_scenes_midi.py scenes.mid
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


# verse
note(0.20, 0.70, 36, 110)
note(0.80, 1.30, 40, 100)
note(1.00, 5.20, 48, 90)       # held across the chorus and the outro recalls
note(1.40, 3.20, 52, 80)       # starts in the verse, ends in the chorus
# chorus
note(2.20, 3.00, 60, 100)
note(2.60, 3.40, 72, 90)
pedal(3.00, True)
note(3.10, 3.40, 64, 100)      # released under the pedal: sustained until 4.6, across the outro recall
note(3.60, 4.40, 84, 100)
note(4.20, 5.50, 67, 90)       # started in the chorus, released in the outro
# outro
note(4.80, 5.40, 55, 100)
note(5.00, 5.50, 40, 100)      # bass zone: the bass is off in the outro, nothing plays it
pedal(4.60, False)

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

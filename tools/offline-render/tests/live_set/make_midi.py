#!/usr/bin/env python3
"""Writes layer_split.mid (Story 4.3): a format 0 SMF, 480 ppq, 120 bpm (960 ticks per second), for layer_split.liveset.

Every note is a keyboard event: the Live Set fans it out to its parts by zone (the channel only names the keyboard source). It covers
the bass zone (up to B2 = 47) and the piano zone (from C3 = 48), the notes exactly on both sides of the split, a chord across it, the
octave-up doubling above C5 (72), a key whose doubling would be out of range (120), a key struck on a second keyboard channel, and the
sustain pedal held over a release. Run:  python3 make_midi.py layer_split.mid
"""
import struct
import sys

TPS = 960  # ticks per second at 120 bpm, 480 ppq
ev = []  # (tick, order, bytes)


def note(t_on, t_off, n, vel=100, ch=0):
    ev.append((int(t_on * TPS), 2, bytes([0x90 | ch, n, vel])))
    ev.append((int(t_off * TPS), 1, bytes([0x80 | ch, n, 0])))


def pedal(t, down, ch=0):
    ev.append((int(t * TPS), 0, bytes([0xB0 | ch, 64, 127 if down else 0])))


# bass zone: a walk up to B2, then C3 and B2 side by side
note(0.00, 0.35, 36, 110)
note(0.40, 0.75, 40, 100)
note(0.80, 1.15, 43, 90)
note(1.20, 1.55, 47, 100)     # B2: the last key of the bass zone
note(1.60, 1.95, 48, 100)     # C3: the first key of the piano zone
note(2.00, 2.60, 47, 90)      # a chord across the split: B2 ...
note(2.00, 2.60, 48, 90)      # ... and C3 together
note(2.00, 2.60, 55, 80)
# piano zone and the octave doubling above C5
note(2.70, 3.00, 60, 100)
note(2.75, 3.00, 64, 90)
note(2.80, 3.00, 67, 80)
note(3.10, 3.40, 71, 100)     # B4: the last key without the doubling
note(3.50, 3.80, 72, 100)     # C5: the doubling starts
note(3.90, 4.10, 84, 100)
note(4.20, 4.35, 120, 100)    # the doubling would play pitch 132: dropped
# a second keyboard source, and the sustain pedal over a release
note(4.45, 4.70, 52, 100, ch=5)
pedal(4.80, True)
note(4.80, 4.95, 62, 100)
note(4.85, 5.00, 66, 90)
pedal(5.40, False)

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

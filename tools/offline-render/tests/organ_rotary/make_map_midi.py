#!/usr/bin/env python3
"""Writes the CLI-mapping MIDI files of the rotary golden (Story 2.6): organ_map_plain.mid is a 1 s chord on part 0 with no controllers;
organ_map_{slow,fast,stop}.mid are the same chord with CC 79 (speed) and CC 89 (on) at tick 0, before the notes. ctest requires
`--rotary X` on the plain file to render byte-equal to the CC file with `--rotary off`. Run:  python3 make_map_midi.py  (writes next to itself)
"""
import struct
import sys
import os

TPS = 960


def build(cc_speed):
    ev = []
    if cc_speed is not None:
        ev.append((0, 0, bytes([0xB0, 79, cc_speed])))
        ev.append((0, 0, bytes([0xB0, 89, 127])))
    for n in (48, 55, 60, 64):
        ev.append((0, 2, bytes([0x90, n, 100])))
        ev.append((TPS, 1, bytes([0x80, n, 0])))
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
    return out



here = os.path.dirname(os.path.abspath(__file__))
for name, v in (("plain", None), ("slow", 64), ("fast", 127), ("stop", 0)):
    with open(os.path.join(here, "organ_map_%s.mid" % name), "wb") as f:
        f.write(build(v))

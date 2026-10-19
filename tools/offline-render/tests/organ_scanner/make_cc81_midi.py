#!/usr/bin/env python3
"""Writes two tiny SMFs for the CC-81-only check (Story 2.4): organ_cc81.mid is a held chord preceded by CC 81 = 127 (scanner on,
no CC 87, so the registry default type c1 applies); organ_plain.mid is the same chord without the controller.
Run:  python3 make_cc81_midi.py organ_cc81.mid organ_plain.mid"""
import struct
import sys


def smf(with_cc):
    trk = bytearray(b"\x00\xff\x51\x03" + (500000).to_bytes(3, "big"))
    if with_cc:
        trk += b"\x00\xb0\x51\x7f"  # CC 81 = 127 at tick 0
    for i, n in enumerate((52, 55, 60)):
        trk += b"\x00\x90" + bytes([n, 100])
    trk += b"\x83\x60\x80\x34\x00" + b"\x00\x80\x37\x00" + b"\x00\x80\x3c\x00"  # 480 ticks (0.5 s), then note offs
    trk += b"\x00\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(trk)) + bytes(trk)


open(sys.argv[1], "wb").write(smf(True))
open(sys.argv[2], "wb").write(smf(False))

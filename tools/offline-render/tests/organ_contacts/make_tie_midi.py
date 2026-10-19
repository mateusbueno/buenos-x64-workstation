#!/usr/bin/env python3
"""Writes tie.mid (Story 2.5): format 0, 480 ppq, 120 bpm. Note On key 60 on channel 0 at tick 0, Note Off at tick 480 (frame 24000).
With organ_tie.contacts (a Note On of the same key at frame 24000) it pins the --midi/--events tie rule: MIDI first, so the Off
comes before the On and the key sounds on past 0.5 s. Run:  python3 make_tie_midi.py tie.mid"""
import struct, sys
body = bytes([0, 0x90, 60, 100]) + bytes([0x83, 0x60, 0x80, 60, 0]) + bytes([0, 0xFF, 0x2F, 0])
trk = b"MTrk" + struct.pack(">I", len(body)) + body
open(sys.argv[1], "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + trk)

"""Story 5.4: the per-library MIDI test suite, generated in code (nothing binary is committed).

Six probes, every one library-independent (they sweep the whole keyboard, the whole velocity range and
the pedal, so they exercise whatever mapping the instrument has, and sfizz decides what is right):

  range           notes 21..108 in steps of 3 (the playable range of every instrument in the corpus)
  velocity        keys 36 / 60 / 84 at velocities 1..127 (every velocity layer boundary is crossed)
  round_robin     keys 48 / 60 / 72, ten repeats each at one velocity (seq / random groups cycle)
  keyswitches     every key 0..47 pressed briefly, each followed by two probe notes (sw_last / sw_down / sw_previous)
  releases        short and long notes at three velocities, then notes released under the sustain pedal
  sustained_loops chords and single notes held for 8-10 s (loop_continuous / loop_sustain / decay tails)

Every file starts with CC7 = 127 on channel 0 (sfizz's default volume curve is then unity, see
tools/offline-render/README.md).

Time base (Story 5.4 VSCO triage): one tick is exactly one frame at 48 kHz (24000 PPQ, 500000 us per quarter) and every
event lies HALF A FRAME after a frame boundary. The offset is made by a first tick of 1.5 frames (tempo 750000 us at tick 0,
back to 500000 us at tick 1), so event tick k is at k + 0.5 frames. Reason: sfizz_render advances its MIDI clock by adding 1/48000 s
per frame and fires an event on the first frame whose accumulated time is >= the event time. For an event exactly ON a frame
boundary the double rounding of that sum decides whether the event fires on frame N or on frame N-1 (the 1 frame jitter changed
with the event time: 4800 right, 14400 one frame early), and a one frame onset error nulls only to about -12 dB on a bright
transient. Half a frame after the boundary is not a tie for either side: sfizz fires on floor(t * 48000) and the engine
(exact integer arithmetic in smf.cpp) uses floor(t * 48000) too.
"""
import struct

PPQ = 24000
TICKS_PER_SECOND = 48000  # one tick = one frame at 48 kHz
SUITES = ("range", "velocity", "round_robin", "keyswitches", "releases", "sustained_loops")
TAIL_SECONDS = {"range": 2.0, "velocity": 2.0, "round_robin": 1.5, "keyswitches": 2.0, "releases": 3.0, "sustained_loops": 3.0}


def _vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def _ticks(seconds):
    return int(round(seconds * TICKS_PER_SECOND))


class _Track:
    def __init__(self):
        self.events = []  # (tick, order, bytes)

    def cc(self, t, num, val):
        self.events.append((_ticks(t), 1, bytes([0xB0, num, val])))

    def note(self, t, key, vel, dur):
        self.events.append((_ticks(t), 2, bytes([0x90, key, vel])))
        self.events.append((_ticks(t + dur), 0, bytes([0x80, key, 0])))  # an Off before an On on the same tick

    def last_tick(self):
        return max(t for t, _o, _b in self.events)

    def smf(self):
        # tempo 750000 us / quarter for the first tick (1.5 frames), then 500000 us (1 frame per tick): tick k is at k + 0.5 frames
        data = bytearray(b"\x00\xff\x51\x03\x0b\x71\xb0")
        last = 0
        events = [(1, -1, b"\xff\x51\x03\x07\xa1\x20")] + self.events
        for tick, _o, ev in sorted(events, key=lambda e: (e[0], e[1])):
            data += _vlq(tick - last) + ev
            last = tick
        data += b"\x00\xff\x2f\x00"
        return b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ) + b"MTrk" + struct.pack(">I", len(data)) + bytes(data)


def _new():
    t = _Track()
    t.cc(0.0, 7, 127)
    return t


def build(name):
    t = _new()
    _build_into(t, name)
    return t.smf()


def _build_into(t, name):
    if name == "range":
        at = 0.1
        for key in range(21, 109, 3):
            t.note(at, key, 90, 0.35)
            at += 0.5
    elif name == "velocity":
        at = 0.1
        for key in (36, 60, 84):
            for vel in (1, 16, 32, 48, 64, 80, 96, 112, 127):
                t.note(at, key, vel, 0.3)
                at += 0.5
    elif name == "round_robin":
        at = 0.1
        for key in (48, 60, 72):
            for _ in range(10):
                t.note(at, key, 80, 0.2)
                at += 0.4
    elif name == "keyswitches":
        at = 0.1
        for ks in range(0, 48):
            t.note(at, ks, 100, 0.05)
            t.note(at + 0.1, 60, 90, 0.3)
            t.note(at + 0.5, 67, 90, 0.3)
            at += 0.9
    elif name == "releases":
        at = 0.1
        for vel in (30, 90, 127):
            for dur in (0.05, 0.12, 0.3, 1.0, 2.0):
                t.note(at, 60, vel, dur)
                at += dur + 0.7
        t.cc(at, 64, 127)  # sustain pedal down: the keys come up while it is held, then it lifts
        for i, key in enumerate((48, 55, 64, 72)):
            t.note(at + 0.1 + 0.15 * i, key, 80, 0.2)
        t.cc(at + 2.5, 64, 0)
    elif name == "sustained_loops":
        for key in (48, 60, 64, 67):
            t.note(0.1, key, 90, 10.0)
        t.note(11.0, 72, 70, 8.0)
        t.note(20.0, 36, 100, 8.0)
    else:
        raise ValueError("unknown suite %r" % name)


def last_event_seconds(name):
    """Time of the last event of a probe (the engine render's tail is counted from here)."""
    t = _new()
    _build_into(t, name)
    return t.last_tick() / TICKS_PER_SECOND


def uses_pedal(name):
    """True when the probe sends sustain (CC64), sostenuto (CC66) or soft pedal (CC67): the engine then defers some note offs to the pedal."""
    t = _new()
    _build_into(t, name)
    return any(b[0] & 0xF0 == 0xB0 and b[1] in (64, 66, 67) for _t, _o, b in t.events)


def write_all(directory):
    import os
    os.makedirs(directory, exist_ok=True)
    paths = {}
    for n in SUITES:
        p = os.path.join(directory, n + ".mid")
        with open(p, "wb") as fh:
            fh.write(build(n))
        paths[n] = p
    return paths


# ---------------------------------------------------------------------------------------------
# Story 13.11: the same six probes for the FluidSynth oracle (SoundFont libraries)
#
# FluidSynth's file player works in whole milliseconds and applies an event at the start of a 64-frame block, one or two blocks late depending on the
# tick (tools/offline-render/tests/sf2_suite/README.md, "MIDI timing"). The suite of Story 5.3 therefore uses 375 PPQ at 500000 us per quarter (one tick = 64
# frames), puts every event on a multiple of 3 ticks (4 ms = 192 frames) and gives the engine the oracle's MIDI delayed by one tick. This does the same with
# the probes above: every event time is rounded to the nearest multiple of 3 ticks. The preset is selected on channel 0 with MMA bank select (the oracle's
# `synth.midi-bank-select=mma`: bank = CC0 * 128 + CC32), a program change and CC7 = 127 at tick 0.
FLUID_PPQ = 375
FLUID_FRAMES_PER_TICK = 64
FLUID_GRID_TICKS = 3
FLUID_LAG_TICKS = 1  # FluidSynth applies events one block late: the engine's MIDI is the oracle's shifted by this


def _fluid_events(name, bank, program):
    t = _new()
    _build_into(t, name)
    ev = [(0, 0, bytes([0xB0, 0, (bank >> 7) & 127])), (0, 0, bytes([0xB0, 32, bank & 127])), (0, 0, bytes([0xC0, program & 127])), (0, 0, bytes([0xB0, 7, 127]))]
    for frames, order, data in t.events:
        if frames == 0 and data[0] == 0xB0 and data[1] == 7:
            continue  # replaced by the setup above
        tick = int(frames / (FLUID_FRAMES_PER_TICK * FLUID_GRID_TICKS) + 0.5) * FLUID_GRID_TICKS
        ev.append((max(tick, FLUID_GRID_TICKS), order, data))
    return ev


def build_fluid(name, bank, program, engine=False):
    """SMF of probe `name` for preset bank:program on the FluidSynth grid; `engine=True` delays every event after tick 0 by one tick."""
    shift = FLUID_LAG_TICKS if engine else 0
    data = bytearray(b"\x00\xff\x51\x03\x07\xa1\x20")
    last = 0
    for tick, _o, ev in sorted(_fluid_events(name, bank, program), key=lambda e: (e[0], e[1])):
        tk = tick + (shift if tick > 0 else 0)
        data += _vlq(tk - last) + ev
        last = tk
    data += b"\x00\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, FLUID_PPQ) + b"MTrk" + struct.pack(">I", len(data)) + bytes(data)


def last_event_seconds_fluid(name, bank=0, program=0, engine=False):
    """Time of the last event of the oracle's MIDI (or of the engine's, one block later)."""
    last = max(tick for tick, _o, _b in _fluid_events(name, bank, program))
    return (last + (FLUID_LAG_TICKS if engine else 0)) * FLUID_FRAMES_PER_TICK / 48000.0


def write_fluid(directory, bank, program):
    """Writes the six probes for bank:program; returns {suite: (oracle midi path, engine midi path)}."""
    import os
    os.makedirs(directory, exist_ok=True)
    out = {}
    for n in SUITES:
        pair = []
        for eng in (False, True):
            p = os.path.join(directory, "%s_%d_%d_%s.mid" % (n, bank, program, "engine" if eng else "oracle"))
            with open(p, "wb") as fh:
                fh.write(build_fluid(n, bank, program, eng))
            pair.append(p)
        out[n] = tuple(pair)
    return out

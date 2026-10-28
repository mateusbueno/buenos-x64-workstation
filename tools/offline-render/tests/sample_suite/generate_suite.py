#!/usr/bin/env python3
"""Generates the synthetic sample-engine test suite: tiny 16-bit WAVs, SFZ files and MIDI files.

Run once, commit the output (the files are deterministic: integer math plus a fixed LCG; the
sines come from math.sin, so regenerating on another libm could change a sample by one LSB --
which is why the outputs are committed and the goldens are rendered from the committed files).

    python3 generate_suite.py            # (re)writes samples/, sfz/, midi/ next to this script
    python3 generate_suite.py story134   # only the Story 13.4 cases (no existing file is touched)
    python3 generate_suite.py story135   # only the Story 13.5 cases (no existing file is touched)
    python3 generate_suite.py story136   # only the Story 13.6 cases (no existing file is touched)
    python3 generate_suite.py story137   # only the Story 13.7 cases (no existing file is touched)
    python3 generate_suite.py story138   # only the Story 13.8 cases (no existing file is touched)

MIDI timing: the files use 32000 PPQ at 500000 us per quarter, i.e. 0.75 audio frames per tick at
48 kHz. A frame is mapped to a tick that puts the event at a quarter-frame fraction (never on an
exact frame boundary), so the renderer under test and sfizz_render agree on the frame whatever
their floating point rounding does.
"""
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 48000


# --- samples ---------------------------------------------------------------------------------------
def lcg(seed):
    s = seed
    while True:
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        yield (s / 0x3FFFFFFF) - 1.0


def partials(freq, rate, n, harmonics, falloff, seed, noise=0.01):
    noise_gen = lcg(seed)
    out = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for h, w in harmonics:
            v += w * math.sin(2 * math.pi * freq * h * t + 0.3 * h) / (h ** falloff)
        out.append(v + noise * next(noise_gen))
    peak = max(abs(x) for x in out) or 1.0
    return [x / peak for x in out]


def shape(x, fade_in, fade_out, gain):
    n = len(x)
    out = []
    for i, v in enumerate(x):
        g = gain
        if fade_in and i < fade_in:
            g *= i / fade_in
        if fade_out and i >= n - fade_out:
            g *= (n - 1 - i) / fade_out
        out.append(v * g)
    return out


def write_wav(path, channels, rate, chans):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    n = len(chans[0])
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            for c in range(channels):
                v = int(round(max(-1.0, min(1.0, chans[c][i])) * 32767))
                frames += struct.pack("<h", v)
        w.writeframes(bytes(frames))


def make_samples(root):
    saw = [(h, 1.0) for h in range(1, 13)]
    odd = [(h, 1.0) for h in range(1, 14, 2)]
    tri = [(h, 1.0) for h in range(1, 10, 2)]
    d = os.path.join(root, "sfz", "samples")
    n = 12000
    write_wav(f"{d}/a.wav", 1, RATE, [shape(partials(261.63, RATE, n, saw, 1.0, 1), 30, 400, 0.6)])
    write_wav(f"{d}/b.wav", 1, RATE, [shape(partials(329.63, RATE, n, odd, 1.0, 2), 30, 400, 0.6)])
    write_wav(f"{d}/c.wav", 1, RATE, [shape(partials(392.0, RATE, n, tri, 2.0, 3), 30, 400, 0.6)])
    write_wav(f"{d}/d.wav", 1, RATE, [shape(partials(523.25, RATE, n, saw, 1.2, 4), 30, 400, 0.6)])
    write_wav(f"{d}/rate44.wav", 1, 44100, [shape(partials(261.63, 44100, 11025, saw, 1.0, 5), 30, 400, 0.6)])
    left = shape(partials(261.63, RATE, n, saw, 1.0, 6), 30, 400, 0.6)
    right = shape(partials(329.63, RATE, n, odd, 1.0, 7), 30, 400, 0.5)
    write_wav(f"{d}/st.wav", 2, RATE, [left, right])
    write_wav(f"{d}/loop.wav", 1, RATE, [shape(partials(220.0, RATE, 14400, saw, 1.0, 8), 30, 0, 0.6)])
    write_wav(f"{d}/trunc.wav", 1, RATE, [shape(partials(293.66, RATE, 6000, odd, 1.0, 9), 30, 0, 0.6)])
    write_wav(f"{d}/short.wav", 1, RATE, [shape(partials(392.0, RATE, 2400, saw, 1.0, 10), 10, 100, 0.6)])
    # Story 5.2: a second loopable sample (the crossfade fixtures fade between loop.wav and loop2.wav) and a bright, noisy one for the filters.
    write_wav(f"{d}/loop2.wav", 1, RATE, [shape(partials(330.0, RATE, 14400, odd, 1.0, 11), 30, 0, 0.6)])
    write_wav(f"{d}/bright.wav", 1, RATE, [shape(partials(110.0, RATE, 14400, [(h, 1.0) for h in range(1, 41)], 0.8, 12, noise=0.05), 30, 0, 0.5)])


# --- midi ------------------------------------------------------------------------------------------
START_OFFSET = 4800  # frames
PPQ = 32000
PITCH_BEND = 256  # pseudo controller number of a pitch wheel event in the `ccs` lists (Story 13.7)
TEMPO = 500000  # us per quarter: 1 tick = 0.75 frames at 48 kHz


def frame_to_tick(n):
    """A tick t with floor(0.75 * t) == n and t % 4 != 0 (so 0.75 * t is never an integer)."""
    k, r = divmod(n, 3)
    t = 4 * k + 1 + r
    assert (3 * t) // 4 == n and t % 4 != 0
    return t


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def write_midi(path, events, ccs=(), setup=()):
    """events: list of (frame, on, key, vel) sorted by frame; note offs sort before ons at a frame.
    ccs: (frame, controller, value) control changes (Story 5.2), at the same time base as the notes; at an equal frame a control
    change comes before the notes. setup: (controller, value) pairs sent at tick 0 after CC7 (initial controller positions)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    # Everything starts START_OFFSET frames in, so sfizz's CC smoothing has settled before the first note.
    evs = [(f + START_OFFSET, 1 if on else 0, k, v) for f, on, k, v in events]
    evs += [(f + START_OFFSET, -1, cc, val) for f, cc, val in ccs]
    evs.sort(key=lambda e: (e[0], e[1]))
    track = bytearray()
    track += vlq(0) + b"\xff\x51\x03" + TEMPO.to_bytes(3, "big")
    # sfizz plays a default MIDI volume of CC7 = 100 (gain 0.62); CC7 = 127 at the start makes it unity.
    track += vlq(0) + bytes([0xB0, 7, 127])
    for cc, val in setup:
        track += vlq(0) + bytes([0xB0, cc, val])
    last = 0
    for frame, on, key, vel in evs:
        tick = frame_to_tick(frame)
        assert tick >= last, (frame, tick, last)
        track += vlq(tick - last)
        if on < 0 and key == PITCH_BEND:  # Story 13.7: `vel` is the 14-bit wheel value (8192 = centre)
            track += bytes([0xE0, vel & 0x7F, vel >> 7])
        elif on < 0:
            track += bytes([0xB0, key, vel])
        else:
            track += bytes([0x90 if on else 0x80, key, vel if on else 0])
        last = tick
    track += vlq(0) + b"\xff\x2f\x00"
    data = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ) + b"MTrk" + struct.pack(">I", len(track)) + bytes(track)
    with open(path, "wb") as f:
        f.write(data)


def notes(spec):
    """spec: list of (on_frame, off_frame, key, vel)."""
    ev = []
    for on, off, key, vel in spec:
        ev.append((on, 1, key, vel))
        ev.append((off, 0, key, vel))
    return ev


def secs(s):
    return int(round(s * RATE))


def blk(frame):
    """Round to a multiple of the 64-frame block (used for loop_sustain note offs)."""
    return (frame // 64) * 64


# --- cases -----------------------------------------------------------------------------------------
HEADER = "<control>\ndefault_path=samples/\n"

CASES = {}   # name -> (sfz text, events, ccs, setup)


def case(name, sfz, events, ccs=(), setup=()):
    CASES[name] = (sfz, events, ccs, setup)


def build_cases():
    # 1. velocity layers: boundaries 40|41 and 80|81, extremes, and two off-center keys.
    case("velocity_layers", HEADER + """<global> ampeg_release=0.05
<group> lokey=48 hikey=72 pitch_keycenter=60
<region> sample=a.wav lovel=1 hivel=40
<region> sample=b.wav lovel=41 hivel=80
<region> sample=c.wav lovel=81 hivel=127
""", notes([(secs(0.00) + i * secs(0.08), secs(0.00) + i * secs(0.08) + secs(0.05), key, vel)
            for i, (key, vel) in enumerate([(60, 1), (60, 40), (60, 41), (60, 80), (60, 81),
                                            (60, 127), (60, 64), (55, 100), (67, 20)])]))

    # 2. round robin: four positions on one key, and an independent two-position cycle on another.
    case("round_robin", HEADER + """<global> ampeg_release=0.03
<group> lokey=60 hikey=60 pitch_keycenter=60 seq_length=4
<region> sample=a.wav seq_position=1
<region> sample=b.wav seq_position=2
<region> sample=c.wav seq_position=3
<region> sample=d.wav seq_position=4
<group> lokey=64 hikey=64 pitch_keycenter=64 seq_length=2
<region> sample=a.wav seq_position=1
<region> sample=d.wav seq_position=2
""", notes([(i * secs(0.07), i * secs(0.07) + secs(0.04), key, 100)
            for i, key in enumerate([60, 64, 60, 60, 64, 60, 60, 64, 60, 64])]))

    # 3. random layers: the two regions of each pair carry the same audio, so the render does not
    #    depend on which random value either engine draws (engine-side distribution is a unit test).
    case("random_layers", HEADER + """<global> ampeg_release=0.04
<group> lokey=60 hikey=60 pitch_keycenter=60
<region> sample=a.wav lorand=0 hirand=0.5
<region> sample=a.wav lorand=0.5 hirand=1
<group> lokey=64 hikey=64 pitch_keycenter=64
<region> sample=b.wav lorand=0 hirand=0.3
<region> sample=b.wav lorand=0.3 hirand=0.7
<region> sample=b.wav lorand=0.7 hirand=1
""", notes([(i * secs(0.06), i * secs(0.06) + secs(0.04), key, 90)
            for i, key in enumerate([60, 64, 60, 64, 60, 60, 64, 64])]))

    # 4. pitched notes far from the key centre, tune + transpose, and a 44.1 kHz sample.
    pitch_sfz = HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=0 hikey=96 pitch_keycenter=60
<region> sample=a.wav lokey=100 hikey=100 pitch_keycenter=60 tune=37 transpose=-5
<region> sample=rate44.wav lokey=97 hikey=99 pitch_keycenter=60
"""
    case("pitch_far", pitch_sfz, notes([(i * secs(0.12), i * secs(0.12) + secs(0.1), key, 100)
                                        for i, key in enumerate([12, 36, 60, 72, 84, 96, 100, 98, 97])]))
    case("pitch_far_linear", pitch_sfz, notes([(i * secs(0.1), i * secs(0.1) + secs(0.08), key, 100)
                                               for i, key in enumerate([36, 84, 100, 98])]))

    # 5. volume and pan, mono and stereo samples.
    case("volume_pan", HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 volume=-6 pan=-50
<region> sample=st.wav lokey=62 hikey=62 pitch_keycenter=62 pan=100
<region> sample=st.wav lokey=64 hikey=64 pitch_keycenter=64 pan=-100 volume=3.5
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65 pan=100
<region> sample=st.wav lokey=67 hikey=67 pitch_keycenter=67 pan=25 volume=-20
<region> sample=a.wav lokey=69 hikey=69 pitch_keycenter=69
<region> sample=st.wav lokey=71 hikey=71 pitch_keycenter=71
""", notes([(i * secs(0.09), i * secs(0.09) + secs(0.06), key, 110)
            for i, key in enumerate([60, 62, 64, 65, 67, 69, 71])]))

    # 6. offset: into the sample, pitched, and close to its end.
    case("offset", HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 offset=2400
<region> sample=a.wav lokey=62 hikey=62 pitch_keycenter=60 offset=6000
<region> sample=a.wav lokey=64 hikey=64 pitch_keycenter=64 offset=11000
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65
""", notes([(i * secs(0.1), i * secs(0.1) + secs(0.07), key, 100)
            for i, key in enumerate([60, 62, 64, 65])]))

    # 7. amplitude envelope stages on a looped sample (so it sustains while the key is held).
    env = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"
    case("envelope", HEADER + f"""<global> sample=loop.wav {env}
<region> lokey=60 hikey=60 pitch_keycenter=60 ampeg_attack=0.03 ampeg_hold=0.02 ampeg_decay=0.06 ampeg_sustain=40 ampeg_release=0.1
<region> lokey=62 hikey=62 pitch_keycenter=62 ampeg_delay=0.03 ampeg_attack=0.02 ampeg_release=0.04
<region> lokey=64 hikey=64 pitch_keycenter=64 ampeg_start=20 ampeg_attack=0.02 ampeg_decay=0.05 ampeg_sustain=60 ampeg_release=0.2
<region> lokey=65 hikey=65 pitch_keycenter=65 ampeg_release=0.3
<region> lokey=67 hikey=67 pitch_keycenter=67 ampeg_decay=0.04 ampeg_sustain=10 ampeg_release=0.005
<region> lokey=69 hikey=69 pitch_keycenter=69 ampeg_delay=0.1 ampeg_attack=0.01 ampeg_release=0.05
""", notes([(secs(0.00), secs(0.20), 60, 100), (secs(0.05) + 13, secs(0.22), 62, 100),  # +13: (start offset + ampeg_delay) must not end on a 64-frame
            # block boundary, or sfizz 1.2.3 loses the note off (its ADSR leaves `delay` at 0 instead of -1)
            (secs(0.10), secs(0.30), 64, 100), (secs(0.15), secs(0.25), 65, 100),
            (secs(0.30), secs(0.38), 65, 100),  # retrigger while the first one still releases
            (secs(0.20), secs(0.35), 67, 100),
            (secs(0.25), blk(secs(0.30)), 69, 100)]))  # released (block aligned) during its 0.1 s delay: never sounds

    # 8. loop modes.
    loop = "loop_start=2400 loop_end=7199"
    case("loops", HEADER + f"""<global> ampeg_release=0.05
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 loop_mode=loop_continuous {loop}
<region> sample=loop.wav lokey=62 hikey=62 pitch_keycenter=62 loop_mode=loop_sustain {loop}
<region> sample=loop.wav lokey=64 hikey=64 pitch_keycenter=64 loop_mode=one_shot
<region> sample=loop.wav lokey=65 hikey=65 pitch_keycenter=65 loop_mode=no_loop
<region> sample=loop.wav lokey=67 hikey=67 pitch_keycenter=60 loop_mode=loop_continuous {loop}
<region> sample=trunc.wav lokey=69 hikey=69 pitch_keycenter=69
<region> sample=loop.wav lokey=71 hikey=71 pitch_keycenter=71 loop_mode=loop_continuous
""",
         notes([(secs(0.00), secs(0.35), 60, 100),
                (secs(0.02), blk(secs(0.20)), 62, 100),
                (secs(0.04), secs(0.09), 64, 100),
                (secs(0.06), secs(0.11), 65, 100),
                (secs(0.08), secs(0.33), 67, 100),
                (secs(0.10), secs(0.40), 69, 100),
                (secs(0.12), secs(0.37), 71, 100)]))

    # 9. release triggers: fired by note off, only while a compatible attack voice is alive.
    case("release_trigger", HEADER + """<group> lokey=60 hikey=60 pitch_keycenter=60 ampeg_release=0.08
<region> sample=a.wav
<group> trigger=release lokey=60 hikey=60 pitch_keycenter=60 volume=-6 ampeg_release=0.05
<region> sample=b.wav
<group> lokey=62 hikey=62 pitch_keycenter=62 ampeg_release=0.04
<region> sample=short.wav
<group> trigger=release lokey=62 hikey=62 pitch_keycenter=62
<region> sample=c.wav
<group> lokey=64 hikey=64 pitch_keycenter=64 ampeg_release=0.05
<region> sample=a.wav lovel=1 hivel=60
<region> sample=d.wav lovel=61 hivel=127
<group> trigger=release lokey=64 hikey=64 pitch_keycenter=64 seq_length=2
<region> sample=b.wav seq_position=1
<region> sample=c.wav seq_position=2
""", notes([(secs(0.00), secs(0.10), 60, 100),
            (secs(0.12), secs(0.19), 62, 100),    # short sample still fading: release fires
            (secs(0.30), secs(0.50), 62, 100),    # attack voice long gone: release does not fire
            (secs(0.52), secs(0.60), 64, 40),
            (secs(0.64), secs(0.72), 64, 100)]))

    # 10. layering, chords, retrigger, and the default (unspecified) release time.
    case("poly_layers", HEADER + """<region> sample=a.wav lokey=48 hikey=84 pitch_keycenter=60
<region> sample=st.wav lokey=48 hikey=84 pitch_keycenter=60 volume=-9 pan=30
""", notes([(secs(0.00), secs(0.12), 60, 100), (secs(0.00), secs(0.12), 64, 90),
            (secs(0.00), secs(0.12), 67, 80), (secs(0.00), secs(0.12), 72, 70),
            (secs(0.15), secs(0.25), 60, 100), (secs(0.18), secs(0.28), 60, 60),
            (secs(0.21), secs(0.30), 48, 127)]))


def sweep(cc, start, stop, first, step_frames):
    """A controller ramp in whole CC steps: one control change per step_frames, from `start` to `stop` inclusive."""
    n = abs(stop - start)
    sgn = 1 if stop >= start else -1
    return [(first + i * step_frames, cc, start + sgn * i) for i in range(n + 1)]


def build_story52_cases():
    """Story 5.2: one fixture instrument per feature (keyswitches, CC crossfades, filters)."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"

    # Keyswitches 1: sw_last / sw_default / sw_lokey..sw_hikey. Keys 36..38 switch silently, key 39 is a keyswitch the SFZ maps to a
    # region of its own (it sounds); the default switch is 36. The keyswitch presses are short notes (the key goes down and up).
    case("keyswitch_last", HEADER + """<global> sw_lokey=36 sw_hikey=39 sw_default=36 ampeg_release=0.05
<group> sw_last=36
<region> sample=a.wav lokey=48 hikey=84 pitch_keycenter=60
<group> sw_last=37
<region> sample=b.wav lokey=48 hikey=84 pitch_keycenter=60
<group> sw_last=38
<region> sample=c.wav lokey=48 hikey=84 pitch_keycenter=60
<group> sw_last=39
<region> sample=d.wav lokey=48 hikey=84 pitch_keycenter=60
<region> sample=short.wav lokey=39 hikey=39 pitch_keycenter=60
""", notes([(secs(0.00), secs(0.08), 60, 100), (secs(0.10), secs(0.18), 64, 90),     # sw_default = 36
            (secs(0.20), secs(0.23), 37, 100),                                         # switch to 37: no sound
            (secs(0.26), secs(0.34), 60, 100), (secs(0.36), secs(0.44), 67, 80),
            (secs(0.46), secs(0.49), 38, 100),                                         # 38
            (secs(0.52), secs(0.60), 60, 100), (secs(0.62), secs(0.70), 55, 110),
            (secs(0.72), secs(0.80), 39, 100),                                         # 39 sounds (mapped) and switches
            (secs(0.84), secs(0.92), 60, 100),
            (secs(0.96), secs(0.99), 36, 100),                                         # back to 36
            (secs(1.02), secs(1.10), 60, 100), (secs(1.12), secs(1.20), 72, 100)]))

    # Keyswitches 2: sw_down / sw_up follow whether key 36 is held.
    case("keyswitch_down_up", HEADER + """<global> ampeg_release=0.05
<group> sw_up=36
<region> sample=a.wav lokey=48 hikey=84 pitch_keycenter=60
<group> sw_down=36
<region> sample=b.wav lokey=48 hikey=84 pitch_keycenter=60
""", notes([(secs(0.00), secs(0.08), 60, 100), (secs(0.10), secs(0.18), 64, 90),     # 36 up: the sw_up region
            (secs(0.22), secs(0.60), 36, 100),                                         # 36 held ...
            (secs(0.26), secs(0.34), 60, 100), (secs(0.38), secs(0.46), 67, 100),    # ... the sw_down region
            (secs(0.50), secs(0.56), 62, 100),
            (secs(0.66), secs(0.74), 60, 100), (secs(0.78), secs(0.86), 64, 100),    # released: sw_up again
            (secs(0.90), secs(1.00), 36, 100), (secs(1.04), secs(1.12), 60, 100),    # held again
            (secs(1.16), secs(1.24), 60, 100)]))

    # Keyswitches 3: sw_previous. The region on key 62 sounds (besides the plain one) only when the note on before it was key 60.
    case("keyswitch_previous", HEADER + """<global> ampeg_release=0.05
<group>
<region> sample=a.wav lokey=60 hikey=72 pitch_keycenter=60
<group> sw_previous=60
<region> sample=c.wav lokey=62 hikey=62 pitch_keycenter=62
""", notes([(secs(0.00), secs(0.06), 62, 100),                                          # nothing before: plain only
            (secs(0.10), secs(0.16), 60, 100), (secs(0.20), secs(0.28), 62, 100),    # after 60: both
            (secs(0.32), secs(0.38), 64, 100), (secs(0.42), secs(0.50), 62, 100),    # after 64: plain only
            (secs(0.54), secs(0.58), 60, 100), (secs(0.62), secs(0.70), 62, 100),    # after 60: both
            (secs(0.74), secs(0.80), 62, 100)]))                                        # after 62: plain only

    # CC crossfades. Key 60 fades between two looped samples on the mod wheel (CC1), key 64 on expression (CC11), key 67 multiplies an
    # xfin on CC1 with an xfout on CC11 (two controllers per region). The controllers sweep in whole CC steps while the notes sustain,
    # so the gains of sounding voices move; key 72 steps the wheel by large jumps (the smoothing).
    def xfade_case(name, curve):
        sfz = HEADER + f"""<global> {loop} ampeg_release=0.05 xf_cccurve={curve}
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 xfout_locc1=0 xfout_hicc1=100
<region> sample=loop2.wav lokey=60 hikey=60 pitch_keycenter=60 xfin_locc1=30 xfin_hicc1=127
<region> sample=loop.wav lokey=64 hikey=64 pitch_keycenter=64 xfin_locc11=0 xfin_hicc11=90
<region> sample=loop2.wav lokey=64 hikey=64 pitch_keycenter=64 xfout_locc11=60 xfout_hicc11=126
<region> sample=loop.wav lokey=67 hikey=67 pitch_keycenter=67 xfin_locc1=20 xfin_hicc1=60 xfout_locc11=70 xfout_hicc11=120
<region> sample=loop2.wav lokey=72 hikey=72 pitch_keycenter=72 xfin_locc1=40 xfin_hicc1=80
"""
        ev = notes([(secs(0.00), secs(3.60), 60, 100), (secs(0.00), secs(3.60), 64, 100),
                    (secs(0.00), secs(3.60), 67, 100), (secs(0.00), secs(3.60), 72, 100)])
        step = 576  # 12 ms per CC step: a full 0..127 sweep takes 1.5 s
        ccs = (sweep(1, 0, 127, secs(0.10), step) + sweep(1, 126, 0, secs(0.10) + 128 * step, step)
               + sweep(11, 127, 0, secs(0.10), step) + sweep(11, 1, 127, secs(0.10) + 128 * step, step))
        # large jumps on the wheel at the end (key 72 follows them through the smoother)
        ccs += [(secs(3.0), 1, 100), (secs(3.12), 1, 20), (secs(3.24), 1, 127), (secs(3.36), 1, 0)]
        return sfz, ev, sorted(ccs), ((1, 0), (11, 127))

    for name, curve in (("xfade_power", "power"), ("xfade_gain", "gain")):
        sfz, ev, ccs, setup = xfade_case(name, curve)
        case(name, sfz, ev, ccs, setup)

    # Filters: every type of the subset, resonance, a stereo sample, the default type (lpf_2p when only `cutoff` is given), a high and a
    # low cutoff. One bright looped sample (110 Hz, 40 harmonics) so that every filter has something to remove.
    case("filters", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 fil_type=lpf_1p cutoff=800
<region> lokey=62 hikey=62 pitch_keycenter=62 fil_type=lpf_2p cutoff=1500 resonance=6
<region> lokey=64 hikey=64 pitch_keycenter=64 fil_type=hpf_1p cutoff=2000
<region> lokey=65 hikey=65 pitch_keycenter=65 fil_type=hpf_2p cutoff=1200 resonance=3
<region> lokey=67 hikey=67 pitch_keycenter=67 fil_type=bpf_2p cutoff=1000 resonance=2
<region> lokey=69 hikey=69 pitch_keycenter=69 sample=st.wav loop_mode=no_loop fil_type=lpf_2p cutoff=3000 resonance=12
<region> lokey=71 hikey=71 pitch_keycenter=71 cutoff=500
<region> lokey=72 hikey=72 pitch_keycenter=72 fil_type=lpf_2p cutoff=15000
<region> lokey=74 hikey=74 pitch_keycenter=74 fil_type=lpf_2p cutoff=300 resonance=20
<region> lokey=76 hikey=76 pitch_keycenter=76 fil_type=hpf_2p cutoff=18000
<region> lokey=77 hikey=77 pitch_keycenter=77
""", notes([(i * secs(0.2), i * secs(0.2) + secs(0.13), key, 100)
            for i, key in enumerate([60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77])]))


def build_story134_cases():
    """Story 13.4: one fixture instrument per opcode family of the velocity and amplitude model. The velocity sweeps use whole MIDI velocities, so
    the engine and sfizz evaluate the same 7-bit value."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"

    def sweep_notes(keys, vels, step=0.09, length=0.06):
        return notes([(i * secs(step), i * secs(step) + secs(length), key, vel)
                      for i, (key, vel) in enumerate((k, v) for k in keys for v in vels)])

    # amp_veltrack: 0 (no velocity law), a partial value, negative values (louder at low velocity), the explicit default and the plain default.
    case("amp_veltrack", HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 amp_veltrack=0
<region> sample=a.wav lokey=62 hikey=62 pitch_keycenter=62 amp_veltrack=50
<region> sample=a.wav lokey=64 hikey=64 pitch_keycenter=64 amp_veltrack=-100
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65 amp_veltrack=-50
<region> sample=a.wav lokey=67 hikey=67 pitch_keycenter=67 amp_veltrack=100
<region> sample=a.wav lokey=69 hikey=69 pitch_keycenter=69
""", sweep_notes([60, 62, 64, 65, 67, 69], [20, 64, 127], step=0.07, length=0.05))

    # amp_velcurve_N: sparse points (gaps filled linearly), only a middle point, a flat curve with a partial veltrack, an inverted curve, and a
    # curve combined with a negative veltrack. Velocity steps hit the points exactly and fall between them.
    case("amp_velcurve", HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 amp_velcurve_1=0.1 amp_velcurve_64=0.8 amp_velcurve_127=1
<region> sample=a.wav lokey=62 hikey=62 pitch_keycenter=62 amp_velcurve_30=0.5
<region> sample=a.wav lokey=64 hikey=64 pitch_keycenter=64 amp_velcurve_1=0.5 amp_velcurve_127=0.5 amp_veltrack=50
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65 amp_velcurve_0=1 amp_velcurve_127=0
<region> sample=a.wav lokey=67 hikey=67 pitch_keycenter=67 amp_velcurve_64=1 amp_veltrack=-100
<region> sample=a.wav lokey=69 hikey=69 pitch_keycenter=69 amp_velcurve_20=0.9 amp_velcurve_21=0.1 amp_velcurve_100=0.2
""", sweep_notes([60, 62, 64, 65, 67, 69], [1, 20, 30, 45, 64, 90, 127], step=0.07, length=0.05))

    # amplitude, group_volume, master_volume (they add to `volume`; amplitude multiplies), and the *_amplitude forms.
    case("amp_gain", HEADER + """<global> ampeg_release=0.05
<master> master_volume=-4
<group> group_volume=3
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60
<region> sample=a.wav lokey=62 hikey=62 pitch_keycenter=62 volume=-2 amplitude=150
<group> group_volume=-6 group_amplitude=50
<region> sample=a.wav lokey=64 hikey=64 pitch_keycenter=64 amplitude=80
<master> master_amplitude=25
<group>
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65
<region> sample=a.wav lokey=67 hikey=67 pitch_keycenter=67 amplitude=0
<master>
<group>
<region> sample=a.wav lokey=69 hikey=69 pitch_keycenter=69 amplitude=50
<region> sample=a.wav lokey=71 hikey=71 pitch_keycenter=71
""", sweep_notes([60, 62, 64, 65, 67, 69, 71], [100], step=0.09, length=0.06))

    # ampeg_vel2*: each scalar on a looped sample that sustains while the key is held; the last region sets all four with ampeg_dynamic=1, which
    # must not change anything (the trigger velocity is constant).
    case("ampeg_vel2", HEADER + f"""<global> sample=loop.wav {loop} pitch_keycenter=60
<region> key=60 ampeg_attack=0.01 ampeg_vel2attack=0.08 ampeg_release=0.05
<region> key=62 ampeg_decay=0.02 ampeg_sustain=70 ampeg_vel2decay=0.1 ampeg_release=0.05
<region> key=64 ampeg_decay=0.04 ampeg_sustain=100 ampeg_vel2sustain=-70 ampeg_release=0.05
<region> key=65 ampeg_release=0.02 ampeg_vel2release=0.25
<region> key=67 ampeg_attack=0.005 ampeg_vel2attack=0.04 ampeg_decay=0.02 ampeg_vel2decay=0.05 ampeg_sustain=50 ampeg_vel2sustain=-30 ampeg_release=0.03 ampeg_vel2release=0.1 ampeg_dynamic=1
""", notes([(i * secs(0.5) + j * secs(0.22) + secs(0.0), i * secs(0.5) + j * secs(0.22) + secs(0.16), key, vel)
            for j, key in enumerate([60, 62, 64, 65, 67])
            for i, vel in enumerate([25, 127])]))

    # amp_keytrack / amp_keycenter: dB per key around the centre (default centre 60 when only the track is given).
    case("amp_keytrack", HEADER + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=48 hikey=71 pitch_keycenter=60 amp_keytrack=0.5 amp_keycenter=60
<region> sample=b.wav lokey=72 hikey=96 pitch_keycenter=84 amp_keytrack=-0.25 amp_keycenter=84
<region> sample=c.wav lokey=24 hikey=47 pitch_keycenter=36 amp_keytrack=0.3
<region> sample=d.wav lokey=98 hikey=100 pitch_keycenter=99 amp_keytrack=12 amp_keycenter=99
""", sweep_notes([48, 54, 60, 66, 71, 72, 84, 96, 24, 36, 47, 98, 99, 100], [100], step=0.08, length=0.05))


def build_story135_cases():
    """Story 13.5: one fixture instrument per CC family. Every controller is moved BEFORE the note it is meant for (sfizz modulates continuously, the
    engine reads the controller when the note starts, so no controller moves while a note sounds: that divergence is a documented limit, not tested).
    The pseudo-CC fixtures use 131 (velocity), 137 (alternate), 140 and 141 (key delta); 135 and 136 are random draws that no two renderers share, so
    they are covered by the engine unit tests only."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"
    def staged(steps, gap=0.15, length=0.10):
        """steps: (cc or None, value or None, key, vel): the controller is set 40 ms before its note. A controller that a sounding voice reads must not move while
        that voice sounds (release tail included): sfizz would follow it, the engine reads it once; `gap` is chosen so that the previous voice has died."""
        ev, ccs = [], []
        for i, (cc, val, key, vel) in enumerate(steps):
            t = 0.05 + i * gap
            if cc is not None:
                ccs.append((secs(t - 0.04), cc, val))
            ev.append((secs(t), 1, key, vel))
            ev.append((secs(t + length), 0, key, vel))
        ev.sort(key=lambda e: (e[0], e[1]))
        return ev, sorted(ccs)

    # locc / hicc gates. Key 60: two layers split at CC20 63|64; key 62: CC20 30..100 and the high-resolution gate lohdcc22=0.25 hihdcc22=0.75 (CC units 31.75 and
    # 95.25: 31 fails, 32 passes, 95 passes, 96 fails); key 64: set_cc21=80 seeds the initial value, locc21=70; key 65: hicc23=10 (the controller starts at 0).
    steps = []
    for v in (0, 63, 64, 100, 101, 127, 30, 29, 10):
        steps += [(20, v, 60, 100), (None, None, 62, 100)]
    steps += [(20, 50, 62, 100)]
    for v in (31, 32, 95, 96, 127, 0):
        steps += [(22, v, 62, 100)]
    steps += [(None, None, 64, 100), (21, 69, 64, 100), (21, 70, 64, 100), (21, 127, 64, 100), (21, 0, 64, 100)]
    steps += [(None, None, 65, 100), (23, 10, 65, 100), (23, 11, 65, 100), (23, 127, 65, 100), (23, 0, 65, 100)]
    ev, ccs = staged(steps, 0.10, 0.06)
    case("cc_gates", "<control>\ndefault_path=samples/\nset_cc21=80\n" + """<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 locc20=0 hicc20=63
<region> sample=b.wav lokey=60 hikey=60 pitch_keycenter=60 locc20=64 hicc20=127
<region> sample=c.wav lokey=62 hikey=62 pitch_keycenter=62 locc20=30 hicc20=100 lohdcc22=0.25 hihdcc22=0.75
<region> sample=d.wav lokey=64 hikey=64 pitch_keycenter=64 locc21=70
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65 hicc23=10
""", ev, ccs)

    # *_oncc on amplitude, volume, pan, tune and offset, through the default curve, a <curve> block and a predefined curve; the SFZ 1 `_cc` spelling and `gain`.
    table = [(60, 20), (62, 20), (64, 21), (65, 22), (67, 22), (69, 23), (71, 24), (72, 25), (74, 26), (76, 27)]
    steps = []
    for key, cc in table:
        for v in (0, 40, 64, 127):
            steps.append((cc, v, key, 100))
    ev, ccs = staged(steps, 0.20, 0.06)   # the release (0.02 s + the 50 ms fade) is over before the next controller move, 40 ms before its note
    case("cc_modulation", HEADER + """<global> ampeg_release=0.02
<curve> curve_index=7 v000=0 v064=0.9 v127=1
<curve> curve_index=8 v000=1 v127=0
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 amplitude_oncc20=100
<region> sample=b.wav lokey=62 hikey=62 pitch_keycenter=62 amplitude_oncc20=80 amplitude_curvecc20=7
<region> sample=c.wav lokey=64 hikey=64 pitch_keycenter=64 volume_oncc21=-12
<region> sample=a.wav lokey=65 hikey=65 pitch_keycenter=65 pan=-20 pan_oncc22=100
<region> sample=st.wav lokey=67 hikey=67 pitch_keycenter=67 pan_oncc22=-60 pan_curvecc22=8
<region> sample=b.wav lokey=69 hikey=69 pitch_keycenter=69 tune_oncc23=700
<region> sample=c.wav lokey=71 hikey=71 pitch_keycenter=71 tune=10 tune_cc24=-300 tune_curvecc24=4
<region> sample=d.wav lokey=72 hikey=72 pitch_keycenter=72 amplitude_cc25=100 amplitude_curvecc25=5
<region> sample=a.wav lokey=74 hikey=74 pitch_keycenter=74 offset_oncc26=5000
<region> sample=b.wav lokey=76 hikey=76 pitch_keycenter=76 gain_cc27=-20 gain_curvecc27=6
""", ev, ccs)

    # The amplitude envelope: each entry on its own looped region (so the level is audible for the whole note), three controller positions. Release is read when the
    # voice starts and applied at the key up. The SFZ 1 spelling `ampeg_attackcc` is one of them.
    table = [(60, 30), (62, 31), (64, 32), (65, 34), (67, 35), (69, 36), (71, 37)]
    steps = []
    for key, cc in table:
        for v in (0, 64, 127):
            steps.append((cc, v, key, 110))
    ev, ccs = staged(steps)
    # longer notes: the sustain/decay/release shapes need room
    ev2, k = [], 0
    for i, (cc, val, key, vel) in enumerate(steps):
        t = 0.05 + i * 0.32
        ev2.append((secs(t), 1, key, vel))
        ev2.append((secs(t + 0.18), 0, key, vel))
        if cc is not None:
            ccs[k] = (secs(t - 0.04), cc, val)
            k += 1
    ev2.sort(key=lambda e: (e[0], e[1]))
    case("cc_envelope", HEADER + f"""<global> sample=loop.wav {loop} pitch_keycenter=60 ampeg_release=0.03
<curve> curve_index=7 v000=0 v127=1 v064=0.2
<region> key=60 ampeg_release_oncc30=0.3
<region> key=62 ampeg_attackcc31=0.1
<region> key=64 ampeg_decay=0.02 ampeg_sustain=90 ampeg_decay_oncc32=0.05 ampeg_sustain_oncc33=-60
<region> key=65 ampeg_attack=0.005 ampeg_delay_oncc34=0.04 ampeg_hold_oncc34=0.03
<region> key=67 ampeg_start=10 ampeg_attack=0.06 ampeg_start_oncc35=70
<region> key=69 ampeg_attack=0.002 ampeg_release_oncc36=0.25 ampeg_release_curvecc36=7
<region> key=71 ampeg_decay=0.03 ampeg_sustain=80 ampeg_sustain_oncc37=-70 ampeg_sustain_curvecc37=7
""", ev2, sorted(ccs))

    # sfizz pseudo-CCs: 131 (note-on velocity), 137 (alternate), 140 and 141 (key delta), on the matrix targets and on the envelope. One note stream; the
    # alternate and the key delta follow every note on of the part, whichever region it hits.
    keys = [60, 62, 64, 65, 67, 69, 71, 74]
    vels = [30, 127, 80, 50, 100, 20, 127, 64, 90, 40, 110, 60, 127, 10, 75, 99]
    seq = [keys[i % len(keys)] for i in range(16)]
    seq2 = [60, 71, 62, 69, 64, 67, 65, 74]       # big key jumps, down as well as up
    steps = [(None, None, k, vels[i % len(vels)]) for i, k in enumerate(seq + seq2 + seq)]
    ev, ccs = staged(steps)
    case("cc_pseudo", HEADER + f"""<global> ampeg_release=0.05
<region> sample=a.wav lokey=60 hikey=60 pitch_keycenter=60 amp_veltrack=0 amplitude_oncc131=100
<region> sample=b.wav lokey=62 hikey=62 pitch_keycenter=62 pan_oncc137=100
<region> sample=c.wav lokey=64 hikey=64 pitch_keycenter=64 volume_oncc140=0.5
<region> sample=d.wav lokey=65 hikey=65 pitch_keycenter=65 tune_oncc141=20
<region> sample=loop.wav lokey=67 hikey=67 pitch_keycenter=67 {loop} ampeg_attack_oncc131=0.06
<region> sample=loop.wav lokey=69 hikey=69 pitch_keycenter=69 {loop} ampeg_attack=0.002 ampeg_release_oncc141=0.2
<region> sample=a.wav lokey=71 hikey=71 pitch_keycenter=71 amp_veltrack=0 amplitude_oncc137=100 amplitude_curvecc137=8
<region> sample=b.wav lokey=74 hikey=74 pitch_keycenter=74 volume_oncc140=-1 volume_oncc131=-12
""", ev, ccs)


def build_story136_cases():
    """Story 13.6: choke groups (group, off_by, off_mode, off_time), the sustain pedal, and polyphony limits (group polyphony, region polyphony, note_polyphony
    with and without note_selfmask). Every cut is a release the engine and sfizz both start at the note on of the cutting voice. The voices are looped samples
    (they sound until released) so a missing or late cut shows as energy in the residual."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"

    # Choke groups. 60 (fast, the default 6 ms), 61 (normal: its own 0.12 s release), 63 (off_time=0.08) are open hats cut by the closed hat 65 (group 2). 67 and 69 share
    # group 3 with off_by=3: another note of the group cuts, a retrigger of the same note does not. The last block cuts a note that already got its note off (a release in
    # progress) with the fast release.
    ev = notes([
        (secs(0.05), secs(0.55), 60, 100), (secs(0.25), secs(0.30), 65, 100),
        (secs(0.80), secs(1.40), 61, 100), (secs(1.00), secs(1.05), 65, 100),
        (secs(1.70), secs(2.30), 63, 100), (secs(1.90), secs(1.95), 65, 100),
        (secs(2.60), secs(3.30), 67, 100), (secs(2.75), secs(3.30), 69, 100), (secs(2.97), secs(3.40), 69, 100),
        (secs(3.90), secs(4.10), 60, 100), (secs(4.14), secs(4.19), 65, 100),
    ])
    ev.sort(key=lambda e: (e[0], e[1]))
    case("choke_groups", HEADER + f"""<global> ampeg_release=0.12
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop} group=1 off_by=2
<region> sample=loop2.wav lokey=61 hikey=61 pitch_keycenter=61 {loop} group=1 off_by=2 off_mode=normal
<region> sample=loop.wav lokey=63 hikey=63 pitch_keycenter=63 {loop} group=1 off_by=2 off_time=0.08
<region> sample=short.wav lokey=65 hikey=65 pitch_keycenter=65 group=2
<region> sample=loop.wav lokey=67 hikey=67 pitch_keycenter=67 {loop} group=3 off_by=3
<region> sample=loop2.wav lokey=69 hikey=69 pitch_keycenter=69 {loop} group=3 off_by=3
""", ev, ())

    # The sustain pedal (CC64) is down from the start: 60 (open hat, off_by=2) and 62 (no choke) are released with the pedal down and keep sounding; the closed hat 65
    # cuts 60 at its note on although the pedal holds it, 62 rings on until the pedal comes up at 1.2 s.
    ev = notes([(secs(0.05), secs(0.30), 60, 100), (secs(0.05), secs(0.30), 62, 100), (secs(0.55), secs(0.60), 65, 100)])
    ev.sort(key=lambda e: (e[0], e[1]))
    case("choke_sustain", HEADER + f"""<global> ampeg_release=0.4
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop} group=1 off_by=2
<region> sample=loop2.wav lokey=62 hikey=62 pitch_keycenter=62 {loop}
<region> sample=short.wav lokey=65 hikey=65 pitch_keycenter=65 group=2
""", ev, [(secs(0.02), 64, 127), (secs(1.20), 64, 0)])

    # Polyphony. Keys 60, 62, 64 are three regions of group 5 with polyphony=2: the third note cuts the oldest, a fourth cuts the next. Keys 67 / 69 / 71: note_polyphony=1
    # (a retrigger at the same or a higher velocity cuts the previous voice, a quieter one does not), note_selfmask=off (the oldest goes whatever the velocity) and
    # note_polyphony=2 (the quietest of the older voices goes). Keys 72..74 are one region with polyphony=1.
    ev = notes([
        (secs(0.05), secs(1.00), 60, 100), (secs(0.20), secs(1.00), 62, 100), (secs(0.35), secs(1.00), 64, 100), (secs(0.50), secs(1.00), 60, 100),
        (secs(1.30), secs(2.50), 67, 100), (secs(1.45), secs(2.50), 67, 100), (secs(1.60), secs(2.50), 67, 60),
        (secs(2.80), secs(3.80), 69, 100), (secs(2.95), secs(3.80), 69, 50),
        (secs(4.10), secs(5.40), 71, 100), (secs(4.25), secs(5.40), 71, 60), (secs(4.40), secs(5.40), 71, 110),
        (secs(5.70), secs(6.40), 72, 100), (secs(5.85), secs(6.40), 74, 100),
    ])
    ev.sort(key=lambda e: (e[0], e[1]))
    case("polyphony_limits", HEADER + f"""<global> ampeg_release=0.08
<group> group=5 polyphony=2
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop}
<region> sample=loop2.wav lokey=62 hikey=62 pitch_keycenter=62 {loop}
<region> sample=loop.wav lokey=64 hikey=64 pitch_keycenter=64 {loop}
<group>
<region> sample=loop.wav lokey=67 hikey=67 pitch_keycenter=67 {loop} note_polyphony=1
<region> sample=loop2.wav lokey=69 hikey=69 pitch_keycenter=69 {loop} note_polyphony=1 note_selfmask=off
<region> sample=loop.wav lokey=71 hikey=71 pitch_keycenter=71 {loop} note_polyphony=2
<region> sample=loop2.wav lokey=72 hikey=74 pitch_keycenter=73 {loop} polyphony=1
""", ev, ())


def build_story137_cases():
    """Story 13.7: pitch bend (bend_up, bend_down, bend_step, bend_smooth) and regions started by a controller (on_locc / on_hicc, hikey=-1).
    Every wheel event sits on a 64-frame block boundary (blk): sfizz ramps the pitch of the block that holds an event up to the event's frame, the engine jumps at the
    event, and the two agree exactly only when the event is the first frame of a block (see the README, "Pitch bend and controller-started voices").
    sfizz accumulates a voice's read position in float32 per 64-frame block (cumsum), so a pitch ratio that is not a power of two drifts from the engine's double
    position by about 1e-6 of a sample per frame (a phase offset of 0.02 to 0.03 frame after 2 s, measured). The fixtures that hold a bent note for a long time therefore
    use ratios that float32 adds exactly (2, 1, 0.5, 0.25: cents that are multiples of 1200); `pitch_bend_fine` visits arbitrary cents and is short."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"

    # The wheel at the extremes (16383, 0), at positions that bend_step quantises to whole octaves, and at the centre; it moves while the notes are released (release 0.5 s).
    # 60: +-1200 cents. 62: bend_up=2400 bend_down=-2400 bend_step=1200 (the wheel at 0.5 and 0.7 gives +1200, at -0.5 and -0.76 gives -1200: trunc towards zero).
    # 64: asymmetric, bend_up=1200 bend_down=-2400. 66 starts with the wheel held at full travel up (the voice starts bent, no ramp).
    ev = notes([
        (secs(0.05), secs(2.20), 60, 100), (secs(0.05), secs(2.00), 62, 100), (secs(0.05), secs(2.40), 64, 100), (secs(2.90), secs(3.60), 66, 100),
    ])
    ev.sort(key=lambda e: (e[0], e[1]))
    wheel = [(blk(secs(0.30)), PITCH_BEND, 16383), (blk(secs(0.60)), PITCH_BEND, 0), (blk(secs(0.90)), PITCH_BEND, 12288), (blk(secs(1.20)), PITCH_BEND, 4096),
             (blk(secs(1.50)), PITCH_BEND, 8192), (blk(secs(1.80)), PITCH_BEND, 14000), (blk(secs(2.10)), PITCH_BEND, 0), (blk(secs(2.60)), PITCH_BEND, 16383)]
    case("pitch_bend", HEADER + f"""<global> ampeg_release=0.5
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop} bend_up=1200 bend_down=-1200
<region> sample=loop2.wav lokey=62 hikey=62 pitch_keycenter=62 {loop} bend_up=2400 bend_down=-2400 bend_step=1200
<region> sample=loop.wav lokey=64 hikey=64 pitch_keycenter=64 {loop} bend_up=1200 bend_down=-2400
<region> sample=loop2.wav lokey=66 hikey=66 pitch_keycenter=66 {loop} bend_up=1200 bend_down=-1200
""", ev, wheel)

    # Arbitrary cents (irrational pitch ratios), short notes: asymmetric ranges, a quantiser of 100 cents, the 14-bit mapping at intermediate positions.
    ev = notes([(secs(0.05), secs(0.55), 60, 100), (secs(0.05), secs(0.55), 62, 100), (secs(0.05), secs(0.55), 64, 100)])
    ev.sort(key=lambda e: (e[0], e[1]))
    wheel = [(blk(secs(0.15)), PITCH_BEND, 12288), (blk(secs(0.30)), PITCH_BEND, 5000), (blk(secs(0.45)), PITCH_BEND, 15000)]
    case("pitch_bend_fine", HEADER + f"""<global> ampeg_release=0.2
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop} bend_up=700 bend_down=-500
<region> sample=loop2.wav lokey=62 hikey=62 pitch_keycenter=62 {loop} bend_up=400 bend_down=-800 bend_step=100
<region> sample=loop.wav lokey=64 hikey=64 pitch_keycenter=64 {loop} bend_up=300 bend_down=-300
""", ev, wheel)

    # Smoothed wheel: bend_smooth=40 ms (1920 frames) on 60 (+-1200) and on 64 (+1200 / -2400). The wheel jumps every 0.5 s, retargets before the first ramp has ended
    # (0.80 then 0.83 s) and moves while the notes are released. Targets are octaves and every ramp step (0.625, 1.25, 1.875, 1.40625 ... cents per frame) is a binary
    # fraction: sfizz adds the step to a float32 cents value in SIMD lanes, which only equals a sequential sum when the step is exactly representable (README).
    ev = notes([(secs(0.05), secs(2.60), 60, 100), (secs(0.05), secs(2.60), 64, 100)])
    ev.sort(key=lambda e: (e[0], e[1]))
    wheel = [(blk(secs(0.40)), PITCH_BEND, 16383), (blk(secs(0.80)), PITCH_BEND, 0), (blk(secs(0.83)), PITCH_BEND, 16383), (blk(secs(1.40)), PITCH_BEND, 8192),
             (blk(secs(1.90)), PITCH_BEND, 0), (blk(secs(2.80)), PITCH_BEND, 8192)]
    case("pitch_bend_smooth", HEADER + f"""<global> ampeg_release=0.3
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop} bend_up=1200 bend_down=-1200 bend_smooth=40
<region> sample=loop2.wav lokey=64 hikey=64 pitch_keycenter=64 {loop} bend_up=1200 bend_down=-2400 bend_smooth=40
""", ev, wheel)

    # Controller-started voices. Salamander's pedal pattern: a master with lokey=-1 hikey=-1 group=1 polyphony=1 holds two groups of regions, one started by the pedal
    # going down (on_locc64=127), one by it coming up (on_locc64=0 is [0, 1) after the fill-gap). A note region rings under the pedal. CC1 over 30..60 starts a
    # voice on every change inside the range (a repeat of the same value starts nothing). key=62 with on_locc2 starts from the note and from the controller.
    ev = notes([(secs(0.10), secs(0.50), 60, 100), (secs(2.40), secs(2.70), 62, 100), (secs(2.95), secs(3.10), 60, 100)])
    ev.sort(key=lambda e: (e[0], e[1]))
    ccs = [(secs(0.30), 64, 127), (secs(0.90), 64, 0), (secs(1.00), 64, 127), (secs(1.04), 64, 0), (secs(1.10), 64, 127),
           (secs(1.60), 1, 20), (secs(1.70), 1, 45), (secs(1.80), 1, 50), (secs(1.90), 1, 50), (secs(2.00), 1, 70), (secs(2.10), 1, 40), (secs(2.20), 1, 61),
           (secs(2.30), 2, 10), (secs(2.80), 2, 10), (secs(2.85), 2, 90), (secs(3.40), 64, 0)]
    case("cc_trigger", HEADER + f"""<global> ampeg_release=0.1
<region> sample=loop.wav lokey=60 hikey=60 pitch_keycenter=60 {loop}
<master> lokey=-1 hikey=-1 group=1 polyphony=1 off_time=0.05
<group> volume=-6 on_locc64=127 on_hicc64=127 locc64=127 hicc64=127
<region> sample=loop2.wav pitch_keycenter=62
<group> volume=-4 on_locc64=0 on_hicc64=0 locc64=0 hicc64=0
<region> sample=short.wav pitch_keycenter=67
<master> hikey=-1 on_locc1=30 on_hicc1=60 ampeg_release=0.05
<region> sample=loop.wav pitch_keycenter=64 volume=-3
<global>
<region> sample=loop2.wav key=62 on_locc2=1 on_hicc2=127 ampeg_release=0.1
""", ev, ccs)


def build_story138_cases():
    """Story 13.8: the per-voice filter. One fixture instrument per opcode family on the bright, noisy loop (110 Hz, 40 harmonics, so a filter always has something to
    remove): fil_keytrack/fil_keycenter, fil_veltrack, cutoff_oncc/cutoff_cc/cutoff_curvecc (controllers and the velocity pseudo-CC 131), resonance_oncc/resonance_cc and the
    second filter (cutoff2, fil2_*). Every controller is moved BEFORE the note it is meant for (sfizz modulates continuously, the engine reads the controller when the note
    starts, so no controller moves while a note sounds: the documented limit of Story 13.5)."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"
    curves = "<curve>\ncurve_index=7\nv000=0 v064=0.9 v127=1\n<curve>\ncurve_index=8\nv000=0 v064=0.3 v127=1\n"

    def staged(steps, gap=0.20, length=0.10):
        """steps: (cc or None, value or None, key, vel): the controller is set 40 ms before its note, and every voice has died (note + 0.05 s release) before the next move."""
        ev, ccs = [], []
        for i, (cc, val, key, vel) in enumerate(steps):
            t = 0.05 + i * gap
            if cc is not None:
                ccs.append((secs(t - 0.04), cc, val))
            ev.append((secs(t), 1, key, vel))
            ev.append((secs(t + length), 0, key, vel))
        ev.sort(key=lambda e: (e[0], e[1]))
        return ev, sorted(ccs)

    # fil_keytrack / fil_keycenter: three pitched regions (the sample follows the key), the cutoff follows the key at 100 cents per key from the default centre 60, at 50 from
    # centre 64 (one pole), and an hpf from centre 72.
    steps = [(None, None, k, 100) for k in (40, 47, 55, 56, 64, 72, 73, 84, 96)]
    ev, ccs = staged(steps, 0.20, 0.10)
    case("fil_keytrack", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=40 hikey=55 pitch_keycenter=48 cutoff=1200 fil_keytrack=100
<region> lokey=56 hikey=72 pitch_keycenter=64 fil_type=lpf_1p cutoff=900 fil_keytrack=50 fil_keycenter=64
<region> lokey=73 hikey=96 pitch_keycenter=84 fil_type=hpf_2p cutoff=600 resonance=3 filtype=hpf_2p fil_keytrack=100 fil_keycenter=72
""", ev, ccs)

    # fil_veltrack: cents at velocity 127 (the raw velocity, not the amplitude curve), positive and negative, with a key track on top.
    steps = []
    for key, vels in ((60, (1, 32, 64, 100, 127)), (62, (1, 64, 127)), (64, (20, 90, 127)), (65, (1, 64, 127))):
        steps += [(None, None, key, v) for v in vels]
    ev, ccs = staged(steps, 0.20, 0.10)
    case("fil_veltrack", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 cutoff=300 resonance=2 fil_veltrack=3600
<region> lokey=62 hikey=62 pitch_keycenter=62 cutoff=8000 fil_veltrack=-2400
<region> lokey=64 hikey=64 pitch_keycenter=64 cutoff=500 fil_veltrack=1200 fil_keytrack=50 fil_keycenter=60
<region> lokey=65 hikey=65 pitch_keycenter=65 fil_type=hpf_2p cutoff=200 fil1_veltrack=2400 fil1_type=hpf_2p
""", ev, ccs)

    # cutoff_oncc / cutoff_cc / cutoff_curvecc: a controller through the default curve, the SFZ 1 spelling with a <curve> block, two controllers (one negative), the
    # velocity pseudo-CC 131 through a curve (kamoepiano301's pattern), and every touch at once.
    steps = []
    for v in (0, 40, 64, 127):
        steps.append((74, v, 60, 100))
    for v in (0, 64, 127):
        steps.append((75, v, 62, 100))
    ev, ccs = staged(steps, 0.20, 0.10)
    n = len(steps)
    t0 = 0.05 + n * 0.20
    ev2, ccs2 = [], []
    # key 64 with two controllers: both are set before the note
    for i, (a, b) in enumerate(((0, 0), (127, 0), (0, 127), (100, 50))):
        t = t0 + i * 0.20
        ccs2 += [(secs(t - 0.05), 76, a), (secs(t - 0.04), 77, b)]
        ev2 += [(secs(t), 1, 64, 100), (secs(t + 0.10), 0, 64, 100)]
    # the velocity pseudo-CC on key 65 (curve 8) and key 67 (default curve), swept over velocities
    t1 = t0 + 4 * 0.20
    for i, v in enumerate((1, 30, 64, 100, 127)):
        t = t1 + i * 0.20
        ev2 += [(secs(t), 1, 65, v), (secs(t + 0.10), 0, 65, v)]
    t2 = t1 + 5 * 0.20
    for i, v in enumerate((10, 64, 127)):
        t = t2 + i * 0.20
        ev2 += [(secs(t), 1, 67, v), (secs(t + 0.10), 0, 67, v)]
    # key 69: controller, velocity track and key track together
    t3 = t2 + 3 * 0.20
    for i, (c, v) in enumerate(((0, 40), (127, 40), (64, 110))):
        t = t3 + i * 0.20
        ccs2 += [(secs(t - 0.04), 78, c)]
        ev2 += [(secs(t), 1, 69, v), (secs(t + 0.10), 0, 69, v)]
    ev = sorted(ev + ev2, key=lambda e: (e[0], e[1]))
    ccs = sorted(ccs + ccs2)
    case("cutoff_oncc", HEADER + curves + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 cutoff=300 cutoff_oncc74=3000
<region> lokey=62 hikey=62 pitch_keycenter=62 cutoff=300 cutoff_cc75=2400 cutoff_curvecc75=7
<region> lokey=64 hikey=64 pitch_keycenter=64 cutoff=2000 cutoff_oncc76=-2400 cutoff1_oncc77=1200
<region> lokey=65 hikey=65 pitch_keycenter=65 cutoff=400 cutoff_oncc131=3000 cutoff_curvecc131=8
<region> lokey=67 hikey=67 pitch_keycenter=67 cutoff=500 cutoff_oncc131=2400
<region> lokey=69 hikey=69 pitch_keycenter=69 cutoff=400 cutoff_oncc78=2400 fil_veltrack=1200 fil_keytrack=100
""", ev, ccs)

    # resonance_oncc / resonance_cc / resonance_curvecc: the Q at the cutoff follows a controller (dB added).
    steps = []
    for v in (0, 64, 127):
        steps.append((79, v, 60, 100))
    for v in (0, 127, 40):
        steps.append((80, v, 62, 100))
    for v in (0, 100):
        steps.append((81, v, 64, 100))
    ev, ccs = staged(steps, 0.20, 0.10)
    case("resonance_oncc", HEADER + curves + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 cutoff=900 resonance_oncc79=24
<region> lokey=62 hikey=62 pitch_keycenter=62 fil_type=hpf_2p cutoff=700 resonance=6 resonance_cc80=12 resonance_curvecc80=7
<region> lokey=64 hikey=64 pitch_keycenter=64 fil_type=bpf_2p cutoff=1500 resonance1_oncc81=20
""", ev, ccs)

    # the second filter: in series after the first, with its own type, resonance, key track, velocity track and CC entries.
    steps = [(None, None, 60, 100), (None, None, 62, 100), (None, None, 64, 100), (None, None, 76, 100)]
    for v in (0, 127):
        steps.append((82, v, 65, 100))
    for v in (20, 127):
        steps.append((None, None, 67, v))
    ev, ccs = staged(steps, 0.20, 0.10)
    case("filter2", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 cutoff=4000 cutoff2=500 fil2_type=hpf_2p
<region> lokey=62 hikey=62 pitch_keycenter=62 cutoff=2000 resonance=3 cutoff2=1500 fil2_type=lpf_2p resonance2=6
<region> lokey=64 hikey=64 pitch_keycenter=64 fil_type=lpf_1p cutoff=3000 cutoff2=300 fil2_type=hpf_1p fil2_keytrack=100 fil2_keycenter=64
<region> lokey=76 hikey=76 pitch_keycenter=76 fil_type=lpf_1p cutoff=3000 cutoff2=300 fil2_type=hpf_1p fil2_keytrack=100 fil2_keycenter=64
<region> lokey=65 hikey=65 pitch_keycenter=65 cutoff=6000 cutoff2=200 fil2_type=hpf_2p cutoff2_oncc82=3000 resonance2_oncc82=12
<region> lokey=67 hikey=67 pitch_keycenter=67 cutoff=1000 fil_veltrack=2400 cutoff2=8000 fil2_veltrack=-1200
""", ev, ccs)


def build_story139_cases():
    """Story 13.9: region EQ and the dedicated LFOs / EGs. One fixture instrument per opcode family on the bright, noisy loop (110 Hz, 40 harmonics): eq_bands (peak, low and
    high shelf, bandwidth, sfizz's positional default frequencies), eq_velocity (eqN_vel2freq / vel2gain), eq_cc (eqN_*_oncc through a curve; the controllers move BEFORE the
    note they are meant for, the engine reads them when the note starts, the documented limit of Story 13.5), amplfo (every wave, delay, fade), pitchlfo, pitcheg and
    fillfo_fileg (the cutoff of the first filter moved by an LFO and by an EG)."""
    loop = "loop_mode=loop_continuous loop_start=2400 loop_end=7199"
    curves = "<curve>\ncurve_index=7\nv000=0 v064=0.9 v127=1\n"

    def staged(steps, gap=0.20, length=0.10):
        """steps: (cc or None, value or None, key, vel): the controller is set 40 ms before its note, and every voice has died before the next move."""
        ev, ccs = [], []
        for i, (cc, val, key, vel) in enumerate(steps):
            t = 0.05 + i * gap
            if cc is not None:
                ccs.append((secs(t - 0.04), cc, val))
            ev.append((secs(t), 1, key, vel))
            ev.append((secs(t + length), 0, key, vel))
        ev.sort(key=lambda e: (e[0], e[1]))
        return ev, sorted(ccs)

    # eq_bands: a peak boost, a cut with a narrow band, a low shelf, a high shelf and the positional default frequencies (eq2 alone is band 2 at 500 Hz with a transparent band 1,
    # eq3 alone is band 3 at 5000 Hz).
    steps = [(None, None, k, 100) for k in (60, 62, 64, 65, 67, 69, 71)]
    ev, ccs = staged(steps, 0.20, 0.10)
    case("eq_bands", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 eq1_freq=800 eq1_bw=1 eq1_gain=9
<region> lokey=62 hikey=62 pitch_keycenter=62 eq1_freq=1500 eq1_bw=0.25 eq1_gain=-12
<region> lokey=64 hikey=64 pitch_keycenter=64 eq1_type=lshelf eq1_freq=600 eq1_bw=1 eq1_gain=-9 eq2_type=hshelf eq2_freq=3000 eq2_bw=1 eq2_gain=6
<region> lokey=65 hikey=65 pitch_keycenter=65 eq2_gain=-8
<region> lokey=67 hikey=67 pitch_keycenter=67 eq3_gain=10 eq3_bw=0.5
<region> lokey=69 hikey=69 pitch_keycenter=69 eq1_freq=300 eq1_gain=6 eq2_freq=1200 eq2_gain=-6 eq3_freq=4000 eq3_gain=5 eq3_type=hshelf
<region> lokey=71 hikey=71 pitch_keycenter=71 eq1_freq=2000 eq1_gain=-20 eq1_bw=2
""", ev, ccs)

    # eq_velocity: the band frequency and gain follow the note velocity.
    steps = []
    for key, vels in ((60, (1, 40, 80, 127)), (62, (1, 64, 127)), (64, (30, 127))):
        steps += [(None, None, key, v) for v in vels]
    ev, ccs = staged(steps, 0.20, 0.10)
    case("eq_velocity", HEADER + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 eq1_freq=500 eq1_gain=0 eq1_vel2gain=12 eq1_bw=1
<region> lokey=62 hikey=62 pitch_keycenter=62 eq1_freq=300 eq1_vel2freq=3000 eq1_gain=8 eq1_bw=0.5
<region> lokey=64 hikey=64 pitch_keycenter=64 eq1_freq=1000 eq1_gain=-3 eq1_veltogain=-9 eq1_veltofreq=1000
""", ev, ccs)

    # eq_cc: the freq, bw and gain of a band follow controllers (Headroom Piano's pattern: eq1_freq_oncc, eq1_gain_oncc with a curve, eq1_bw_oncc).
    steps = []
    for v in (0, 64, 127):
        steps.append((80, v, 60, 100))
    for v in (0, 100, 127):
        steps.append((81, v, 62, 100))
    for v in (0, 127):
        steps.append((82, v, 64, 100))
    ev, ccs = staged(steps, 0.20, 0.10)
    t0 = 0.05 + len(steps) * 0.20
    ev2, ccs2 = [], []
    for i, (a, b, c) in enumerate(((0, 0, 0), (127, 127, 127), (60, 30, 90))):
        t = t0 + i * 0.20
        ccs2 += [(secs(t - 0.06), 83, a), (secs(t - 0.05), 84, b), (secs(t - 0.04), 85, c)]
        ev2 += [(secs(t), 1, 65, 100), (blk(secs(t + 0.10)), 0, 65, 100)]
    ev = sorted(ev + ev2, key=lambda e: (e[0], e[1]))
    ccs = sorted(ccs + ccs2)
    case("eq_cc", HEADER + curves + f"""<global> sample=bright.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 eq1_freq=300 eq1_gain=9 eq1_freq_oncc80=3000
<region> lokey=62 hikey=62 pitch_keycenter=62 eq1_freq=1000 eq1_gain=-12 eq1_gain_oncc81=18 eq1_gain_curvecc81=7
<region> lokey=64 hikey=64 pitch_keycenter=64 eq1_freq=1200 eq1_gain=10 eq1_bw=0.1 eq1_bw_oncc82=2.0
<region> lokey=65 hikey=65 pitch_keycenter=65 eq1_freq=500 eq1_gain=4 eq1_freq_oncc83=1000 eq1_gain_oncc84=6 eq1_bw_oncc85=1
""", ev, ccs)

    # The modulated notes are long (they cross the loop several times), so the four cases below use loop.wav: periodic, without the noise of bright.wav, whose wrap point
    # only matches sfizz to about -45 dB in a 100 ms window (a property of the loop, not of the modulation: a plain looped note of bright.wav shows the same).
    # amplfo: every wave (0 triangle, 1 sine, 2 pulse75, 3 square, 4 pulse25, 5 pulse12.5, 6 ramp, 7 saw), then a delay and a fade. Notes are held for a second.
    keys = (60, 61, 62, 63, 64, 65, 66, 67)
    ev = []
    for i, k in enumerate(keys):
        t = 0.05 + i * 1.15
        ev += [(blk(secs(t)), 1, k, 100), (blk(secs(t + 1.0)), 0, k, 100)]
    regions = "".join(f"<region> lokey={k} hikey={k} pitch_keycenter={k} amplfo_depth=6 amplfo_freq=4 amplfo_wave={i}\n" for i, k in enumerate(keys))
    t = 0.05 + len(keys) * 1.15
    ev += [(blk(secs(t)), 1, 68, 100), (blk(secs(t + 1.2)), 0, 68, 100), (blk(secs(t + 1.4)), 1, 69, 100), (blk(secs(t + 2.6)), 0, 69, 100)]
    regions += "<region> lokey=68 hikey=68 pitch_keycenter=68 amplfo_depth=-9 amplfo_freq=3 amplfo_delay=0.3 amplfo_fade=0.4\n"
    regions += "<region> lokey=69 hikey=69 pitch_keycenter=69 amplfo_depth=4 amplfo_freq=7.5 volume=-3 amplfo_fade=0.6\n"
    ev.sort(key=lambda e: (e[0], e[1]))
    case("amplfo", HEADER + f"<global> sample=loop.wav {loop} ampeg_release=0.05\n" + regions, ev)

    # NOTE (sfizz 1.2.3): a voice that starts inside a 64-frame block sees its PITCH modulation (LFO, EG) late by the offset inside the block, in that first block only (the
    # pitch is read from the delayed sub-span, the modulation buffers are block aligned; the amplitude and filter stages are aligned). The oracle of the corpus renders with
    # blocks of 1 frame, where it cannot happen; the fixtures with a pitch LFO / EG therefore start their notes on block boundaries (blk).
    # pitchlfo: vibrato with the default sine, a triangle, a saw with a delay and a fade, and a deep wide one.
    ev = []
    for i, k in enumerate((60, 62, 64, 65)):
        t = 0.05 + i * 1.15
        ev += [(blk(secs(t)), 1, k, 100), (blk(secs(t + 1.0)), 0, k, 100)]
    ev.sort(key=lambda e: (e[0], e[1]))
    case("pitchlfo", HEADER + f"""<global> sample=loop.wav {loop} ampeg_release=0.05
<region> lokey=60 hikey=60 pitch_keycenter=60 pitchlfo_depth=50 pitchlfo_freq=5
<region> lokey=62 hikey=62 pitch_keycenter=62 pitchlfo_depth=-100 pitchlfo_freq=3.5 pitchlfo_wave=0
<region> lokey=64 hikey=64 pitch_keycenter=64 pitchlfo_depth=200 pitchlfo_freq=6 pitchlfo_wave=7 pitchlfo_delay=0.2 pitchlfo_fade=0.3
<region> lokey=65 hikey=65 pitch_keycenter=65 pitchlfo_depth=1200 pitchlfo_freq=1.5
""", ev)

    # NOTE (sfizz 1.2.3, ADSREnvelope.cpp): an EG whose delay ends exactly at the end of a render block is never released (the release test needs `delay < 0`, and the delay
    # loop leaves 0 when the block runs out first). The delays below (0.05 s = 2400 frames) are not multiples of the 64-frame block; the oracle of the corpus renders with
    # blocks of 1 frame, where every EG with a non-zero delay shows this.
    # pitcheg: attack, decay to a sustain level, a release, a delay and a start level, a negative depth, the velocity depth.
    steps = []
    ev = []
    specs = ((60, 100), (62, 100), (64, 127), (64, 30), (65, 100))
    for i, (k, v) in enumerate(specs):
        t = 0.05 + i * 1.15
        ev += [(blk(secs(t)), 1, k, v), (blk(secs(t + 0.8)), 0, k, v)]
    ev.sort(key=lambda e: (e[0], e[1]))
    case("pitcheg", HEADER + f"""<global> sample=loop.wav {loop} ampeg_release=0.4
<region> lokey=60 hikey=60 pitch_keycenter=60 pitcheg_depth=2400 pitcheg_attack=0.2 pitcheg_decay=0.3 pitcheg_sustain=50 pitcheg_release=0.3
<region> lokey=62 hikey=62 pitch_keycenter=62 pitcheg_depth=-2400 pitcheg_delay=0.05 pitcheg_start=20 pitcheg_attack=0.1 pitcheg_hold=0.1 pitcheg_decay=0.2 pitcheg_sustain=50 pitcheg_release=0.2
<region> lokey=64 hikey=64 pitch_keycenter=64 pitcheg_depth=100 pitcheg_vel2depth=800 pitcheg_attack=0.05 pitcheg_sustain=100 pitcheg_release=0.1
<region> lokey=65 hikey=65 pitch_keycenter=65 pitcheg_veltodepth=1200 pitcheg_decay=0.4 pitcheg_sustain=0 pitcheg_release=0.05
""", ev)

    # fillfo_fileg: the cutoff of the first filter moved by a LFO (sine, square, triangle with a delay and a fade) and by an EG (attack, decay, sustain, release, velocity depth),
    # both together, and a cutoff_oncc with the LFO.
    ev = []
    specs = ((60, 100), (62, 100), (64, 100), (65, 127), (65, 30), (67, 100), (69, 100))
    ccs = [(secs(0.05 + 6 * 1.15 - 0.1), 74, 127)]
    for i, (k, v) in enumerate(specs):
        t = 0.05 + i * 1.15
        ev += [(blk(secs(t)), 1, k, v), (blk(secs(t + 0.8)), 0, k, v)]
    ev.sort(key=lambda e: (e[0], e[1]))
    case("fillfo_fileg", HEADER + f"""<global> sample=loop.wav {loop} ampeg_release=0.3
<region> lokey=60 hikey=60 pitch_keycenter=60 cutoff=800 resonance=3 fillfo_depth=1200 fillfo_freq=3
<region> lokey=62 hikey=62 pitch_keycenter=62 cutoff=500 fillfo_depth=-1200 fillfo_freq=2.5 fillfo_wave=3
<region> lokey=64 hikey=64 pitch_keycenter=64 fil_type=lpf_1p cutoff=1000 fillfo_depth=900 fillfo_freq=4 fillfo_wave=0 fillfo_delay=0.2 fillfo_fade=0.3
<region> lokey=65 hikey=65 pitch_keycenter=65 cutoff=400 resonance=2 fileg_depth=2400 fileg_vel2depth=1200 fileg_attack=0.15 fileg_decay=0.2 fileg_sustain=40 fileg_release=0.2
<region> lokey=67 hikey=67 pitch_keycenter=67 cutoff=600 fil_type=hpf_2p fileg_depth=3000 fileg_delay=0.05 fileg_start=10 fileg_attack=0.2 fileg_hold=0.1 fileg_decay=0.3 fileg_sustain=20 fillfo_depth=600 fillfo_freq=5 fillfo_wave=1
<region> lokey=69 hikey=69 pitch_keycenter=69 cutoff=500 cutoff_oncc74=1200 fillfo_depth=1200 fillfo_freq=2 fillfo_wave=6
""", ev, ccs)


def make_demo_probe():
    """midi/demo_probe.mid: a short phrase over the keyboard range for the env-gated real-library null test."""
    keys = [28, 40, 52, 64, 76, 88, 100]
    vels = [40, 80, 120, 60, 100, 127, 90]
    spec = [(i * secs(0.25), i * secs(0.25) + secs(0.2), k, v) for i, (k, v) in enumerate(zip(keys, vels))]
    write_midi(os.path.join(HERE, "midi", "demo_probe.mid"), notes(spec))


def make_demo_features():
    """midi/demo_features.mid: the probe of the env-gated real-library test for Story 5.2 (Karoryfer instruments): a keyswitch ladder (every key from
    C0 = 12 to B2 = 47 is pressed briefly, then two notes are played on the new articulation), then a chord held while the mod wheel (CC1) and
    CC2 sweep up and down in whole steps. Instruments without keyswitches or crossfades just play the phrases. CC7, CC10 and CC11 are NOT
    swept: sfizz maps them to volume, pan and expression of every region unless the instrument uses them itself, a default mapping the engine
    does not implement (engine/core/README.md, "Known differences from sfizz")."""
    spec = []
    ccs = []
    t = 0.0
    for ks in range(12, 48):
        spec.append((secs(t), secs(t + 0.03), ks, 100))
        spec.append((secs(t + 0.08), secs(t + 0.28), 60, 90))
        spec.append((secs(t + 0.32), secs(t + 0.52), 67, 100))
        t += 0.6
    start = t + 0.2
    for k in (52, 60, 64, 71):
        spec.append((secs(start), secs(start + 6.6), k, 100))
    step = 576
    ccs += sweep(1, 0, 127, secs(start + 0.2), step) + sweep(1, 126, 0, secs(start + 0.2) + 128 * step, step)
    ccs += sweep(2, 127, 0, secs(start + 0.2), step) + sweep(2, 1, 127, secs(start + 0.2) + 128 * step, step)
    write_midi(os.path.join(HERE, "midi", "demo_features.mid"), notes(spec), sorted(ccs), ((1, 0), (2, 127)))


def main():
    stories = {"story134": build_story134_cases, "story135": build_story135_cases, "story136": build_story136_cases, "story137": build_story137_cases, "story138": build_story138_cases, "story139": build_story139_cases}
    if sys.argv[1:] != [] and not (len(sys.argv) == 2 and sys.argv[1] in stories):
        sys.exit("usage: generate_suite.py [story134|story135|story136|story137|story138|story139]")
    if len(sys.argv) == 2:  # only that story's cases (leaves every older file untouched)
        stories[sys.argv[1]]()
        for name, (sfz, events, ccs, setup) in CASES.items():
            with open(os.path.join(HERE, "sfz", name + ".sfz"), "w") as f:
                f.write(sfz)
            write_midi(os.path.join(HERE, "midi", name + ".mid"), events, ccs, setup)
        print("wrote", len(CASES), "cases")
        return
    make_samples(HERE)
    make_demo_probe()
    make_demo_features()
    build_cases()
    build_story52_cases()
    build_story134_cases()
    build_story135_cases()
    build_story136_cases()
    build_story137_cases()
    build_story138_cases()
    build_story139_cases()
    for name, (sfz, events, ccs, setup) in CASES.items():
        with open(os.path.join(HERE, "sfz", name + ".sfz"), "w") as f:
            f.write(sfz)
        write_midi(os.path.join(HERE, "midi", name + ".mid"), events, ccs, setup)
    print("wrote", len(CASES), "cases")


if __name__ == "__main__":
    main()

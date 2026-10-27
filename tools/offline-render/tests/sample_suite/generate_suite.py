#!/usr/bin/env python3
"""Generates the synthetic sample-engine test suite: tiny 16-bit WAVs, SFZ files and MIDI files.

Run once, commit the output (the files are deterministic: integer math plus a fixed LCG; the
sines come from math.sin, so regenerating on another libm could change a sample by one LSB --
which is why the outputs are committed and the goldens are rendered from the committed files).

    python3 generate_suite.py            # (re)writes samples/, sfz/, midi/ next to this script
    python3 generate_suite.py story134   # only the Story 13.4 cases (no existing file is touched)
    python3 generate_suite.py story135   # only the Story 13.5 cases (no existing file is touched)

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
        if on < 0:
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
    if sys.argv[1:] not in ([], ["story134"], ["story135"]):
        sys.exit("usage: generate_suite.py [story134|story135]")
    if sys.argv[1:] in (["story134"], ["story135"]):  # only that story's cases (leaves every older file untouched)
        (build_story134_cases if sys.argv[1] == "story134" else build_story135_cases)()
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
    for name, (sfz, events, ccs, setup) in CASES.items():
        with open(os.path.join(HERE, "sfz", name + ".sfz"), "w") as f:
            f.write(sfz)
        write_midi(os.path.join(HERE, "midi", name + ".mid"), events, ccs, setup)
    print("wrote", len(CASES), "cases")


if __name__ == "__main__":
    main()

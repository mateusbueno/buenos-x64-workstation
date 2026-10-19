#!/usr/bin/env python3
"""Generates the synthetic sample-engine test suite: tiny 16-bit WAVs, SFZ files and MIDI files.

Run once, commit the output (the files are deterministic: integer math plus a fixed LCG; the
sines come from math.sin, so regenerating on another libm could change a sample by one LSB --
which is why the outputs are committed and the goldens are rendered from the committed files).

    python3 generate_suite.py            # (re)writes samples/, sfz/, midi/ next to this script

MIDI timing: the files use 32000 PPQ at 500000 us per quarter, i.e. 0.75 audio frames per tick at
48 kHz. A frame is mapped to a tick that puts the event at a quarter-frame fraction (never on an
exact frame boundary), so the renderer under test and sfizz_render agree on the frame whatever
their floating point rounding does.
"""
import math
import os
import struct
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


def write_midi(path, events):
    """events: list of (frame, on, key, vel) sorted by frame; note offs sort before ons at a frame."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    # Everything starts START_OFFSET frames in, so sfizz's CC smoothing has settled before the first note.
    evs = sorted(((f + START_OFFSET, on, k, v) for f, on, k, v in events), key=lambda e: (e[0], e[1]))
    track = bytearray()
    track += vlq(0) + b"\xff\x51\x03" + TEMPO.to_bytes(3, "big")
    # sfizz plays a default MIDI volume of CC7 = 100 (gain 0.62); CC7 = 127 at the start makes it unity.
    track += vlq(0) + bytes([0xB0, 7, 127])
    last = 0
    for frame, on, key, vel in evs:
        tick = frame_to_tick(frame)
        assert tick >= last, (frame, tick, last)
        track += vlq(tick - last)
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

CASES = {}   # name -> (sfz text, events)


def case(name, sfz, events):
    CASES[name] = (sfz, events)


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


def make_demo_probe():
    """midi/demo_probe.mid: a short phrase over the keyboard range for the env-gated real-library null test."""
    keys = [28, 40, 52, 64, 76, 88, 100]
    vels = [40, 80, 120, 60, 100, 127, 90]
    spec = [(i * secs(0.25), i * secs(0.25) + secs(0.2), k, v) for i, (k, v) in enumerate(zip(keys, vels))]
    write_midi(os.path.join(HERE, "midi", "demo_probe.mid"), notes(spec))


def main():
    make_samples(HERE)
    make_demo_probe()
    build_cases()
    for name, (sfz, events) in CASES.items():
        with open(os.path.join(HERE, "sfz", name + ".sfz"), "w") as f:
            f.write(sfz)
        write_midi(os.path.join(HERE, "midi", name + ".mid"), events)
    print("wrote", len(CASES), "cases")


if __name__ == "__main__":
    main()

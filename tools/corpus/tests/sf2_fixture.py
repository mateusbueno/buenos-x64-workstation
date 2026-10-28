"""Story 13.11: a tiny SoundFont 2 writer for the corpus tests (stdlib only, generated in code, nothing binary is committed).

Writes a valid SF2 2.01 file with one looped 16-bit sine sample (48 kHz, root key 60), one instrument that plays it over the whole keyboard with a
15 ms attack and a 300 ms release, and one preset per (bank, program, name) given. Layout follows the public SoundFont 2.04 specification; the
importer (formatlib/src/sf2_import.cpp) and the FluidSynth oracle both read it. Test tooling only.
"""
import math
import struct

GEN_ATTACK_VOL_ENV, GEN_RELEASE_VOL_ENV, GEN_INSTRUMENT, GEN_SAMPLE_ID, GEN_SAMPLE_MODES, GEN_KEY_RANGE = 34, 38, 41, 53, 54, 43


def _chunk(cid, data):
    return cid + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def _list(kind, *chunks):
    return _chunk(b"LIST", kind + b"".join(chunks))


def _name(text):
    return text.encode("latin-1")[:19].ljust(20, b"\0")


def _gen(op, amount):
    return struct.pack("<Hh", op, amount)


def _range_gen(op, lo, hi):
    return struct.pack("<HBB", op, lo, hi)


def make_sf2(presets=((0, 0, "Test Sine"),), frames=4800, key_range=(0, 127)):
    """presets: sequence of (bank, program, name). Returns the file as bytes."""
    pcm = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 261.6256 * i / 48000.0))) for i in range(frames))
    # sample data followed by 46 zero frames, as the specification asks
    smpl = pcm + b"\0" * 92
    info = _list(b"INFO", _chunk(b"ifil", struct.pack("<HH", 2, 1)), _chunk(b"isng", b"EMU8000\0"), _chunk(b"INAM", b"corpus-test\0"))
    sdta = _list(b"sdta", _chunk(b"smpl", smpl))
    # one instrument zone: key range, attack, release, loop continuously, sample 0 (the sampleID generator is last)
    igen = (_range_gen(GEN_KEY_RANGE, *key_range) + _gen(GEN_ATTACK_VOL_ENV, -4800) + _gen(GEN_RELEASE_VOL_ENV, -3000) + _gen(GEN_SAMPLE_MODES, 1)
            + _gen(GEN_SAMPLE_ID, 0) + b"\0" * 4)
    ibag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 5, 0)
    imod = b"\0" * 10
    inst = _name("Sine") + struct.pack("<H", 0) + _name("EOI") + struct.pack("<H", 1)
    shdr = (_name("Sine C4") + struct.pack("<IIIIIBbHH", 0, frames, 100, frames - 50, 48000, 60, 0, 0, 1)
            + _name("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0))
    phdr = b"".join(_name(n) + struct.pack("<HHHIII", prog, bank, i, 0, 0, 0) for i, (bank, prog, n) in enumerate(presets))
    phdr += _name("EOP") + struct.pack("<HHHIII", 0, 0, len(presets), 0, 0, 0)
    pbag = b"".join(struct.pack("<HH", i * 1, 0) for i in range(len(presets) + 1))
    pmod = b"\0" * 10
    pgen = b"".join(_gen(GEN_INSTRUMENT, 0) for _ in presets) + b"\0" * 4
    pdta = _list(b"pdta", _chunk(b"phdr", phdr), _chunk(b"pbag", pbag), _chunk(b"pmod", pmod), _chunk(b"pgen", pgen), _chunk(b"inst", inst),
                 _chunk(b"ibag", ibag), _chunk(b"imod", imod), _chunk(b"igen", igen), _chunk(b"shdr", shdr))
    body = b"sfbk" + info + sdta + pdta
    return b"RIFF" + struct.pack("<I", len(body)) + body

# SF2 render comparison against FluidSynth (Story 5.3)

The SF2 import (`formatlib/src/sf2_import.cpp`, `librarian import x.sf2`) is rendered by the sample engine and null-tested against
**committed FluidSynth 2.6.1 renders**, so `ctest` needs no FluidSynth. The fixtures are SoundFont 2 files **generated in code** by the
test-only writer `formatlib/tests/sf2_builder.hpp` (`sf2_fixtures.hpp` here describes them, `../sf2_suite_test.cpp` builds, imports and
renders them); the MIDI files are generated in code as well. The goldens (`golden/<case>.wav`) are FluidSynth's own 16-bit stereo output,
cut after the last non-silent frame plus 100 ms (its player appends 2 s of silence), made by `regenerate_goldens.sh` through
`tools/oracle/render-fluidsynth.sh` (`../../../oracle/README.md`). `cases.txt` lists the cases; CMake registers one ctest per line
(`sf2_null_<case>`).

FluidSynth is an LGPL-2.1 program that is only ever *run* as a separate process on test fixtures. It is not vendored, linked or shipped;
`tools/ci/check_third_party_notices.py` fails if a shipped CMake file mentions it.

## How a case runs

1. `sf2_suite_test <case>` builds the case's SF2 in code and imports it with `import_sf2` (the real importer, into a scratch data root).
2. `velora-offline-render --engine sample --bundle ... --instrument <bank>:<program> --gain-db 0` renders the case's MIDI (channel 0; the
   instrument key is the preset's `bank:program`, e.g. `0:0`, `128:0`).
3. `velora-null-test --case golden/<case>.case`: residual RMS relative to the reference RMS over both channels, alignment search of
   +-1 frame (`max_offset_frames=1`, so a timing error cannot hide), and `check_excess=1` (a tail in one file only counts too).

Every preset of the bank is a different case (the bank file also holds the others, so presets are selected out of a multi-preset file by FluidSynth
as well as by the engine).

## Cases and results

Residual = 10 log10(sum (ref - engine)^2 / sum ref^2), measured on macOS arm64 (Apple clang 21, no CMake build type). Tolerance = measured plus
3..4 dB. Target of the story: -40 dB or better where possible.

| case | file / preset | what it covers | residual | worst 100 ms | tolerance |
|------|---------------|----------------|---------:|-------------:|----------:|
| sf2_keys | bank `0:0` | key splits (36..59 / 60..84), a velocity split at 80/81 on the same keys, three samples (48 kHz, 44.1 kHz with root 48 and -10 cents of `pitchCorrection`, 96 kHz), `overridingRootKey`, fine tune, attenuation, a global zone with attack and release; velocities 127..64 | -39.7 dB | -36.0 dB | -36 dB |
| sf2_envelope | bank `0:1` | `delayVolEnv`, `attackVolEnv`, `holdVolEnv`, `decayVolEnv`, `sustainVolEnv`, `releaseVolEnv` on a continuous loop; a note released in its sustain and one in its decay | -40.4 dB | -34.8 dB | -37 dB |
| sf2_loops | bank `0:2` | the loop of the sample header moved by `startloopAddrsOffset`/`endloopAddrsOffset`, loop until release (`sampleModes` 3) released inside the loop, no loop with `startAddrsOffset` | -44.1 dB | -40.0 dB | -40 dB |
| sf2_stereo | bank `0:3` | a linked left/right pair with hard pans in its zones, a panned mono zone, attenuation, preset-level fine tune and attenuation | -41.5 dB | -39.0 dB | -38 dB |
| sf2_filter | bank `0:4` | `initialFilterFc`/`initialFilterQ`: 7 kHz without resonance, 2.96 kHz with a 6 dB peak, 1.48 kHz with a 10 dB peak, over a sample with partials up to 8 times the base | -32.0 dB | -25.3 dB | -28 dB |
| sf2_filter_q0 | bank `0:5` | a filter **without resonance** (Q 0) at 1.48 kHz with content at its cutoff: the documented gap, see below | -9.2 dB | -5.8 dB | -7 dB |
| sf2_kit | bank `128:0` | the percussion bank: one-shot hits on three keys with coarse and fine tune, attenuation and releases | -42.1 dB | -37.2 dB | -38 dB |
| sf2_single | `sf2_single.sf2` | a single-instrument file, 16-bit, looped with a full envelope, three notes | -41.1 dB | -34.9 dB | -38 dB |
| sf2_single24 | `sf2_single24.sf2` | the same file with 24-bit samples (`sm24`) | -41.1 dB | -34.9 dB | -38 dB |
| sf2_calibration | bank `0:6` | the oracle's gain: one looped sample at its root key, 15 ms attack, nothing else | -44.4 dB | - | -40 dB |

`steady_state.py golden candidate first last` (in this directory, stdlib only) aligns a window inside a held note at a fractional lag; it shows
that the mapping itself agrees with FluidSynth to the oracle's floor: -52.6 dB (`sf2_single`, 8000..11000), -67.8 dB
(`sf2_keys`, 6000..8000), -67.9 dB (`sf2_loops`, 9000..12000), -58.5 dB (`sf2_stereo`, 6000..8000), -62.4 dB (`sf2_envelope`, sustain,
17000..20000), -66.1 dB (`sf2_filter`, the 2.96 kHz zone, 22000..24000), -38.0 dB (`sf2_filter`, the 10 dB peak, 31000..32500); -63.6 .. -78.1 dB after
fitting a gain.

### Why the whole-file residual is -40 dB and not lower (FluidSynth artefacts, none an engine or importer feature)

- **A constant 0.2-frame delay.** FluidSynth always runs a 2-pole low-pass at 0.45 x the sample rate on every voice ("the default SF2 filter that
  always runs", `src/rvoice/fluid_iir_filter_impl.cpp`), which delays by about 0.2 frame (4 microseconds). The integer alignment search cannot absorb it:
  with a fractional lag the steady state is at -52 .. -68 dB. The error grows with frequency (at 0.2 frame, a partial at 4 kHz is at -20 dB, one at 400
  Hz at -40 dB), which is why the filter case, with partials up to 4 kHz, is lower. Voices that have an SF2 cutoff of their own (the filter zones) are
  not delayed (best lag 0.00 in `steady_state.py`).
- **A 64-frame ramp at the start of every voice.** FluidSynth's mixer interpolates the voice amplitude linearly over each 64-frame block from the
  previous value (0 for a new voice), `fluid_rvoice_calc_amp` in `src/rvoice/fluid_rvoice.c`. The fixtures therefore give every zone an attack of at least
  15 ms (through `attackVolEnv`, or a sample that fades in over 10 ms) and let sustained samples end faded out: a note that starts at full level, or a
  sample that is cut dead at its end, is measured against that ramp instead of the engine (an instant-attack percussive file measured -14 .. -18 dB
  during development).
- **Envelope stages in whole 64-frame blocks** (`NUM_BUFFERS_*` in `src/synth/fluid_voice.c`) and a **minimum release** of -7200 timecents (15.6 ms,
  `FLUID_MIN_VOLENVRELEASE`): every release in the fixtures is -4800 .. -2400 timecents. The engine has its own 6 ms floor for a release; a SoundFont
  with an instant release (the default, 1 ms) sounds differently in the first 16 ms after a note off on both players.
- **MIDI timing.** FluidSynth's player works in whole milliseconds and 64-frame blocks: an event is applied one block (64 frames) late when its tick is
  a multiple of 3 (4 ms), sometimes two blocks late otherwise (measured: ticks 75, 76, 78, 79, ... one block, 77, 80, 83, ... two). The SMF has 375
  ticks per quarter note at 120 bpm, i.e. exactly 64 frames per tick; every event of every fixture sits on a multiple of 3 ticks (`Midi::note` aborts
  otherwise) and the engine's MIDI is the oracle's delayed by one tick (`kOracleLagTicks`). The `lag` negative test shows the harness notices one more block.

### Where FluidSynth departs from the SoundFont specification (compensated in the oracle's copy of the SF2)

The importer follows the specification to the letter. These are FluidSynth behaviours that differ from it; `sf2_fixtures.hpp: for_oracle()` rewrites
the oracle's copy of every fixture so that both players render what the specification says (`sf2_suite_test --emit`; the engine always imports the
unmodified file). Sources are the FluidSynth 2.6.1 tree built by `tools/oracle/build-fluidsynth.sh`.

| FluidSynth | specification | compensation |
|------------|---------------|--------------|
| `initialAttenuation` is multiplied by 0.4 (`EMU_ATTENUATION_FACTOR`, `src/sfloader/fluid_defsfont.c`: "most existing soundfonts expect exactly this (strange, non-standard) behaviour") | the value is the attenuation in centibels (section 8.1.3 #48) | attenuation x 2.5 (fixture values are even) |
| the sustain level is `1 - cB/1000` of a volume envelope that spans 96 dB (`FLUID_PEAK_ATTENUATION` 960, `src/synth/fluid_voice.c`): a depth of 0.96 x cB | the sustain is the attenuation in centibels | sustain x 25/24 (fixture values are multiples of 24) |
| decay and release take their time for 96 dB (the same 960 cB range) | "a 100% change of the volume envelope", whose full scale is 1000 cB = 100 dB | those timecents -71 (x 0.9598 in time), applied once to the instrument-level value (`for_oracle` aborts if a preset zone sets them) |
| an always-on output filter with a gain of 1/sqrt(0.707) = +1.51 dB (`src/rvoice/fluid_iir_filter_impl.cpp`, `fluid_iir_filter_q_from_dB`) | none | the master gain of the oracle is `-g 0.8409` = 2^(-1/4) (README of `tools/oracle`), checked by `sf2_calibration` |
| a left or right sample is played with the zone's pan only | the same here (stereo fonts carry -500 / +500 in the zones) | none |

**What this comparison proves, and what it does not.** The importer follows the literal SoundFont 2.04 specification. FluidSynth deviates from it
on the attenuation scale (a factor of 0.4: 100 cB are 10 dB in the importer and 4 dB in FluidSynth, 60 % less), on the sustain range (depth x 0.96),
on the reference range of decay and release (96 dB instead of 100) and in its output filter gain (+1.51 dB). The null test therefore compares the
engine playing the ORIGINAL file against FluidSynth playing an ADAPTED copy in which those four quantities are pre-compensated. It validates the
plumbing (envelope timing and shape, pan, filter, loop points, tuning, sample data, key and velocity mapping), and it can only be as good as the
adaptation; it does NOT validate FluidSynth compatibility on the four deviating quantities, and an SF2 played in FluidSynth itself will sound different
from the same file played through the importer in exactly those four ways. Those quantities are therefore checked independently of any oracle by
analytic assertions in `format_sf2_test` (`spec_table_tests`: 0.1 dB per centibel of attenuation, the sustain level `100 x 10^(-cB/200)` %, a
100 dB reference for decay and release, the pan law and pan units), written from the specification text with their own formulas.
The master gain `-g 0.8409` is not part of the file adaptation: `sf2_calibration` (a plain looped note, no attenuation, pan, filter or envelope shape)
checks it on its own with its own tolerance (-40 dB: a gain error of more than about 0.08 dB fails it).

### Where the importer or the model cannot follow (documented gaps, measured)

- **A filter without resonance (Q 0 .. 3 dB).** `initialFilterQ` is the height of the peak over a non-resonant filter: Q 0 means Butterworth (Q 0.707,
  -3 dB at the cutoff), which FluidSynth implements. The model's low-pass is the RBJ biquad with a resonance of 0 .. 40 dB, i.e. Q >= 1, so the importer
  maps `q/10 - 3.01` dB clamped at 0: a filter with less than 3.01 dB of peak is played with Q 1 (+3 dB at the cutoff, a small hump below it).
  `sf2_filter_q0` measures it on content at the cutoff: -9.2 dB, steady state -12.6 dB with a gain error of 2.1 dB. The case is a guard (tolerance
  -7 dB): widening the model's resonance range below 0 dB would need a bundle format change, which this story does not make. Zones with a peak of
  3.01 dB or more (`sf2_filter`) have no such error.
- **Gain compensation of resonance.** The specification takes half of the peak height off the level of the whole signal (section 8.1.3 #9); the importer
  puts `q/20` dB into the region volume (FluidSynth's normalisation `1/sqrt(Q)`, which it computes from the Q after subtracting 3.01 dB, is the same
  once the always-on filter's +1.51 dB is calibrated away).
- **Not in the model** and reported as ignored (never skipped): LFOs, the modulation envelope, non-default modulators, effects sends, `scaleTuning`,
  `exclusiveClass`, `keynum`/`velocity`, key-number scaling of the volume envelope, sample end offsets (`librarian/README.md`). The fixtures contain none.
- **Shared samples are stored once per preset** (the bundle format keeps samples per instrument): a GM bank whose presets share samples is stored
  larger than the SF2 file's sample data.
- **The default velocity -> filter cutoff modulator** (-2400 cents at velocity 0) is not played: the cutoff is fixed, so filtered presets sound brighter
  than in an SF2 player at velocity below 127. It is reported once per preset that has a filter (`ignored_sf2_feature`). The fixtures play filters at
  velocity 127, where it has no effect.
- **Sample end offsets** (`endAddrsOffset`, `endAddrsCoarseOffset`) are not applied (the model has no sample end) and are reported as ignored; start
  and loop offsets are applied and validated.

## Negative tests

They prove the harness can fail: `sf2_null_neg_gain` (a 1 dB gain error), `sf2_null_neg_lag` (the engine's MIDI one more block late) and
`sf2_null_neg_strip_<case>` (the SF2 imported without its filter, pan, volume-envelope, attenuation and tuning generators: the filter case, the envelope
case, the stereo case, the kit, the keys and the single file all have to fail on residual). `sf2_render_cli` renders every preset of the bank
and checks the renderer's SF2 error paths.

## Regenerating the goldens

```
cmake --build build --target sf2_suite_test
tools/offline-render/tests/sf2_suite/regenerate_goldens.sh build        # all cases, or: ... build sf2_keys sf2_kit
```

It needs the FluidSynth oracle (`tools/oracle/build-fluidsynth.sh`: network, git, cmake, libsndfile; about two minutes). If the oracle cannot be built,
stop: goldens are never written by hand. A regenerated golden that differs from the committed one means the oracle, the suite or the platform changed:
review before committing.

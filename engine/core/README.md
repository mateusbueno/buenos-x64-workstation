# engine/core

Role: platform-independent engine (DSP, voices, pool arbiter, engines, event processors). No OS dependencies.

Dependency rule: may include and link only `engine/core`, `engine/ports` and the C++ standard library. Enforced by `tools/ci/check_core_deps.py` in CI.

Block loop modules (Story 1.8): `ump` (UMP-only events), `spsc_queue`, `arena`, `engine` (static engine interface and reserved type IDs Sample=1, Organ=2, FM=3, VA=4), `engine_registry`, `test_tone_engine`, `block_renderer` (64-frame stereo 48 kHz serial loop; events beyond the block are deferred to later blocks).

## Sample engine (Story 1.15)

`SampleEngine` (`sample_engine.hpp`, type id Sample=1) plays one fully RAM-resident instrument behind the `Engine` interface. It
reads the instrument through the `engine/ports` sample-source port (`velora/ports/sample_source.hpp`: region table + planar float PCM)
and has no OS, file or formatlib dependency; `engine/host/sample_bundle_loader` fills the port from a bundle. Bind it with
`BlockRenderer::bind_engine()` and route channels to it with `set_channel_engine(ch, kSample)`; until it is bound the Sample type
stays `ENGINE_NOT_AVAILABLE` (FM/VA remain so). `set_instrument()`/`clear_instrument()` are setup time (they allocate, and must not
run concurrently with audio); `start_voice`, `stop_voice`, `set_param`, `render`, `voice_info` never allocate, lock or block
(`rtsan_sample_engine_test`: 10,000 blocks of mixed traffic under RealtimeSanitizer). Generation swaps and partial residency are
Stories 1.16/1.17.

Behaviour (SFZ subset of Story 1.13; sfizz 1.2.3 is the reference, the null tests in `tools/offline-render` prove it):

- **Region selection**: per note, regions in table order whose key range holds the note, 7-bit velocity in `[vel_lo, vel_hi]`
  (the UMP 16-bit velocity is rounded to the nearest 7-bit step), `trigger=attack`. Every matching region plays (layers).
- **Round robin**: each region counts the note ons whose key is in its range (regardless of velocity/random) and plays when
  `counter % seq_length == seq_position - 1`; counters are per region and start at 0, so position 1 plays first. A release region
  uses the state left by the latest note on of its key.
- **Random**: one draw in `[0, 1)` per note on and per note off from a seedable xorshift64* (`Config::seed`, `set_seed`,
  `kParamSeed`); a region plays when `lorand <= r < hirand`. Same seed and events give the same render.
- **Pitch**: `cents = 100 * (note - pitch_keycenter) + tune + 100 * transpose`, ratio `= 2^(cents/1200) * sample_rate / engine_rate`
  in single precision (as sfizz), position kept in double. The first frame reads the sample at `offset` exactly, then advances by
  the ratio per frame. Interpolation: `kHermite3` (4-tap Catmull-Rom, sfizz quality 2, default) or `kLinear` (sfizz quality 1);
  taps outside the sample read as 0.
- **Cost units** per voice (`cost_units_per_voice()`): 1 unit of envelope/gain/pan work + 1 per interpolation tap, so Hermite = 5,
  linear = 3 (a test-tone voice is 1). Relative figures, to be calibrated on the board in Stories 1.18/1.19.
- **Gain and pan**: `gain = vel^2` (vel = vel7/127, SFZ `amp_veltrack=100`, computed as `1 - (1 - vel^2)` in float like sfizz) x
  `10^(volume/20)` x envelope. Pan: sfizz's equal-power law, a 4095-entry cosine table indexed by `round((pan/100 + 1)/2 * 4094)`;
  a mono sample then gets +3.01 dB (`x1.41254`, so centred mono is 0.99881), a stereo sample goes through pan, an identity width stage
  and a centre position stage, +6.02 dB (centred stereo is 0.99763).
- **Amplitude envelope** (`ampeg_*`), a sample-by-sample port of sfizz's `ADSREnvelope`, single precision, state per voice:
  `delay` frames at `start` (percent), linear `attack` (`v += 1/(rate*t)`, the first attack frame is `start + step`), `hold` frames at
  1, exponential `decay` (`v *= exp(-9/(t*rate))` down to `sustain`, so `t` is the time of nine time constants), `sustain` held
  (a level at or below 0.0032 releases by itself), exponential `release` after note off (`v *= exp(-9/(t*rate))`, `t` floored at
  6 ms) until `v <= 1e-4`, then a 50 ms linear fade-out, then the voice frees. A zero decay/attack time is instantaneous. The model
  cannot tell an unspecified `ampeg_release` from an explicit 0, so 0 means sfizz's default of 1 ms (-> 6 ms floor); an explicit
  `ampeg_release=0` (sfizz: 50 ms linear fade) is therefore not reproduced. A note off during the `delay` stage drops the voice
  without it ever sounding.
- **Loops**: `loop_continuous` wraps `idx > loop_end` by `loop_end + 1 - loop_start` (inclusive end, fractional position kept,
  taps read the real samples past the loop end, no crossfade); `loop_sustain` does so until the envelope is released, then plays on
  through the rest of the sample; `one_shot` ignores note off (reported as `kSustained`); `no_loop` plays to the end. A loop range
  outside the sample disables the loop (sfizz).
- **Sample end**: when the position reaches `frames - 2` (not looping) the voice starts a 6 ms release on that very frame and holds
  the last frame's value while it fades (sfizz behaviour), then frees after the fade-out.
- **Release triggers**: on note off, `trigger=release` regions of the note fire with the note-on velocity and pitch, but only while
  an attack voice (any state until it frees) for a note inside the region's key range and velocity range is alive (sfizz's
  `rt_dead=no` rule). They are not tied to the key (note off ignores them) and are reported as `kReleasing`.
- **Errors**: pool full -> `kPoolExhausted` (the regions that fit still start; stealing arrives in Story 1.19); a note off with
  no held voice and no release region fired -> `kVoiceNotFound`; a note with no region -> `kOk`, silence. A region whose sample is
  absent, empty, of an unsupported shape or whose `offset` is beyond the sample is *unplayable*: `unplayable_regions()` counts them
  at `set_instrument`, `skipped_unplayable_starts()` counts note ons that would have used one; nothing is played for them.
- **Validation at `set_instrument`**: a region is also *unplayable* (counted, never started) when an `ampeg_*` time is non-finite, negative
  or above 100 s, a level/`pan`/`tune` is non-finite, `volume` is outside -144..48 dB, `lorand >= hirand`, `seq_position` is outside
  `1..seq_length`, or the pitch ratio at the ends of its key range leaves `[1/4096, 1024]` (this also bounds `tune`/`transpose`).
  Loop wrapping is a modulo (bounded cost); release-trigger voices never loop; `kParamGain` is clamped to `[0, kMaxGain=16]` and
  `kParamSeed` ignores negative, non-finite or `>= 9e18` values. Note-on velocity (for release triggers) and "attack voice alive"
  are tracked per (channel, note); round-robin counters stay per region. A release region that finds no free voice makes
  `stop_voice` return `kPoolExhausted`.
- Out of scope (later stories): streaming/heads (1.16/1.17), parallel render (1.18), arbiter stealing (1.19), filters/effects,
  keyswitches, CC modulation, `amp_veltrack`/`pitch_veltrack`, `rt_decay`, SF2.

Known differences from sfizz that the null tests work around (not engine features):

- sfizz applies a default master volume of -7.35 dB and default MIDI controllers (CC7 = 100/127 as `amplitude`, squared; CC10 pan
  centred; CC11 expression 1). The tests send CC7 = 127 and set the engine's output gain to -7.35 dB (`kParamGain`).
- sfizz 1.2.3 holds every voice's sample position for one frame at the first block boundary after the block the voice started in
  (`age = min(age + block - trigger_delay, 0)` never leaves 0). `Config::oracle_block_frames = 64` (test only, default 0 = off)
  reproduces that hold, aligned with the engine frame clock, so the residual measures the engine and not sfizz's stutter.
- sfizz loses the note off when a voice's start offset plus `ampeg_delay` ends exactly on a block boundary (its ADSR leaves `delay`
  at 0 instead of -1) and then sustains forever; `loop_sustain` checks "released" per 64-frame block. The suite avoids both
  (note-offs for `loop_sustain` sit on block boundaries; see `tools/offline-render/tests/sample_suite`).

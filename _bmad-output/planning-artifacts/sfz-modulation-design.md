# SFZ modulation design note (Story 13.9)

Written before any code of Story 13.9, from the sfizz 1.2.3 source (commit `4e70dc0b`, `tools/oracle/.work/sfizz/src/sfizz`) and the corpus reports of Stories 13.7 and 13.8
(`build/corpus138/all/report.json`). Section 8 (measured cost) is the only part filled in after the implementation; everything else was fixed first and the implementation
follows it.

## 1. Scope decision taken on measured need

`_bmad-output/planning-artifacts/sfz-scope-decision.md` (still an unratified proposal) deferred tiers 8 (region EQ) and 9 (LFO/EG modulation) to "decide after 13.8 from the
corpus report". The report after 13.8 (full `corpus.py run`, 12 libraries) shows, per unsupported opcode, the number of files that carry it:

| Library | Blocked by this tier | Other blockers left after this story |
|---|---|---|
| Sonatina (747 files) | `amplfo_delay/depth/fade/freq` 256, `eq1_bw/freq` 86, `lfo01_*` 86 to 87 | `delay` 536, `offset_random` 164, `delay_random`, invalid `amplitude`/`gain_cc1`/`group_volume` values |
| Headroom Piano (4 files) | `eq1_*` / `eq2_*` (freq, bw, gain with `_oncc` and `_curvecc`) | `amp_veltrack_oncc73`, `width_oncc76/79`, `width_curvecc76/79` |
| Karoryfer Meatbass / Bear Sax / Weresax | `lfoNN_*` (pitch, volume, freq, delay, fade, eq1freq, eq1gain, cutoff targets), `egNN_time1_oncc`, `eq1_*`, `pitcheg_depth_oncc` | `offset_random`, `pitch_keytrack`, `var01_mod` |

Taken up on measured need: files remain blocked by this tier, and Sonatina's `amplfo_*` (256 files) and Headroom's `eq1/eq2` are the largest blocks that are not shared with
another unimplemented tier. The story is **not cancelled**. Because the full tier is too big for one story (the generic `lfoNN` / `egNN` matrix with CC-driven frequency,
phase, delay and depth that sfizz follows continuously is the largest piece), this story lands a coherent subset (section 2) and defers the rest with reasons (section 9).
Honest consequence: **no library of the corpus imports completely after this story** (every library listed above has at least one other blocker), see the spec for the numbers.

## 2. The subset that lands

| Family | Opcodes accepted | Evaluated |
|---|---|---|
| Region EQ, 3 bands | `eq1_` .. `eq3_` `freq`, `bw`, `gain`, `type` (`peak`, `lshelf`, `hshelf`), `vel2freq`/`veltofreq`, `vel2gain`/`veltogain`; `freq`, `bw`, `gain` `_oncc<N>`/`_cc<N>`/`cc<N>`/`_curvecc<N>` | band frequency, bandwidth and gain at voice start, constant for the life of the voice |
| Amplitude / pitch / filter LFO | `amplfo_`, `pitchlfo_`, `fillfo_` `depth`, `freq`, `delay`, `fade`, `wave` (0..7) | per frame, voice local |
| Pitch / filter EG | `pitcheg_`, `fileg_` `delay`, `start`, `attack`, `hold`, `decay`, `sustain`, `release`, `depth`, `vel2depth`/`veltodepth` | per frame, voice local, same ADSR as `ampeg_` |

Fixed targets (sfizz `Region.cpp` lines 877 to 925): amplitude LFO to **Volume** (dB), pitch LFO and pitch EG to **Pitch** (cents), filter LFO and filter EG to the **cutoff of
the first filter** (cents, `FilCutoff` with filter id 0). The LFO / EG of a target that does not exist is dropped with a warning: `fillfo_*`/`fileg_*` on a region without
`cutoff` modulate nothing in sfizz either (no filter object).

## 3. Modulation matrix (what sfizz does, and what the engine does)

sfizz routes every source to a target through `ModMatrix::getModulation(target)` (`modulations/ModMatrix.cpp` lines 378 to 490): for each connection, `buffer[i] = sourceDepth *
source[i]`, summed for *additive* targets (Pitch, FilCutoff, Eq*, in cents or dB) and multiplied for *multiplicative* targets (Amplitude). Volume is additive in dB. For a per-voice
source `sourceDepth += triggerValue * velToDepth` where the trigger value is the note-on velocity `vel / 127`.

Engine rule, per frame f of a voice that has the corresponding source:

```
pitch_cents(f)  = pitchlfo_depth * lfo_p(f) + (pitcheg_depth + vel * pitcheg_vel2depth) * eg_p(f)   // added to the bend; ratio *= 2^(pitch_cents / 1200)
volume_db(f)    = amplfo_depth * lfo_a(f)                                                        // gain *= 10^(volume_db / 20), on top of volume / ampeg
cutoff_cents(f) = fillfo_depth * lfo_f(f) + (fileg_depth + vel * fileg_vel2depth) * eg_f(f)       // + the cutoff_oncc entries (13.8), constant
cutoff(f)       = clamp(cutoff_base * 2^(cutoff_cents(f) / 1200), 0, 20 kHz)                       // sfizz FilterHolder::process
```

Sum rules: all terms are added in cents or dB in float, exactly as the sfizz buffers, then converted once (`centsFactor`, `db2mag`). The bend (13.7) and the CC `tune_oncc` (13.5)
are separate additive terms of the same Pitch target; the engine multiplies their factors, which is the same number up to float rounding. `sourceDepth` defaults to **0** in
sfizz (`Region::Connection`): an `amplfo_freq` without `amplfo_depth` is a silent connection, so the importer stores no LFO then (and warns).

Not modelled: the CC-controlled depths (`amplfo_depth_oncc`, `pitcheg_depth_oncc`, ...), `*_depthchanaft`, `*_depthpolyaft`, the CC-controlled frequency (`amplfo_freq_oncc`).
They are rejected at import (`unsupported_features`), never ignored.

## 4. LFO (sfizz `LFO.cpp`, the SFZ v1 form)

- **Phase.** `phase_0 = 0` at voice start (the descriptor's `lfoN_phase` does not exist for `amplfo_` / `pitchlfo_` / `fillfo_`). Per frame the output is `eval(wave, phase)`
  of the phase *before* the increment, then `phase = wrap(phase + freq / sample_rate)` in float. `ratio` is 1. A `freq` of 0 holds the phase at 0 (the sine/triangle output is 0).
- **Delay.** `ceil(sample_rate * delay)` frames of output 0 and no phase advance (sfizz skips those frames before `generatePhase`); `fade` starts after the delay.
- **Fade.** If `fade > 0`: `fadePosition` runs from 0 to 1 in steps of `1 / (sample_rate * fade)`, the output is `out * fadePosition` (position read before the step); else the gain is 1.
- **Waveforms** (value `-1..1`, pulses `0..1`; `loPulse = 0`, as ARIA): 0 triangle (`4p` below .25, `2 - 4p` to .75, `4p - 4` above), **1 sine (the default of these
  three LFOs, a parabolic approximation `x = 2p - 1; -4x(1 - |x|)`, not a true sine)**, 2 pulse75, 3 square, 4 pulse25, 5 pulse12.5, 6 ramp (`2p - 1`), 7 saw (`1 - 2p`).
  Wave 12 (random sample and hold) uses sfizz's global RNG; the engine has no way to reproduce it, so it is rejected (`unsupported_value`).
- Default `freq` 0, `delay` 0, `fade` 0. Ranges clamped as the importer does for every opcode: freq 0..100 Hz, delay and fade 0..30 s, depth by target (dB -10..10, cents
  -1200..1200).
- **Smoothing: none** for the LFO value itself. The sfizz gain stage does smooth the *final* gain (`gainSmoother_`) but `config::gainSmoothing` is 0 in 1.2.3
  (`Config.h` line 41), so a modulated volume reaches the output unsmoothed; the engine matches.

## 5. EG (sfizz `ADSREnvelope`, shared with `ampeg_`)

`pitcheg_*` / `fileg_*` use the same `ADSREnvelope` as the amplitude EG, which Story 1.15 ported sample by sample (`SampleEngine::env_next`): states delay, attack (linear,
`+= 1 / (sr * attack)`), hold, decay (`*= exp(-9 / (decay * sr))`), sustain (with the 50 ms transition to the sustain level), release (`*= exp(-9 / (release * sr))`,
then a 50 ms linear fade), done. Differences of the descriptor, from `EGDescription`: the **defaults** are all 0 except `sustain` 100% (the amplitude EG alone has a release
of 1 ms: `Region::Region`), so a bare `pitcheg_depth` gives an instantaneous attack, a full-level sustain and an immediate end of release. `start` 0..100%; `sustain` 0..100%;
`free_running` (sustain at or below 0.0032) releases by itself. The engine reuses `Env` / `env_next` unchanged for both; the EG is released when the voice's amplitude EG
enters release (key up, pedal, choke, end of sample), exactly when sfizz calls `release` on all three. Its value is `depth * eg(f)`; after `done` it is 0.

Not modelled: `*_vel2attack/decay/delay/hold/release/sustain` on pitch and filter EGs, `*_oncc` EG times, `*_dynamic`.

## 6. Cutoff modulation, control rate and smoothing (filter LFO / EG only)

`FilterHolder::process` builds `cutoff[i] = clamp(baseCutoff * centsFactor(mod[i]))` per frame, and `Filter::processModulated` reconfigures the Faust biquad every
`config::filterControlInterval = 16` frames with `cutoff[frame]` of the first frame of the chunk. The Faust code then **smooths the normalised coefficients per frame**
with a one-pole of pole `exp(-1000 / sample_rate)` (1 ms time constant): `c[n] = pole * c[n-1] + (1 - pole) * c_target`. The engine does the same for a filter that has a
`fillfo` or `fileg`: every 16 frames of the *voice's own clock* (not of the host block: a different block size must not change the sound) it computes target coefficients with
`make_sample_filter` and the biquad runs on coefficients smoothed per frame. A filter without modulation is untouched (static coefficients, bit identical to 13.8).
The control chunks of sfizz restart at each host block; the engine's restart at each voice start. With the null tests' 64-frame blocks the grids coincide for a voice that
starts on a block boundary and differ by at most one chunk (16 frames) otherwise: documented difference, inside the tolerance.

## 7. Region EQ band model (sfizz `EQPool.cpp`, `SfzFilter.cpp`, `gen/filters/sfzEq*.hxx`)

- A band is a biquad of one of three RBJ forms, run in double: **peak**, **low shelf**, **high shelf**, with `A = 10^(gain_dB / 40)`, `w0 = 2 pi f / fs`, and the bandwidth in
  octaves `bw` turned into the Q-like term `alpha_q = 0.5 / sinh(ln(2) / 2 * bw * w0 / sin(w0))` (floored at 0.001). Clamps from the generated code: `f` 1..20000 Hz (the
  engine also keeps it below 0.49 fs), `bw` 0.01..12, `gain` -120..60 dB. The stored (validated) ranges are the ones the DSP honours: `bw` 0.01..4 octaves, `gain` -96..+60 dB; the importer clamps to them and logs an ignored value. A band with `gain` 0 is transparent (peak: `A = 1`, numerator equals denominator).
- Per voice: `freq = eqN_freq + vel * eqN_vel2freq (+ oncc entries)`, `bw = eqN_bw (+ entries)`, `gain = eqN_gain + vel * eqN_vel2gain (+ entries)`, evaluated at voice start
  (`EQHolder::setup` for the velocity part, the mod matrix for the CC part: the engine reads the controller once). Defaults `bw` 1 octave, `gain` 0, type peak.
- **Default frequencies** (Synth.cpp lines 855 to 866): `eq1` 50 Hz, `eq2` 500 Hz, `eq3` 5000 Hz, applied *positionally*: eq2's default applies only if eq1's frequency is still
  0 as well, and eq3's only if both are (a quirk of that code). The importer replicates it; the stored `freq` is final.
- A region keeps `max band number` bands (a region that only sets `eq3_*` has three; the first two are transparent peaks and cost nothing in the engine: skipped).
- Position in the chain: after the filters, before panning, in the same stage as the filters (`filterStage*`, Voice.cpp lines 1020 to 1065). Smoothing: only the one-pole
  of coefficients, which with constant targets is the identity, so a voice-start-constant band needs none.
- Not modelled: continuous CC following of `eqN_*_oncc` (the CC is read at voice start), `eqN_*` LFO/EG targets of the generic matrix, band types other than the three.

## 8. Cost per voice (target: the declared per-voice units)

Declared today (`sample_voice_cost_units`): stereo Hermite voice 5 units, +2 filter, +1 crossfade, +3 second filter (= 11 for the macOS host). `sample_bench` measures
`1 unit = 0.077 us per 64-frame block` on the development Mac (baseline run before this story: plain stereo 0.384 us, filter +2.17 units, tuned filter 0.552 us).

Estimates from operation counts (before measuring): LFO per frame ~ 12 flops (+ a `pow` for the dB gain, ~20 ns); EG per frame ~ the amplitude EG (~3 units / 64-frame
block for the EG state machine); a stereo peak EQ band = one biquad pair, about the cost of one filter (2.2 units); the filter-modulation coefficients (sin, cos per 16
frames, +5 smoothing multiplies per frame) are the dominant new cost, about 3 to 4 units. The declaration therefore has to grow: a new per-engine declaration
`Config::modulated_voices` with `kSampleModCostUnits` (the worst case voice: all three LFOs, both EGs, a cutoff that follows them with per-frame coefficient smoothing, three EQ
bands), fixed per engine exactly like `second_filter_voices`, so the default and the extended engines keep every existing floor and budget.

**Measured after the implementation** (`build/engine/core/sample_bench`, host Mac, -O3, 64-frame blocks, stereo Hermite voice = 5 units = about 0.41 us; extra units over the plain
voice, ranges over repeated runs): amplitude LFO +2.7 .. 3.0, pitch LFO +2.7 .. 3.2, pitch EG +2.7 .. 3.1, one EQ band +2.5 .. 3.0, three EQ bands +6.7 .. 7.2, a filter LFO or EG on a
filter +3.3 .. 3.8 (over the filter's own 2.0); everything at once (three LFOs, two EGs, the moving cutoff, three bands, two filters, a crossfade) 26.4 .. 27.3 units over a zero base. The
first estimate (12 units for the modulation on top of the 11 of the extended two-filter voice, 23 in all) was too low; the declaration is `kSampleModCostUnits` = 18, 29 units for the
worst-case voice. Starting a voice with everything costs 0.17 us (0.10 us for three bands), once per note. Host figures, not calibrated on the board (Stories 1.18/1.19 own that).
The plain voice and every existing engine declaration are unchanged (5 / 8 / 11).

**What the oracle showed that the sfizz source alone did not make obvious** (all in the engine README and the suite README, with the fixtures that avoid them):
1. sfizz steps the playhead into frame f with frame f's pitch (`jump[0] = 0`, then `cumsum`): a pitch EG that moves one octave over its attack is a whole sample off if the step
   uses the previous frame's value. The engine corrects the step at each frame.
2. A voice that starts inside a 64-frame block reads its pitch modulation from the delayed sub-span of block-aligned buffers: the pitch LFO / EG lag by the offset in that first block.
3. `ADSREnvelope` releases only when `delay < 0`; an EG whose delay ends exactly at the end of a render block (always, with 1-frame blocks and a non-zero delay) never releases.
4. The decay of `ADSREnvelope` runs on to the exact sustain level (no 0.001 threshold ramp except at a block end inside the threshold); a pitch EG shows the difference as a whole
   sample of position. The amplitude EG's port keeps its ramp (a gain does not show it).
5. The Faust biquads read their smoothed coefficients one or two frames late for some terms; a cutoff step differs for about 3 ms unless the engine uses the same timing.

## 9. Determinism, real time and partitions

- Every state (LFO phase, delay and fade counters, two `Env`, smoothed coefficients, EQ biquad state) lives in the `Voice` struct, written only by that voice's render. No
  allocation, lock, syscall or engine-wide write in `render_voice` (the rule in the comment above `render_partition`). Coefficient maths (`sin`, `cos`, `pow`) runs in the audio
  thread at voice start and every 16 frames of a modulated filter; no random draw is added (waveform 12 is rejected), so the RNG sequence of existing instruments is unchanged.
- The voice-to-partition assignment is `slot % 3` and every voice renders in its own order into its partition's scratch, so 1 versus 3 worker threads is bit identical by the
  existing argument; the new per-voice state does not change it. A test renders a modulated instrument with 1 and 3 threads and compares bit for bit.
- A region without any of these opcodes takes the old branches (`modded` / `eq_on` false): same arithmetic, bit-identical output (a test compares against a render with the
  new code paths reachable but off).
- Stability: depths and times are bounded at import and again in `prepare` (a descriptor out of range makes the region unplayable); cutoff is clamped, band parameters are
  clamped as above, resonance/Q floors as in 13.8, every filter tick keeps the 13.8 finiteness guard.

## 10. Deferred, with the reason

`lfoNN_*` (generic LFOs: `freq`, `wave` with sub-waves, `phase`, `delay`, `fade`, `beats`, `count`, `ratio`, steps, and the targets `pitch`, `volume`, `cutoff`, `pan`, `eqNfreq`,
`eqNgain`, `eqNbw`, with `_oncc` depths): needs the continuous per-block CC following sfizz does and random S&H; `egNN_*` (flex EG with breakpoint times, levels, `time_oncc`):
the largest piece (Meatbass / Bear Sax use `eg06_time1_oncc` thousands of times); CC-driven depth and frequency of the dedicated LFOs and EGs; per-block following of the
`eqN_*_oncc` entries; `var01_mod`; waveform 12. All are rejected at import with `unsupported_features` and file:line, none is ignored silently.

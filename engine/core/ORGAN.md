# Tonewheel organ engine (Stories 2.2, 2.3, 2.4)

`OrganEngine` (`organ_engine.hpp`, `tonewheel_tables.hpp`, type id Organ=2, engine name "tonewheel organ") is a clean-room
tonewheel organ: one shared wheel generator per part and one voice per key, 73 keys x 9 drawbars sounding at once, never
voice-stolen. This file is the audit trail of where every number comes from.

## Clean-room statement

The DSP was written from public physical and musical facts only (list below). No GPL or other organ implementation source
(setBfree included) was read, copied or translated. An organ implementation may later be used only as an unshipped oracle
(Story 2.13), never as source. User-visible and log strings never contain the three forbidden product names; `organ_tests`
greps the organ sources, the tables, this file and `proto/params.json` for them.

## Facts the tables come from

1. **Wheel frequencies.** A tonewheel generator is a set of 91 rotating tone wheels, one per semitone over 7.5 octaves
   (about 32.7 Hz to 5920 Hz). The mechanical gear/pinion ratios of a real instrument approximate twelve-tone equal
   temperament; we use the exact equal-tempered values: wheel 1 = C1 = MIDI note 24, wheel `w` = MIDI note `23 + w`,
   `f = 440 Hz * 2^((midi - 69) / 12)` (A4 = 440 Hz is wheel 46). Real gear ratios deviate by a few cents; modelling that is
   outside this story.
2. **Keybed.** 73 keys, E1 (MIDI 28) to E7 (MIDI 100) (a six-octave-and-a-note keybed). The 8' drawbar of key `n` uses wheel
   `n - 23`.
3. **Drawbar footages.** 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' are pipe-organ footages: the 8' drawbar sounds
   the key's own pitch; 16' sounds an octave below; each shorter footage is the corresponding harmonic of the 16' pitch
   (2nd, 3rd, 4th, 6th, 8th, 10th, 12th, 16th harmonic of 16', i.e. ratios 0.5, 1.5, 1, 2, 3, 4, 5, 6, 8 relative to 8').
   On equal-tempered wheels the exact harmonics 3, 5, 10 and 12 of 16' (5 1/3', 2 2/3'... ) are placed on the nearest wheel:
   semitone offsets from the 8' pitch `{-12, +7, 0, +12, +19, +24, +28, +31, +36}`. The worst deviation from the exact harmonic
   is 13.7 cents (1 3/5', the major third); `organ_tests` asserts < 14 cents.
4. **Foldback.** The wheel set has a fixed range. A footage that would need a wheel below wheel 1 or above wheel 91 is moved
   by whole octaves (12 wheels) until it falls into 1..91: up when below, down when above. So at the top of the keyboard the
   higher footages repeat the pitch class an octave (or two) lower; at the bottom the 16' repeats the 8' pitch class an
   octave higher. The rule is `tonewheel::fold_wheel`, tested for every integer in -200..300 and at both ends of the keybed
   (E7: 1' -> wheel 89, 2' -> 89, 1 1/3' -> 84; E1: 16' -> wheel 5, the same as its 8').

## Structure

- **Phase 2 hook.** `Engine::begin_block(frames)` (default no-op) is called once per engine per block by `BlockRenderer`
  after the event queue is drained and before any voice renders. `OrganEngine` produces the block of all 91 wheels for every
  enabled part into a fixed table (`wheels_`: parts x 91 x 64 floats). Each wheel is a unit phasor rotated by a per-frame
  complex multiplication (double precision, renormalised each block, no allocation, no trig in the loop). A wheel has one
  phase per part regardless of how many keys use it (tested: two keys on the same wheel add in phase to twice the amplitude;
  a key started later reads the running wheel).
- **Voices (phase 3).** A voice is one key. It writes only its own state (`env`, state, slot) and sums its nine wheels
  scaled by the part's per-frame drawbar gains into a per-part scratch mix; the part mix is soft-limited and added to the
  output. `render()` can be called for contiguous segments of a block (the block loop splits at event frames).
- **Drawbar parameters.** Registry ids 100..108 (`drawbar_16` ... `drawbar_1`, performance, part scope, 0..8, CC 70..78, default
  `008000000`). The block loop forwards each applied change to the part's engine through `Engine::set_part_param` at the
  event's frame (between render segments); on a generation change the changed initial values are forwarded, and
  `set_channel_engine` syncs the current values of a part when an engine is routed to it.
- **Drawbar curve.** Step 0 is silent; step `s` in 1..8 has gain `10^(-3 dB * (8 - s) / 20)`: 3 dB per step, step 8 = 1.0.
  Strictly monotonic. Out-of-range values clamp to 0..8; non-finite values are ignored.
- **Smoothing.** A change starts a linear gain ramp at exactly the event's in-block frame (the block loop calls
  `set_part_param` between render segments), from the current (possibly mid-ramp) gain to the new target. The ramp is a
  fixed `kDrawbarRampFrames` = 240 frames (5 ms at 48 kHz, derived from `kSampleRate`), advanced one step per frame inside
  `render()` chunks, so it does not depend on block boundaries. It advances whether or not keys are down. The per-sample
  gain step is bounded by the ramp slope (no zipper); tested against an unramped reference render that matches exactly before
  the event frame. Drawbar gains (`Part::cur`/`left`) are advanced in the render phase and are per-part state: if parts are
  ever split across workers, one part must stay on one worker.
- **Voice gate.** A 1 ms attack and a 2 ms release gate (`kAttackFrames`, `kReleaseFrames`) only prevent clicks; this is not
  a key-click or percussion model (see Percussion and Key click below).
- **Level.** One drawbar at step 8 has amplitude `kVoiceScale` = 0.06; the part mix is linear below `kKnee` = 0.8 and bounded
  by 1.0 above (`tanh` shoulder), so a full chord at full registration stays within +-1.

## Percussion (Story 2.3)

Registry ids 110..113 (`percussion_on`, `percussion_harmonic` second|third, `percussion_decay` fast|slow, `percussion_volume`
soft|normal), part scope, performance class, arriving through `set_part_param` like the drawbars. CC 80 is `percussion_on`;
CC 84/85/86 (added in this story, the enums had no controller) are harmonic/decay/volume (value below 64 = first entry).

Public facts used (musical and service-documentation level, no organ source read):

1. Percussion is a short decaying tone at the 2nd or 3rd harmonic of the key pitch, i.e. the 4' (octave above) or 2 2/3'
   (twelfth above) footage. The tone comes from the same wheels as those drawbars (`kPercSecondDrawbar` = 3, `kPercThirdDrawbar` = 4,
   foldback included).
2. It is single-triggered: the percussion envelope fires when a key closes while no other key of the part is held, so playing
   legato does not retrigger it; releasing every key re-arms it. Here the envelope belongs to the voice that fired it (the
   triggering key); other keys of the part carry no percussion tone. A key that is still releasing (key up) does not count as held;
   a key held by the pedal in the arbiter does (the arbiter defers the note off, so the engine still sees it held; tested at block-loop level: pedal down, key, key up, second key gives the second key no percussion). Parts trigger independently.
3. Percussion on cancels the 1' drawbar (the 1' tone and the percussion share the highest busbar), and the NORMAL volume setting
   lowers the drawbar tones so the louder percussion keeps its place; SOFT leaves the drawbars alone and the percussion is lower.
4. Decay: FAST is a short pluck, SLOW lingers for about a second. Public descriptions give audible durations of roughly a quarter
   second and roughly a second; they do not give an exact curve.

Design values (an exponential envelope `exp(-t / tau)`; chosen to match those audible durations, to be refined against recordings in
Story 2.13; each is a named constant in `organ_engine.hpp`):

| constant | value | meaning |
|---|---|---|
| `kPercTauFastSeconds` | 0.06 s | tau of FAST; -40 dB after 0.28 s |
| `kPercTauSlowSeconds` | 0.20 s | tau of SLOW; -40 dB after 0.92 s |
| `kPercNormalGain` | 1.6 | peak amplitude of the percussion tone, in units of one step-8 drawbar |
| `kPercSoftGain` | 0.8 | SOFT = NORMAL - 6 dB |
| `kPercDrawbarGain` | 0.70794578 | every drawbar x -3 dB while percussion is on at NORMAL volume |
| `kPercRampFrames` | 240 | 5 ms linear ramp of the 1' cancel and of the drawbar lowering |

Behaviour details. The harmonic, decay and volume are latched into the voice at the press (a change mid-decay applies to the next
trigger); `percussion_on` is read at the press, and its two part-level effects (1' cancel factor, drawbar factor) are
`Ramp` values advanced in the part loop of `render_chunk` exactly like the drawbar gains (per-part state, same one-worker rule).
Turning percussion off lets decaying tones finish and brings the 1' and the drawbar level back over 5 ms. The decaying envelope is
flushed to 0 below 1e-6 (no denormals). Disabling a part (`set_enabled_parts`) deactivates its voices and with them their percussion envelopes and bursts; re-enabling starts clean, and the part's ramps simply freeze while it is disabled. Voices carry the percussion state, so a held key with finished percussion stays a plain
organ voice.

## Key click (Story 2.3)

Registry id 116 `key_click` (0..1, default 0.5, CC 82, part scope). Level 0 is off: exact silence added, no cost.

Public facts: a key click is the short broadband transient from the key contacts closing and opening, audible on the real
instrument at the start and end of every note, different from key to key and from press to press because the contacts bounce
differently. Model: one burst per contact event (the voice's onset and its release), each a first-difference of white noise
(a broadband, high-frequency-weighted click) under a fast exponential decay of 0.3..0.7 ms time constant, hard-ended after
`kClickFrames` = 192 (4 ms). Peak scale is `kClickGain` = 2.0 x level x per-key factor x per-press jitter, in step-8 drawbar units
(so level 1 is a clearly audible tick next to a single drawbar; the default 0.5 is subtle); the release burst is `kClickReleaseRatio`
= 0.6 of the onset burst. The click is added to the part mix next to the tone and is not shaped by the 1/2 ms declick gate; a
releasing voice stays alive until its release burst is out, then ends. The onset and release bursts are separate, additive and have their own noise streams: a release within 4 ms of the onset lets the onset burst finish (no discontinuity). When the voice pool is full the reclaim prefers a releasing voice with no click pending. `voice_info().level` includes the percussion tone and any pending burst.

Reproducible variation: noise comes from SplitMix64 (fixed-width integer arithmetic, identical on every platform). Per key: amplitude
factor 0.75..1.25 and decay time constant derived from a hash of the note number and the fixed `kClickSeed`. Per press: a stream
seeded from `kClickSeed`, the part's own press counter (so one part's clicks do not depend on other parts' activity), the note and the part, and a +-10 % level jitter. The same event sequence
therefore renders identically every run, while repeated notes differ. The level is read at the contact event (onset: at the press;
release: at the key-up), so a changed `key_click` applies to the next event.

## Vibrato/chorus scanner (Story 2.4)

Registry ids 114 `vibrato_chorus_on` (bool, CC 81) and 115 `vibrato_chorus_type` (v1, v2, v3, c1, c2, c3; CC 87 added in this
story, the enum had none: the 7-bit range is spread evenly, index = round(value / 127 x 5)), part scope, performance class. The effective
setting is `off` when `vibrato_chorus_on` is 0, else the type. Registry defaults: off, type c1.

Public facts used (musical and service-documentation level, no organ source read):

1. The vibrato/chorus unit is a tapped delay line (an LC ladder) of 9 sections with a scanner that sweeps across the taps at about
   412 revolutions per minute, i.e. 6.87 Hz. The moving read point makes the delay change continuously, which frequency-modulates the
   signal (vibrato). It is a delay modulation, not an amplitude one.
2. The three depth settings use 3, 5 and 9 taps of the line (V1/C1, V2/C2, V3/C3): deeper settings sweep a longer stretch of delay.
3. Vibrato settings (V) pass only the scanned signal. Chorus settings (C) mix the scanned signal with the unscanned one, which adds the
   slow phasing of a delay-modulated copy against the dry signal.

Design values (named constants in `vibrato_scanner.hpp`; to be refined against recordings in Story 2.13):

| constant | value | meaning |
|---|---|---|
| `kSweepHz` | 6.87 Hz | triangle sweep of the read point (one back-and-forth per period) |
| `kTapSpacingSamples` | 6 (0.125 ms) | delay between taps |
| tap intervals | 2, 4, 8 | V1/C1, V2/C2, V3/C3 sweep 3, 5, 9 taps: peak-to-peak delay 0.25, 0.5, 1.0 ms |
| `kChorusWet` | 0.5 | chorus output = 0.5 dry + 0.5 scanned |
| `kCrossfadeFrames` | 240 (5 ms) | crossfade when the setting changes |

The pitch deviation of a setting follows from the swing: the delay slope is `2 x swing x sweep rate`, so V1 is about +-6 cents, V2 +-12 and
V3 +-24 (a 6.87 Hz triangle). The read point moves continuously and the line is read with linear interpolation between the two nearest
samples (the taps define where the sweep starts and ends; reading between samples avoids the comb filtering of blending two taps 6 samples
apart). Fixed 64-sample delay line, no allocation.

Where it sits. Per part, on the summed organ signal (tones, percussion and key click), after the voice mix and before the soft limit.
One `VibratoScanner` per part: delay line, sweep phase (a 32-bit accumulator, deterministic) and crossfade belong to the part, so a part
must stay on one worker. The sweep phase starts at 0 when the engine is created and runs with the part's block time. After the last voice of a part ends the scanner keeps
running zeros through the delay line for 64 frames, so the delayed tail (up to 48 samples) drains instead of being cut; after that, and
while a part is disabled (`set_enabled_parts`), the scanner is `skip()`-ed: the sweep time advances, the delay line is cleared, so stale signal
never returns when a key starts or the part is enabled again. A setting change while the part has no sounding voice applies at once with no
crossfade (nothing to click; this also makes the frame-0 setup of an offline render exact). Off is bit-exact pass-through.

No click on switching. A change of setting (on/off or type), or a new change in the middle of a crossfade, interpolates the output weights
of the settings over 240 frames: `start` weights (those in force when the change arrived) to the one-hot target. The weights always sum to
1, so the output is continuous. Tested on every pair of settings at several sweep phases (largest sample step within 20 % of the dry tone's,
while a hard switch exceeds 2x), with rapid re-switching, and on a held 73-key chord cycled every 5.3 ms (within 25 % of the steady chord's).

## Leakage (Story 2.4)

Registry id 117 `leakage` (0..1, default 0.5, CC 83, part scope). 0 is off: the code path is not executed, so the output is exactly the
leakage-free tone.

Public fact used: on a tonewheel generator the pickups of neighbouring wheels pick up a little of each other's tone (magnetic and
electrical cross-talk), so every sounding note carries faint, fixed, unit-specific contributions of adjacent wheels.

Model: for each drawbar of each sounding key, the wheel that drawbar reads also receives its two neighbours, wheel - 1 and wheel + 1:
`wheel_signal + leakage x (a x wheel[w-1] + b x wheel[w+1])`, with `a`, `b` from a fixed table, all inside the drawbar gain and the voice
gate (so the drawbar curve, percussion cancel and key release act on the leakage too). The table has two entries per wheel (91 x 2). It is
generated at engine construction from `kLeakageSeed` (0x1EAC4A6E5EED2024) with SplitMix64 (the key click generator), drawing in wheel order,
side 0 then side 1, always one draw per entry: `gain = kLeakageMaxGain x (0.25 + 0.75 r)`, `r` in [0, 1); `kLeakageMaxGain` = 0.02 (-34 dB). The
two entries that have no neighbour (wheel 1 below, wheel 91 above) are 0. The table is the same on every run, platform and part ("per unit"
means one fixed pattern for the instrument; a different unit would use another seed); the sum of the table is asserted in the test, so a seed
or draw-order change fails it. Neighbour leakage is scaled linearly by the parameter; moving it ramps over 5 ms while the part sounds (a change on a part with
no sounding voice applies at once, which also keeps frame-0 setup of an offline render exact). The percussion tone is not leaked.

Cost: with leakage > 0 each live drawbar of each voice costs two more multiply-adds per frame (about 3x the tone sum); at leakage 0 nothing is
added. Cost units stay the Story 2.2 placeholders until measured on the board.

Purity test: a single drawbar on a single key at leakage 0 is compared with the synthetic sine `kVoiceScale x sin(2 pi f n / 48000)` of its wheel
after the 1 ms declick (residual relative to the tone). The achievable residual is the float32 resolution of the output, about -150 dB; the
tolerance is -100 dB, the same as the null-test goldens (loose enough for libm differences across platforms, tight enough that any leakage at all,
about -37 dB at 1 and -43 dB at 0.5, fails by 55 dB or more).

## Voices are never stolen; reservation

`Engine::steal_protected()` (new, default false) is true for the organ. `VoiceArbiter::bind` records it; protected engines
are skipped when choosing victims, may not be given a floor (`kInvalidConfig`), and their live cost counts first against
`LiveSetConfig::organ_rotary_cost` and only beyond it against the shared remainder. Callers set
`organ_rotary_cost = OrganEngine::reservation_cost_units(organ_parts)` = parts x (generator + rotary + 73 x voice cost), reserved
ahead of every engine floor. With that reservation, a pool full of sample voices cannot keep the 73 organ keys from sounding
(`organ_tests`: arbiter protection). `reserve()` returns `kInvalidConfig` when an organ is bound and `organ_rotary_cost` is below 73 voices' cost. When all 640 voice slots are busy the oldest releasing voice is reclaimed (held voices never); disabling a part (`set_enabled_parts`) deactivates its voices. Voice capacity is 640 (8 parts x 73 keys plus release tails).

## Cost units

Placeholders until measured on the dev board: voice 2, generator 48 per part, rotary 24 per part (reserved for Story 2.6).
`organ_bench` prints host figures labelled "host, not representative".

## Not in Stories 2.2 to 2.4

Multi-contact onset, rotary, effects, UI, Live Set loading.

## Tests

`organ_tests` (tables, generator accuracy, per-drawbar harmonics by single-bin DFT, foldback, drawbar curve and clamping,
no-zipper, shared phase, 73-key chord (no drops, bounded, deterministic), parameter routing, arbiter protection, forbidden
strings), `rtsan_organ_test` (10,000 blocks including percussion and key click controllers, Linux CI only), `organ_bench`, and `velora-offline-render --engine organ
--drawbars 888000000`. Story 2.3: `organ_percussion_tests` (single trigger incl. legato, re-arm, releasing key and parts; measured
decay constants; harmonic selection by spectrum; normal vs soft level and drawbar lowering; 1' cancelled and its smooth ramp; key click at
onset and release, broadband, scaled by level, per key, per press, level 0 exact silence; determinism; the CC path through the block loop)
and the golden `tools/offline-render/tests/organ_percussion/` (MIDI, null-test case, golden WAV; see the offline-render README).
Because key click and leakage default to 0.5, tests that look at the bare tones of Story 2.2 set both to 0 first.
Story 2.4: `organ_scanner_tests` (registry and CC mapping; constants; off is bit-exact; sweep rate and depth measured on a test tone by I/Q
demodulation for V1-V3; frequency-modulation sidebands; chorus = half dry + half scanned; no click on any switch, with a hard switch as the
contrast; engine routing; click-free cycling on a held 73-key chord; parts independent; determinism; leakage 0 purity against a synthetic sine;
leakage table reproducibility, level and linear scaling by single-bin DFT of the difference render; no click when leakage moves),
`rtsan_organ_test` (CC 81/87/83 traffic in the main loop plus a 6,000-block phase cycling every scanner setting and leakage on a held
73-key chord, Linux CI only) and the golden `tools/offline-render/tests/organ_scanner/` (see the offline-render README).

# Tonewheel organ engine (Story 2.2)

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
  a key-click or percussion model (Stories 2.3-2.5).
- **Level.** One drawbar at step 8 has amplitude `kVoiceScale` = 0.06; the part mix is linear below `kKnee` = 0.8 and bounded
  by 1.0 above (`tanh` shoulder), so a full chord at full registration stays within +-1.

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

## Not in this story

Percussion, key click, vibrato/chorus scanner, leakage, multi-contact onset, rotary, effects, UI, Live Set loading.

## Tests

`organ_tests` (tables, generator accuracy, per-drawbar harmonics by single-bin DFT, foldback, drawbar curve and clamping,
no-zipper, shared phase, 73-key chord (no drops, bounded, deterministic), parameter routing, arbiter protection, forbidden
strings), `rtsan_organ_test` (10,000 blocks, Linux CI only), `organ_bench`, and `velora-offline-render --engine organ
--drawbars 888000000`.

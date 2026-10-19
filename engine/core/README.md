# engine/core

Role: platform-independent engine (DSP, voices, pool arbiter, engines, event processors). No OS dependencies.

Dependency rule: may include and link only `engine/core`, `engine/ports` and the C++ standard library. Enforced by `tools/ci/check_core_deps.py` in CI.

Block loop modules (Story 1.8): `ump` (UMP-only events), `spsc_queue`, `arena`, `engine` (static engine interface and reserved type IDs Sample=1, Organ=2, FM=3, VA=4), `engine_registry`, `test_tone_engine`, `block_renderer` (64-frame stereo 48 kHz serial loop; events beyond the block are deferred to later blocks), `voice_arbiter` (Story 1.19, below).

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
- **Errors**: no voice slot (pool full and no steal victim) -> `kPoolExhausted` (the regions that got a slot still start); a note off with
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
- Out of scope (later stories): streaming/heads (1.16/1.17), parallel render (1.18), filters/effects,
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

## Voice pool arbiter (Story 1.19, AD-22)

`VoiceArbiter` (`voice_arbiter.hpp`) is the only allocator and stealer of voices. `BlockRenderer` owns one (`arbiter()`),
binds every engine to it (`bind_engine`) and runs it in the serial phase: no allocation, locks or blocking on the RT path
(`rtsan_arbiter_test`: 10,000 blocks of dense traffic with stealing; scenarios in `voice_arbiter_tests`).

**Engine side.** An engine takes a slot only through `Engine::acquire_slot()` (inside `start_voice`), which asks the arbiter.
The slot tables stay in the engines (arbiter-owned in the sense that only the arbiter decides which slot is used); an engine
with no arbiter attached (standalone tests) falls back to first-free. The interface additions: `VoiceInfo` carries `channel`
and `note` next to `start_time`, `level` and `state`; `begin_steal_fade(slot, ramp_frames)` makes the engine fade the voice with
a monotone gain `fade_left/fade_total` (strictly falling, 0 ends the voice) while it stays active, reports `kReleasing` and
ignores note offs. FM/VA need nothing beyond this.

**Reservation** (`reserve(LiveSetConfig)`, called when a Live Set loads, never on RT): `budget` is in cost units; the order is
1. `fx_cost`, 2. `organ_rotary_cost` (0 without an organ; never stolen), 3. each engine's floor (`floors[]`: engine type id and
voices, cost = voices x `cost_units_per_voice()`); `shared = budget - fx - organ - sum(floors)`. All arithmetic is 64-bit
integer with overflow checks. It is atomic: on any error (`ReserveStatus`: `kFxExceedsBudget`, `kOrganExceedsBudget`,
`kFloorsExceedBudget`, `kFloorExceedsCapacity` (capacity < floor + `kFadeHeadroomVoices` = 64: stolen voices keep their slot for the ramp, so a below-floor engine always finds a free slot), `kUnknownEngine`, `kInvalidConfig`, `kSampleFloorTooLow`) nothing changes.
A bound sample engine must have a floor of at least `kSampleFloorVoices` = 256 (a constant; small-pool tests lower it with a
setter compiled only under `VELORA_ARBITER_TEST_HOOKS`). `reserve()` also rejects an engine whose capacity or cost changed since
`bind()`; `bind()` is refused after a reservation and for a zero-cost engine. A successful `reserve()` and `clear_reservation()`
reset the event state (keys, pedal, deferred releases) and all counters; sounding voices keep sounding. Bind engines before reserving. Until a reservation succeeds the arbiter is *unreserved*: first-free
allocation, no stealing, a full pool reports `kPoolExhausted` (the Story 1.8/1.15 behaviour).

**Admission.** A new voice of engine E is granted without stealing when E stays inside its own floor, or when the shared
remainder has room for it with fading voices counted. Otherwise the arbiter steals; if no sufficient victim set exists nothing
is stolen and the engine reports `kPoolExhausted` (`exhausted()` counts these). A request also fails when E's slot table has no
free slot (live plus fading): size `voice_capacity()` above the voices the budget admits by the fade headroom.

**Victims** (whole Live Set, all engines and channels). First-stolen order, deterministic:
1. class: releasing; held; pedal-sustained (key up, sustain pedal down, or the engine reports `kSustained`); the lowest held
   note (the lowest note number among all held voices; the last resort);
2. quieter (`VoiceInfo::level`, lower first; a voice still in its attack is quiet);
3. newer (`start_time`, larger first);
4. lower (engine bind order, slot) id.
A note-on never steals voices started by its own event (all regions of one SFZ note). Floors: another engine is a candidate only
while it would stay at or above its floor after losing the voice (so an engine at or below its floor is never robbed); an engine
may always steal from itself. A steal replaces cost 1:1: enough victim cost is taken to cover the live excess and, when only
fading voices block the grant, the new voice's own cost (so a burst of notes does not steal ever more voices).

**Ramp and accounting.** A victim is told `begin_steal_fade(slot, steal_ramp_frames)`: 192 frames (4 ms at 48 kHz) by default
(`kStealRampFrames`, per Live Set `steal_ramp_frames`, at most 100 ms). It stays active, so it still counts in `usage()`
(`fading_cost`) until the engine reports it silent; the budget can therefore be exceeded by the fading cost for at most one ramp.
Counters are relaxed atomics readable from any thread (`usage()` refreshes the slot cache: RT-thread only). `steals()` (global), `steals(engine_type_id)` (voices stolen *from* that engine), `exhausted()`; `last_steal()` has the
latest victim (engine, channel, note, slot, class).

**Pedal.** `BlockRenderer` handles CC 64 (UMP control change, value >= 0x80000000 = down, per channel): a note off while the
pedal is down is not sent to the engine until the pedal goes up (release triggers fire then); meanwhile the voice is
pedal-sustained. The deferred release remembers its engine and goes to it at pedal up (dropped and counted in
`dropped_deferred_releases()` if the engine is gone). A note off whose voices were stolen is benign: `stolen_note_offs()` instead of
`kVoiceNotFound`, as is a pedal-up release of a note that never sounded. Other controllers are still ignored.

## Organ scanner and leakage (Story 2.4)

`VibratoScanner` (`vibrato_scanner.hpp`) is the per-part vibrato/chorus scanner of `OrganEngine` (tapped delay line, 6.87 Hz sweep, V1-V3/C1-C3,
5 ms crossfade on changes); leakage adds seeded neighbour-wheel contributions. Parameters 114, 115 (CC 81, 87) and 117 (CC 83). Details, public
facts and design values: `ORGAN.md`. RT-safe by construction (fixed storage, no allocation); covered by `organ_scanner_tests` and `rtsan_organ_test`.

## Parameters, snapshots and the last-value table (Story 2.1)

The parameter registry (`proto/params.json`, see `proto/README.md`) is generated into `velora/core/params_gen.hpp` (build output):
`params::ParamDef` table sorted by id (`params::find(id)`, `find_by_cc`), per-id constants (`params::kDrawbar8`), enum value
constants, `kPartCount` (8), `kInsertSlotCount` (5) and `perf_slot()` (dense index into the last-value table). core has no
protobuf dependency; the wire format lives in `engine/host`.

- `param_events.hpp` -- a performance parameter change is **one timestamped UMP** (MIDI 2.0 assignable controller, channel = part,
  bank/index = param id, 32-bit normalised value). `make_param_event`, `decode_param_event`, `cc_to_param_event`. `BlockRenderer`
  applies them at their frame; unknown ids, structural ids and parts above 7 are refused and counted (`param_events_rejected()`).
- `LastValueTable` -- RT owns the live value of every performance parameter; one 32-bit atomic per slot, written by RT, read by the
  control thread (and anyone) without locks.
- `Snapshot` -- the immutable structural state of one generation: 8 parts (timbre reference, insert chain of up to 5 slots, output),
  master settings, and the initial performance values (`perf_init`, including send levels). Built only by the control thread
  (`engine/host`), published with one atomic pointer store (`SnapshotExchange::publish`). RT adopts the newest generation at the start
  of each block and applies `perf_init` only for slots that changed since the previous generation (so a structural edit does not
  reset the drawbars a player moved).
- Generation lifetime (AD-23, epoch/hazard style, lock-free): RT announces the generation it reads (`rt_acquire`, a hazard pointer),
  and every note that starts while a generation is held takes one reference on it (the ledger, 32 slots). The renderer drops a
  note's reference when no voice of that (channel, note) is left in any engine. The control thread frees a generation only when
  it is not current, not held by RT and has zero references (`SnapshotExchange::safe_to_free`), never on RT. If 32 generations are
  pinned by sounding notes the next structural command is refused with `BUSY`.
- Tests: `params_tests` (registry, UMP round trip for every performance parameter, initial values only on generation change, held note
  across a swap); `rtsan_control_test` in `engine/host` covers RT safety (10,000 changes + 100 swaps).

## Tonewheel organ (Story 2.2)

`OrganEngine` (type id Organ=2, "tonewheel organ"): shared 91-wheel generator per part in the new `Engine::begin_block` phase-2 hook,
one voice per key summing nine drawbar footages with foldback, 5 ms drawbar ramps started at the event frame via
`Engine::set_part_param`, `Engine::steal_protected()` plus the organ reservation in `VoiceArbiter`. Clean-room sources of every
table, the drawbar curve, smoothing and the reservation are in `ORGAN.md`.

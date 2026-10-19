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
1. the FX budget (`fx_cost` plus the declared costs in `fx_instance_costs`, Story 2.7), 2. `organ_rotary_cost` (0 without an organ; never stolen), 3. each engine's floor (`floors[]`: engine type id and
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

## Multi-contact onset (Story 2.5)

`contact_events.hpp` defines the vendor per-note `contact_k` controller (MIDI 2.0 assignable per-note controller, UMP status 0x1: channel,
note, contact index k in the low byte, closed/open in the 32-bit value) that the Epic 3 keybed reader will emit. `BlockRenderer` routes it
to the engine serving the channel at its exact in-block frame through the new `Engine::supports_contacts()` / `Engine::set_contact()`; it
is counted in `contact_events_routed()` / `contact_events_dropped()`, never in `ignored_events()`. `OrganEngine` starts the key voice on
contact 1 (before any Note On), brings in a busbar group per later contact and takes them out in reverse as contacts open; the contact-to-footage
table (3-contact and 2-contact variants), the ramps, the defined handling of malformed sequences and the interplay with percussion,
key click and the arbiter are in `ORGAN.md` ("Multi-contact onset"). Note On/Off-only sources are unchanged. RTSan-covered
(`rtsan_organ_test`), unit tests in `organ_contact_tests`.

## Rotary speaker (Story 2.6)

`RotarySpeaker` (`rotary_speaker.hpp`) is a self-contained DSP unit (crossover into horn and drum, per rotor amplitude modulation, Doppler from a fixed delay line,
stereo spread through two virtual microphones, separate acceleration and braking time constants, slow/fast/stop) that the organ runs per part when
`rotary_on` (id 201, CC 89, default off) is set; `rotary_speed` (id 200, CC 79) is stop/slow/fast. Off is a bit-exact bypass of the mono organ output; on, the
part's output is stereo. `OrganEngine::rotary_status(part)` publishes the commanded speed and the actual horn/drum speeds lock-free, and the target is in the RT
last-value table (so `ws.v1` `GetPerformanceValues` reports it). The rotary cost is part of the organ reservation. Design values, public facts, behaviour and
tests: `ORGAN.md` ("Rotary speaker"). Covered by `rotary_tests` and `rtsan_organ_test`.

## Routing topology (Story 2.7)

Phase 5 of the block loop (after the voices render) is the `Mixer` (`mixer.hpp`), a fixed topology; `BlockRenderer::process_block(const OutputBuses&)` hands
the host **Main L/R and Aux L/R** per block (`process_block(left, right)` is the Main bus only, which is what the CoreAudio adapter plays for now; physical outputs are Epic 3):

```
voices --(per part, Engine::render_split)--> part bus (stereo, 64 frames)
part bus --> insert chain (<= 5 slots) --> post-insert signal
  --> Main or Aux bus per the part's part_output (structural; switched with a 5 ms ramp)
  --> Reverb send (x send_reverb_level) --> Reverb return (placeholder, unity) --> the same bus (Main or Aux) as the part
  --> Delay  send (x send_delay_level)  --> Delay return  (placeholder, unity) --> the same bus as the part
line-in (x line_in_level) --> Main only
Main, Aux --> master section per bus (master EQ, then master compressor, each with its own bypass; Story 2.8; then the master_level trim) --> outputs
```

Decisions: a part's send returns go to the bus the part sits on, so an Aux part's reverb never leaks into Main. The Mixer keeps one send accumulator and one send placeholder per output bus; the real
reverb and delay of Stories 2.10/2.11 therefore need **one instance per bus**, and the declared FX cost already counts them twice (`kFixedFxCost` = 2 x (reverb + delay + master EQ + compressor)).
`process_block(left, right)` (an adapter with a single output) folds Aux into Main (counted in `aux_folded_blocks()`), so a part on Aux is still audible there; `process_block(OutputBuses)` with only one of
`aux_left`/`aux_right` is refused (Main silenced, `rejected_output_blocks()`). UMP channels 8..15 alias parts 0..7 in the routing (`Engine::render_split` is pure virtual; channel c and c+8 share a part bus), and
`velora-offline-render` rejects events above channel 8 (index 7) when a routing option is given.
Line-in is mixed into the Main bus at the master (before the master EQ/compressor and the trim) and never into Aux; the master section (and its FX cost) exists once per bus.
Levels (`send_*_level`, `line_in_level`, `master_level`) ramp linearly over one block (64 frames); a level event is taken at the start of the next block (<= 1.3 ms).
Registry additions: `line_in_level` (id 800, performance, master scope, 0..1, default 0: line-in is muted until raised) and `master_level` (id 520, structural, dB, default 0).

**Engines render per part.** `Engine::render_split(left[16], right[16], frames)` adds the voices of UMP channel c into `left[c]`/`right[c]` (the Mixer points c and c+8 at the
same part bus); `render()` is the same code with all pointers equal. Sample, Organ and TestTone implement it; an engine that does not puts everything on channel 0's bus.

**Trivial routing renders straight into Main.** When every part is on Main at unity, no chain holds an effect, the sends and line-in are 0 and the master is 0 dB, the Mixer is
inactive and the engines render into the output exactly as before this story: those renders are bit-identical to the existing goldens (`Mixer::routed()` tells).
Any non-trivial routing switches to the part-bus path (sums in part order, so the audio differs from the direct sum only by float rounding, ~1e-7).

**Effects (`fx.hpp`).** `Effect` (`type()`, `cost_units()`, `prepare()`, `reset()`, `set_params()` and the RT `process(StereoBlock&)`), `make_effect()` (control thread only; the only factory,
types are the registry `insert_type` list: no plugin loading) and `FxChain` (<= 5 slots, bypass flag each). The EQ and the compressor are real DSP since Story 2.8 (below); the chorus/phaser, EP amp and rotary inserts are `PassThroughEffect` placeholders until Stories 2.9-2.11; the rotary insert is a
pass-through too (the organ's built-in rotary stage, Story 2.6, is unchanged; the insert becomes a `RotarySpeaker` in Story 2.9). Declared costs (`effect_cost_units()`, `kReverbSendCost`, `kDelaySendCost`,
`kMasterEqCost`, `kMasterCompCost`) are placeholders until measured on the board (2.12); the EQ (16 units, three active bands, ~1.0 us per block on the dev Mac) and the compressor (8 units, ~0.5 us per block with signal above the
threshold) were re-based on host measurements in Story 2.8 (`eq_compressor_tests` prints them). Raising the EQ from 6 to 16 units raises `kFixedFxCost` by 20 and each EQ insert by 10, so a Live Set that fitted a tight FX budget before can now be refused with `FX_BUDGET_EXCEEDED`.

**Chain changes.** The control thread builds the part's `FxChain` (new, prepared instances) when it builds the snapshot generation (`build_chain()`; `PartCfg::chain`, shared by generations that do not touch the part,
so an unchanged chain keeps its instances and RT does not cross-fade). RT adopts the generation, starts the new chain fading in linearly over 240 frames (5 ms) while the old one fades out (weights always sum to 1: no dip, no
bump, no click), and never allocates. A change that arrives during a fade waits and the latest request wins (at most two chains in flight per part). Each chain entry holds a ledger reference on the generation that owns its
instances (`SnapshotExchange::rt_ref`), moved to the newest generation that still carries the chain, so instances are freed (control thread, `ControlPlane::reclaim()`) only after RT stopped processing through
them and after the notes that started on that generation ended (AD-23). Instance state is not carried across a changed chain: a changed chain starts with fresh instances (bypass toggles included).

**FX budget (AR18).** `VoiceArbiter::LiveSetConfig` gets `fx_instance_costs`/`fx_instance_count` next to the flat `fx_cost`; `reserve()` sums them as the FIRST step (FX -> organ+rotary -> floors; `fill_fx_costs(snapshot, cfg)` in
`mixer.hpp` fills the list from a snapshot: per bus reverb, delay, master EQ and compressor, plus the insert chains). The control plane (`engine/host`) keeps the same budget (`ControlPlane::set_fx_budget()`, default `kMaxFxCost` = every slot of every part holding
the heaviest insert) and refuses a command that raises the cost above the budget with `ERROR_CODE_FX_BUDGET_EXCEEDED`, changing nothing (a command that does not raise the cost, e.g. removing an effect after the budget was lowered, always goes through). Callers reserve the arbiter with the same amount as the control plane's budget.

**Line-in.** `velora/ports/line_in_source.hpp`: `LineInSource::read_block()` is called once per block (also while the level is 0, to keep the source in step); no source = silence.

Tests: `routing_tests` (bus summation, Main/Aux, sends, master level, line-in to the master only, cross-fade continuity with a known gain effect and the step bound, add/remove/reorder/bypass, overlapping changes,
generation lifetime of the instances with a held note, FX budget and reservation order, default path bit-exact), `routing_traffic_test` / `rtsan_routing_test` (10,000 blocks with a chain change every 3 blocks,
part output switches, send and line-in changes; allocation-free under RTSan, deterministic: two runs hash equal), an organ per-part split check inside `routing_tests`; offline null tests in `tools/offline-render`
(`offline_routing_tests`); control plane tests in `engine/host` (`control_tests`).

## EQ and compressor, inserts and master section (Story 2.8)

`eq.hpp`/`eq.cpp` (`EqEffect`) and `compressor.hpp`/`compressor.cpp` (`CompressorEffect`) are the real `eq` and `compressor` effects: `make_effect()` returns them, and the Mixer owns one of each per output bus (Main, Aux) as the
master section. Fixed storage, no allocation in `process()`, stereo-linked where it matters (the EQ filters the channels independently with the same coefficients, the compressor detects on `max(|L|, |R|)`).

**Parameter mapping (the one table).** Insert parameters are the normalised registry params `insert_param_1..5` (id 302-306, 0..1; `insert_param_5` is new in Story 2.8, default 0); the master section uses the physical
`master_*` registry parameters (units in the registry). The four EQ controls are the encoder functions LOW, MID, MID FREQ, HIGH of the panel.

| effect | insert param | physical mapping | master registry param (id, unit) |
|---|---|---|---|
| EQ | 1 LOW | low shelf gain, -15..+15 dB linear (0.5 = 0 dB) | `master_eq_low_gain` (500, dB) |
| EQ | 2 MID | mid peak gain, -15..+15 dB linear (0.5 = 0 dB) | `master_eq_mid_gain` (501, dB) |
| EQ | 3 MID FREQ | mid frequency, 200..8000 Hz logarithmic (0.5 = 1265 Hz) | `master_eq_mid_freq` (502, Hz; default 1000) |
| EQ | 4 HIGH | high shelf gain, -15..+15 dB linear (0.5 = 0 dB) | `master_eq_high_gain` (503, dB) |
| EQ | - | master bypass | `master_eq_bypass` (504, flag, new) |
| compressor | 1 THRESHOLD | -60..0 dB linear (0.5 = -30 dB) | `master_comp_threshold` (510, dB; default -12) |
| compressor | 2 RATIO | 1..20 : 1, `20^p` (0 = 1:1, 0.5 = 4.5:1) | `master_comp_ratio` (511, ratio; default 1) |
| compressor | 3 ATTACK | 0.1..100 ms logarithmic (0.5 = 3.2 ms) | `master_comp_attack` (512, ms; default 10) |
| compressor | 4 RELEASE | 10..1000 ms logarithmic (0.5 = 100 ms) | `master_comp_release` (513, ms; default 100) |
| compressor | 5 MAKEUP | 0..24 dB linear (default 0 = 0 dB) | `master_comp_makeup` (514, dB; default 0) |
| compressor | - | master bypass | `master_comp_bypass` (515, flag, new) |

The ids are unchanged; `insert_param_5`, `master_eq_bypass` and `master_comp_bypass` are additive (structural, registry defaults 0). The registry defaults of the master section are exactly transparent (EQ 0 dB, compressor ratio 1 with no makeup).
The default insert parameters (0.5, 0.5, 0.5, 0.5, 0) make an EQ flat (transparent) and a compressor work (-30 dB, 4.5:1).

**EQ DSP.** Low shelf at 100 Hz, mid peak (Q = 1) at the Mid freq, high shelf at 4 kHz, designed at the fixed 48 kHz, transposed direct form II in double precision. The shelves are the public RBJ Audio-EQ-Cookbook biquads (S = 1,
bilinear, pre-warped at the corner); the peak is the analog RBJ prototype realised with M. Vicanek's matched-magnitude design (the RBJ bilinear peak is pulled down far above a high centre, up to 2.8 dB at 8 kHz). The targets are approached by a
one-pole per block (about 10 ms) and the coefficients are recomputed once per block while something moves. A band at exactly 0 dB is skipped and its state cleared, so a flat EQ is bit-exact transparent.
**Design curve** = the magnitude of the ANALOG RBJ prototype `H(s)` (s normalised to the corner / centre frequency) at the real frequency. `eq_compressor_tests` compares the biquads with it, from the coefficients and with sine measurements through
`process()`, over 20 Hz..20 kHz (20, 31.5, 63, 100, 125, 250, 500 Hz, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20 kHz) for gains +-15 and +-6 dB and the mid frequencies 200, 500, 1000, 2000, 3000, 4000, 6000, 8000 Hz, with no restricted range:
within +-0.5 dB up to 16 kHz and within 1 dB at 20 kHz (0.42 x fs); the shelves stay within 0.29 dB everywhere.
Defaults differ on purpose in one place: the insert MID FREQ value 0.5 is the geometric middle of 200..8000 Hz (1265 Hz), the master default `master_eq_mid_freq` is 1000 Hz (both are pinned by a test; the EQ is flat at either).

**Compressor DSP.** Feed-forward, per sample: peak detector `x = max(|L|, |R|)`, `x_dB`, hard-knee static curve (`target = -(x_dB - threshold) (1 - 1/ratio)` above the threshold), then a log-domain branching one-pole smoother on the
gain change `g` (attack coefficient when more reduction is needed, release otherwise; `c = exp(-1/(t fs))`), output `in * 10^((g + makeup)/20)`. Attack and release are time constants: with the input stepped to a level that needs a final reduction
G, `GR(t) = G (1 - e^(-t/attack))` and the release falls to `1/e` of its start after `release` ms, i.e. the times are the 63.2 % / 36.8 % points; the unit test measures both on a DC step within 10 % (it finds < 1 %).
Makeup ramps linearly over a block when it changes; threshold, ratio, attack and release apply at once (the gain smoother makes any jump click-free). Ratio 1 with makeup 0 dB and a settled reduction of 0 is the identity: bit-exact, not processed.
Non-finite input samples are ignored by the detector and the EQ states, so a NaN cannot poison them.

**Master section.** After the send returns and the line-in, per bus: master EQ, then master compressor, then the `master_level` trim. The `master_*` values of the adopted snapshot drive both buses (same settings, separate state);
a structural change reaches RT with the next generation and the EQ/compressor smoothing makes it click-free. `master_eq_bypass` / `master_comp_bypass` cross-fade the stage out and in over 240 frames (5 ms; dry/wet mix, no click);
a fully bypassed stage does not run and its state is cleared (a bypass flag value >= 0.5 bypasses; 0, anything below 0.5 and NaN do not). A bypassed insert slot is reset by its chain so stale state cannot step in when it is un-bypassed. A stage that is bypassed or at identity settings counts as trivial: with nothing else routed the Mixer stays inactive and the render is bit-identical to the default path.

**Gain reduction.** Each compressor stores its last-block reduction (dB, >= 0, `Effect::gain_reduction_db()`). The Mixer copies it once per block into the RT last-value table (`LastValueTable::load_gain_reduction(slot)`;
`gr_insert_slot(part, slot)`, `gr_master_slot(bus)`; the largest value of the chains still audible in the slot, a chain fading out included; 0 for a bypassed or empty slot, and zeros while the routing is trivial). The control thread reads it lock-free: `ws.v1` `GetPerformanceValues` returns, additively,
`PerformanceValues.gain_reductions` (`GainReduction{scope, part, slot, gain_reduction_db}`) for every insert slot that holds a compressor in the current snapshot and for the two master compressors.

Tests: `eq_compressor_tests` (mapping, design curve, sine sweep, identity, smoothing step bound, static curve, attack/release times, stereo link, makeup, master order EQ -> compressor against a reference, master and insert bypass cross-fade step
bound and exact return, identity bit-exact, published gain reduction, host cost print), `routing_traffic_test` / `rtsan_routing_test` (insert and master EQ/compressor settings and both bypass flags change with every generation for 10,000 blocks, gain reduction checked), `control_tests`
(gain reduction over ws.v1), offline null tests in `tools/offline-render` (`fx_eq_*`, `fx_compressor_*` goldens; `offline_routing_tests`).

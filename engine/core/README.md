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
- **Program generation (Story 4.4).** A scene recall is a generation with a new `Snapshot::program_generation`; `perf_force` flags the performance slots the scene stores. RT keeps the program generation it last adopted and, when a generation carries a different one, applies the `perf_force` slots once even if they equal the previous initial value (`BlockRenderer::adopt_generation`), so a recall overrides a live knob move of the stored parameters and nothing else. Later generations copy the number, so it is applied once. `Snapshot::scene_index` / `scene_count` / `scene_source` are control-thread bookkeeping published in the engine state. Details: `engine/host/README.md` ("Scene recall").
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
  --> Reverb send (x send_reverb_level x send_reverb_master) --> Reverb (Story 2.10, own instance per bus, x reverb_level) --> the same bus (Main or Aux) as the part
  --> Delay  send (x send_delay_level)  --> Delay (Story 2.11, own instance per bus, x delay_level) --> the same bus (Main or Aux) as the part
line-in (x line_in_level) --> Main only
Main, Aux --> master section per bus (master EQ, then master compressor, each with its own bypass; Story 2.8; then the master_level trim) --> outputs
```

Decisions: a part's send returns go to the bus the part sits on, so an Aux part's reverb never leaks into Main. The Mixer keeps one send accumulator and one send processor per output bus; the
reverb (Story 2.10) and the delay (Story 2.11) therefore need **one instance per bus**, and the declared FX cost counts them twice (`kFixedFxCost` = 2 x (reverb + delay + master EQ + compressor)).
`process_block(left, right)` (an adapter with a single output) folds Aux into Main (counted in `aux_folded_blocks()`), so a part on Aux is still audible there; `process_block(OutputBuses)` with only one of
`aux_left`/`aux_right` is refused (Main silenced, `rejected_output_blocks()`). UMP channels 8..15 alias parts 0..7 in the routing (`Engine::render_split` is pure virtual; channel c and c+8 share a part bus), and
`velora-offline-render` rejects events above channel 8 (index 7) when a routing option is given.
Line-in is mixed into the Main bus at the master (before the master EQ/compressor and the trim) and never into Aux; the master section (and its FX cost) exists once per bus.
Levels (`send_*_level`, `line_in_level`, `master_level`) ramp linearly over one block (64 frames); a level event is taken at the start of the next block (<= 1.3 ms).
Registry additions: `line_in_level` (id 800, performance, master scope, 0..1, default 0: line-in is muted until raised) and `master_level` (id 520, structural, dB, default 0).

**Engines render per part.** `Engine::render_split(left[16], right[16], frames)` adds the voices of UMP channel c into `left[c]`/`right[c]` (the Mixer points c and c+8 at the
same part bus); `render()` is the same code with all pointers equal. Sample, Organ and TestTone implement it; an engine that does not puts everything on channel 0's bus.

**Trivial routing renders straight into Main.** When every part is on Main at unity, no chain holds an effect, the sends (or the REV master send level, for the reverb) and line-in are 0, no reverb or delay tail rings (`ReverbSend::quiet()`, `DelaySend::quiet()`) and the master is 0 dB, the Mixer is
inactive and the engines render into the output exactly as before this story: those renders are bit-identical to the existing goldens (`Mixer::routed()` tells).
Any non-trivial routing switches to the part-bus path (sums in part order, so the audio differs from the direct sum only by float rounding, ~1e-7).

**Effects (`fx.hpp`).** `Effect` (`type()`, `cost_units()`, `prepare()`, `reset()`, `set_params()` and the RT `process(StereoBlock&)`), `make_effect()` (control thread only; the only factory,
types are the registry `insert_type` list: no plugin loading) and `FxChain` (<= 5 slots, bypass flag each). All six insert types are real DSP: the EQ and the compressor since Story 2.8 and the chorus/phaser, EP amp and rotary since Story 2.9 (both below); the reverb send is real since Story 2.10 and the delay send since Story 2.11 (both below); `PassThroughEffect` only remains as the factory's fallback. Declared costs (`effect_cost_units()`, `kReverbSendCost`, `kDelaySendCost`,
`kMasterEqCost`, `kMasterCompCost`) are placeholders until measured on the board (2.12); the EQ (16 units, three active bands, ~1.0 us per block on the dev Mac, Release build) and the compressor (8 units, ~0.5 us per block with signal above the
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

## Chorus/phaser, EP amp/drive and rotary inserts (Story 2.9)

`chorus_phaser.hpp`/`.cpp` (`ChorusPhaserEffect`), `ep_amp.hpp`/`.cpp` (`EpAmpEffect`) and `rotary_effect.hpp`/`.cpp` (`RotaryEffect`) replace the pass-through placeholders: `make_effect()` returns them for the registry insert types
`chorus_phaser`, `ep_amp` and `rotary`. Same conventions as the EQ and the compressor: fixed storage, no allocation in `process()`, the five normalised registry params `insert_param_1..5`, parameters approached per 64-frame block (one-pole, about 10 ms)
and interpolated per sample inside the block, non-finite input samples are replaced by 0 (a NaN is exactly a zero sample), `reset()` clears all signal state (a bypassed slot is reset by its chain, so stale state cannot step in on un-bypass; a bypassed slot is skipped bit for bit),
`VELORA_NONBLOCKING` on the overrides, declared cost in `effect_cost_units()` (host-measured placeholders until Story 2.12). Registry: no new entries (ids unchanged); `ws.v1` is untouched.

**Parameter mapping (the one table).**

| effect | insert param | physical mapping |
|---|---|---|
| chorus/phaser | 1 MODE | chorus for p <= 0.5, phaser for p > 0.5 (one threshold, no hysteresis; the next float above 0.5 is already a phaser; NaN is 0.5, i.e. chorus) |
| chorus/phaser | 2 RATE | LFO 0.1..10 Hz, logarithmic `0.1 * 100^p` (0.5 = 1 Hz) |
| chorus/phaser | 3 DEPTH | 0..1 linear sweep depth |
| chorus/phaser | 4 FEEDBACK | phaser feedback -0.8..+0.8, bipolar linear (0.5 = 0); unused by the chorus |
| chorus/phaser | 5 MIX | wet amount 0..1, `out = dry + mix (wet - dry)`; the registry default 0 is the dry signal: bit-exact identity |
| EP amp | 1 DRIVE | pre-gain 0..30 dB, linear in dB |
| EP amp | 2 TONE | tilt +-6 dB around 1 kHz (0.5 = flat) |
| EP amp | 3 PRESENCE | peak at 3 kHz (Q 0.9), 0..+9 dB |
| EP amp | 4 LEVEL | output level -12..+12 dB (0.5 = 0 dB) |
| EP amp | 5 CABINET | cabinet filter blend 0..1 (default 0 = off) |
| rotary | 1 SPEED | stop for p < 0.25, slow for 0.25 <= p < 0.75, fast for p >= 0.75 (thresholds belong to the upper band; the default 0.5 and NaN are slow) |
| rotary | 2 WIDTH | stereo width of the rotary output, `2 p`: 0 = mono, 0.5 = as designed, 1 = twice the side signal |
| rotary | 3..5 | unused |

**Chorus/phaser.** Per channel a 2048-sample delay line (chorus: 4-point Lagrange read at `15 ms + 10 ms * depth * lfo`, i.e. 5..25 ms) and four first-order all-pass stages (phaser: break frequency swept exponentially over 200 Hz..4 kHz around 894 Hz by `depth * lfo`,
feedback of the chain output around the chain, coefficient held 16 frames). Stereo: both channels use the same LFO (parabolic sine approximation, error < 0.1 %) with a quarter-cycle (90 degree) offset, so the wet signals are decorrelated. A mode change on a running
instance ramps the phaser weight over 480 frames (10 ms): the wet signal is `(1 - w) chorus + w phaser`, the delay line is always written, each mode runs only while its weight is non-zero (the phaser state is cleared while it is out). A mode change that arrives through the
control plane is structural (a new chain, cross-faded by the Mixer like every insert change); the in-effect cross-fade covers `set_params()` on a live instance (tests, later performance routing). Mix exactly 0 leaves the block untouched and clears the state.

**EP amp/drive.** Pre-gain, a soft clipper (the Pade approximation `x (27 + x^2) / (27 + 9 x^2)` of tanh, clamped at |x| = 3 where it is exactly +-1 with a continuous slope; odd, so only odd harmonics), then `1/sqrt(g)` loudness compensation, a 40 Hz high-pass, the tone tilt, the
presence peak, the optional cabinet (80 Hz high-pass + 5 kHz low-pass, 2nd-order Butterworth, blended by CABINET) and the level. The clipper runs 4x oversampled (48 -> 96 -> 192 kHz): two cascaded Kaiser-windowed half-band stages (47 taps at 96 kHz, 15 taps at 192 kHz;
round-trip passband ripple below 0.01 dB to 18 kHz, -0.13 dB at 20 kHz), block based and polyphase; the whole oversampler delays the signal by 25.75 frames (0.54 ms). A 9.7 kHz sine at full drive aliases 20 dB (drive 0.6: 60 dB) less than the same shaper at the base
rate (`ep_amp_tests`). There is no identity setting (the shaper is always in the path): bypass is the slot flag. Declared cost 24 units (Release build ~1.3 us / block on the dev Mac, oversampling included), which makes it the heaviest insert together with the rotary
(`kMaxInsertCost` = 24, so `kMaxFxCost` is unchanged).

**Rotary insert.** The `RotarySpeaker` of Story 2.6 on the part bus. The organ's built-in rotary takes the mono part signal; the insert takes the stereo part bus, so it **sums to mono** `m = (L + R) / 2`, runs the rotary unit on `m` and writes the stereo result, with the side
signal scaled by WIDTH (the input's stereo image is replaced by the rotary's own; a hard-panned source arrives at half level). Speed changes ramp with the horn and drum acceleration and braking time constants of the unit: on a running instance `set_params()` sets the unit's target (identical to
the organ's behaviour, tested sample for sample against the unit), before the first block the rotors start settled at the speed. A structural speed change from the control plane builds a new chain like every insert change. The insert publishes no rotary state (the organ's built-in rotary does:
`OrganEngine::rotary_status()`). **No automatic duplication:** an organ part keeps its built-in rotary (when `rotary_on`) and gets no second instance unless the user inserts one; `RotaryEffect::live_instances()` counts the inserts, and `rotary_insert_tests` shows one rotary stage for an organ part
with the built-in rotary and no insert, two with an insert (and the render equals the built-in output run through a standalone insert). Declared cost 24 units (= `RotarySpeaker::kCostUnits`; ~1.2 us / block Release).

Tests: `chorus_phaser_tests` (mapping and the mode threshold, delay range 5..25 ms and rate by ramp inversion, quarter-cycle stereo offset, phaser response against the analytic all-pass chain incl. feedback and the notch depth, swept notch, identity bit-exactness, mode cross-fade step bound against the
click a hard switch would make, parameter-jump step bound, determinism, NaN/inf, reset equals fresh, chain bypass), `ep_amp_tests` (mapping, shaper shape, oversampler flatness / delay / block-split independence, aliasing with and without oversampling, drive harmonics and loudness
compensation, tone / presence / cabinet / level against analytic references, determinism, NaN/inf, reset, bypass, step bound, cost), `rotary_insert_tests` (mapping and thresholds, mono/stereo handling against the unit, speed ramps equal to the unit's, step bound, NaN/inf, reset, bypass, the organ
one-instance rule), `routing_traffic_test` / `rtsan_routing_test` (the three types, mode and speed thresholds and parameter extremes cycle with every generation for 10,000 blocks), offline null tests in `tools/offline-render` (`fx_chorus_phaser_*`, `fx_ep_amp_*`, `fx_rotary_insert_*` goldens).

## Reverb send (Story 2.10)

`reverb.hpp`/`reverb.cpp` (`ReverbSend`, an `Effect`) is the processor behind the Reverb send bus. The Mixer owns one instance per output bus (Main, Aux): each is fed by the sum of the per-part sends of the parts that sit on that bus
(`send_reverb_level` per part, smoothed per block, x `send_reverb_master`), and its wet output (x `reverb_level`) is added to the same bus. The declared cost covers both instances (`kReverbSendCost` per bus in `kFixedFxCost`).
Fixed storage (the engines are members, about 0.3 MB per instance), nothing allocates in `process()`, all RT functions are `VELORA_NONBLOCKING`. **Clean-room:** only public algorithms were used (no reverb source was consulted, GPL or otherwise):
the feedback delay network of Stautner and Puckette (1982) and Jot and Chaigne (1991), with the Householder reflection `x - (2/N) sum(x)` as feedback matrix, Jot's rule for the loop gain of a delay line,
and Schroeder all-pass diffusers (Schroeder 1962). Mono-summed send in, stereo out. `process()` accepts any block length (processed in slices of 64 frames; bit-identical to 64-frame calls).

```
send sum (stereo) -> (L+R)/2 -> DC blocker -> pre-delay -> 4 all-pass diffusers -> FDN (8 lines, Householder matrix, one-pole damping and gain per line, slowly modulated read positions)
  -> left tap and right tap (two orthogonal +-1 sign patterns over the 8 line outputs: decorrelated stereo) -> x reverb_level -> wet
```

**Time (RT60).** `reverb_time` is RT60 in seconds, 0.2 .. 10, **at 1 kHz**. A delay line of d samples gets the loop gain `10^(-3 d / (T60 fs))` at 1 kHz: the line gain is that value divided by the magnitude of its one-pole damping filter at 1 kHz
(`(1-p)/sqrt(1 - 2 p cos w + p^2)`, w = 2 pi 1000 / 48000) and, for a modulated line, of the mean linear-interpolation loss at 1 kHz. The orthogonal feedback matrix loses no energy, so every line decays at the same rate per second.
Above 1 kHz the damping shortens the decay (the "colour" of each type), below it the decay is the same (the gain is limited below unity). **Level.** The two taps are scaled so that white noise at the send comes out with about the same power
whatever the time (measured -2.3 .. +2.6 dB between 0.3 s and 10 s, asserted within +-6 dB); `reverb_level` 1 is that unit return, 0 mutes (and clears) the reverb.

**Types.** Three parameter sets of the same engine (48 kHz, delay lengths in samples; all primes so the modes do not pile up):

| type | FDN lines | pre-delay | diffusers (all-pass, g) | damping p | modulation |
|---|---|---|---|---|---|
| room (0) | 601, 709, 827, 947, 1061, 1193, 1297, 1427 (12.5 .. 29.7 ms) | 96 (2 ms) | 89, 131, 191, 263 (0.50) | 0.20 | none |
| hall (1, default) | 1499, 1699, 1901, 2111, 2347, 2593, 2837, 3109 (31 .. 65 ms) | 960 (20 ms) | 137, 199, 293, 419 (0.60) | 0.32 | 0.8 samples, ~0.45 Hz triangle |
| plate (2) | 977, 1151, 1303, 1481, 1657, 1861, 2053, 2273 (20 .. 47 ms) | 0 | 73, 109, 157, 223 (0.70) | 0.10 (bright) | 1.2 samples, ~0.9 Hz triangle |

**Type change.** Two engines run during a 50 ms cross-fade (2400 frames, linear weights that sum to 1): the old type keeps ringing while the new one fills; a change that arrives mid-fade waits and the latest wins (one fade at a time).
Time and level changes apply at once to every running engine (the gains sit inside the loop, so the output stays continuous); the level ramps over one block. A change while nothing rings (the reverb is quiet) switches at once.

**Robustness.** Non-finite input samples are silence, finite ones are held to +-1e6 before summing (nothing near FLT_MAX can overflow the sums or filters); the damping low-pass state follows the signal even with damping 0, so raising it mid-tail starts settled; values below 1e-20 in the loop and on the wet tap are flushed to zero (no denormals). The first generation reads the live tempo before it configures the delays (`Mixer::adopt(s, &live)`), so an instant synced target is right at any tempo.

**Idle and transparency.** `quiet()` is true once the state was cleared and no input has arrived since. The tail clears itself: after `kFlushAfterFrames` (8192) frames without input, once the wet output stayed below `kFlushLevel` (1e-8, -160 dBFS) for
8 consecutive blocks; a steady return level of 0 clears it at once (a fade or waiting type change in flight is finished then: the newest requested type is adopted). With a return level of 0 and part sends up the Mixer stays routed (a send is up, so it is not trivial) but the reverb does no work and the output is exactly the dry one (tested). A quiet reverb with silent input does no work (`engine_blocks()` does not advance), and the Mixer treats it as transparent: the routing is trivial (the engines render straight into Main, bit-identical to
the pre-reverb behaviour) when all part sends are 0, **or** the REV master send level is 0 (the part ramps are settled), **and** every reverb instance is quiet. While a tail still rings after the sends went to 0 the Mixer stays routed (the tail is not cut), then goes inactive again.
Mixer mapping: type, time and level come from the adopted snapshot's `master` values (`reverb_type`, `reverb_time`, `reverb_level`; NaN or out-of-range values are clamped by `reverbmap`); the first generation applies without a cross-fade or ramp.

**Registry (additive, ids stable).** `reverb_type` (530, structural, master scope, enum room/hall/plate, default hall), `reverb_time` (531, structural, master, s, 0.2 .. 10, default 2), `reverb_level` (532, structural, master, ratio 0..1, default 1),
and `send_reverb_master` (801, **performance**, master scope, ratio 0..1, default 1): the REV master send level, one value that scales the reverb send of all parts together (so the 9th fader in MIXER can drive it later); it multiplies the
per-part `send_reverb_level` (400, unchanged, part scope, CC 91) per frame with a one-block ramp, so only the part whose send moves changes its own contribution (the wet signals add linearly: tested). Like `line_in_level` it is a master-scope performance parameter: its UMP uses channel 0
and it has **no CC** (CC mappings exist only for part-scope parameters in the registry; the natural CC, 91, already drives the per-part send). The ws.v1 path is the existing one: `SetMasterParams` (structural, all or nothing, range checked), `SetPerformanceParam` /
`GetPerformanceValues` for `send_reverb_master`, `GetState` (`master`, `performance_init`) and the dev configuration; `ParamId` and the `ReverbType` enum are generated from the registry. No new messages.

**RT60 measurement (the test).** `reverb_tests`: an impulse through the send (return level 1, both channels), the left and the right response band-passed with a third-octave filter around 1 kHz (RBJ band-pass, Q = 4.32), squared and Schroeder backward-integrated;
a least-squares line through the decay curve between -5 and -25 dB (T20) and between -5 and -35 dB (T30) is extrapolated to 60 dB. Test points: each of room, hall, plate at 0.3, 0.6, 1.2, 2.5, 5 and 10 s; both channels, T20 and T30 must be within 15 % of the parameter
(measured worst 8 %, at 0.3 s where the line fit spans only 60 ms; at 1.2 s and above the deviation stays within about 2 %). A running instance whose time changes settles to the new time too.

**Cost (host, not representative).** `reverb_bench` (Release numbers on the dev Mac, longest time, loud input): one reverb instance 0.75 us per 64-frame block, about 1.1 x an EQ insert (0.67 us, 16 units); while a type cross-fade runs two engines, 1.5 us (2.2 x). With all 8 parts
sending the Mixer adds about 0.6 us per block over the same Mixer without sends. The declared `kReverbSendCost` is **48 units** per instance (it was a 40-unit placeholder): it covers the cross-fade case ratio-wise (2.2 x 16 = 36) with a margin; the bench asserts that in optimised
builds (since Story 2.11 the bench links a bench-private -O2 core, so this holds in default builds as well; the bench test fails on any `FAIL` line). The dev-board measurement that replaces it is Story 2.12. Raising 40 to 48 raises `kFixedFxCost` by 16. Story 2.11 made the benches run on -O3 code in every configuration (and serially), where the cross-fade case measured 2.2..3.5 x an EQ insert across runs, so the declared cost became **64 units** (`kFixedFxCost` +32), and **96 units** in Story 4.4 (`kFixedFxCost` +64): in Docker on x86 with the host busy the ratio reaches 4.0..5.2 x (up to 83 units), above 64. The benches (`reverb_bench`, `delay_bench`, `organ_bench`) now keep the best of 5 repeats of every measurement and retry the whole declared-cost check up to 3 times before printing `FAIL`; a lowered declared cost still fails them.

Tests: `reverb_tests` (registry, RT60 per type and time, level normalisation and linearity, type-change step bound with a hard-switch control, mid-fade requests, idle/no work, bit-exact idle through the Mixer for sends 0 / REV master 0 / level 0, per-part linearity, REV master scaling and live
change, Main vs Aux instance, type/time/level through new generations, determinism), `reverb_bench`, `routing_traffic_test` / `rtsan_routing_test` (type, time, level, part sends and REV master change continuously for 10,000 blocks; both instances must run), `control_tests` (reverb over ws.v1),
offline null tests in `tools/offline-render` (`fx_reverb_room`, `fx_reverb_hall`, `fx_reverb_plate` goldens with wrong-setting renders that must fail; `offline_routing_tests` compares the render with a stand-alone `ReverbSend`).

## Delay send (Story 2.11)

`delay.hpp`/`delay.cpp` (`DelaySend`, an `Effect`) is the processor behind the Delay send bus. Same arrangement as the reverb: the Mixer owns one instance per output bus (Main, Aux), each fed by the sum of the per-part sends of the parts that sit on that bus
(`send_delay_level`, id 401, smoothed per block), its wet output (x `delay_level`) is added to the same bus, and the declared cost covers both instances (`kDelaySendCost` per bus in `kFixedFxCost`). Fixed storage (two circular buffers of 2^18 samples, about 2 MB per
instance), nothing allocates in `process()`, all RT functions are `VELORA_NONBLOCKING`. **Clean-room:** a textbook feedback delay line, no delay source was consulted. `process()` accepts any block length (slices of 64 frames; bit-identical to 64-frame calls).

```
send sum (stereo) -> (L+R)/2 -> input gains (1, 1 - pingpong) -> + headroom-limited feedback -> write L / write R (circular buffers)
read L / read R at the delay D (fractional, 4-point Catmull-Rom, glides to its target) -> held to +-1 -> x delay_level -> wet (stereo)
feedback source = (1 - pp) own read + pp other read -> damping one-pole low-pass -> DC blocker -> x feedback (<= 0.95)
```

**Registry (additive, ids stable).** Structural, master scope, set with `SetMasterParams`: `delay_time` (540, ms, 1 .. 2000, default 350), `delay_sync` (541, flag, default 0), `delay_note` (542, enum, default eighth), `delay_feedback` (543, ratio 0 .. **0.95**, default 0.35),
`delay_level` (544, return level, ratio 0 .. 1, default 1), `delay_damping` (545, ratio 0 .. 1, default 0.25) and `delay_pingpong` (546, ratio 0 .. 1, default 0). Performance, master scope: `tempo_bpm` (802, bpm, 20 .. 300, default 120; UMP channel 0, no CC; set with `SetPerformanceParam`).
The per-part delay send level is `send_delay_level` (**401**, performance, part scope, CC 94), unchanged: **the fixed encoder function DELAY binds to that id** (the reverb has 400 / CC 91). There is no delay counterpart of `send_reverb_master`: the delay's master controls are the return level and the sync/tempo settings.
`delay_note` values (enum `delay_note`, `DelayNote`): quarter 0, eighth 1, sixteenth 2, dotted quarter 3, dotted eighth 4, dotted sixteenth 5, triplet quarter 6, triplet eighth 7, triplet sixteenth 8 (beats 1, 1/2, 1/4, x 3/2 for dotted, x 2/3 for triplet). All of it is additive to `ws.v1`
(the existing master-param and performance paths; `ParamId` and `DelayNote` enums are generated from the registry; no new messages). `delaymap` is the engine's mapping: NaN and infinities become the registry default, finite out-of-range values are clamped, a flag is on at >= 0.5.

**Time.** Tempo sync off: `delay_time` in ms (1 .. 2000). Tempo sync on: `note beats x 60000 / bpm` ms (`delaymap::sync_time_ms`), limited to 5000 ms (the longest value, a dotted quarter at 20 bpm, is 4500 ms). The effective delay glides toward its target, an exponential approach (20 ms time constant)
with a rate limit of 0.25 samples per sample, i.e. the read position moves at 0.75 .. 1.25 x real time (a pitch glide like a tape delay, never a click); a delay that is idle (nothing rings) arrives at a new target at once. A new generation changes the target (the first generation applies at once); feedback,
damping and ping-pong approach their targets over about 10 ms per sample-wise one-pole; the return level ramps over one block.
**Tempo (the RT plane's single tempo value).** `tempo_bpm` is an ordinary performance parameter, so the RT plane owns the one live tempo value in the last-value table; `BlockRenderer::tempo_bpm()` reads it (clamped, any thread). Internal tempo is the only source today: it is set by a
parameter event (`ws.v1` `SetPerformanceParam`, a UMP, the offline renderer's `--tempo` / `--tempo-change`) and by the snapshot's initial value on generation change. Epic 7's external clock follower will write the same slot, so every consumer keeps this API. The Mixer reads it once per block in
`begin_block` and hands it to both delays (`set_tempo`): an event inside block k is applied at its frame, the delays see it at block k + 1 ("the next block"), where the target moves and the glide applies.

**Interpolation.** 4-point Catmull-Rom (cubic Hermite): exact samples at integer delays, polynomial-exact for linear signals (the centroid of a fractionally delayed impulse is exactly the delay), magnitude response at most 1 for every fraction (checked numerically, `test_interpolator_gain`).
**Feedback path and stability.** The feedback (strictly capped at 0.95 in code, whatever is configured: `delaymap::feedback`) comes from the reads through the ping-pong mixing matrix [[1 - pp, pp], [pp, 1 - pp]] (symmetric, rows sum to 1: gain <= 1), the damping low-pass (one-pole, gain <= 1;
cutoff `20 kHz x 0.05^damping`, 1 kHz at 1, 0 = no filter at all) and a DC blocker (high-pass at about 40 Hz, normalised to a Nyquist gain of exactly 1, so DC cannot build up in the loop). Every element has a gain of at most 1 in magnitude, so the linear loop gain is at most 0.95 per pass
and a bounded input gives a bounded output (a sparse input of amplitude a is below a / (1 - 0.95) = 20 a; the long-run test checks it). On top of that a **headroom limiter** confines what the feedback adds to the input: with the input x on a line and h = 1 - |x|, the added feedback g is the identity
up to 0.8 h and then approaches h softly, so the written value |x + g| stays below full scale; the input itself is written unchanged (the first echo is exact). The wet tap is held to +-1 as well (the cubic interpolator can overshoot a stored value by up to 25 %). Hence for any input within full scale the wet
output never exceeds full scale times the return level, whatever the feedback, glide or parameters do; a loop driven hard saturates softly instead of running away. (The dry signal plus the wet one can of course exceed full scale at the master: the Mixer adds them.)
**Ping-pong** (`delay_pingpong` p): the input enters the left line at gain 1 and the right line at 1 - p, and each line is fed back from (1 - p) itself and p the other line. 0 is dual mono; 1 sends the first echo to the left, the second to the right, and so on; between is a mix.

**Idle and transparency.** `quiet()` is true once the delay went idle and no input has arrived since; it goes idle when the return level is 0 (steady), or when the input is silent and every value written to the lines during one whole buffer length (5.5 s) stayed below -120 dBFS (`kFlushLevel`), i.e. the
line holds nothing audible. The lines are never wiped (no bulk clear exists, so no block ever pays for the 2 MB): a restart or `reset()` only resets the filters and the count of samples written since (`valid_`), and a tap older than that reads as silence, so stale data is never heard and the first block after an idle spell costs like any other (`delay_bench` prints it; tested bit-exact against a fresh instance). A quiet delay with silent input does no work (`engine_blocks()` does not advance) and
the Mixer treats it as transparent: the routing is trivial (the engines render straight into Main, bit-identical to the pre-delay behaviour) when all part delay sends are 0 **and** both delay instances are quiet; while a tail still rings after the sends went to 0 the Mixer stays routed (the tail is not cut), then goes inactive.
With sends up and the return level 0 the Mixer is routed (a send is up) but the delay does no work and the output is exactly the dry one (tested).
Mixer mapping: the `delay_*` values of the adopted snapshot's `master` drive both instances (NaN or out-of-range values are clamped by `delaymap`).

**Cost (host, not representative).** `delay_bench` (Release numbers on the dev Mac, loud input, damping and ping-pong on): one instance 0.4 us per 64-frame block steady, 0.55 us while the time glides (0.9 x an EQ insert, 0.62 us); with all 8 parts sending the Mixer adds about 0.3 us per block. `delay_bench` also times the first block after an idle spell (about 0.6 us) and checks it against the declared cost. The benches (`delay_bench`, `reverb_bench`, `organ_bench`) link a bench-private `-O3` copy of the core sources (`velora_core_o2`), so their cost assertions run in unoptimised default builds too. The declared `kDelaySendCost` is **40 units**
per instance (it was a 16-unit placeholder; raising it raises `kFixedFxCost` by 48): the bench asserts that the ratio-wise need (EQ = 16 units -> 14..22 on the Mac, up to about 36 on x86 gcc in Docker, where the gliding path is relatively dearer) is covered, in every configuration (the bench test fails on any `FAIL` line).
The dev-board measurement that replaces it is Story 2.12.

Tests: `delay_tests` (registry and mapping incl. NaN/inf/out-of-range, echo timing in ms mode at 11 times up to 2000 ms within 1 sample (sub-sample centroid exact), every note value at 7 tempos from 20 to 300 bpm, the tempo change at the next block through the renderer, click-free glides with hard-switch controls (a 1 kHz tone: glide steps stay within the tone's own slope at 1.25 x,
a hard switch steps by more than 6 x), feedback cap and stability with NaN/inf/out-of-range feedback, full-scale adversarial input at maximum feedback, DC, the limiter, the damping response against the analytic one-pole x DC-blocker response, interpolator gain, ping-pong, block-size independence, idle/restart bit-exactness, Mixer sends (analytic), Main vs Aux instances,
bit-exact idle through the Mixer for sends 0 / return level 0 / tails ringing out, settings through new generations, determinism), `delay_longrun_tests` (slow label: 3 x 10 minutes of audio at maximum feedback, 28.8 M frames each, about 8 s in a Debug build: adversarial aligned impulses, sparse quiet input, tempo-synced with the tempo changing every second;
peak <= full scale, no growth from minute to minute, the tail dies and the delay goes idle), `delay_bench`, `routing_traffic_test` / `rtsan_routing_test` (time, sync, note, feedback (beyond the cap too), level, damping, ping-pong cycle with every generation, tempo 20 .. 300 bpm by live events, part delay sends; both instances must run and every note value and both sync modes must be seen),
`control_tests` (the delay over ws.v1, tempo as a performance parameter, the delay heard), offline null tests in `tools/offline-render` (`fx_delay_ms`, `fx_delay_sync` goldens with wrong-setting renders that must fail; `offline_routing_tests` compares the render with a stand-alone `DelaySend`, tempo change included).

## Live Set parts, zones, layers and splits (Story 4.3)

A generation built from a Live Set (`Snapshot::live_set = true`, made by `engine/host`'s `LiveSetLoader`) turns the block loop into a multi-part instrument. Four pieces are in `engine/core`: the part model in the snapshot, the `PartRouter` (input-side routing), per-part instruments in the sample engine, and the part fader in the mixer.

**Part model (`PartCfg`, additive).** `low_key`/`high_key`, `octave` (-4..4), `transpose` (-12..12), `keyboard_on`, `state` (`PartState`: `kReady`, `kEmptyTimbre`, `kMissingLibrary`, `kEngineNotAvailable`), `engine_type` (`EngineTypeId` value), `volume` (0..1) and `sample_bank` (the part's prepared instrument). Their defaults are the pre-4.3 behaviour; nothing reads them unless `Snapshot::live_set` is set. A part that is not `kReady` maps to no engine at the generation change: it is silent and never substituted.

**Keyboard channel convention.** In a Live Set generation every note on / note off, on *any* UMP channel 0..15, is a keyboard event; the channel only names the keyboard source (it is kept, so two sources are two keys) and is not a part address. The router fans each event out to the parts and the part events then take the existing channel == part path (part p is UMP channel p; engines, arbiter, mixer and parameter events are unchanged). Outside a Live Set generation the router is idle and channel c plays engine c as before, so every pre-4.3 render and null test is bit-identical. Parameter events (and the registry-mapped CCs) keep addressing a part by their channel. `contact_k` events (the organ keybed) keep channel == part addressing and are not zone-routed. Pitch bend, mod wheel and every controller other than the pedal are *ignored events* in Live Set mode (counted in `ignored_events()`): no engine reads them yet, so nothing is passed to the parts; fanning them out to all parts is for when an engine does.

**Zones, layers, splits.** A part with the keyboard on sounds a key only when `low_key <= key <= high_key`, the key being the one *as played*. Octave and transpose only move the pitch handed to the part (`key + 12 * octave + transpose`), never the zone test: overlapping zones layer, adjacent zones split (B2 = 47 / C3 = 48 is a split of `[0, 47]` and `[48, 127]`). A pitch outside 0..127 drops that part's note (`router().dropped_out_of_range()`); a key no part takes counts in `unzoned_keys()`.

**Note-off consistency.** For every (source channel, key) the router records, per part that got the note on, the pitch it sounded and the engine it went to. The note off releases exactly those, with that pitch and engine, whatever zones, keyboard flags, octave, transpose, engine or Live Set the part has now. A part with the keyboard off gets no new notes but still gets the note off of a key it received before (`router().held_keys()` reaches 0 when every key is up). A note on for a key that is still recorded first releases the part's earlier note when its pitch or engine would now differ. Keys (or keyboard sources) that end up on the same (part, pitch) on the same engine are counted: the part's note off goes out when the LAST of them lets go, in either order, so one key's release never cuts a note another still holds. The engine id is kept whole (`kTestTone` = 0xF000 included) and is part of that identity. The pedal: a note off the pedal holds is released at pedal up with the engine it went to; if the part's engine changed in between and the same (part, note) is struck again, the pedal-held voice of the old engine is released at that note on (one deferred slot per key).

**Leaving a Live Set.** When the block loop adopts a plain generation after a Live Set one, the notes the router still holds are released (with their recorded engine and pitch), the sample engine forgets its per-part banks (`Engine::clear_part_sample_banks()`: they belong to generations that get freed) and the channels get the engine mapping they had before the Live Set. **Release triggers.** The sample engine records the bank each key started on; a release-trigger region fires at note off only on that bank, so a part rebound while the key is down never mixes the new bank with the old note. In a Live Set generation every note on / off binds the generation to the key whenever the engine started a voice, even if a later region found the pool exhausted (voices hold their bank by pointer).

**Sustain.** CC 64 is a keyboard event too: pedal down reaches the parts with the keyboard on, pedal up reaches every part (so a part switched off while the pedal held its notes still lets them go).

**Per-part instruments in the sample engine (the design decision).** The engine registry and the arbiter hold one engine per type id, so a Live Set cannot add a SampleEngine per part. Instead `SampleEngine` keeps *banks*: `SampleBank` (`sample_bank.hpp`) is the prepared form of one instrument (the former per-region tables, per-note region lookup and the round-robin counters), built on the control thread by `SampleEngine::prepare(src, owner)` and owned by the snapshot generation (`PartCfg::sample_bank`, a `shared_ptr` whose identity is the instrument's). At a generation change the block loop binds each part's bank with `Engine::set_part_sample_bank(part, bank)` (a pointer store, real-time safe); a part never bound plays the default instrument of `set_instrument()` (the single-instrument path of Story 1.15, same arithmetic, same null tests). A voice keeps the bank it started on, and the generation that started the note holds a ledger reference until its voices are gone (also for release-trigger voices started at note off), so a rebind never cuts, corrupts or frees an instrument under a sounding note. Heads and streaming (Stories 1.16/1.17) plug in behind the `SampleSource` the bank borrows from; the host-side seam is `HeadProvider`.

**Part fader.** `PartCfg::volume` scales the part's feed to the bus it sits on (`Mixer::adopt`, main/aux target = volume, ramped like any bus change; 1 = unity = the previous value). The part's sends follow the fader. A volume below 1 makes the routing non-trivial.

**Arbiter.** `LiveSetConfig` gains `waive_sample_floor` and `waive_organ_reservation` (default false): a Live Set with no sample part reserves a sample floor of zero, one with no organ part reserves nothing for the organ, even though the engines stay bound; the waivers apply only when the floor / reservation really is zero. The loader fills the rest from the loaded parts (`fill_fx_costs()`, `OrganEngine::reservation_cost_units(organ_parts)`, a 256 voice sample floor when a sample part exists; the sample engine capacity must be at least floor + 64).

**Part count.** This build's engine has `core::params::kPartCount` = 8 parts. A Pro 16-part Live Set is therefore not loadable here: the loader answers `kPartCountUnsupported` (and a 16-part file on a v1 tier is invalid).

**Tests.** `part_router_tests` (router unit, layer, split at B2/C3 with notes on both sides and exactly at the boundary, zone on the played key, octave/transpose pitch, out-of-range drop, keyboard off with a held note, note off after a zone/transpose/instrument change, retrigger, silent parts, sample + organ in one Live Set, sustain, pedal across an engine change, part volume, channel conventions); `part_router_traffic_test` / `rtsan_part_router_test` (10,000 blocks of dense keyboard traffic on 16 channels with the pedal, a new generation every 3 blocks that moves zones, octave, transpose, keyboard flags, volume, part state, engine and instruments, a pool small enough to steal; every key released at the end means no voice and no router key left; the RTSan build runs it as a nonblocking function; the plain build also checks two runs are bit-identical). The Live Set golden and the CLI are in `tools/offline-render/README.md`, the loader in `engine/host/README.md`.


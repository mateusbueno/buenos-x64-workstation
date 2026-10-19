# tools/offline-render

Role: offline renderer and null-test harness driving `engine/core` without a live audio device.

Dependency rule: may depend on `engine/core`, `engine/ports`, `engine/host` (the bundle loader for the sample engine; the Live Set loader and protobuf for `--live-set`) and `formatlib`; nothing in `engine/` may depend on it.

## Tools

- `velora-offline-render --midi in.mid --out out.wav [--engine test-tone] [--voices N] [--tail-seconds S] [--detune-cents C]`
  Renders a Standard MIDI File (format 0/1, PPQ) through `BlockRenderer` to a 32-bit float, stereo, 48 kHz WAV.
  Tick-to-frame conversion uses the file's tempo map in integer arithmetic (default 120 bpm); each event is applied
  at its exact in-block offset. Output is bit-identical across runs. `--detune-cents` is test-only (used to prove
  the null test fails on a wrong render). A malformed SMF exits non-zero and writes no output.
- `velora-null-test --reference r.wav --candidate c.wav [--candidate ...] --tolerance-db X --max-offset-frames N`
  or `--case file.case` (key=value lines: `reference`, `tolerance_db`, `max_offset_frames`; the reference path is
  relative to the case file; flags override). Tries integer alignment offsets only within +-max, reports per-file
  residual dB (residual RMS relative to reference RMS, overlap only), the chosen offset, length difference and the
  worst 100 ms window, and exits non-zero on failure. A silent reference passes only against a silent candidate.

- `velora-offline-render --engine sample --bundle B --instrument KEY [--seed N] [--gain-db G] [--interpolation hermite|linear] [--voices N]`
  (Story 1.15) renders the MIDI through `SampleEngine` playing instrument `KEY` (its `instrument_key`) of the bundle file `B`
  (`<root>/lib/<library_id>/<bundle_id>/bundle.vlb`, made by `librarian import`). All samples are decoded to RAM first; voices default to
  64; the report line counts unplayable regions and pool exhaustion. `--seed` seeds `lorand/hirand`; `--gain-db` scales the
  output; `--oracle-block-frames N` (test only) emulates sfizz's per-voice block-boundary hold (see `engine/core/README.md`).
  `--bundle/--instrument` are errors without `--engine sample`; an unknown instrument or an unreadable bundle exits 1 and writes nothing.
- `velora-null-test` also takes `--max-length-diff-frames N`, `--check-excess` (the energy outside the compared overlap, i.e. a tail in
  one file only, must stay below the tolerance too) and `--reference-decode sfizz-s16` (case keys `check_excess`, `reference_decode`).
  16-bit PCM WAVs are readable; `sfizz-s16` undoes `sfizz_render`'s float->s16 conversion (dr_wav maps silence to -1 and scales by
  32767.5, so the reference is decoded as `(s + 1) / 32767.5`), which would otherwise inject a -90 dBFS DC offset and a gain error of 1.5e-5.

- `velora-offline-render --engine organ [--drawbars 888000000]` (Story 2.2) renders the MIDI through the tonewheel organ on
  channels 0..7 (one organ part per MIDI channel; notes on channels 8..15 are refused as engine-not-available and stay silent). `--drawbars` is nine digits 0..8 for 16', 5 1/3',
  8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' (default `008000000`), applied to every part at frame 0.

- `--percussion second|third,fast|slow,soft|normal` (any subset, each category once, unnamed ones keep the registry default
  third, fast, soft) or `--percussion off`, and `--key-click 0..1` (default 0.5) (Story 2.3) set the organ's percussion and key click on every
  part at frame 0; both are errors without `--engine organ`. The MIDI file can change them too: control changes are now parsed and
  those mapped by the registry (`proto/params.json` `cc`: drawbars 70-78, percussion 80, key click 82, percussion harmonic/decay/volume
  84/85/86, ...) become performance parameter events on the channel's part; unmapped controllers (volume, pedal, ...) are dropped and do
  not extend the render.

## Organ percussion golden (Story 2.3)

`tests/organ_percussion/` holds `organ_percussion.mid` (made by `make_midi.py`: staccato and legato playing, all eight
harmonic/decay/volume combinations through CC 80/84/85/86, then key click through CC 82 with a repeated key), `organ_percussion.wav`
(about 4.85 s, 1.9 MB) and `organ_percussion.case` (null test, -100 dB). ctest renders twice (`organ_perc_bit_identical`),
null-tests the fresh render against the golden (`organ_perc_null_golden`; a null test rather than a byte compare because libm may differ
in the last bit between macOS and Linux) and requires a different key click level and a different registration to fail it. The render
command is `velora-offline-render --midi organ_percussion.mid --engine organ --percussion off --key-click 0.4 --tail-seconds 0.3`.
Regenerate after an intentional engine change with `cmake --build build --target update-organ-percussion-golden` and review.

## Organ scanner and leakage golden (Story 2.4)

`velora-offline-render --engine organ` also takes `--vibrato off|v1|v2|v3|c1|c2|c3` (default off) and `--leakage 0..1` (default 0.5, the registry
default), applied to every organ part at frame 0; both are errors without `--engine organ`. The file's CC 81 (on/off), CC 87 (type) and CC 83
(leakage) change them later. `tests/organ_scanner/` holds `organ_scanner.mid` (made by `make_midi.py`: a six-note chord held for 4.4 s on part 0
while the controllers step through V1, V2, V3, C1, C2, C3, off, then raise leakage in two steps and finish in C3; a short chord on part 1 stays
unscanned), `organ_scanner.wav` (about 4.7 s, 1.8 MB) and `organ_scanner.case` (null test, -100 dB). The render command is
`velora-offline-render --midi organ_scanner.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2 --vibrato off --leakage 0 --tail-seconds 0.3`.
ctest renders twice (`organ_scan_bit_identical`), null-tests the render against the golden (`organ_scan_null_golden`) and requires a different
starting scanner setting or leakage to fail it. Regenerate after an intentional engine change with
`cmake --build build --target update-organ-scanner-golden` and review. The percussion golden now passes `--leakage 0` (leakage did not exist
when it was made), so it is unchanged.

## Organ multi-contact golden and `--events` (Story 2.5)

`velora-offline-render --events file.contacts` (test only, `--engine organ`; `--midi` becomes optional, and when both are given the events are merged by
frame, MIDI first on ties) reads a small deterministic text event list, because a Standard MIDI File cannot carry the per-note `contact_k`
controller. Format (full description in `src/events_file.hpp`): one event per line, `<time> <kind> <args>`, `#` comments, unsorted allowed;
times are frames (`480` or `480f`) or milliseconds with up to three decimals (`12ms`, `0.125ms`, floored to 48 frames per ms with integer
arithmetic); kinds `on ch note [vel]`, `off ch note`, `close ch note k`, `open ch note k` (k 0..255, the organ refuses what is out of range),
`cc ch controller value`; `--midi` events come first on a frame tie (test `organ_tie_midi_first`); a `contacts 2|3` line sets the keybed's contacts per key (default 3). Errors name the file and line and write nothing.

`tests/organ_contacts/` holds `organ_contacts.contacts` (made by `make_events.py`: rolled chords with 1 ms between contacts, a 20 ms smear, half
presses, a skipped contact, malformed traffic, a Note On/Off-only key, percussion on the first contact), `organ_contacts_two.contacts` (the same
events for a two-contact keybed), `organ_contacts.wav` (about 2.55 s, 0.98 MB) and `organ_contacts.case` (null test, -100 dB). The render command is
`velora-offline-render --events organ_contacts.contacts --engine organ --drawbars 888600000 --percussion off --key-click 0.3 --vibrato off --leakage 0 --tail-seconds 0.3`.
ctest renders twice (`organ_contacts_bit_identical`), null-tests the render against the golden (`organ_contacts_null_golden`) and requires other
drawbars, another key click level and the two-contact list to fail it. Regenerate after an intentional engine change with
`cmake --build build --target update-organ-contacts-golden` and review.

## Golden render

`tests/golden/` holds `golden.mid`, `golden.wav` and `golden.case`. ctest renders twice (byte-equality), null-tests a
fresh render against the golden, and requires a `--detune-cents 20` render to fail. Regenerate the golden after an
intentional engine change with `cmake --build build --target update-golden` and review the diff.

## Sample engine against sfizz (Story 1.15)

`tests/sample_suite/` holds a synthetic SFZ library (generated by `generate_suite.py`, tiny 16-bit WAVs), MIDI files, and sfizz
renders of them as committed goldens (`golden/*.wav`, the untouched `sfizz_render` output, ~1.6 MB). ctest runs, per case,
`librarian import` (in process) -> `velora-offline-render --engine sample` -> `velora-null-test --case` against the golden
(`sample_null_<case>`), so **no sfizz is needed to run the tests**. Further tests: `sample_null_neg_gain` / `sample_null_neg_interp`
(a 1 dB gain error and the wrong interpolation must fail the same null test), `sample_render_cli_errors`, and the env-gated
`sample_demo_null_test` for real libraries (`VELORA_SFZ_DEMO_DIR`; skipped, exit 77, without it or without an oracle). Tolerances,
measured residuals and every deviation are in `tests/sample_suite/README.md`. Regenerating the goldens needs the oracle:
`tests/sample_suite/regenerate_goldens.sh` (see `tools/oracle/README.md`).

## Oracle rule

A reference oracle (sfizz's full engine) may only be used offline to produce reference renders for null tests. It is never linked
into any shipped target and no oracle source is in this repository: `tools/oracle/` fetches sfizz 1.2.3 into an ignored build
directory and builds its `sfizz_render` CLI there. `tools/ci/check_third_party_notices.py` enforces this.

## Organ rotary speaker golden (Story 2.6)

`velora-offline-render --engine organ` also takes `--rotary off|slow|fast|stop` (default off, the registry default), applied to every organ part at frame 0
(`slow`/`fast`/`stop` set the speed and switch the rotary stage on at once); it is an error without `--engine organ`. The file's CC 89 (stage on/off) and CC 79
(speed: 0-31 stop, 32-95 slow, 96-127 fast) change them later, and mid-file speed changes ramp with the horn and drum time constants. With the stage on the output is
a true stereo pair. `tests/organ_rotary/` holds `organ_rotary.mid` (made by `make_midi.py`: a six-note chord held for 5 s on part 0 while CC 89 switches the stage on at 0.3 s
and CC 79 goes fast at 1.0 s, stop at 2.4 s and slow at 4.2 s; a short chord on part 1 stays unprocessed), `organ_rotary.wav` (about 5.3 s, 2.0 MB) and `organ_rotary.case`
(null test, -100 dB). The render command is `velora-offline-render --midi organ_rotary.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2 --vibrato off
--leakage 0 --rotary off --tail-seconds 0.3`. ctest renders twice (`organ_rotary_bit_identical`), null-tests the render against the golden (`organ_rotary_null_golden`) and requires
a different starting rotary state (`--rotary fast`, `--rotary stop`) to fail it. Regenerate after an intentional engine change with `cmake --build build --target
update-organ-rotary-golden` and review. The other organ goldens do not use the rotary and are unchanged.

The CLI mapping is pinned by `organ_rotary_map_{slow,fast,stop}_equal`: `--rotary X` on `organ_map_plain.mid` (a 1 s chord, made by `make_map_midi.py`) renders byte-equal
to `organ_map_X.mid` (the same chord with CC 79 = 64 / 127 / 0 and CC 89 at tick 0) with `--rotary off`.

## Routing options (Story 2.7)

`velora-offline-render` can run the fixed routing topology so its null tests can check it: `--part-output P:main|aux` (repeatable; parts are the MIDI channels 0..7), `--send P:REVERB[:DELAY]` (levels 0..1),
`--inserts P:type[@p1/p2/..][!],...` (at most 5 of `none rotary chorus_phaser ep_amp eq compressor`; `@` gives the normalised insert parameters 1..5, each 0..1, missing ones keep the defaults; `!` bypasses the slot; every type is real DSP: EQ and compressor since Story 2.8, chorus/phaser, EP amp and rotary since Story 2.9), `--master-level DB` (-60..12),
`--line-in in.wav [--line-in-level 0..1]` (mono or stereo 48 kHz WAV, mixed into the master Main bus only, silent after its end) and `--aux-out aux.wav` (the Aux bus, same length as the Main output). Any of them publishes
a snapshot with those settings before the first block; with none of them no snapshot is published and the render is the pre-2.7 one, bit for bit (all existing goldens). The reverb return (Story 2.10) and the delay return (Story 2.11) are real DSP (below): tests that want plain sums mute them with `--reverb-level 0 --delay-level 0`. `tests/routing_test.cpp` (`offline_routing_tests`) does exactly that: Main/Aux assignment, send levels, master level, line-in to Main only and pass-through
chains, each as a null test at -110..-120 dB against the analytically mixed reference, plus the CLI flags and their error cases.

## EQ and compressor (Story 2.8)

`--inserts 0:eq@LOW/MID/MIDFREQ/HIGH` and `--inserts 0:compressor@THRESHOLD/RATIO/ATTACK/RELEASE/MAKEUP` set the normalised insert parameters (mapping table in `engine/core/README.md`); the master section is
`--master-eq LOW_DB:MID_DB:MID_HZ:HIGH_DB[!]` (gains -15..15, mid 200..8000 Hz) and `--master-comp THRESHOLD_DB:RATIO:ATTACK_MS:RELEASE_MS:MAKEUP_DB[!]` (-60..0, 1..20, 0.1..100, 10..1000, 0..24), `!` = the stage is bypassed.
Without them the master EQ is flat and the compressor 1:1 without makeup, which is exactly transparent. Goldens (`tests/fx_eq/`, `tests/fx_compressor/`): the golden test-tone MIDI through an insert and the master effect on part 0
(`--inserts 0:eq@0.9/0.2/0.6/0.8 --master-eq -4:5:2500:3` and `--inserts 0:compressor@0.2/0.6/0.3/0.4/0.15 --master-comp -30:3:5:150:2`, `--tail-seconds 0.25`); ctest renders each twice (`fx_*_bit_identical`),
null-tests it at -100 dB (`fx_*_null_golden`) and requires a changed insert or master setting to fail (`fx_*_wrong_insert_fails`, `fx_*_wrong_master_fails`). `offline_routing_tests` also checks the master EQ, compressor,
their order and bypasses, and the insert effects against the same DSP classes applied to the parts rendered alone. Regenerate a golden after an intentional change by rendering with the commands above over the `.wav`.

## Chorus/phaser, EP amp and rotary inserts (Story 2.9)

`--inserts 0:chorus_phaser@MODE/RATE/DEPTH/FEEDBACK/MIX`, `--inserts 0:ep_amp@DRIVE/TONE/PRESENCE/LEVEL/CABINET` and `--inserts 0:rotary@SPEED/WIDTH` set the normalised insert parameters (mapping table in `engine/core/README.md`). Goldens (`tests/fx_chorus_phaser/`,
`tests/fx_ep_amp/`, `tests/fx_rotary_insert/`): the golden test-tone MIDI through the insert on part 0 (`--tail-seconds 0.25`): `--inserts 0:chorus_phaser@0.0/0.55/0.8/0.5/0.6,chorus_phaser@1.0/0.4/0.8/0.8/0.6` (a chorus slot, then a phaser slot),
`--inserts 0:ep_amp@0.7/0.6/0.5/0.55/0.5` and `--inserts 0:rotary@0.9/0.5` (fast). Same scheme as the Story 2.8 goldens: ctest renders each twice (`fx_*_bit_identical`), null-tests it at -100 dB (`fx_*_null_golden`) and requires a different insert setting to fail
(`fx_*_wrong_insert_fails`, `fx_*_wrong_master_fails`; these effects have no master counterpart, so the second wrong render is another changed insert setting). Regenerate a golden after an intentional change by rendering with the command above over the `.wav`.

## Reverb (Story 2.10)

`--reverb room|hall|plate` (default hall), `--reverb-time S` (RT60 at 1 kHz, 0.2..10 s, default 2), `--reverb-level 0..1` (return level, default 1) and `--rev-master 0..1` (the REV master send level, default 1) set the reverb of the
snapshot the routing options publish; the part send levels are `--send P:REVERB[:DELAY]`. Invalid values exit 2 and write nothing. With every send at 0 (or `--rev-master 0` or `--reverb-level 0`) the render is bit-identical to the default path.
Goldens (`tests/fx_reverb_room/`, `fx_reverb_hall/`, `fx_reverb_plate/`, one per type, 1.75 s each): the golden test-tone MIDI with `--send 0:0.7 --reverb-level 0.8 --rev-master 0.9 --tail-seconds 1.0` and the type with its time
(`--reverb room --reverb-time 0.9`, `hall` 1.4, `plate` 1.1); the 1 s tail is the reverb ringing out. ctest renders each twice (`fx_reverb_*_bit_identical`), null-tests it at -100 dB (`fx_reverb_*_null_golden`, with `check_excess`) and requires a wrong type,
time, return level, REV master or part send level to fail (`fx_reverb_*_wrong_*_fails`). Regenerate after an intentional change with `cmake --build build --target update-reverb-goldens` and review.
`offline_routing_tests` (`test_reverb`) also null-tests Main and Aux against a stand-alone `ReverbSend` fed with the part's dry render x send x REV master (-110 dB), per type.

## Delay (Story 2.11)

`--delay-time MS` (1..2000, default 350), `--delay-sync on|off` (default off), `--delay-note 1/4|1/8|1/16|1/4d|1/8d|1/16d|1/4t|1/8t|1/16t` (the registry enum `delay_note`; `d` dotted, `t` triplet; default 1/8), `--delay-feedback 0..0.95` (default 0.35), `--delay-level 0..1` (return level, default 1), `--delay-damping 0..1` (default 0.25)
and `--delay-pingpong 0..1` (default 0) set the delay of the snapshot the routing options publish; the part send levels are `--send P:REVERB:DELAY`. `--tempo BPM` (20..300, default 120) is the initial value of the RT plane's tempo, and `--tempo-change SECONDS:BPM` (repeatable) sends a `tempo_bpm` performance event at that frame
(the delay sees it at the next block and glides to the new time). Each option may be given once (`--tempo-change` as often as wanted); invalid values exit 2 and write nothing. With every send at 0 (or `--delay-level 0`) the render is bit-identical to the default path.
Goldens (`tests/fx_delay_ms/`, `tests/fx_delay_sync/`, 2.25 s each): the golden test-tone MIDI with `--send 0:0:0.7 --tail-seconds 1.5` and either `--delay-time 230 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5` (milliseconds) or
`--delay-sync on --delay-note 1/8d --tempo 100 --tempo-change 0.5:140 --delay-feedback 0.55 --delay-level 0.8 --delay-damping 0.3` (tempo-synced, the tempo changing during the render). ctest renders each twice (`fx_delay_*_bit_identical`), null-tests it at -100 dB (`fx_delay_*_null_golden`, with `check_excess`)
and requires a wrong time, feedback, return level, damping, ping-pong, send level or sync setting (milliseconds golden) and a wrong note value, initial tempo, tempo-change value, tempo-change time, a missing tempo change or sync off (synced golden) to fail (`fx_delay_*_wrong_*_fails`). Regenerate after an
intentional change with `cmake --build build --target update-delay-goldens` and review. `offline_routing_tests` (`test_delay`) also null-tests Main and Aux against a stand-alone `DelaySend` fed with the part's dry render x send (-110 dB), in milliseconds mode and tempo-synced with two tempo changes, and shows that a tempo change is ignored while sync is off.

## Live Set rendering and golden (Story 4.3)

`velora-offline-render --live-set FILE --library-root DIR --midi in.mid --out out.wav [--scene-at SECONDS:INDEX]... [--seed N] [--tail-seconds S]` loads a Live Set through the engine host's `LiveSetLoader` (the same code path as `engine-host --live-set`) into a fresh block renderer with the sample engine and the organ bound (`render_live_set.cpp`), and renders the MIDI through it. `DIR` is the library root (`<DIR>/lib/<library_id>/current`). The Live Set decides engines, routing and instruments, so no other engine, instrument, routing or organ option may be combined with it. Every note on or off in the MIDI file, on any channel, is a keyboard event fanned out to the parts by zone (`engine/core/README.md`); CC 64 is the sustain pedal for every part; registry-mapped CCs address the part that is their channel; contact events are not used. The report prints each part's state (ready, empty timbre, missing library, engine not available) and the router's drop counters. A Live Set that does not load exits 1 and writes nothing.

`tests/live_set/` holds the golden: `layer_split.liveset` (bass up to B2 = 47, piano from C3 = 48, a pad layered under the whole keyboard an octave down on the Aux bus, the piano doubled an octave up from C5, and four silent parts: keyboard off, missing library, FM, empty timbre), `root/` (three tiny libraries in the resolver's layout, `lib/<library_id>/current -> <bundle_id>/bundle.vlb`), `layer_split.mid` (made by `make_midi.py`: the notes on both sides of the split and exactly at the boundary, a chord across it, the doubling zone and a key whose doubling is out of range, a second keyboard channel, the sustain pedal) and `layer_split.wav` with `layer_split.case`. Tests `live_set_*`: two renders are bit-identical, the fresh render null-tests against the golden at -100 dB (`check_excess`), a Live Set with the split moved by one key (`wrong_split.liveset`) or the pad octave changed (`wrong_octave.liveset`) fails the null test, and bad option combinations and unreadable Live Sets are refused. The fixtures are made with integer arithmetic only (no libm) by `live-set-fixture-gen`; regenerate them on purpose with `cmake --build build --target live-set-fixtures`, then the golden with `cmake --build build --target update-live-set-golden` (review the diff).


## Scene recall and golden (Story 4.4)

`--scene-at SECONDS:INDEX` (Live Set mode only, repeatable, INDEX 0..7) recalls scene INDEX through the `SceneController` (the same code path as a ws.v1 `RecallScene`) at the start of the 64-frame block that holds SECONDS (the last block included), before that block's events; recalls are applied in order of time whatever the order on the command line. A scene the Live Set does not have (`scene N does not exist (the Live Set has K scenes)`, checked before rendering), a non-integer index or a recall after the end of the render (last note plus tail) is an error and nothing is written; the report prints the number of recalls. Without `--scene-at` no scene is recalled (the base Live Set plays, as in Story 4.3).

`tests/live_set/scenes.liveset` (made by `live-set-fixture-gen`, the same three libraries plus an organ part) has the bass up to B2, the piano from C3, a pad an octave down on Aux and an organ, both with the keyboard off in the base, and three scenes: Verse (the pad joins up to B3, the piano softer), Chorus (the pad on the whole keyboard, the organ with a brighter registration, more piano reverb) and Outro (the piano alone, bass off). `scenes.mid` (made by `make_scenes_midi.py`) holds notes, a pedal-sustained note and notes that are releasing across both recalls (`--scene-at 0:0 --scene-at 2.0:1 --scene-at 4.5:2`). Tests `scenes_*`: two renders are bit-identical, the order of the options does not matter, the render null-tests against `scenes.wav` at -100 dB (`scenes.case`, `check_excess`), the scenes in the wrong order (`0:0 2.0:2 4.5:1`) and the same render with no recall both fail it, and bad `--scene-at` values and a missing scene are refused. Regenerate with `live-set-fixtures` and then `cmake --build build --target update-scenes-golden` (review the diff).

## Seamless Live Set switch and golden (Story 4.5)

`--switch-at SECONDS:LIVESET_FILE` (Live Set mode only, repeatable) switches seamlessly to another Live Set at the start of the 64-frame block that holds SECONDS, before that block's events, through the engine host's `LiveSetSwitcher` (the same code path as a ws.v1 `JumpToLiveSet`; jumps are applied in order of time). The background load is **simulated deterministically**: the switcher runs without its worker thread and the new generation is published `--switch-load-blocks N` blocks after the jump (default 0 = in the same block; the sound of the old Live Set continues untouched meanwhile), so a render is a pure function of its inputs. Held, sustained and releasing notes finish on the old Live Set (with the pitch, part and engine they started on), new notes play on the new one, and the voice-pool reservation of the new Live Set takes over in the block that adopts the generation. Every target file is read before rendering; an unreadable or invalid one, a switch after the end of the render, a load that has not completed by the end (`did not complete before the end of the render`) and a refused switch are errors and nothing is written; the report prints the number of switches. `--scene-at` indices are checked against the Live Set that is loaded first.

`tests/live_set/switch/` holds the two Live Sets (made by `live-set-fixture-gen`, the same three libraries): `switch_a.liveset` (bass up to B2, piano from C3 with reverb, a long-release pad an octave down on Aux) and `switch_b.liveset` (the pad on the whole keyboard, the piano an octave down from C3 with more reverb, the bass an octave up below C4, an organ from C4: different zones, octaves, outputs and engines, and a pool reservation that includes an organ where A's does not). `switch.mid` (made by `make_switch_midi.py`) plays a piano chord (52, 55, 59) held across the swap, a pad note whose 0.8 s release is still sounding at the swap, two notes sustained by the pedal and released on B's watch (pedal up at 4.0 s), and new notes on B (one under the pedal, the organ zone included), with `--switch-at 3.0:switch/switch_b.liveset --switch-load-blocks 100`. Tests `switch_*`: two renders are bit-identical, the render null-tests against `switch.wav` at -100 dB (`switch.case`, `check_excess`), and a render with no switch, a switch at the wrong time (3.6 s) and a different simulated load time (700 blocks) all fail it; bad `--switch-at` / `--switch-load-blocks` values, a missing file, a switch after the end and an unfinished load are refused. Regenerate with `live-set-fixtures` and then `cmake --build build --target update-switch-golden` (review the diff).

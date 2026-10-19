# engine/host

Role: host/OS adapters (ALSA, UMP, input edge threads, io_uring streaming, OP-TEE, control thread) implementing `engine/ports`.

Dependency rule: may depend on `engine/core`, `engine/ports` and OS libraries; `engine/core` must never depend on it.

## Portable code (all platforms, unit-tested in ctest)

- `time_mapper` -- fixed-scheduling-delay rule: `due = event_ns + delay`; `frame = round((due - block_start) * rate / 1e9)`.
  Late (due before block start) -> frame 0 and counted; beyond the block -> deferred by the renderer;
  beyond the 100 ms horizon -> clamped to the horizon and counted. Monotone, so events are never reordered.
  Default delay 3 ms.
- `midi_ingest` -- normalises UMP words (MIDI 1.0 type 2 up-converted, MIDI 2.0 type 4 passed through) to note on/off,
  stamps them with host time and pushes them on an SPSC queue (single producer = the MIDI thread).
  Non-note messages are ignored and counted; a full queue drops and counts (never blocks).
- `render_driver` -- the audio-thread path shared by the live CoreAudio callback and the device-free tests:
  drain queue -> map timestamp to frame -> `BlockRenderer::submit` -> `process_block`. No allocation, locks or blocking
  (`VELORA_NONBLOCKING`; `rtsan_host_render_test` checks it under RealtimeSanitizer where the compiler supports it).

- `mcu_frame` -- Story 3.1. C++ wrapper (`velora/host/mcu_frame.hpp`) over the portable C library in `mcu/common`: typed messages
  (`Message` variant), `make_frame`/`parse_message`/`encode`, `StreamDecoder` (any chunking, counters, events), and the
  `FrameWriter`/`Loopback` helpers for Stories 3.2/3.5 and the Mac virtual panel. The generated `velora/host/mcu_ids.hpp` holds
  the constexpr control/button/LED ID tables. Control/I-O thread only (it allocates); the C library underneath never does.
- `library_resolver` (+ `fs_port`, `posix_fs`) -- Story 1.14. Resolves `(library_id, instrument_key)` to an opened bundle
  and its instrument entry from the index, never by scanning. Control/I-O thread only (allocates, does file I/O); not for
  the audio thread. Links `velora_format`; `engine/core` does not.
  - Layout (root is a parameter, default `/data`): `<root>/lib/<library_id>/current -> <bundle_id>` (relative symlink,
    one component, 64 lowercase hex) and `<root>/lib/<library_id>/<bundle_id>/bundle.vlb` (`kBundleFileName`, shared with
    `tools/library-gen`). `library_id` is a canonical lowercase UUID string.
  - No-scan rule: the resolver only does `lstat(current)`, `readlink`, `lstat(<bundle_id>)`, `open(bundle.vlb)` and three
    preads (header, manifest, index): a constant 8 filesystem ops regardless of library count or size, zero directory
    listings, no sample blocks read. All access goes through `FsPort`; `CountingFs` counts ops so tests assert this.
  - Results: `kOk`, `kInvalidId` (rejected before any fs access: non-UUID, `..`, `/`, empty), `kMissingLibrary`
    (absent/dangling `current`), `kMissingInstrument`, `kBundleError` (+ formatlib `Error`), `kInvalidLink` (not a symlink
    to a sibling `<bundle_id>` directory: absolute, nested, `..`, loop), `kIdentityMismatch` (bundle's library_id/bundle_id
    disagree with its path). A failed resolve never substitutes another instrument or library.
  - The returned `shared_ptr<const BundleReader>` keeps the file descriptor open (the lease) until released. Opening reads the
    whole index into RAM (formatlib behaviour); fine for the test bundles, to be revisited for very large single bundles.
  - Tests: `library_resolver_test` (every matrix row, op-count equality across 10 / 1,000 (1 GiB sparse each) / 3,000 (20,000 opt-in)
    libraries, no-readdir and only-requested-paths assertions). Default scale is 10 / 1,000 / 3,000 libraries; set `VELORA_SCALE_TEST=1` to add the 20,000-library case (~30 s to generate). Timings it prints are "host, not representative".

- `sample_bundle_loader` -- Story 1.15. Decodes every sample of one bundle instrument into RAM as planar float (integer PCM
  `value / 2^(bits-1)`, float32 bit for bit; no resampling or requantization) and exposes it with the region table through the
  `engine/ports` sample-source port (`BundleInstrument`) for the `SampleEngine`. Control thread only (allocates, decodes). A sample
  that fails to decode is left empty and counted (`failed_samples()`), so the engine skips and counts its regions instead of playing
  garbage; an unknown `instrument_key` is `kInstrumentNotFound` and nothing else is substituted. Test: `sample_bundle_loader_test`.

## Live Set format and store (Story 4.2, in `velora_control`)

The persisted Live Set is the protobuf `velora.liveset.v1.LiveSet` (`proto/live_set.proto`; the file format, distinct from the
`ws.v1` wire protocol, reusing its `InsertSlot` and `PartOutput`). `velora/host/live_set_store.hpp` is the control-thread side
(control/I-O threads only, never audio; protobuf stays out of `engine/core`).

- **Schema.** `format_version` (current N = 1), `id` (UUIDv4, canonical lowercase), `name` (1..64 bytes), `tempo` (the range of registry `tempo_bpm`, 20..300 bpm; read from the generated registry),
  exactly the tier's part count of `Part`s (8 v1, 16 Pro), 0..8 `Scene`s, `assign` (exactly 8 slots, 0 = empty, else a
  *performance* `param_id`) and `master` (master-scope registry params). A `Part` holds `timbre` `(library_id, instrument_key)`
  or unset, `low_key`/`high_key` 0..127 (low <= high), `octave` -4..4, `transpose` -12..12 semitones, `keyboard_on`, `volume` 0..1,
  `output` Main/Aux, `inserts` (<= 5 `ws.v1.InsertSlot`), `send_reverb`/`send_delay` (registry 400/401 ranges), `engine`
  (`engine_type_id` + `(param_id, value)` list: no per-engine message, so FM/VA fit), `envelope` and `eq` (lists of registry
  part-scope params) and `arp` (`ArpSettings`: enabled, pattern, rate, gate, octave_range, hold, swing; all optional, unset in v1,
  names from Epic 7 stories 7.2-7.4; using them needs no version bump or migration).
- **Scene semantics.** A scene is a full parameter snapshot *excluding the timbre assignment*, stored sparsely: each `ScenePart`
  field is `optional` (presence flag) and the `engine_params`/`envelope`/`eq`/`master` lists override by `param_id`; an `InsertChain`
  wrapper replaces the whole chain; `arp` merges field-wise. Anything absent takes the base Live Set's value. A scene never holds the
  timbre, the engine type, the tempo (Live-Set-scoped, Story 7.4) or the ASSIGN map (Live-Set-scoped, Story 4.10).
- **Determinism.** No map fields; serialisation uses protobuf's deterministic mode (fields in number order, unknown fields last)
  (`serialize()`; golden `tests/golden/live_set_fixed.bin`). read -> write is byte-for-byte identical only for files in canonical
  form, i.e. written by this schema's writer. A rewrite canonicalises: field order (a file from another producer with unknown
  fields interleaved gets them moved after the known ones, still preserved), default-valued proto3 fields are dropped, a present
  but empty `timbre` becomes unset, and an N-1 file is written as N.
- **Reading.** `parse`/`read_file` (file <= 1 MiB, regular file, no symlink) return a closed `Status` (`kParseError`, `kTooLarge`,
  `kUnsupportedVersion`, `kInvalid`, ...) and a `ValidationReport` (typed `IssueCode`s; no exceptions across the API). Version
  policy: N and N-1 load (N-1 goes through the `migrate` stub, which today only moves `format_version`), everything else
  (including 0/unset) is `kUnsupportedVersion`. Tests exercise N-1 by giving the reader `current_version = 2`.
- **Forward compatibility.** Unknown fields are kept in the parsed message and written back. An unknown `param_id` (or insert type)
  is preserved and listed in `report.unknown_params`, never rejected. Out-of-range values, wrong scope (a master id in a part list,
  tempo in `master`), duplicate ids, non-integral enum/int values, NaN/inf, wrong part count, > 8 scenes, an ASSIGN of another
  length or naming a known structural parameter are errors (`kInvalid`, nothing written). Class rules: the part lists (`engine`, `envelope`, `eq`, scene overrides) take
  *performance* part-scope params only (the structural `part_output`/`timbre_ref` have their own field / are the timbre assignment:
  `kWrongClass`); the master lists take master-scope params except `tempo_bpm` (own field; `kWrongScope`) and `line_in_level`
  (device input; `kWrongClass`). Strings (names, `instrument_key`) must be valid UTF-8; `instrument_key` must not be absolute,
  contain a backslash or a `..` segment. A scene's `low_key`/`high_key` override is checked as the effective pair against the base
  part. Enum values above this build's range (`PartOutput`, `ArpPattern`, `ArpRate`) and insert slots of an unknown type (their
  params are not range-checked) are preserved with a warning (`kUnknownEnumValue`, `kUnknownInsertType`), never rejected.
- **IDs.** `mint_uuid_v4(RandomSource&)` (`OsRandom` = `/dev/urandom`, `SeededRandom` for tests; no RTC). Fails closed: a failing source yields no id (`mint_uuid_v4` returns "").
  `adopt_with_collision_check(ls, existing_ids, rng)` gives a colliding (or malformed) id a fresh UUIDv4 and keeps the content; at most 16 draws, then `kFailed` with the Live Set untouched.
- **Resolution report.** `resolve_parts(ls, availability)` returns per-part `kOk` / `kEngineNotAvailable` (reserved engine with
  none compiled, e.g. FM/VA, or an unknown type: silent, never substituted) / `kTimbreEmpty` (a Sample part with no timbre)
  without loading anything; `availability_of(EngineRegistry)` snapshots the bound engine ids (it does not keep the registry).
- **Store.** `LiveSetStore::open` takes an exclusive `flock` on `<dir>/.writer.lock` (a second store, even in the same process, gets
  `kWriterBusy`) and records the calling thread as *the* control thread; `write` (the single writer entry point) and `reconcile`
  from any other thread return `kNotControlThread` (handover is explicit: `rebind_control_thread()` from the new thread; thread ids may be reused after a thread exits, so keep one long-lived control thread); `read` is allowed anywhere and refuses (`kInvalid`) a file whose embedded id differs from its filename. `write` validates, then: temp file
  `<id>.liveset.tmp` in the same directory, fsync (F_FULLFSYNC on macOS), rename over `<id>.liveset`, fsync the directory. The
  directory must be on the journaled partition. `kCommittedNotDurable` means the rename happened (new file in place) but the directory fsync failed. A crash leaves the old file or the new one; leftovers `*.liveset.tmp` are removed by
  `reconcile` (which returns `kIo` if a leftover could not be removed). `StoreOptions::on_stage` is the fault-injection hook (tests `_exit` in a forked child at each stage).
- **Corpus.** `tests/corpus/live_sets/*.liveset` (minimal, full 8-part with 8 scenes and ASSIGN, FM/VA parts, missing library,
  Pro 16-part, unknown fields from a newer schema) were written by this release; `live_set_tests` opens every file on every build,
  checks it is byte-stable under read -> write and equals what the fixtures produce today (drift fails the test). Regenerate with
  `cmake --build build --target live_set_corpus_regen` (also rewrites the golden file) only when a change is intended.
- Tests: `live_set_tests` (round trip, golden bytes, every validation rule, scene semantics, version policy, unknown-field
  preservation, reserved engines, UUIDs and collisions, atomic write with crash injection, single writer, hostile input incl. a
  deterministic mutation/garbage fuzz, corpus). protobuf logs one stderr line per invalid-UTF-8 string the fuzz feeds it.

## Live Set load (Story 4.3, `velora_control`)

`velora/host/live_set_loader.hpp`: `LiveSetLoader::load(live_set)` / `load_file(path)` turn a Live Set into ONE new snapshot generation of the engine and swap it in only when every part's instrument is resident. Control thread only; the voice-pool reservation (`VoiceArbiter::reserve()`) must run while the audio callback is not rendering, so load at start-up (engine-host does, before audio starts) or between renders in tests; background loading and a seamless handover are Stories 4.5/4.7.

- **Steps.** (1) validate against the tier (`LiveSetLoaderOptions::part_count`, 8 on v1: a 16-part file on a v1 tier is `kInvalidLiveSet`/`kBadPartCount`; a Pro tier accepts 16, but this build's engine carries `core::params::kPartCount` = 8 parts, so **Pro 16-part Live Sets are rejected with `kPartCountUnsupported`** — a typed refusal that publishes nothing; supporting them needs a larger engine part count); (2) per part resolve the timbre `(library_id, instrument_key)` through the `LibraryResolver` (Story 1.14) and make the instrument resident through a `HeadProvider`; (3) build the generation: zone, octave, transpose, keyboard flag, volume, output, insert chain (effect instances built here, off the RT thread), sends, engine type, engine/envelope/eq sound parameters (registry performance ids become the generation's initial values), master settings and tempo; (4) compute the Live Set's voice-pool reservation (`fill_fx_costs()` of the new generation, `OrganEngine::reservation_cost_units(organ parts)`, a 256 voice sample floor when there is a sample part; no sample part = floor 0, no organ part = no organ reservation); (5) check the control plane's FX budget and ledger (`ControlPlane::can_publish()`), reserve, publish (`ControlPlane::publish_snapshot()`).
- **Quiescence.** `LiveSetLoaderOptions::is_rendering` (optional callback) is asked first; when it says the audio callback is rendering, the load is refused with `kNotQuiesced` before anything is touched. engine-host answers "the audio stream has started" (so the Live Set is loaded before `audio.start()`); the offline renderer renders on the calling thread after the load. Unset = the caller guarantees quiescence.
- **All or nothing.** Nothing is published, and the arbiter reservation stays as it was, when a resolved instrument cannot be made resident (`kInstrumentLoadFailed`, `failed_part`, `load_status`), the arbiter refuses the reservation (`kArbiterRejected`, `reserve_status`), the effects cost more than the FX budget (`kFxBudgetExceeded`) or no ledger slot is free (`kBusy`). The previous generation keeps playing. (If the last step fails after the reservation, the arbiter's previous `LiveSetConfig` — snapshotted from `VoiceArbiter::config()` before `reserve()` — is reserved again exactly, or the reservation is cleared if there was none.) Scalars read from the file (volume, sends, tempo, parameter values) are clamped to their registry range, non-finite ones take the default, before they reach the generation.
- **Part states, never a substitute.** A timbre whose library or instrument does not resolve leaves the part silent with `PartState::kMissingLibrary` (`PartLoadReport::resolve` says which `ResolveStatus`); a reserved engine type with no compiled engine (FM, VA) is `kEngineNotAvailable`; a sample part with no timbre is `kEmptyTimbre`; an organ part needs no timbre. These are states of the part, not load failures: the Live Set loads and the other parts play. Without a sample engine bound, sample parts are `kEngineNotAvailable`.
- **`HeadProvider` seam.** `HeadProvider::load(resolved, out)` returns a RESIDENT `BundleInstrument`. `RamHeadProvider` (the Mac path) decodes whole instruments into RAM through `sample_bundle_loader`; an instrument with a sample that fails to decode is not resident (`kBundleInconsistent`). Heads, streaming and locked RAM (Stories 1.16/1.17) are board work and implement the same interface. The same (library, instrument) in two parts is made resident once and shared; each part still gets its own bank.
- **Engine per part, sound routing.** See `engine/core/README.md` ("Live Set parts, zones, layers and splits"): banks in the one SampleEngine, the keyboard-channel convention, the `PartRouter`.
- **Not here.** Scenes (4.4), ASSIGN maps (4.10), setlists, UI. The loader ignores a Live Set's scenes and ASSIGN map.
- **engine-host.** `engine-host --live-set FILE --library-root DIR` binds the sample engine (340 voices) and the organ, loads the Live Set before audio starts as the first generation (the dev configuration is not published when `--live-set` is given, and the pool is reserved for the whole Live Set instead of the flat FX budget), prints each part's state, and exits 1 with the reason when the load fails. `--library-root` is the root of the library layout (`<root>/lib/<library_id>/current`).
- Tests: `live_set_loader_tests` (synthetic `BundleWriter` libraries in a temp root: layer, split at B2/C3, keyboard off, octave/transpose pitch, volume, missing library and instrument, FM/VA/organ parts and the reservation, no-sample-part floor, no sample engine, replacing a Live Set, fault-injected atomic publication with the old generation still playing, arbiter/FX-budget/capacity refusals, tier part counts, the corpus files, bad files).

## Control plane (Story 2.1, `velora_control`)

The only place protobuf is used next to the engine. A **control thread** is the sole writer of structural settings; clients (ui,
panel, Velora Studio) talk to it with `ws.v1` (`proto/ws_v1.proto`) over a Unix domain socket (mode 0600, default
`/tmp/velora-engine.sock`, `engine-host --socket PATH`).

- `control_plane` -- validates a command against the generated registry, builds a new immutable `core::Snapshot` generation,
  publishes it with one atomic store and replies `{request_id, revision}`. Any invalid command (unknown id, out of range, part > 7,
  performance param sent as structural or the reverse, too many insert slots, bad enum, empty command) returns a closed-enum
  `ErrorCode` and changes nothing: the revision does not move and nothing is published. `SetPerformanceParam` never creates a
  generation: it becomes one UMP stamped with host time on the control->RT lane (`RenderDriver`'s second SPSC queue, single producer
  = the control thread); the live value is read back from the lock-free last-value table (`GetPerformanceValues`).
  Commands: `SetInsertChain`, `SetSendLevels`, `SetPartOutput`, `SetMasterParams` (all-or-nothing), `SetTimbreRef`,
  `SetPerformanceParam`, `GetState`, `GetPerformanceValues`, `Subscribe`. The Ack of `SetPerformanceParam` means *queued*: RT can
  still drop the event when its queues overflow (counted: `param_events_rejected()`, `kEventOverflow`). Performance values travel
  as 32-bit normalised values (floats are quantised to ~2^-32 of the range; int/bool/enum are exact). `SetSendLevels` and
  `performance_init` apply on a generation change only where the value differs from the previous generation's initial value and
  never override a live value otherwise. `fill_state`/`fill_live_values` are control-thread only; `revision()`,
  `live_generations()`, `freed_generations()` are safe from any thread.
  Events at the same frame: the MIDI lane is submitted before the control lane, so at an identical timestamp the control
  value lands last (test `test_lane_tie_break_is_midi_first`). Generations are reclaimed here, per AD-23
  (`reclaim()`): freed once not current, not held by RT and not referenced by any sounding note.
- `control_server` -- one `poll()` thread: accept, length-prefixed frames (4-byte big-endian + protobuf, <= 65536 bytes),
  `Hello` handshake first (version must be 1, features are advertised: `params.v1`, `snapshots.v1`, `subscribe.v1`), commands
  serialized through the plane (concurrent clients get strictly increasing revisions, no lost update), `State` pushed to
  subscribers after each new generation. Framing violations (frame too large, wrong first message, unsupported version) get an
  error reply and the connection closes, as does an unparsable *first* frame (before Hello) and a client that has not handshaken
  within 5 s (`ControlServerLimits::handshake_timeout_ms`); beyond `max_clients` (64) connections are closed right after accept.
  After the handshake an unparsable frame or an invalid command gets an error reply and the connection survives. `Subscribe(true)`
  immediately pushes the current `State`. The socket is created 0600 (umask 0177), a path that is not a socket is never touched,
  and a stale socket is replaced only when connecting to it is refused. A reply above 64 KiB becomes an `INTERNAL` error. A client that falls 4 MiB behind is dropped. A stale socket file is replaced, a live listener is never stolen.
- **Effect instances and the FX budget (Story 2.7).** A `SetInsertChain` (or a dev-config part) makes the control thread build the part's `core::FxChain` (new, prepared effect instances
  from `core::make_effect()`; no plugin loading) and publish it with the generation; a command that does not change the chain (same types, bypass flags and params, or any other command) keeps the
  same instances, so RT only cross-fades when the chain really changed. `set_fx_budget(units)` (default `core::kMaxFxCost`) bounds `core::fx_cost_units()` of the resulting snapshot (insert effects,
  sends, master): a command that raises the cost above it returns `ERROR_CODE_FX_BUDGET_EXCEEDED` (details `fx_cost`, `fx_budget`), nothing is published and the instances built for it are freed on the spot; the same
  check refuses a dev configuration that does not fit. `fx_cost()` is the current generation's cost. `engine-host` reserves the arbiter with this budget FIRST
  (`LiveSetConfig::fx_cost = plane.fx_budget()`), so voices cannot eat into it. It is the flat budget, not `fill_fx_costs()` of the initial snapshot, because the arbiter reservation is static for the run while commands may raise the FX cost up to the budget. The dev configuration now sounds: part 0's reverb send (0.25) feeds the real reverb since Story 2.10 (default hall, 2 s, return level 1) and its delay sends feed the real delay since Story 2.11 (350 ms, feedback 0.35, return level 1, tempo sync off, tempo 120 bpm) and part 2 is on the Aux bus, which no adapter plays yet; the single-output `render_block` folds Aux into Main, so it is audible in `engine-host`.
  `RenderDriver::render_block_buses()` gives both buses.
- `dev_config` + `dev/engine-dev.json` -- the first generation until Live Sets exist (Epic 4): the JSON mapping of the `ws.v1`
  `State` message (parts 0-7: `timbreRef`, `inserts`, `sendReverb`/`sendDelay`, `output`; `master`; `performanceInit`). It is
  validated like a command and published as generation 1 **before audio runs**; an invalid file (unknown field, bad enum, out of range,
  duplicate part) makes `engine-host` print a clear error and exit 1. `engine-host --config PATH` selects another file.
- `midi_ingest` also turns MIDI CCs the registry maps (drawbars CC 70-78, ...) into performance parameter UMPs (channel = part);
  CC 64 (sustain) is forwarded unchanged to the renderer's pedal handling, other CCs stay ignored.
- Tests: `control_tests` (a real UDS client for every row of the story's matrix: handshake, structural commands, every invalid
  command, performance path via ws.v1 and CC, held note across a swap, 8 concurrent clients, generation exhaustion, socket
  lifecycle), `control_rt_traffic_test` / `rtsan_control_test` (10,000 parameter changes + 100 swaps beside a rendering RT thread;
  the latter under RealtimeSanitizer, Docker/Linux clang only).
- Requires `protoc` + libprotobuf (`brew install protobuf`; `protobuf-compiler libprotobuf-dev`). The library links Homebrew's
  abseil next to formatlib's vendored abseil only in the `engine-host` binary; the two use different inline namespaces.

## macOS only (`macos/`, built only when `APPLE`)

- `CoreAudioLineIn` (Story 2.7, `ports::LineInSource`): optional capture of the default CoreAudio input device as 48 kHz stereo float into a lock-free ring that the engine drains once per block; mixed into the
  master (Main) only, at `line_in_level` (default 0). `engine-host --line-in` opens it; without the flag nothing is opened (so macOS asks for microphone permission only on request). With no input device, no permission, or an input device whose rate is not 48 kHz (captured audio is never resampled)
  `engine-host` prints "line-in: unavailable (reason)" and starts with a silent line-in. A mono device is duplicated onto both channels; callbacks larger than the slice buffer are captured in chunks. Capture and playback run on different clocks: a short jitter buffer pads with silence on underrun and drops the oldest
  frames when it runs ahead (`underruns()`/`drops()`). Not covered by automated tests (needs an input device); the port and the mixing are (`routing_tests`, `offline_routing_tests`).

- `CoreAudioOutput`: HAL output AudioUnit at 48 kHz stereo float; requests a 64-frame device buffer and reports
  what the device granted. The engine always renders 64-frame blocks; leftover frames carry to the next callback.
  The block start time is the callback's `mHostTime` (when the first frame is heard) plus the frame position.
- `CoreMidiInput`: UMP input port (MIDI 2.0 protocol, falls back to 1.0), connects all sources; `rescan()` picks up new ones.
  Events are stamped with mach host time (-> ns) in the CoreMIDI callback.
- `engine-host`: dev app.

```
cmake -S . -B build && cmake --build build
build/engine/host/macos/engine-host                 # runs until q + Enter or Ctrl-C
build/engine/host/macos/engine-host --duration 2    # headless short run, prints stats and exits 0
build/engine/host/macos/engine-host --socket /tmp/e.sock --config engine/host/dev/engine-dev.json
```

Notes arriving from any MIDI source play the engine test tone. It exits non-zero with a message if there is no output
device or the device cannot run at 48 kHz (the engine never resamples).

## Rules

- All timing figures printed by `engine-host` are for the developer's Mac and are labelled "host, not representative".
  Latency and xrun budgets are accepted only on the dev board.
- ctest needs no audio device or MIDI keyboard. `coremidi_virtual_test` uses CoreMIDI virtual sources and exits 77
  (reported as skipped) when CoreMIDI is unavailable.

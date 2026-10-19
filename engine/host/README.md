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
  (`LiveSetConfig::fx_cost = plane.fx_budget()`), so voices cannot eat into it. It is the flat budget, not `fill_fx_costs()` of the initial snapshot, because the arbiter reservation is static for the run while commands may raise the FX cost up to the budget. The dev configuration now sounds: its sends feed the unity placeholder returns (part 0 reverb 0.25 adds 25% of its own signal
  to Main) and part 2 is on the Aux bus, which no adapter plays yet; the single-output `render_block` folds Aux into Main, so it is audible in `engine-host`.
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

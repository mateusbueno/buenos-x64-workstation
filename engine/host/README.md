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

## macOS only (`macos/`, built only when `APPLE`)

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
```

Notes arriving from any MIDI source play the engine test tone. It exits non-zero with a message if there is no output
device or the device cannot run at 48 kHz (the engine never resamples).

## Rules

- All timing figures printed by `engine-host` are for the developer's Mac and are labelled "host, not representative".
  Latency and xrun budgets are accepted only on the dev board.
- ctest needs no audio device or MIDI keyboard. `coremidi_virtual_test` uses CoreMIDI virtual sources and exits 77
  (reported as skipped) when CoreMIDI is unavailable.

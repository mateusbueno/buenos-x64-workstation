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

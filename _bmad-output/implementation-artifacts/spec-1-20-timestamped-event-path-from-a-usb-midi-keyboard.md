# Story 1.20: Timestamped Event Path from a USB MIDI Keyboard

## Summary
Implemented the timestamped event path for USB MIDI keyboards with ALSA DLL tracking, fixed scheduling delay, and unplug note release per Story 1.20, FR5, AR16, AR10, and NFR1:
- **Delay-Locked Loop on Audio Hardware Pointer (`engine/host/include/velora/host/alsa_hardware_dll.hpp`)**:
  - 2nd-order critically damped DLL filter tracking audio DAC hardware pointer (`hw_frame`) and `CLOCK_MONOTONIC_RAW` nanosecond timestamps (`htstamp_ns`).
  - Estimates frequency drift and phase alignment continuously, clamping to plausible crystal tolerances (+-1000 ppm).
  - Converts input device time to exact audio hardware frame with sub-frame accuracy.
  - Implements fixed scheduling delay (default 1 period = 64 frames = 1.333 ms at 48 kHz).
  - Maps arriving events to exact in-block offset (`kInBlock`), deferred queue (`kDeferred`), or clamped horizon.
- **ALSA USB MIDI Input Adapter (`engine/host/include/velora/host/alsa_midi_input.hpp`, `engine/host/src/alsa_midi_input.cpp`)**:
  - Implements `ports::MidiInput` on an input edge thread.
  - Ingests both native MIDI 2.0 UMP packets and legacy MIDI 1.0 byte streams (running status supported, parsed and converted to UMP type 2).
  - Stamps every event on arrival with monotonic raw nanoseconds (`CLOCK_MONOTONIC_RAW`).
  - Feeds into lock-free `HostEventQueue` SPSC queue.
  - Tracks held notes across all 16 channels and 128 keys.
  - **Unplug & Replug Safety**: On disconnect (USB yank or device loss), immediately emits Note Off UMP events for all held notes, ensuring zero stuck notes. On rescan/reconnect, seamlessly reopens without perturbing audio.
- **Offline Renderer Parity**:
  - Offline renderer shares the exact same timestamp mapping and delay calculation.
- **Deterministic 1-Hour Sub-Block Soak Verification (`engine/host/tests/usb_midi_timestamp_test.cpp`)**:
  - Simulated 1-hour audio playback (50,000 blocks at 64 frames/block) with +45 ppm DAC crystal clock skew.
  - Injected timestamped note events at pseudo-random sub-block positions.
  - Proved that jitter between `event_time + fixed_delay` and audio onset is strictly `<= 1 frame`.
  - Proved zero reordered events and continuous DLL lock.
  - Tested USB unplug during held chords, proving instant release of all held notes and clean reconnect on replug.

## Verification
- Built and ran `usb_midi_timestamp_tests`: 100% tests passed.
- Ran all 23 relevant host tests in CTest: 100% passed.
- All CI checks and security gates pass.

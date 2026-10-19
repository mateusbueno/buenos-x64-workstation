# Epic 3 Context: Physical instrument

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

A keyboardist plays the real prototype: the Fatar keybed (attack/release velocity, multi-contact timing, aftertouch where present), the panel (faders, endless encoders with LED rings, toggles, buttons, SoC-driven LEDs, wheels), pedals, MIDI DIN, and audio through the PCM3168A codec (four balanced outs, headphones, line in), with pop-free mute, power-fail protection and 24 h thermal stability in the closed chassis. Portable work (frame protocol, SoC-side readers, velocity/calibration math, virtual panel) is built Mac-first against recorded and simulated MCU frames; MCU firmware starts on a dev kit; codec, power and thermal work need the prototype board and hardware-partner deliverables.

## Stories

- Story 3.1: Versioned MCU frame protocol library and control/LED ID maps
- Story 3.2: Prototype board bring-up
- Story 3.3: Keybed MCU firmware with raw per-contact timestamps
- Story 3.4: MCU dual-bank bootloader and reflash from the SoC
- Story 3.5: Keybed reader on the SoC with drift-tracked device time and contact_k events
- Story 3.6: Attack and release velocity with selectable curves
- Story 3.7: Aftertouch read and routed when the keybed has it
- Story 3.8: Panel MCU firmware — inputs
- Story 3.9: Panel MCU firmware — SoC-driven LEDs
- Story 3.10: Panel reader on the SoC and the Mac virtual panel
- Story 3.11: Pedals with half-pedal calibration and expression
- Story 3.12: SoC-driven LED state and toggle state with LED per state
- Story 3.13: PCM3168A codec as clock master with the ALSA machine driver
- Story 3.14: Balanced outputs and headphone output
- Story 3.15: Stereo line input mixed into the master
- Story 3.16: Hardware mute for pop-free power-on and power-off
- Story 3.17: MIDI DIN in and out
- Story 3.18: Power-fail fade and write freeze within the hold-up window
- Story 3.19: Key-to-sound latency on the prototype
- Story 3.20: 24-hour thermal run in the closed chassis

## Requirements & Constraints

- Key-to-analog-output latency must be ≤5 ms for every note at full polyphony; keybed scan plus transport must use <1 ms of it (contact change to frame complete on the wire ≤200 µs).
- Hardware-dependent stories name their partner dependency (MCU family, keybed contact count/aftertouch, POWER_FAIL timing, codec reference design, thermal result, physical panel spec). Stories 3.13, 3.18 and 3.20 are blocked by Story 11.9 (target month 10); others by 11.6–11.8.
- Power loss: outputs fade to silence in ≤20 ms then hardware mute asserts; supercap hold-up ≥150 ms; POWER_FAIL leads rail drop by ≥100 ms; journaled partition must mount clean after random cuts.
- Thermal: 24 h at full audio load in a closed chassis at 40 °C ambient with no throttling, zero xruns, zero stream underruns.
- Protocol is versioned: unknown major version rejected with a counter, N−1 accepted; decoder must resync on noise, never deliver a corrupt frame, count CRC errors and seq gaps.
- Panel inputs: 8 endless encoders with LED rings, endless VALUE encoder with push, 9 non-motorized faders (≥10-bit, ≥500 Hz), pitch/mod wheels, 3 pedal inputs (continuous sustain with half-pedal, expression, switch; polarity auto-detected at boot), debounced buttons including 3 toggles (two 2-state, one 3-state). LED on:off contrast ≥10:1 at 50 klx, brightness adjustable from the SoC.
- Codec outputs: 4 balanced TRS plus headphone stage; line in is mixed into master only.

## Technical Decisions

- Two MCUs: keybed MCU (keys only, ≥10 kHz scan, free-running ≥1 MHz tick) and panel MCU. Link is UART ≥3 Mbaud or SPI slave plus IRQ. The keybed MCU sends raw per-contact times only; no velocity or calibration on the MCU.
- One frame format shared by both MCUs and the SoC: sync 0xA5, version, type, length, 16-bit seq, 32-bit tick, payload, CRC-16/CCITT (version-to-payload); little-endian. Types: HELLO, KEY_CONTACT, AFTERTOUCH, CONTROL, BUTTON, LED_SET, TIME_SYNC, ENTER_BOOTLOADER. Portable C library in `mcu/common/` (no OS, no heap) with a C++ wrapper in `engine/host`. Control and LED ID maps are generated from a single source file and published for partners.
- Device time is `CLOCK_MONOTONIC_RAW` ns. Input edge threads convert MCU ticks using offset and drift estimated from periodic TIME_SYNC round trips, then push UMP to the RT plane only via SPSC queues. Engines consume only MIDI 2.0 UMP. Target: converted times within 20 µs over 12 h at ±200 ppm drift, never reordered.
- Velocity curve is applied exactly once, on the SoC reader (calibration table, then selected curve, giving 16-bit velocity). The reader emits a vendor per-note `contact_k` controller at each closure, then Note On at the contact that completes velocity measurement; the organ starts on the first contact. Keybed model (2 or 3 contacts, velocity pair, key mapping) is table-driven with no code change. Lost frames must never cause stuck notes. Calibration/curves live in `sysd` config with `schema_version`.
- The `engine` process owns MCU links, MIDI DIN and the power-fail GPIO (with `sysd`). The panel MCU never sets LEDs itself; all LED state is SoC-commanded via LED_SET.
- Each MCU has an immutable dual-bank bootloader; a power cut mid-flash leaves the old bank bootable; `sysd` reflashes only on version mismatch; no field flashing via external connectors.
- Audio: PCM3168A is clock master on its own 24.576 MHz crystal; SoC I2S/TDM is slave and its PLL generates no audio clocks. Mainline `rockchip-i2s-tdm` and `pcm3168a` drivers plus a board machine driver under `yocto/`. 6-ch playback / 2-ch capture at 48 kHz, 64-frame periods. Channel map: DAC 1–2 main, 3–4 aux, 5–6 headphone; ADC 1–2 line in. The rate DLL on the ALSA hardware pointer locks to the codec clock; USB never clocks the engine. Hardware mute line is held muted from power-on until the SoC releases it.
- Power-fail: an engine edge thread signals the RT plane to fade; `sysd` freezes the journaled partition and flushes NVMe, published as one freeze signal that all writers subscribe to (no per-writer POWER_FAIL handling). MIDI DIN: dedicated UART at 31,250 baud, opto-isolated input.
- Mac-first: the reader core, protocol, curve and pedal calibration math are tested against recorded frames and a simulated MCU; a Mac virtual panel stands in for the panel.

## UX & Interaction Patterns

- Panel has toggle buttons that advance one state per press (DRAWBARS/MIXER and PARTS/SCENES 2-state; PART/ALL/ASSIGN 3-state cycle), with one LED per state mirrored in the screen Top Bar.
- Encoders are endless with no value jumps; their LED ring shows the target's current value. Faders are non-motorized, so after any recall the UI and engine apply pickup (Pickup Ghost on screen until the fader crosses the stored value).
- Panel LED brightness is user-adjustable (Low/Mid/High/Follow screen) from Settings; Sustain calibration is a Settings › Keyboard & Pedals row added later in Story 10.2 (CLI only in this epic).
- A control with no target (e.g. DRAWBARS with no organ) blinks its LED twice rather than changing mode.

## Cross-Story Dependencies

- 3.1 (protocol, ID maps) underpins all MCU firmware and SoC reader stories (3.3, 3.5, 3.7, 3.8–3.10, 3.12).
- 3.5 feeds 3.6 (velocity) and 3.7 (aftertouch); 3.8 provides pedal polarity detection used by 3.11; 3.9/3.10 feed 3.12.
- 3.4 (bootloader/reflash) needs the MCU firmware from 3.3 and 3.8.
- 3.2 (bring-up) gates the board-only stories; 3.13 gates 3.14, 3.15 and the codec path used by 3.16, 3.19 and 3.20; 3.18 builds on 3.16.
- 3.19 and 3.20 depend on Epic 2 (organ, Story 2.12 worst-case soak) and Epic 1 (latency, DLL from Story 1.20).
- 3.18 is later extended by Story 8.11 (brown-out recovery); 3.11 by Story 10.2 (Settings UI). 11.6–11.9 are hardware-partner and panel-spec blockers.

# Epic 12 Context: FM and VA engines (post-launch)

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Provide a clean-room 6-operator FM synthesis engine and a clean-room virtual-analog (VA) subtractive synthesis engine via an atomic A/B firmware update to shipped Velora 73 units. Both engines run inside the shared voice pool behind the single unified engine interface, preserve all pre-existing user data and Live Sets, and support classic SysEx voice import without trademark infringement.

## Stories

- Story 12.1: Clean-room 6-operator FM voice in the engine core
- Story 12.2: FM null test against an unshipped reference oracle
- Story 12.3: Import 6-OP SysEx banks and single voices from files (USB-A and Velora Studio)
- Story 12.4: Receive 6-OP SysEx over USB MIDI and DIN
- Story 12.5: FM voices in the shared pool with a 128-voice floor
- Story 12.6: Clean-room virtual-analog voice in the engine core
- Story 12.7: VA voices in the shared pool with a 64-voice floor
- Story 12.8: FM and VA timbre parameters in the versioned formats
- Story 12.9: Init and factory FM/VA timbres in the Library
- Story 12.10: Design story — FM and VA Sound sections and not-available state
- Story 12.11: Part surface Sound section for FM timbres
- Story 12.12: Part surface Sound section for VA timbres
- Story 12.13: Velora Studio Part Editor Sound sections for FM and VA
- Story 12.14: FM and VA firmware update reaches shipped v1 units

## Requirements & Constraints

- **Engine Invariants (FR12, FR13):** Clean-room DSP implementations for 6-operator FM (32 algorithms, sinusoidal oscillators, feedback, pitch/amp envelopes, LFO) and virtual-analog (dual oscillators, hard sync, noise, ZDF 4-pole/2-pole filter, dual ADSR envelopes, LFO).
- **Voice Pool Floors (NFR8):** FM engine declared floor is >= 128 voices; VA engine declared floor is >= 64 voices. Both share pool headroom dynamically with sample and organ engines.
- **Latency & Reliability (NFR1, NFR4):** Key-to-audio latency <= 5 ms under full 8-part load (buffer 64 frames @ 48 kHz). Zero audio dropouts or xruns across sustained runs.
- **Trademark Hygiene (NFR29, NFR51):** Generic names only ("6-operator FM", "virtual analog", "rotary speaker"). Never mention proprietary brand or model names in UI, marketing, code, or release notes.
- **Atomic Firmware Update & Rollback (FR45, FR47, FR49, NFR12):** RAUC A/B signed bundle installation via Velora Studio or USB-A. Power-fail safe under 100 random cuts. Backward/forward compatibility: Live Sets created on earlier firmware render bit-for-bit identical; Live Sets saved with FM/VA parts open in `kEngineNotAvailable` state upon rollback with data fully preserved.
- **Multilingual Delivery (FR66):** Release documentation in English (EN), Brazilian Portuguese (PT-BR), and Spanish (ES).

## Technical Decisions

- **Single Engine Interface (AD-7):** All engines implement `velora::core::Engine` and register with `EngineRegistry`. The voice arbiter reserves voice floors and tracks dynamic steal hierarchies.
- **Persistence & Migration (AD-12):** Protobuf schemas store engine parameters in `EngineData`. Engines not present on older firmware are flagged `PartState::kEngineNotAvailable` without mutating stored protobuf fields.
- **Content Delivery Separation (FR46):** Firmware bundles (.raucb) contain engine binaries and OS. Factory sound packs (.vlb) install separately via the librarian service. Built-in "Init FM" and "Init VA" timbres are accessible immediately without content packs.

## Cross-Story Dependencies

- Story 12.14 is the capstone release acceptance story integrating Stories 12.1–12.13 with sysd A/B update infrastructure (Epic 8) and audio engine latency/performance budgets (Epic 1, 2).

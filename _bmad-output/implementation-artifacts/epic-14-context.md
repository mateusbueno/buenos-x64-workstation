# Epic 14 Context: Modeled electric piano (post-launch)

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Provide modeled tine and reed electric pianos via an atomic firmware update to shipped Velora 73 units, with continuous touch response, physical modeling of hammer/tine/reed/pickup/damper, parameter voicing on Part surface and Velora Studio, and integration into the shared voice pool with a guaranteed >=64 voice floor while preserving all existing user content and backward compatibility.

## Stories

- Story 14.1: Decision: clean-room record and patent search before the design closes
- Story 14.2: Modeled tine voice in the engine core
- Story 14.3: Modeled reed voice in the engine core
- Story 14.4: Continuous touch, release and half-pedal
- Story 14.5: Modeled electric piano voices in the shared pool with a 64-voice floor
- Story 14.6: Objective comparison against the development references
- Story 14.7: Modeled electric piano timbres in the versioned formats
- Story 14.8: Design story: modeled electric piano Sound section
- Story 14.9: Part surface Sound section for modeled electric pianos
- Story 14.10: Velora Studio Part Editor section for modeled electric pianos
- Story 14.11: Init and factory modeled electric piano timbres
- Story 14.12: Final reference recordings and blind panel
- Story 14.13: Modeled electric piano firmware reaches shipped units

## Requirements & Constraints

- **Physical Modeling Synthesis (FR123):** Compute tine (electromagnetic pickup) and reed (electrostatic pickup) sounds from physical models, zero samples. Model hammer strike, vibrating tine/reed/tone bar, pickup interaction, and damper. Tuning across keys E1–E7 within specified test tolerance.
- **Continuous Touch & Articulation (FR124):** Velocity 1–127 sweep yields monotonic level and spectral brightness changes without steps. Release velocity modulates damper sound. Half-pedal follows continuous pedal law (Story 3.11).
- **Voice Pool Floor (FR6, FR7, FR123, AR51):** Reserved voice floor of >= 64 voices at declared cost unit in the shared arbiter pool. Dynamic stealing adheres to whole-Live-Set rules (protecting lowest note and pedal-sustained notes).
- **Clean-Room & IP Governance (NFR56, NFR57, EXT16, EXT18):** Clean-room documentation in `engine/core/engines/ep/CLEAN-ROOM.md`. Strictly no decompilation, disassembly, or proprietary source code of competitors. Only CC BY / public domain references or founder recordings. Preliminary patent search verified before design closes; formal attorney freedom-to-operate opinion gates release (Story 14.13).
- **Persistence & Compatibility (FR14, FR49, FR58, FR125):** Timbres store voicing parameters (hammer hardness, pickup position, pickup level, damper noise, attack noise) and instrument type using existing versioned formats without migration. Missing engine on older firmware renders `kEngineNotAvailable` silently while preserving data.
- **Real-Time Safety & Latency (AD-3, NFR1, NFR4):** RT-safety discipline: zero allocations, zero lock contention in audio render path (64-frame blocks @ 48 kHz). Overall latency <= 5 ms under full load; zero xruns over 24 h soak.
- **Trademark Hygiene (NFR29):** Generic naming only ("Modeled Electric Piano", "Tine", "Reed"). No third-party trademarks ("Rhodes", "Wurlitzer") in UI, code, or release notes.

## Technical Decisions

- **Single Engine Interface (AD-7):** New reserved engine type ID (`kEngineModeledEP` / `EngineType::kModeledEP`) implementing `velora::core::Engine`.
- **Voicing Separation (AD-7, AD-12):** Voicing parameters encompass mechanical and electroacoustic properties (hammer, pickup, damper, attack). Amp, drive, chorus, phaser, and tremolo rely on the existing fixed effects topology.
- **Development vs. Production Oracles:** Development reference audio (Freesound CC BY) is strictly offline for tuning and never bundled into shipping firmware or factory packs. Shipped bundle uses founder recordings and blind ABX verification.

## Cross-Story Dependencies

- Story 14.1 establishes the clean-room record and reference verification required before 14.2/14.3/14.6.
- Story 14.2 & 14.3 establish the core tine and reed DSP required by touch (14.4), pool (14.5), objective comparison (14.6), persistence (14.7), and factory presets (14.11).
- Story 14.8 design spec is required for UI implementation in Part surface (14.9) and Velora Studio (14.10).
- Story 14.13 release is blocked by attorney freedom-to-operate opinion (EXT16), blind panel (14.12), and hardware bring-up for RK3588 voice budget (Epic 12 retro item 12).

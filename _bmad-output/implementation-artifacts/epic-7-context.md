# Epic 7 Context: Arpeggiator and clock

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

A keyboardist can run an arpeggiator on any part, with on/off state and settings stored per scene. It locks to the Live Set tempo or to external MIDI clock (DIN or USB), and keeps playing at the last tempo if the external clock drops out. The epic builds on the engine core (UMP events, in-block frame offsets, offline renderer, null-test harness), MIDI DIN input, Live Sets/scenes, the parameter registry, the Part surface and the Top Bar. Engine logic runs on the Mac and in the offline renderer. DIN clock and the ≤1 ms jitter acceptance need the dev board or prototype.

## Stories

- Story 7.1: Single tempo and transport clock on the RT plane
- Story 7.2: Event-processor framework and arpeggiator note generation
- Story 7.3: Swing, latch and arpeggiator performance behaviour
- Story 7.4: Arpeggiator settings in Live Sets and scenes
- Story 7.5: Part surface Arpeggiator section and Live Set tempo
- Story 7.6: External MIDI clock follower and clock-lost behaviour
- Story 7.7: Clock source setting and DIN/USB clock inputs
- Story 7.8: Top Bar tempo, clock source and CLOCK LOST display
- Story 7.9: Arpeggiator timing acceptance on the board

## Requirements & Constraints

- Arpeggiator parameters per part: On/Off, Mode (Up, Down, Up-Down, Random, As played), Rate (1/4–1/32 incl. triplets), Octaves 1–4, Gate %, Swing % (50–75), Latch.
- Arpeggiated notes land sample-accurately on the grid of the single RT tempo clock. The clock is the Live Set tempo when the source is Internal.
- Tempo range is 20–300 BPM at 0.1 resolution.
- With an external source, jitter must stay ≤1 ms against the incoming clock, and tempo is shown read-only with its source (`♩ 72 · DIN`).
- On clock loss the arp keeps running at the last received tempo and the UI shows `⚠ CLOCK LOST` until clock returns.
- The Live Set holds initial arp values. Each scene stores arp on/off and all arp parameters. Tempo stays Live-Set-scoped.
- Switching scene or Live Set never cuts held, sustained or releasing notes.
- Audio RT threads never allocate, lock, block, do disk I/O or take page faults.
- Every UI label is a locale-catalog key (EN, PT-BR, ES) with CLDR number formatting. State is never conveyed by colour alone.

## Technical Decisions

- The RT plane owns exactly one tempo/transport clock. Musical position (ticks) is computed from an integer frame count and the tempo, never from accumulated float increments.
- An event-processor framework sits in the serial phase of the block loop. Each part has an optional processor slot that consumes UMP and emits UMP with exact frame positions before voice allocation. The slot type is reusable for a future Pro sequencer.
- Random mode is seeded per part so tests are reproducible.
- Arp parameters live in the `proto/` parameter registry as structural, scene-scoped parameters. They use the arp fields already defined in the Live Set format, so there is no format bump. Unknown fields are preserved and N/N−1 readers must still open written files. A scene change is a parameter-only snapshot swap.
- The external clock follower runs on the RT plane. It estimates tempo and phase from device-time timestamps with a DLL-style filter that tracks tempo changes within 2 beats.
- The clock source (Internal/DIN/USB, default Internal) is owned by the sysd config service, versioned with `schema_version`, and published to the engine.
- The input edge thread timestamps clock on arrival and forwards only the selected port's clock.
- Clock lost means no ticks for more than 3 ticks at the last tempo (minimum 150 ms) without a Stop. The engine publishes a `clock_lost` state with that tempo. Re-lock must not double-trigger.
- MIDI Start/Continue aligns the step grid to the next tick. Stop keeps running at the last tempo and does not raise `clock_lost`.
- Verification uses the offline renderer with committed MIDI files, exact expected frame lists, golden null-tests in CI, and synthetic clock streams with ±0.5 ms jitter and tempo ramps. Timing results are written to reports.

## UX & Interaction Patterns

- The Part surface Arpeggiator section uses Segmented Controls for on/off and multi-choice fields. VALUE edits the highlighted field and touch selects. Edits mark ◆ and appear in the Write Dialog "Changed: …" list, with Scene scope available.
- The Settings › MIDI & Clock section has a Clock source Segmented Control. Changes take effect without interrupting sound.
- The Top Bar shows `♩ 72`, or `♩ 72 · DIN/USB` rounded to whole BPM and updated at most twice per second.
- `⚠ CLOCK LOST` appears in the Top Bar, and the Status Bar warn takeover reads "⚠ CLOCK LOST · ARP AT ♩ 72". Both use glyph plus text, nothing blinks faster than 3 Hz, and both clear when the clock returns. Strings must pass the pseudo-localization clipping check.

## Cross-Story Dependencies

- 7.2 and 7.3 depend on the 7.1 clock. 7.4 needs the arp parameters from 7.2/7.3 and the Live Set/scene fields from the earlier Live Set epic.
- 7.5 needs the 7.4 registry parameters. 7.8 makes the 7.5 Tempo field read-only when following external clock.
- 7.6 drives the 7.1 clock. 7.7 supplies the clock source and port feeds to 7.6. 7.8 depends on 7.6 (`clock_lost`) and 7.7 (source).
- 7.9 validates 7.1–7.8 and needs the hardware rig.
- This epic depends on engine core, MIDI DIN input, Live Sets/scenes, the Part surface and the Top Bar from earlier epics. The 7.3 seamless-switching behaviour relies on the voice-generation model that lets notes finish across scene switches.

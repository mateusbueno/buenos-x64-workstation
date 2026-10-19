# Epic 2 Context: Organ and effects

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

A keyboardist plays a tonewheel organ (9 drawbars, percussion, key click, vibrato/chorus, leakage, multi-contact onset, rotary speaker) that is never voice-stolen, and shapes every part with an ordered insert chain, reverb and delay sends and a master section, edited on the device's Effects surface. Organ fidelity is accepted against Hammond M-solo renders and a blind ABX panel. Development is Mac-first: all DSP lives in `engine/core`, is null-tested and played live on the Apple-silicon Mac through the Epic 1 host adapters; the device UI is built in LVGL's SDL simulator at 1024x600. CPU budgets are accepted only on the dev board. Until the panel (Epic 3) and Live Set format (Epic 4) exist, drawbars, rotary and FX are driven by MIDI CC and parts come from an in-memory engine configuration loaded by the control thread.

## Stories

- Story 2.1: Parameter registry and control-thread snapshots for engine settings
- Story 2.2: Tonewheel generator and drawbar voices at full polyphony
- Story 2.3: Organ percussion and key click
- Story 2.4: Vibrato/chorus scanner and tonewheel leakage
- Story 2.5: Multi-contact onset hook in the organ
- Story 2.6: Rotary speaker in the organ voice path
- Story 2.7: Fixed routing topology with insert chains, sends, master and output buses
- Story 2.8: EQ and compressor as inserts and in the master section
- Story 2.9: Chorus/phaser, EP amp/drive and rotary speaker inserts
- Story 2.10: Reverb send
- Story 2.11: Delay send with tempo sync
- Story 2.12: Full effects load within the latency and dropout budget
- Story 2.13: Organ objective A/B against Hammond M-solo renders
- Story 2.14: Organ blind ABX listening test with professional keyboardists
- Story 2.15: Design story, Effects surface mockup
- Story 2.16: Design story, organ section of the Part surface
- Story 2.17: Device UI shell on the LVGL simulator and the dev board
- Story 2.18: Design tokens, Stage and Studio themes and the contrast gate
- Story 2.19: Hint Bar, List Row, Segmented Control and the Mac virtual panel
- Story 2.20: Effects surface, part inserts
- Story 2.21: Effects surface, sends, master and outputs

## Requirements & Constraints

- Organ: all 73 keys x 9 drawbars simultaneously, never stolen; footages 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'. Percussion is single-triggered (cancels 1' drawbar), plus key click, V1-V3/C1-C3 scanner, seeded reproducible leakage, multi-contact onset, rotary slow/fast/stop with separate horn/drum ramps.
- Effects (fixed set, no plugins): rotary speaker, chorus/phaser, EP amp/drive, EQ, compressor as inserts (up to 5 slots per part, with bypass); global reverb and delay (tempo sync) sends; master EQ then compressor; line-in mixes into master only; per-part Main/Aux output.
- Effects share the CPU budget and must not break latency or dropout targets. Worst-case config: 8 parts x 5 inserts, sends, master, 73 organ keys, >=256 sample voices; block time within 70% of the 1.333 ms period; zero xruns/underruns in the 24 h soak.
- No user-visible or log string may say "Hammond", "Leslie" or "B-3". Use "tonewheel organ" and "rotary speaker" (UI label ROTARY).
- Clean-room DSP only. setBfree (GPL) may serve as an unshipped oracle, never as a code source.
- Fidelity: objective A/B vs M-solo recordings (CI uses stored recordings) and blind ABX with 3-5 keyboardists (pass: none above p<0.05, or mean rating >=4/5). Stories 2.13/2.14 are blocked by Epic 11 recording/panel recruitment stories.
- Determinism: offline renders are bit-identical and stored as goldens. Null tests use analytic references.
- UI: every string is a locale-catalog key (EN source, lint enforced); no colour literals in `ui` outside generated files; text >=4.5:1, boundaries >=3:1, touch targets >=56 px, nothing conveyed by colour alone; CLDR number/unit formatting per locale; pseudo-localization at +40%.

## Technical Decisions

- Control thread is the single writer of structural state (Live Set parts, insert chains, routing). It hands state to RT only as immutable snapshot generations swapped by one atomic pointer. Clients (ui, panel, Studio) only send commands over `ws.v1` (Unix domain socket); invalid commands return a closed-enum error and change nothing; replies carry `request_id` and revision.
- Generated parameter registry in `proto/`: each `param_id` has exactly one class, structural or performance; the build fails on duplicates or missing class. It emits C++ tables for `engine/core` and `engine/host` plus `ws.v1` enums. Performance params (drawbars, rotary speed, send levels) reach RT only as timestamped UMP events; RT owns the live value and publishes a lock-free last-value table. Snapshots carry initial performance values applied only on generation change.
- RT rules: no allocation, locks, blocking or I/O on audio threads; only SPSC queues, atomic swaps and the last-value table cross threads; RTSan runs in CI on host builds. Effect instances are pre-allocated off RT; chain changes cross-fade with no click.
- Block = 64 frames at 48 kHz. Phases: 1 serial (events, arbiter), 2 parallel per-part shared generators (tonewheels), 3 parallel voice render to per-worker scratch, 4 barrier, 5 sum, per-part FX, sends, master. Voices write only their own state and worker scratch.
- Arbiter reserves in order: FX budget, then organ+rotary reservation (never stolen), then engine floors; surplus is shared. Commands exceeding the FX budget are refused with a typed error. Each effect/engine declares its cost, measured on the dev board.
- Generation lifetime: a generation is freed only after RT reports no live voice references it (test with a held note across a swap).
- Routing is fixed: per-part insert chain, reverb and delay sends, master FX, Main/Aux assignment; routing is structural state.
- `engine/core` has no OS/hardware dependency (macOS and Linux builds); the offline renderer drives it for null tests. The organ is one engine behind the single engine interface.
- Organ multi-contact input: vendor per-note `contact_k` UMP controllers precede Note On; sources with only Note On/Off start all busbars together (hook is optional per source).
- Engine state publishes rotary speed (`SLOW`/`FAST`/`STOP`) and compressor gain reduction for the UI.
- `ui` is a client only: LVGL 9.6, same sources on macOS (SDL, 1024x600) and the dev board (DRM/KMS + evdev); full snapshot then deltas; killing `ui` must not interrupt audio. One token generator (`tools/tokens/`) turns DESIGN.md tokens into the LVGL theme (Stage dark, Studio light, theme-invariant drawbar/LED tokens), is pluggable for later outputs, and a CI contrast gate fails the build below thresholds.

## UX & Interaction Patterns

- Non-Performance skeleton: Top Bar 56 / content 488 / Hint Bar 56 at 1024x600. Hint Bar shows only active controls (VALUE, WRITE, EXIT) as keycap-outlined English names with localized verbs.
- Widgets: List Row and Segmented Control; VALUE turn moves/changes, WRITE confirms, touch selects; the selected item is raised with an accent border. A Mac virtual panel maps keys to EXIT, MENU, WRITE, VALUE, LIVE SET and SCENE and emits the same `PanelEvent`s as the real panel.
- Effects surface sections: Part inserts (part 1-8, ordered chain rows, add/remove, reorder by hold up/down or drag, bypass), Sends (reverb type/time/level, delay time/sync/feedback/level, per-part sends), Master (EQ, compressor with live gain reduction, line-in level), Outputs (Main/Aux segmented control). The screen reflects engine-published state, not local guesses; refused edits show the reason in the Hint Bar; EXIT goes one level back and keeps edits. Held WRITE is reserved for the Epic 4 Write Dialog.
- Organ Part sound section (design only here): 9 drawbars with numeric registration (e.g. `88 8600 000`), percussion, key click, vibrato/chorus, leakage, rotary SLOW/FAST/STOP; header `PART n · timbre · ORGAN`. Mockups in `mockups/` reviewed in both themes and pseudo-localized.
- Panel EQ bands map to the encoder functions LOW, MID, MID FREQ, HIGH; the REV master send is the 9th MIXER fader target.

## Cross-Story Dependencies

- 2.1 (registry, snapshots) underpins every other story; 2.2 precedes 2.3-2.6 (organ features) and 2.7 depends on the Epic 1 block loop and arbiter (Story 1.19). 2.7 precedes 2.8-2.11 (effects land in its slots, placeholders until then) and 2.12.
- 2.6 rotary DSP is reused by 2.9; the organ reservation from 2.2 includes rotary cost.
- 2.12 extends the Story 1.18 render limit and Story 1.22 soak gate; needs the dev board.
- 2.13 and 2.14 depend on Epic 11 recording and panel-recruitment stories (target months 8 and 11) and on 2.2-2.6.
- UI: 2.15/2.16 mockups feed 2.20/2.21; 2.17 (shell), 2.18 (tokens) and 2.19 (widgets, virtual panel) precede 2.20 and 2.21. The Effects surface is opened by a developer entry point until the Epic 4 Launcher.
- Forward links: Epic 3 supplies the real keybed `contact_k` events, panel controls, physical outputs and line-in hardware; Epic 4 supplies Live Set format, Organ Card/Status Bar display and the Write Dialog.

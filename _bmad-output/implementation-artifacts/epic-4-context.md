# Epic 4 Context: Complete show: Live Sets, scenes and panel

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

A keyboardist builds and plays a whole show on the instrument: setlists of Live Sets, each with 8 parts (zones, layers, splits) and up to 8 scenes, with seamless scene, Live Set and setlist switching (next Live Set preloaded), the Performance screen, Setlist and Setlist Browser, Write with hold guard and the unsaved indicator, fader pickup, the three panel toggles, ASSIGN, navigation/MENU/Launcher, footswitch, accidental-press guards and the Part surface. Mac-first: model, control thread, switching and Write run in `engine-host` on the Mac, verified with the offline renderer; device surfaces are built in the LVGL SDL simulator at 1024x600 against the Mac virtual panel (Story 3.10). Head-budget, preload timing and the final run-through are accepted on the dev board/prototype. Undrawn surfaces get a design story before implementation.

## Stories

- Story 4.1: Tier profile read at boot
- Story 4.2: Versioned Live Set format with parts, scenes and ASSIGN map
- Story 4.3: Live Set load with 8 parts, zones, layers and splits
- Story 4.4: Scene recall as a parameter-only snapshot swap
- Story 4.5: Seamless Live Set switching with background load
- Story 4.6: Setlists, setlist tags and Live Set stepping
- Story 4.7: Next Live Set preload within the head budget
- Story 4.8: Seamless switching render suite
- Story 4.9: Edits in memory and Write scopes
- Story 4.10: ASSIGN maps with a global default
- Story 4.11: Live Set favorites and Recent in the catalog
- Story 4.12: Fader bindings with pickup
- Story 4.13: Encoder bindings for PART, ALL and ASSIGN
- Story 4.14: Design story, bring existing mockups in line with the spines
- Story 4.15: Design story, Launcher Grid mockup
- Story 4.16: Design story, On-screen Keyboard mockup
- Story 4.17: Design story, Scenes surface mockup
- Story 4.18: Design story, Part surface mockup
- Story 4.19: Design story, Action Sheet and ASSIGN Overlay mockups
- Story 4.20: Performance surface, Top Bar, Live Set Hero and Scene strip
- Story 4.21: Performance surface, Part Strips, Volume Bars and Status Bar
- Story 4.22: Navigation rules, MENU and the Launcher
- Story 4.23: PARTS/SCENES toggle and the part/scene buttons
- Story 4.24: Pickup Ghost and the Status Bar takeover
- Story 4.25: DRAWBARS/MIXER toggle with the Organ Card and Drawbar Columns
- Story 4.26: PART/ALL/ASSIGN toggle and encoder LED rings
- Story 4.27: ASSIGN Overlay
- Story 4.28: On-screen Keyboard
- Story 4.29: Setlist surface
- Story 4.30: Setlist Browser with search, chips, tags and favorites
- Story 4.31: Scenes surface
- Story 4.32: Write Dialog, Write hold and the Unsaved Indicator
- Story 4.33: Part surface, timbre, zone and keyboard, level and output
- Story 4.34: Part surface, sound, envelope and EQ, effects
- Story 4.35: Settings surface with Knobs · Global Assign
- Story 4.36: Footswitch function
- Story 4.37: Accidental-press guards and Touch on Performance
- Story 4.38: First playable unit, full show run-through

## Requirements & Constraints

- Switching scenes, Live Sets or setlists never cuts sound: held, sustained and releasing notes finish on the old generation; new notes use the new one. Loading a setlist never stops the current sound.
- Hierarchy: Setlist > Live Set > 8 parts (v1; 16 on Pro) + up to 8 scenes. Scenes store everything except timbre assignment; a scene never loads heads.
- In-setlist change is immediate because the next Live Set is preloaded (`● READY`); a non-preloaded jump keeps the old sound and shows `Loading <name>... Ns`. Off-script jump keeps the setlist loaded and LIVE SET stepping returns to it; with no setlist, stepping walks All Live Sets by name; end of setlist: "next" does nothing, `End of setlist`.
- A missing timbre plays silent with "Missing library", never substituted. Reserved engine types without a compiled engine (FM, VA) round-trip unchanged and stay silent.
- Edits stay in memory across scene/Live Set changes until Write or power-off; Write writes the current Live Set only, scopes Scene / Whole Live Set / Save as new (Scene unavailable if edits include a timbre change; Save as new suggests a free name). No confirmation dialogs on stage.
- v1 faders are non-motorized: pickup after any scene, Live Set or mode change; encoders are relative, never need pickup.
- Fixed encoder functions: DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH (relative in ALL). REV is the 9th fader in MIXER and drives the reverb send. Footswitch default: advance scene (configurable).
- Tier profile (v1/Pro) sets part count, head budget (v1 configured 5 GB), voice/cost budgets from one image, no per-tier compile paths; Pro bank A/B deferred.
- Accidental-press guards: Write hold 0.5 s (0.3-1.0, never 0); Touch on Performance Full/Safe/Off (default Safe: >=150 ms contact, <=10 px movement, acts on release); long press 400/500/800 ms (default 500); optional Panel guard (300 ms hold on LIVE SET).
- Timings: ASSIGN Overlay 3 s restarting per turn; Notice Toast 8 s; scene LED dip 200 ms; LED blink twice 2x(250 on/250 off), <=3 Hz; transitions <=150 ms; nothing animates in the Live Set Hero.
- User-facing text only via locale-catalog keys (EN source, PT-BR/ES); user content names are never translated. Terms: Setlist, Live Set, Scene, Part, Timbre.

## Technical Decisions

- Single writer: only the engine control thread writes structural state (Live Sets, parts, scenes, setlists, tags, ASSIGN maps incl. global default); clients send commands and read published state (full snapshot then deltas; `request_id`, `(epoch, seq)`, `base_revision`).
- Performance parameters (volumes, drawbars, sends, knobs) have one live value owned by RT; scene snapshots carry initial values tagged with `program_generation`, applied once at the new generation. Write waits on a control-to-RT fence before reading the last-value table.
- A generation = snapshot + resident heads (+ keys); swapped atomically only when all heads are resident, freed only when RT reports no voice references it (BundleLeases released then). Preload shares the tier head budget; an over-budget target is refused with a typed error while the current set keeps playing; a second jump cancels the first load.
- Live Set persisted as versioned protobuf in `proto/`: id (UUIDv4, regenerated on collision), name, tempo, per-part data (timbre ref `(library_id, instrument_key)`, zone, octave/transpose, keyboard on/off, volume, Main/Aux, inserts, sends, engine type ID + params keyed by `param_id`, envelope, EQ), up to 8 scenes, 8-slot ASSIGN map. Arpeggiator fields defined now but unset (Epic 7), no version bump later. Readers accept N and N-1, preserve unknown fields, refuse unknown major versions; compatibility corpus checked in CI. Atomic write: temp file, fsync, rename, on the journaled partition.
- Zone test uses the played key before octave/transpose (which apply to pitch); keyboard-off parts get no new notes but release held ones.
- Disk is read-only during performance; writes only on Write/import/update, plus the tiny Boot-into pointer (last Live Set and scene), deferred to an idle-safe moment.
- Live Set favorites/Recent live in the minimal `librarian` catalog (sole catalog writer); guard settings and footswitch function live in `sysd` config.
- Panel buttons reach `ui` as PanelEvents; `ui` drives LEDs through commands to `engine`. `ui` is LVGL 9.6, a client only.
- Seamless switching joins the release gate (render suite); RealtimeSanitizer, no allocation/locks/IO on RT threads.

## UX & Interaction Patterns

- Device: 7" 1024x600 landscape, fixed. Touch targets >=56 px, >=12 px apart on Performance, no nested targets. Never color-only state (shape/glyph/text plus color); `◆` unsaved is distinct from `● READY` at a fixed position after the Live Set name. Only the Live Set name is read at a glance; pickup takeover line is 24 px.
- Performance: Top Bar (mode mirrors, setlist context), Live Set Hero (current + next with readiness), Scene strip (current is the only filled tile), Part Strips with Volume Bars (card body opens Part; ⏻ toggles; off parts muted/hollow/`OFF`; missing library shown), Status Bar (voices, streaming, `ROTARY SLOW/FAST`, `REV` shortcut to Effects; pickup or warning takes the whole bar, e.g. `▲ FADER 3 → 65%`).
- Pickup Ghost: keylined ghost plus chevron, dashed warn outline on the strip/column, vanishes when the fader crosses the stored value.
- Panel toggles (LED per state, mirrored in Top Bar): DRAWBARS/MIXER (2), PARTS/SCENES (2; scene LEDs solid current, weak existing, off empty, with 200 ms dip on recall), PART/ALL/ASSIGN (3, cycling; Top Bar `KNOBS PART n / ALL / ASSIGN`, outlined strip). Part buttons: short press on/off, long press selects knob target.
- Navigation: MENU toggles the Launcher (7 tiles 4+3: Setlist Browser, Setlist, Library, Effects, Scenes, Import, Settings); EXIT goes one level back (nothing on Performance); WRITE hold on Performance opens the Write Dialog or "Nothing to write"; VALUE push on Performance opens the Part surface of the knob target; hold EXIT + VALUE sets brightness. Hint Bar on every non-Performance surface. Everything panel-reachable also works by touch, subject to Touch on Performance.
- Surfaces: Setlist, Setlist Browser (chips: Favorites, Recent, Setlists, All Live Sets, user tags; long press, then Action Sheet `★ Add to Favorites`), Scenes (8 slots, actions New/Rename/Duplicate/Move/Clear), Part (sections Timbre, Sound, Envelope & EQ, Zone & keyboard, Level & output, Effects), ASSIGN Overlay (never blocks sound), On-screen Keyboard (never over the Top Bar or edited field). Launcher, OSK, Scenes, Part, Action Sheet and ASSIGN Overlay have no mockups yet; design stories 4.14-4.19 precede their implementation. Mockups live under the UX designs `mockups/` folder.

## Cross-Story Dependencies

- Depends on Epics 1-3: engine types (1.8), arbiter reservations (Epics 1-2), tier strap (3.2), MCU frame protocol/ID maps (3.1), Mac virtual panel (3.10).
- Engine chain: 4.2 (format) > 4.3 (load) > 4.4 (scenes), 4.5 (switch) > 4.7 (preload) > 4.8 (render suite); 4.6 (setlists) feeds 4.7; 4.9 (Write) and 4.10 (ASSIGN) build on 4.2; 4.12/4.13 build on 4.10 and RT performance state.
- Design stories 4.14-4.19 gate UI stories: 4.15 > 4.22, 4.16 > 4.28, 4.17 > 4.31, 4.18 > 4.33/4.34, 4.19 > 4.27/4.30.
- Performance UI (4.20, 4.21) is the base for 4.23-4.27; 4.24 needs 4.12; 4.26 needs 4.13; 4.27 needs 4.10; 4.32 needs 4.9; 4.35 needs 4.10; 4.36/4.37 need `sysd` config guard settings.
- 4.38 is the end-to-end acceptance on the prototype; Story 5.18 later refines Action Sheet library/pairing/install variants; Epic 7 uses the unset arpeggiator fields.

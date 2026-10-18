---
name: Ladiani Velora 73
status: final
created: 2026-10-18
updated: 2026-10-18
sources:
  - ../../../specs/spec-arm64-stage-workstation/SPEC.md
  - ../../../specs/spec-arm64-stage-workstation/engines.md
  - ../../architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md
  - ../../architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md
  - DESIGN.md
  - .memlog.md
---

# Ladiani Velora 73 — Experience Spine

> Mockups in `mockups/` illustrate composition; this spine and DESIGN.md win on any conflict (known mock deviations listed in DESIGN.md Brand & Style).

## Foundation

Two surfaces, one mental model (Setlist → Live Set → Scenes → Parts) and one vocabulary:

| Surface | Form factor | UI system |
|---|---|---|
| **Velora 73 (device)** | 7" capacitive touchscreen, 1024×600 landscape, plus the physical panel (faders, buttons with LEDs, encoders with LED rings, wheels, pedals) | LVGL (architecture spine, Stack). No inherited component library; DESIGN.md is the visual reference. |
| **Velora Studio (desktop)** | Desktop window, macOS / Windows / Linux, connected to the device over USB-C | Tauri (architecture spine, Stack). Same tokens as the device. |

- **Themes.** Device: **Stage** (dark, `{colors.bg-dark}`) and **Studio theme** (light, `{colors.bg-light}`), user-selectable in Settings › Display & Language; there is no Auto (no light sensor). Velora Studio: dark and light, following the OS or a user setting.
- **Languages.** See Localization.
- **Prepared on the keyboard.** Players build sets on the device itself (Flow 4); Velora Studio is the "everything visible at once" alternative, not a prerequisite.
- **UI never interrupts sound.** The UI process is isolated from the engine (spec Constraints); no UI action, load or import may cut audio — see Seamless Switching. The one exception is installing a firmware update, which the player starts explicitly.

**Terminology mapping** (UI term ↔ upstream term), until the spec and architecture align (Upstream Feedback 11):

| UI term | Spec / architecture term |
|---|---|
| Live Set | performance (CAP-7), program / "default program" (CAP-16), "programs and performances" (AD-1, AD-9, AD-10) |
| Timbre | instrument (library unit, AD-10) |
| Part | part (glossary) |
| Velora Studio | companion app / desktop app (glossary, AD-13, AD-26) |
| ROTARY | rotary speaker ("Leslie-style" in engines.md) |
| Performance (home surface) | — (not the architecture's "performance state") |

## Information Architecture

**Hierarchy.** Setlist → **Live Set** (one per song) → **Parts 1–8** (timbre + zone + volume + FX + arpeggiator) + **Scenes** (up to 8 per Live Set). The timbre assignment of the 8 parts lives in the Live Set; **scenes never change a part's timbre** — a scene stores everything else (part volumes, keyboard on/off per part, FX, drawbars and other parameters).

### Device surfaces

| Surface | Reached from | Purpose | Visual ref |
|---|---|---|---|
| Boot | Power-on | Brand moment; restores the **Boot into** target | [key-screens-device.html §6](mockups/key-screens-device.html), animation in [logo-drawbar-refine.html §4](mockups/logo-drawbar-refine.html) |
| Performance (home) | Boot; EXIT from any first-level surface | Play: Live Set, scenes, 8 parts or drawbars, status | [key-screen-performance.html](mockups/key-screen-performance.html), [key-screen-drawbars-categories.html](mockups/key-screen-drawbars-categories.html) |
| Launcher | **MENU** from any surface | Grid of 7 tiles — **Setlist Browser, Setlist, Library, Effects, Scenes, Import, Settings** (canonical list) | spine-only |
| Setlist | Launcher | Choose, create and reorder setlists and their Live Sets; create a Live Set; edit setlist tags | [key-screens-device.html §1](mockups/key-screens-device.html) |
| Setlist Browser | Launcher; Top Bar setlist context on Performance | Search across setlists and Live Sets; jump off-script. Chips: ★ Favorites, Recent, Setlists, All Live Sets, then user tags | [key-screens-device.html §1b](mockups/key-screens-device.html) |
| Library | Launcher; Part › Change timbre | Find and assign a timbre by search, favorites, recent, category | [key-screens-device.html §2](mockups/key-screens-device.html) |
| Part | Part Strip card body on Performance (touch only) | One part's timbre, sound, zone, level, effects and arpeggiator | spine-only |
| Effects | Launcher; `REV` in the Performance Status Bar | Part inserts, sends, master, outputs | spine-only |
| Scenes | Launcher; long press a Scene Tile | Create, name and order the up-to-8 scenes | spine-only |
| Write | WRITE hold (Interaction Primitives) | Confirm scope and name of a save | [key-screens-device.html §4](mockups/key-screens-device.html) |
| Import | Launcher; the "USB drive found · Import?" Notice Toast | Import SFZ/SF2 from a USB-A pen drive or SSD | [key-screens-device.html §3](mockups/key-screens-device.html) |
| Settings | Launcher | Device settings (below) | [key-screens-device.html §5](mockups/key-screens-device.html) |

**Settings structure.**

| Section | Rows |
|---|---|
| Display & Language | Theme (Stage / Studio) with guidance "Studio theme reads best in daylight; Stage theme in dark venues"; Language; **Screen brightness** 5–100%; **Panel LED brightness** (Low / Mid / High / Follow screen); Large text on Performance; Reduced motion; **Touch on Performance** (Full / Safe / Off, default Safe); Write hold (0.3–1.0 s, default 0.5 s); **Boot into** (Last sound used — default / Start of setlist / Default Live Set) |
| Keyboard & Pedals | Velocity curve; Aftertouch (when the keybed has it); Footswitch function; Sustain (half-pedal calibration) and Expression pedal; pedal polarity is auto-detected at boot |
| MIDI & Clock | **Clock source** (Internal / DIN / USB); MIDI settings |
| Knobs · Global Assign | The global default ASSIGN map that seeds each new Live Set |
| Computer (Velora Studio) | Pair new computer; paired computers (Forget); USB audio/MIDI |
| Storage & Library | Free space; libraries |
| System & Update | Firmware version; Install update (from a USB drive or sent by Velora Studio) |

### Surface content (device, spine level)

**Launcher.** The 7 tiles above in two rows (4 + 3). VALUE moves the highlight, WRITE or touch opens; opening a tile replaces the Launcher.

**Part** (header `PART n · timbre · CATEGORY`, ⏻ on/off). Sections in a list-left / fields-right layout; VALUE edits the highlighted field; the 8 encoders keep their fixed functions on this part.

| Section | Fields |
|---|---|
| Timbre | Name, category; **Change timbre** → Library ("CHOOSE TIMBRE FOR PART n") |
| Sound | Engine parameters (organ: drawbars, percussion, key click, vibrato/chorus scanner, leakage, rotary; sample, FM, VA: their engine set); Brightness |
| Envelope & EQ | Attack, Release (plus the engine's other envelope stages); EQ Low, Mid, Mid freq, High |
| Zone & keyboard | Low key, High key (splits and layers, CAP-7); Octave / transpose; Keyboard on/off for this part |
| Level & output | Volume; Output Main / Aux (AD-28) |
| Effects | This part's insert chain (opens Effects on this part); Reverb send; Delay send |
| Arpeggiator | See below |

**Arpeggiator** (CAP-19; per part; Part surface and Velora Studio Part Editor) **[ASSUMPTION — confirm with user]:**

| Field | Values |
|---|---|
| Arp | On / Off |
| Mode | Up, Down, Up-Down, Random, As played |
| Rate | 1/4, 1/8, 1/16, 1/32, including triplets (1/4T, 1/8T, 1/16T, 1/32T) |
| Octaves | 1–4 |
| Gate | % of step |
| Swing | % |
| Latch | On / Off |
| Tempo | The Live Set tempo (shared by all parts) when Clock source is Internal; read-only with the source shown when following DIN or USB clock |

Arpeggiator settings belong to the Live Set; whether a scene may switch Arp on/off is part of this assumption. On clock loss the arpeggiator keeps running at the last received tempo.

**Effects** (topology fixed by AD-28; effect types per engines.md).

| Section | Content |
|---|---|
| Part inserts | Per part 1–8: ordered insert chain from rotary speaker, chorus/phaser, EP amp/drive, EQ, compressor; add, remove, reorder, bypass |
| Sends | Reverb (type, time, level) and Delay (time, tempo sync, feedback, level); per-part send levels. The REV fader in MIXER drives the reverb send |
| Master | Master EQ, compressor; line-in level (line-in mixes into the master only) |
| Outputs | Per-part assignment to Main or Aux |

**Scenes** **[ASSUMPTION — confirm with user]** for the action set.

| Section | Content |
|---|---|
| Scene list | Slots 1–8 in Scene Tile style, current marked ▶; empty slots dashed |
| Actions | New scene (copies the current scene into the next empty slot) · Rename · Duplicate · Move ◀ ▶ · Clear |
| What a scene stores | Everything except timbre assignment (see Hierarchy) |
| Saving | Edits become permanent with Write (Scene scope) |

Scene order is the order of SCENE ◀ ▶, scene buttons 1–8 and the footswitch.

### Velora Studio surfaces

| Surface | Reached from | Purpose |
|---|---|---|
| Main window | App launch | Sidebar + Live Set with Scene Tiles and 8 Studio Part Strips + Part Editor |
| Setlists | Sidebar · SETLISTS | Setlists and their Live Sets; setlist tags |
| Library management | Sidebar · LIBRARY (by category); Import button | Browse, import/convert and transfer libraries to the device (CAP-14) |
| Device | Sidebar · DEVICE | Storage, firmware (send update), backups |
| Backup & Restore | Backup button; Sidebar · DEVICE · Backups | Back up setlists and Live Sets to a `.velorabackup` file in a user-chosen location; restore asks **Merge** or **Replace** |

→ [mockups/key-screen-studio.html](mockups/key-screen-studio.html). Backup & Restore is spine-only. All surfaces except live edit, transfer, firmware and Write to Velora also work **offline** (no Velora connected).

## Voice and Tone

Microcopy rules. Brand voice lives in DESIGN.md Brand & Style.

| Do | Don't |
|---|---|
| Instructions name the control, direction and target: `▲ FADER 3 → 65%`, "DRAWBAR 2⅔': MOVE TO 6 TO TAKE OVER (NOW 3)" | "Value mismatch" / "Pickup active" |
| State facts plainly: "No organ in this Live Set", "Missing library", "No matches for 'x'. Try another word or a category." | Apologies or error codes |
| Say what happened and what still holds: "3 of 7 timbres imported; the rest were not changed", "Your edits are still on the Velora" | Vague failures ("Something went wrong") |
| Reassure about sound where it matters: "Loading a setlist keeps the current sound playing", "you can keep playing" | Silent background work the player may fear will cut audio |
| Short questions: "Write changes?" | "Are you sure you want to save all changes?" |
| Pair actions with the physical control: "WRITE · Yes", "EXIT · Cancel"; Hint Bar as `CONTROL verb` ("VALUE scroll") | Touch-only wording ("tap OK") |
| System state in uppercase tracked labels (`● READY`, `STREAMING OK`); content names in title case | Uppercase song or timbre names |
| Fixed vocabulary: Setlist, Live Set, Scene, Part, Timbre, Library, Category, Write | Synonyms (program, patch, preset, performance, instrument, save) in the UI |
| ROTARY | "Leslie", "Hammond", "DX7" |

## Component Patterns

Behavioral. Visual specs in DESIGN.md Components (same names). Timings and press rules live in Interaction Primitives.

| Component | Behavioral rules |
|---|---|
| Top Bar | Always mirrors the three toggle states and tempo with its clock source, so mode is readable from screen and LEDs alike. On Performance the setlist context is a shortcut to the Setlist Browser. |
| Live Set Hero | Shows the current Live Set (with the Unsaved Indicator when edits are pending) and the next one in the setlist with its readiness. |
| Scene Tile | Recalls the scene (same result as SCENE ◀ ▶ or a button in SCENES mode); long press opens Scenes. Exactly one current. |
| Part Strip | Card body → Part surface; ⏻ → part on/off (same as the part button in PARTS). No other touch targets inside the card. Off parts stay readable. Shows the category short label under the name; a missing library shows "Missing library" and the part stays silent. |
| Volume Bar | Shows the stored (sounding) volume of the part; it changes only when the fader has taken over. |
| Pickup Ghost | Appears whenever a fader's physical position differs from the stored value; the chevron points the way to move; disappears when the fader crosses the value and takes over. |
| Status Bar | Voices, streaming, rotary, reverb, Velora Studio live edit. A pending pickup or a warning takes over the whole bar until resolved. `REV` is a shortcut to Effects. |
| Hint Bar | Lists only the physical controls active on this surface; right side carries reassurance or the blocking warning. |
| Organ Card | Shows the organ part the faders control in DRAWBARS mode; with two organs, the one selected by long press of its part button. |
| Drawbar Column | Mirrors fader position 0–8 with pickup ghost; numeric registration updates live. |
| List Row | VALUE scrolls, touch selects; reorder by drag or hold ▲▼. The now-playing row is marked ▶ NOW; Live Sets with pending edits show ●. |
| Search Field | Typing via the On-screen Keyboard; Setlist Browser searches setlists and Live Sets. |
| Filter Chip | One active chip filters results; Setlist Browser chips include user-created setlist tags. |
| Result Tile | In Library, playing keys previews the highlighted timbre before assigning; WRITE assigns. In Setlist Browser, WRITE loads a setlist or jumps to a Live Set. ★ marks favorites. |
| Import Row | Category is auto-suggested from filename/metadata; user confirms or changes; a row without category cannot import; problem rows say why and are skipped without blocking the rest. |
| Category Chip | Opens the Category Picker. |
| Progress Bar | Import: "Importing n of m · name · converting & verifying (lossless)"; also firmware update, library transfer and boot. |
| Write Dialog | Lists what changed; scope Scene / Whole Live Set / Save as new…; WRITE confirms, EXIT cancels. Scene scope is unavailable when the edits include a timbre change ("Timbre changes are saved with the Live Set"). Save as new suggests a free name ("Perfect 2") if the name exists. |
| Unsaved Indicator | `●` after the Live Set name while the Live Set has edits not yet written. Edits stay in memory across scene and Live Set changes until Write or power-off; Write writes the current Live Set only (open another Live Set to write it). No confirmation dialog ever interrupts performance. |
| Notice Toast | Non-modal; never takes focus or opens a surface by itself. Touch acts on it; it dismisses after 8 s (paused while touched) or on EXIT; MENU › Import stays the persistent path. Also shows the brightness level during the quick path. |
| Launcher Grid | Opened by MENU; VALUE moves the highlight, WRITE or touch opens the tile; MENU or EXIT closes to the surface it was opened from. |
| ASSIGN Overlay | In ASSIGN, turning any encoder shows K1–K8 with their assigned parameter names; purely informational, sound is never blocked. |
| Segmented Control | VALUE changes the highlighted setting; touch selects directly. Used for every on/off and multi-choice setting. |
| On-screen Keyboard | Opens for text entry (search, names); locale layout with accented letters; WRITE or Done confirms, EXIT closes it keeping the typed text. Playing keys still sounds. |
| Category Picker | 17 categories with full localized labels; VALUE + WRITE or touch picks; in Import, "Apply one category to all" sets every row in a batch. |
| Action Sheet | A short choice raised by a player action (missing library: Replace timbre / Import; pairing code; install update); WRITE confirms the highlighted action, EXIT cancels. Never appears unprompted during performance. |
| Boot Screen | Says what is being restored (Live Set · Scene); ends on Performance with that sound playable. |
| Studio Title Bar | Shows connection state; Import, Backup, and "Write to Velora" (same scopes as the device Write Dialog). |
| Studio Sidebar | Selecting a Live Set loads it into the main view; library listed by category. |
| Studio Part Strip | Click selects the part and fills the Part Editor; "+ Add timbre" opens library choice for an empty part. |
| Part Editor | All parameters of the selected part at once (sound, envelope, EQ, inserts/sends, zone/keyboard, arpeggiator, assign) — no menus. |
| Studio Knob | Drag, type a value, scroll wheel, or keyboard (Accessibility Floor). Edits play live on the Velora. |
| Primary Button | One per window: "Write to Velora". |
| Studio Footer | Shows unsaved scope, live edit, write/transfer progress and last backup; announced to screen readers as a polite live region. |
| Studio Dialog | Modal for pairing-code entry, Write to Velora scope and Restore; Enter confirms, Esc cancels; focus is trapped inside and returns to the trigger. |
| Studio Banner | Non-modal notice for disconnect, version mismatch, write failure or stale edit, with one action; stays until resolved. |
| MENU Button | Toggles the Launcher Grid; no long-press function. |
| Navigation Button | EXIT, WRITE, LIVE SET ◀ ▶, SCENE ◀ ▶ — per the rule table in Interaction Primitives. |
| Panel Toggle Button | Each press advances to the next state (2-state, or 3-state cycle PART → ALL → ASSIGN); LED and Top Bar confirm. |
| Part/Scene Button | Meaning depends on PARTS/SCENES — see Interaction Primitives. |
| Encoder | Endless; no value jumps; LED ring shows the current value of its target. |
| Fader | Non-motorized in v1; pickup behavior after any recall. |

## State Patterns

Warn and ok states use the theme's role (`{colors.warn-dark}` / `{colors.warn-light}`, `{colors.ok-dark}` / `{colors.ok-light}`) and always carry a glyph or text.

**Performance**

| State | Treatment |
|---|---|
| Pickup pending | After a scene, Live Set or mode change, a fader does not move the value until it crosses the stored value. Pickup Ghost shows the physical position with a ▲/▼ chevron; the Part Strip or Drawbar Column gets a dashed warn outline; the Status Bar takeover reads `▲ FADER 3 → 65%`. Encoders never need pickup. |
| Ready / preloaded | The next Live Set in the setlist is preloaded (heads resident); `● READY` in ok. |
| Loading (not preloaded) | Jumping to a Live Set that is not preloaded: the **current sound keeps playing**, the hero shows `Loading <name>… Ns`, and the switch happens as soon as it is ready — no cut. |
| End of setlist | NEXT reads `End of setlist`; LIVE SET ▶ does nothing. |
| No setlist loaded | Top Bar context reads `NO SETLIST · <Live Set>`; LIVE SET ◀ ▶ step through All Live Sets. |
| Missing library | The part is **silent**; its Part Strip shows "Missing library"; the Status Bar warn takeover names the part. The card opens an Action Sheet: **Replace timbre** (Library) or **Import**. |
| Part off | Name and category in muted, hollow LED ring, `OFF` tag; still readable. |
| No organ | DRAWBARS does not change mode; its LED blinks twice; screen shows "No organ in this Live Set". |
| Two organs | Faders control the organ part selected by long press of its part button. |
| Clock following | Top Bar `♩ 72 · DIN` or `♩ 72 · USB`. |
| Clock lost | External clock stops arriving: Top Bar `⚠ CLOCK LOST` in warn and a Status Bar warn takeover "⚠ CLOCK LOST · ARP AT ♩ 72"; clears when clock returns. |
| Velora Studio live edit | Status Bar shows `STUDIO`; edits from Velora Studio play immediately and mark ● like local edits. |
| Unsaved changes | `●` after the Live Set name; also on that Live Set's List Rows and Result Tiles elsewhere; **no confirmation dialogs on stage**. |
| USB drive inserted | Notice Toast "USB drive found · Import?" (or "Update 1.3 found · Open?" when the drive holds a firmware bundle); never auto-opens anything. |
| Empty scene / empty part | Dashed tile "6"; part "—  empty". |
| Highlighted / focused | Device: raised + `{spacing.selection-device}` accent border on the VALUE-highlighted item. Velora Studio: `{spacing.focus-ring}` accent focus ring at 2 px offset. |

**Lists and search**

| State | Treatment |
|---|---|
| Search no results | "No matches for 'x'. Try another word or a category." |
| First run, no setlists | Setlist: "No setlists yet." with "+ New setlist" highlighted. |
| Empty ★ Favorites / Recent | "No favorites yet." / "Nothing played yet." — other chips stay available. |

**Write**

| State | Treatment |
|---|---|
| Nothing to write | WRITE hold with no pending edits: Hint Bar "Nothing to write". |
| Name in use | Save as new: "A Live Set named 'x' is already in this setlist" with the suggested free name. |
| Storage full | "Not enough space to write. Free space in Settings › Storage & Library." Edits stay in memory with ●. |

**Import** (power loss never leaves a partial library, CAP-17)

| State | Treatment |
|---|---|
| Needs category | Row marked `needs category`; Hint Bar: "● 1 timbre needs a category before import". |
| In background | EXIT sends the import to the background; playing continues; progress visible on return to Import. |
| Insufficient storage | Before start: "Needs 18.4 GB, 9.2 GB free. Free space in Settings › Storage & Library or import fewer files." WRITE does not start the import. |
| Drive removed | "USB drive removed. 3 of 7 timbres imported; the rest were not changed. Reconnect to import them." The timbre in progress is discarded whole. |
| Unsupported or corrupt file | Row: `skipped: uses SFZ features Velora doesn't support` (outside the v1 subset, e.g. scripting or effect buses) or `skipped: file is damaged`; the rest of the batch imports. |
| Duplicate | Row: `already imported` with **Skip** (default) or **Import again** (replaces it; Live Sets keep pointing at it). |

**Firmware update** (CAP-12, CAP-17; bundle from a USB drive or sent by Velora Studio)

| State | Treatment |
|---|---|
| Ready to install | Settings › System & Update: "1.3 ready to install". Install opens an Action Sheet: "Install update? Sound stops until the Velora restarts." `WRITE · Install`, `EXIT · Later`. |
| Installing | Boot Screen layout: `UPDATING TO 1.3` with Progress Bar; **sound is unavailable**; "If power is lost, the Velora starts on 1.2." |
| Success | After restart, Notice Toast "Updated to 1.3". |
| Failure | "Update failed: the file is damaged or not signed. Still on 1.2." |
| Rolled back after power cut | Boots the previous firmware; Notice Toast "Update did not finish. Still on 1.2 · Try again in Settings › System & Update." |

**Velora Studio**

| State | Treatment |
|---|---|
| First-time pairing | The player starts it on the device (Settings › Computer › Pair new computer): an Action Sheet shows a 6-digit code; Velora Studio shows a Studio Dialog "Enter the code shown on Velora 73". Wrong code: "That code doesn't match. Check the Velora screen." Sound is unaffected. |
| Connected | Title bar "● VELORA 73 · CONNECTED VIA USB · PAIRED". |
| Offline | No Velora connected: backups and the computer-side library stay editable; live edit, transfer, firmware and Write to Velora wait for a device. |
| Version mismatch | Studio Banner: "Velora 73 runs 1.2; this Velora Studio needs 1.3 for some features. Update firmware / Update app." Features the device lacks are hidden; live edit is disabled only if the versions cannot talk. |
| Live edit | Footer "● LIVE EDIT ON · CHANGES PLAY ON THE VELORA"; last backup time shown. |
| Unsaved | Footer "UNSAVED: SCENE 2"; edits play live but persist only with Write to Velora. |
| Write to Velora | Studio Dialog with the device's scopes (Scene / Whole Live Set / Save as new…); footer "WRITING TO VELORA…" with the Primary Button disabled; success clears ● on both. |
| Write failed | Studio Banner: "Couldn't write to Velora 73: connection lost. Your edits are still on the Velora (●). Reconnect and write again." |
| Stale edit | Studio Banner: "Changed on the Velora since you opened it · Reload". |
| Disconnect mid-edit | Unsaved live edits **stay on the device in memory**, marked ●, until Write or power-off. Studio Banner: "Velora 73 disconnected · Reconnect"; on reconnect Velora Studio reloads the device state. |
| Library transfer | Library management and footer: "Transferring Viola Caipira · 4.2 of 18.4 GB"; the device keeps playing; the library activates when complete (device Toast "Viola Caipira ready in Library"). |
| Transfer failed | "Transfer stopped: connection lost. Nothing was installed. Start again when connected." |
| Firmware send | Device panel: transfer progress, then "Installing on Velora 73 — sound is off on the device until it restarts"; the connection drops and returns on restart. |
| Restore | Restoring a `.velorabackup` asks **Merge** (add/update, keep what is not in the backup) or **Replace** (device content becomes the backup). |

**Boot**

| State | Treatment |
|---|---|
| Restoring | "RESTORING · {Live Set} · SCENE n" with Progress Bar; ≤ 10 s to playable (CAP-16) for the Boot into target. |
| Target missing | If the Boot into target is gone or its library is missing, boot ends on Performance with the target loaded and Missing library shown on affected parts. |

## Interaction Primitives

Canonical home for press rules, shortcuts and timings; other sections refer here. → Physical layout and toggle behavior: [mockups/panel-v2.html](mockups/panel-v2.html) (its knob toggle shows two LEDs; the approved toggle is 3-state).

**Touch equivalence.** Everything reachable by panel controls also works by touch, and vice versa for Performance actions (scene and Live Set change, part on/off), subject to Touch on Performance below. Exception: the Part surface is touch-only (Open Questions); knob-target selection is panel-only in Safe and Off.

**Navigation rules** (one table; Hint Bars and flows follow it):

| Control | Where | Press | Hold |
|---|---|---|---|
| WRITE | Performance | Nothing (Hint: "Hold WRITE to write") | **0.5 s** (Write hold, 0.3–1.0 s) → Write Dialog if edits are pending, else "Nothing to write" |
| WRITE | Lists, grids, Launcher, pickers, Action Sheet, Write Dialog | Acts on release: confirms the highlighted item or the primary action (`WRITE · Yes`) | On editing surfaces (Part, Effects, Scenes), held past the Write hold → Write Dialog |
| EXIT | Any surface | Acts on release: **one level back** (Library → Part → Performance; dialog or sheet → cancel; Launcher → the surface it was opened from; a tile opened from the Launcher → the surface the Launcher was opened from). From Performance: nothing | Held while turning VALUE → screen brightness; the back action is cancelled |
| MENU | Any surface | Toggles the Launcher Grid; closing returns to the surface it was opened from | No function |
| VALUE | Any surface | Turn: scroll, move highlight or change the highlighted value | — |
| LIVE SET ◀ ▶ / SCENE ◀ ▶ | Any surface | Step through the setlist / scenes (seamless) | — |

**Touch on Performance** **[ASSUMPTION — confirm with user]** (Settings › Display & Language; default **Safe**). On press-down every target shows an armed state (raised + accent edge).

| Target | Full | Safe (default) | Off |
|---|---|---|---|
| ⏻ on Part Strip | Tap: part on/off | Acts on release after ≥ 150 ms contact with ≤ 10 px movement | Ignored (use the part button) |
| Scene Tile | Tap: recall | Acts on release after ≥ 150 ms, ≤ 10 px | Ignored (use SCENE ◀ ▶ or scene buttons) |
| Scene Tile, long press 500 ms | Scenes | Scenes | Scenes |
| Part Strip card body | Tap → Part; long press → knob target | Long press → Part | Long press → Part |
| Setlist context (Top Bar) | Tap → Setlist Browser | Long press → Setlist Browser | Long press → Setlist Browser |
| `REV` in Status Bar | Tap → Effects | Long press → Effects | Long press → Effects |

**Timings** (canonical):

| Rule | Value |
|---|---|
| Long press (touch and part buttons) | 500 ms |
| Write hold | 0.5 s default, configurable 0.3–1.0 s (never 0) |
| Safe touch | ≥ 150 ms contact, ≤ 10 px movement, acts on release |
| ASSIGN Overlay | 3 s, restarts on each turn |
| Notice Toast | 8 s, paused while touched |
| Scene recall LED pulse | 200 ms dip on the current scene's LED |
| LED "blinks twice" | 2 × (250 ms on, 250 ms off); all blinking ≤ 3 Hz |
| UI transitions | ≤ 150 ms ease-out; nothing animates in the Live Set Hero |
| Reduced motion | Cross-fades ≤ 80 ms or none; Toast appears in place; LED pulse becomes an instant change |

**Panel toggles** (single button, LED per state, mirrored in the Top Bar):

| Toggle | States | Effect |
|---|---|---|
| DRAWBARS / MIXER | 2 | The 9 faders drive the organ part's drawbars, or the volumes of parts 1–8 + reverb send (9th, REV). |
| PARTS / SCENES | 2 | PARTS: short press toggles the part on/off; long press selects it as the knobs' target (and drawbar target with two organs). SCENES: the 8 buttons recall scenes 1–8 — current = solid strong LED, existing = weak LED, empty = off; on recall the current LED dips once (200 ms), so state never depends on brightness alone. |
| PART / ALL / ASSIGN | 3, cycling | The 8 encoders act on the selected part, on all parts (values shown as relative), or on the Live Set's ASSIGN map. |

**Fixed knob functions:** DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH; reverb is the 9th fader in MIXER. **ROTARY**: STOP and S/F (slow/fast). Pitch and modulation are two wheels.

**ASSIGN.** Maps the 8 encoders to user-chosen parameters, saved **per Live Set**; a user-editable global default (Settings › Knobs · Global Assign) seeds each new Live Set. Velora Studio edits the map in the Part Editor ("ASSIGN · KNOBS (THIS LIVE SET)"). Turning a knob in ASSIGN shows the ASSIGN Overlay.

**Footswitch:** default **advance scene**; configurable in Settings › Keyboard & Pedals to previous scene, next Live Set or rotary speed.

**Brightness quick path:** hold EXIT and turn VALUE on any surface; the level shows in a Notice Toast, never over the Live Set Hero.

**Library preview.** While browsing the Library, playing the keys auditions the highlighted timbre.

## Seamless Switching

Hard requirement: switching scenes, Live Sets or setlists **never cuts sound** — held, sustained and releasing notes continue (architecture AD-23 generations already let old voices finish).

- **Scene change** is instant: scenes never change timbres, only parameters of the Live Set's 8 timbres.
- **Live Set change** within the setlist is seamless because the next Live Set is preloaded (`● READY`).
- **Loading a setlist** keeps the current sound playing.
- **Off-script jump** via the Setlist Browser goes to any Live Set without cutting sound; the setlist stays loaded and LIVE SET ◀ ▶ returns to it. A target that is not preloaded follows the Loading state.
- **Import and library transfer** run in the background; the player keeps playing. Only a firmware install stops sound, and only when the player starts it.

## Panel ↔ Screen Mapping

| Panel | Screen counterpart |
|---|---|
| DRAWBARS/MIXER LED | Top Bar mode; part area shows Part Strips (MIXER) or Organ Card + Drawbar Columns (DRAWBARS) |
| PARTS/SCENES LED | Top Bar mode; button LEDs mean part on/off (PARTS) or scene current/exists/empty (SCENES) |
| PART/ALL/ASSIGN LED | Top Bar `KNOBS PART n / ALL / ASSIGN`; selected Part Strip outlined |
| Fader position | Volume Bar / Drawbar Column + Pickup Ghost and Status Bar takeover |
| Encoder LED ring | Parameter value of its target (relative in ALL) |
| Part button (PARTS) | Part Strip LED dot / ⏻ / OFF tag; long press outlines the selected Part Strip |
| ROTARY S/F | Status Bar `ROTARY SLOW/FAST`; Organ Card meta |
| VALUE / WRITE / EXIT | Hint Bar on every non-Performance surface |
| MENU | Launcher Grid (opens / closes) |
| LIVE SET ◀ ▶ / SCENE ◀ ▶ / footswitch | Live Set Hero / current Scene Tile |
| Encoder turned in ASSIGN | ASSIGN Overlay |

## Categories

Shown under every timbre name (instead of engine type) and used as Library filters. Every imported timbre must have one. Part Strips use the **short label** (≤ 10 characters); Library, Import and the Category Picker use the full label.

| # | EN (default) | EN short | PT-BR | PT-BR short | ES | ES short |
|---|---|---|---|---|---|---|
| 1 | Piano | Piano | Piano | Piano | Piano | Piano |
| 2 | E.Piano | E.Piano | Piano Elétrico | P.Elétrico | Piano Eléctrico | P.Eléctr. |
| 3 | Organ | Organ | Órgão | Órgão | Órgano | Órgano |
| 4 | Keys | Keys | Teclas | Teclas | Teclas | Teclas |
| 5 | Mallet & Bells | Mallets | Percussão Melódica | Perc.Mel. | Percusión Melódica | Perc.Mel. |
| 6 | Strings | Strings | Cordas | Cordas | Cuerdas | Cuerdas |
| 7 | Brass | Brass | Metais | Metais | Metales | Metales |
| 8 | Woodwind | Woodwind | Madeiras | Madeiras | Maderas | Maderas |
| 9 | Choir & Vocal | Choir | Coral & Vozes | Coral | Coro & Voces | Coro |
| 10 | Guitar | Guitar | Violão & Guitarra | Violão/Gt | Guitarra | Guitarra |
| 11 | Bass | Bass | Baixo | Baixo | Bajo | Bajo |
| 12 | Synth Lead | Synth Lead | Synth Lead | Synth Lead | Synth Lead | Synth Lead |
| 13 | Synth Pad | Synth Pad | Synth Pad | Synth Pad | Synth Pad | Synth Pad |
| 14 | Synth Poly | Synth Poly | Synth Poly | Synth Poly | Synth Poly | Synth Poly |
| 15 | Drums & Perc | Drums | Bateria & Percussão | Bateria | Batería & Percusión | Batería |
| 16 | World | World | Étnicos | Étnicos | Étnicos | Étnicos |
| 17 | FX & Ambient | FX/Amb | Efeitos & Ambiências | Efeitos | Efectos & Ambientes | Efectos |

Scope of each category ("includes"): [key-screen-drawbars-categories.html](mockups/key-screen-drawbars-categories.html).

## Localization

- **Languages.** English is primary and the default; Brazilian Portuguese and Spanish are selectable in Settings › Display & Language; more arrive via system updates. All strings are catalog keys (AD-20). Velora Studio follows the OS language, with an override in its settings.
- **Panel legends** stay English in every locale. Hint Bar control names stay English (they match the panel); verbs localize, budgeted at +40% ("VALUE desplazar").
- **User content** (setlist, Live Set, scene, tag names) is never translated.
- **Truncation.** Setlist context in the Top Bar ellipsizes in the middle; timbre names ellipsize at the end, 2 lines max on Part Strips (1 in Large text); Live Set name 1 line, end ellipsis; categories on Part Strips use the short label; the Status Bar shows only the takeover line when one is active.
- **Formatting** follows CLDR per locale: `65 %` (ES), `18,4 GB` (PT-BR), `18.4 GB` (EN).
- **QA rules.** Every mockup review includes pseudo-localization at **+40% length with accented capitals**, and the clipping test string `ÁÇÊÕÑ Ação`. LVGL fonts are built with ascent measured from Á/Ç/Ñ/Ê.

## Accessibility Floor

Behavioral. Visual contrast, legibility tiers and Large-text sizes live in DESIGN.md.

**Device**

- **Stage legibility.** Only the Live Set name is read at a glance from 1.5 m; mid-song information that must be read at 0.7 m (pending pickup) uses the 24 px takeover line. The current scene is the only filled tile.
- **Large text on Performance** (Settings) scales hero, scenes, part names and status by ≈ 25%, drops scene sub-labels and Part Strip categories, and keeps names to one line.
- **Contrast floor.** Text ≥ 4.5:1 and meaningful boundaries ≥ 3:1 in both themes and in Velora Studio. No opacity on text.
- **Brightness.** Screen brightness 5–100% and panel LED brightness in Settings, plus the EXIT + VALUE quick path. There is no light sensor, so the player fixes glare or blinding by hand, fast.
- **Touch targets.** ≥ 56 px (`{spacing.touch-target-min}`), ≥ 12 px apart on Performance, no nested targets; Touch on Performance guards against brushing the screen while playing.
- **Reduced motion.** Setting in Settings › Display & Language; boot animation becomes a static frame; transitions per Timings. Velora Studio follows the OS preference.
- **Never color-only.** Modes: LED + legend + Top Bar text. Current scene: fill + position (screen), solid LED + recall pulse (panel). Now playing: ▶ + `NOW`. Part off: muted text + hollow dot + `OFF`. Pickup: keylined ghost + chevron + takeover text. Warnings: glyph + text. Unsaved: ● at a fixed position after the name.
- **Accidental-press guard.** Write needs a hold; Touch on Performance defaults to Safe; mode buttons show unmistakable state; no confirmation dialogs interrupt performance.

**Velora Studio**

- **Keyboard.** Every control reachable by Tab in visual order; F6 cycles regions (sidebar, Live Set, scenes, parts, editor, footer); arrow keys move within the scene and part grids.
- **Knobs.** Studio Knob is an ARIA `slider` with name, unit and `aria-valuetext`; ↑/↓ step 1, Shift ±10, PgUp/PgDn, Home/End; Enter opens value entry.
- **Reorder** without dragging: Alt+↑/↓ and a "Move to…" menu.
- **Focus.** Visible ring: `{spacing.focus-ring}` accent at 2 px offset, never removed.
- **Screen readers.** Live regions for connection state, unsaved changes, write, transfer and backup results; the Unsaved ● has `aria-label="Unsaved changes"` and a tooltip.
- **Text size.** All sizes in rem (minimum 13 px body, 11 px labels); honours OS text size and ⌘/Ctrl +/− to 200%; the Part Editor reflows to 2 columns then 1 with no horizontal scrolling.
- **Increase contrast.** The OS preference swaps hairlines for `line-strong`.

## Responsive & Platform

- **Device:** fixed 1024×600 landscape; no responsive behavior.
- **Velora Studio:** macOS, Windows, Linux; dark/light follows the OS or user. **Minimum window 1280×800**; above it the main area grows; text zoom reflows per Accessibility Floor.

## Inspiration & Anti-patterns

Brand posture (Nord, Apple, Ferrari) lives in DESIGN.md Brand & Style.

- **Lifted from Nord:** a player-first panel — every stage action has its own control.
- **Rejected — deep menus to edit a sound:** Velora Studio shows every parameter at once.
- **Rejected — button pairs per mode:** single toggles with LED per state.
- **Rejected — value jumps:** endless encoders and fader pickup.
- **Rejected — a finish that wears** (Motif XS); **orange/red brand accents** (Studiologic/Roland, Nord).

## Key Flows

### Flow 1 — Stage night (Mateus, wedding gig, setlist "Wedding Ana & Pedro")

1. Mateus powers on. Boot Screen: drawbars pull out, VELORA fades in, "RESTORING · PERFECT · SCENE 2". Under 10 s the last sound used (Boot into default) is playable.
2. He presses MENU › Setlist, picks "Wedding Ana & Pedro" and presses WRITE to load it; the current sound keeps playing.
3. Song 3, "Perfect". Performance shows the Live Set name large, scene 2 "Verse" filled, NEXT "Thinking Out Loud · ● READY".
4. Chorus: he taps the footswitch. Scene 3 recalls; part 3 strings come in. Fader 3 sits low: the keylined ghost and the takeover line `▲ FADER 3 → 65%` tell him what to do; he pushes up and takes over.
5. Bridge needs organ: he presses DRAWBARS. The Organ Card and drawbars appear; he pulls the 2⅔' past its stored 6 to take over, hits ROTARY S/F for fast.
6. End of song: LIVE SET ▶. "Thinking Out Loud" was preloaded; the last chord rings out under the new Live Set.
7. The band calls a song not on the setlist. He long-presses the setlist context in the Top Bar (Safe touch) to open the Setlist Browser, types "thou", picks "Thou Art Worthy" (a Live Set from the Sunday Service setlist) and jumps to it.
8. **Climax:** the switch does not cut sound — his sustained chord carries over, the new Live Set is under his hands, and the setlist is still there: LIVE SET ◀ ▶ brings him back to song 4.
9. After the gig he tweaks part 2's volume (the `●` appears next to "Perfect"), holds WRITE 0.5 s, chooses Scene 2 "Verse", WRITE · Yes.

Failure paths: DRAWBARS pressed in a Live Set without organ → LED blinks twice, "No organ in this Live Set", mode unchanged. A brief press of WRITE on Performance does nothing. A hand brushing a Scene Tile or ⏻ under 150 ms does nothing (Safe). Jump target not preloaded → Loading state. The drummer's clock drops out mid-song → `⚠ CLOCK LOST`, the arpeggiator keeps the last tempo.

### Flow 2 — Studio afternoon (Mateus, importing a Viola Caipira and refining in Velora Studio)

1. Mateus plugs a USB drive ("SANDISK 128GB") into USB-A; a Notice Toast reads "USB drive found · Import?". He taps it to open Import: 7 timbres, 18.4 GB.
2. Categories are suggested from file names (Rhodes_Mk1 → E.Piano ✓, Upright_Bass_Jazz → Bass ✓). Viola_Caipira has none: the row shows "Choose category ▾" and the Hint Bar warns.
3. He opens the Category Picker, chooses Strings (the Library later lists it as "Brazilian Viola · STRINGS · IMPORTED"), confirms the suggestions, and presses WRITE to start.
4. He presses EXIT: the import continues in the background while he keeps playing.
5. Later he opens Velora Studio on his laptop for the first time. On the Velora he opens Settings › Computer › Pair new computer; he types the code shown on the device into Velora Studio, and the title bar shows "● VELORA 73 · CONNECTED VIA USB · PAIRED".
6. He selects a Live Set in the sidebar and clicks an empty part ("+ Add timbre"); he assigns the new viola from the library, and the Part Editor shows every parameter at once — he shapes the envelope and sets the zone.
7. He maps K1–K3 in ASSIGN for this Live Set.
8. **Climax:** every edit plays live on the Velora under his hands; the footer reads "UNSAVED: SCENE 2" until he clicks "Write to Velora", picks the scope he knows from the panel, and the change is permanent on the device.
9. He clicks Backup; the footer shows the new last-backup time.

Failure paths: a row without a category cannot import; a file outside the SFZ subset is skipped with a reason; not enough space is caught before the import starts; pulling the drive keeps every finished timbre. No Velora connected → offline editing. The cable is pulled mid-edit → edits stay on the Velora with ●, Velora Studio shows "Reconnect". Power loss during import never leaves a partial library.

### Flow 3 — Choosing a timbre on the device (Mateus, rehearsal, part 3 of "Perfect" needs strings)

1. On Performance he long-presses part 3's card to open its Part surface and selects **Change timbre**: "LIBRARY · CHOOSE TIMBRE FOR PART 3".
2. He taps the Strings chip (or types "strings" on the On-screen Keyboard).
3. He turns VALUE to highlight "Chamber Strings" and plays a few keys: the highlighted timbre previews.
4. **Climax:** WRITE assigns it to part 3 without his hands leaving the keyboard. Inside the Library that is three interactions (chip, highlight, WRITE) when the timbre is on the first page; reaching the Library from Performance adds two (open the part, Change timbre). A favorite takes two touches inside the Library (★ Favorites chip, tile). CAP-15's "one touch / three interactions" is therefore not met as written (Upstream Feedback 17).
5. EXIT returns to Part, EXIT again to Performance; the Part Strip reads "Chamber Strings · STRINGS" and ● marks the Live Set (a timbre change is written with the Live Set).

Failure: EXIT cancels without changing the part. Search with no results → "No matches for 'x'. Try another word or a category."

### Flow 4 — Building a Live Set on the keyboard (Mateus, Saturday, preparing "Goodness of God" for Sunday Service)

1. MENU › Setlist; in "Sunday Service" he selects **+ New Live Set** **[ASSUMPTION — confirm with user: entry point]**, types "Goodness of God" on the On-screen Keyboard, WRITE. Performance shows the new Live Set with 8 empty parts and one scene.
2. He long-presses part 1 → Part → Change timbre → Library: Piano chip, "Concert Grand", WRITE. EXIT back to Part 1; in Zone & keyboard he sets Low key C3.
3. Part 2 → Change timbre → Synth Pad "Warm Pad", full range: a layer over the piano. Part 3 → Bass "Upright Bass", High key B2: a split under the piano.
4. MENU › Effects: Sends — reverb Hall, part 1 send 25%, part 2 send 40%; Part inserts — a compressor on part 3.
5. Back on part 2's Part surface he turns on the **Arpeggiator**: Up, 1/8, 2 octaves, Latch off. The Top Bar shows `♩ 72`; he sets the Live Set tempo in the same section.
6. Long press Scene Tile 1 → Scenes: renames it "Verse". New scene → slot 2 "Chorus". On Performance, in Chorus, he turns part 2 on and pushes fader 2 up.
7. **Climax:** he holds WRITE 0.5 s, chooses **Whole Live Set**, WRITE · Yes — the song is built and saved entirely on the keyboard, and every step sounded as he went.
8. Next Sunday "Goodness of God" sits in the setlist with `● READY` when its turn comes.

Failure paths: EXIT from any surface never discards edits — they stay in memory with ● until Write or power-off. Storage full → "Not enough space to write". A Library missing for a chosen timbre → Missing library on that part.

## Open Questions

- **Favorites on the device:** how a timbre or Live Set is starred (★) on the device is not defined.
- **Part surface without touch:** it is reachable only from a Part Strip; there is no panel path.
- **Stage guard for hardware buttons:** whether LIVE SET ◀ ▶ (song change) and part-off presses need a hold or double press (review M8).
- **Pro tier, 16 parts:** how Performance shows 16 Part Strips (paging, two banks of 8).
- **Long-press threshold** as a user setting (400 / 500 / 800 ms) for players who trigger it by accident.

Deferred (not UX questions): final typeface, physical panel specs, app icon and avatar, hardware asks — see DESIGN.md Deferred.

## Upstream Feedback

Decisions that must flow back to the spec (`bmad-spec`) and architecture (`bmad-architecture`):

1. **Seamless switching** as a new spec capability: scene, Live Set and setlist changes never cut held, sustained or releasing notes (AD-23 already supports it).
2. **Next-Live-Set preload** (heads resident) within the streaming budget (AD-24), so in-setlist Live Set changes are seamless.
3. **Scenes never change timbres**: timbre assignment per Live Set; scenes store parameters only. Architecture to state whether a scene recall is a batch of performance events or a new `program_generation` (AD-2).
4. **Backup & restore** of setlists and Live Sets in Velora Studio (new requirement): `.velorabackup` file, user-chosen location, restore as merge or replace; Velora Studio usable offline.
5. **English default locale**, PT-BR and ES selectable, more by update — supersedes AD-20's "PT-BR as the source locale" and the architecture Deferred row "Locales beyond PT-BR — before export".
6. **ASSIGN per Live Set with a global default** mapping (structural state, part of the Live Set format).
7. **17 categories**, with a mandatory category on every import (auto-suggested); category shown in UI (CAP-8, CAP-15).
8. **Setlist tags** (user-created, filterable, editable on device and in Velora Studio).
9. **Motorized faders reserved for Pro**; v1 uses non-motorized faders with pickup.
10. **Panel control set** for HARDWARE-INTERFACES and AD-16 ("knobs, 9 drawbars"): knobs are **endless encoders with LED rings** (resolves "absolute pots or encoders, to be decided with UX"); 9 faders are dual-function drawbars/mixer; 3 toggle buttons with per-state LEDs; a **MENU** button between EXIT and WRITE; footswitch advances scene by default. This refines spec "one knob per function".
11. **Terminology**: UI uses Setlist / Live Set / Scene / Part / Timbre / Velora Studio (mapping in Foundation). Align the spec glossary and CAP-7/CAP-16 ("performance", "default program", "Companion app") and the architecture's "programs and performances" (AD-1, AD-9, AD-10).
12. **Boot target**: boot restores the "Boot into" target (default: last sound used). CAP-16 should measure time-to-playable for that target; AD-24 should load the boot target's heads first (before favorites); AD-18 should allow a small journaled "last-sound pointer" write (or record it whenever the Live Set or scene changes).
13. **Pairing UX (AD-13)**: pairing starts on the device (Settings › Computer › Pair new computer), shows a 6-digit code typed into Velora Studio; paired computers can be forgotten.
14. **Display resolution 1024×600** to HARDWARE-INTERFACES §8, together with DESIGN.md Deferred › Hardware asks (luminance, AG/AR glass, dimming range, LED intensity, legend color/size/band, tactile cues).
15. **AD-9 dataset owner** for setlists, Live Sets (including ASSIGN maps, scenes and arpeggiator settings) and setlist tags; today the table names only "programs and performances" (engine control thread) and the Catalog's tags.
16. **"Leslie"** still appears in the SPEC success signal ("organ with Leslie") and engines.md ("Leslie-style"); use "rotary speaker" / ROTARY per the trademark constraint.
17. **CAP-15 wording**: define what counts as an interaction and where counting starts. The device today reaches a favorite in two touches inside the Library and any timbre in three interactions inside the Library (five from Performance).
18. **Arpeggiator UX (CAP-19, AD-21)**: the parameter set in IA › Arpeggiator, tempo per Live Set, clock source in system config, the CLOCK LOST state and continue-at-last-tempo behaviour, and whether scenes may switch the arpeggiator.
19. **Touch on Performance** (Full / Safe / Off) and **Write hold** as system config (AD-9 sysd), alongside **screen and LED brightness**, whose hardware asks are in DESIGN.md.
20. **Hardware-facing UX notes**: pedals (half-pedal calibration, expression, polarity auto-detect) appear in Settings › Keyboard & Pedals; the Pro tier's 16 parts (CAP-7, AD-27) need a Performance layout (Open Questions); a USB-A drive carrying a RAUC bundle (AD-26) raises "Update found · Open?" instead of "Import?", and firmware install stops sound until restart.

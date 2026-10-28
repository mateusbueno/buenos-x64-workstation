---
status: final
stepsCompleted: [1, 2, 3, 4]
inputDocuments:
  - _bmad-output/specs/spec-arm64-stage-workstation/SPEC.md
  - _bmad-output/specs/spec-arm64-stage-workstation/performance-targets.md
  - _bmad-output/specs/spec-arm64-stage-workstation/engines.md
  - _bmad-output/specs/spec-arm64-stage-workstation/hardware-platform.md
  - _bmad-output/specs/spec-arm64-stage-workstation/library-format.md
  - _bmad-output/specs/spec-arm64-stage-workstation/acceptance-tests.md
  - _bmad-output/specs/spec-arm64-stage-workstation/go-to-market.md
  - _bmad-output/specs/spec-arm64-stage-workstation/glossary.md
  - _bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md
  - _bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md
  - _bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md
  - _bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md
  - _bmad-output/research/pesquisa-tecladistas-brasil.md
notes:
  - The SPEC stands in for the PRD. CAP-1..CAP-23 are the functional requirements; Constraints, performance-targets.md, acceptance-tests.md and Non-goals feed the NFRs.
  - pesquisa-tecladistas-brasil.md is a market survey design that has not been run yet; it feeds External Dependencies, not requirements.
---

# Ladiani Velora 73 - Epic Breakdown

## Overview

This document provides the complete epic and story breakdown for Ladiani Velora 73, decomposing the requirements from the PRD, UX Design if it exists, and Architecture requirements into implementable stories.

Source conventions: the SPEC (`SPEC.md` + companions) stands in for the PRD, so FRs trace to CAP IDs. Architecture decisions are cited as AD-n (`ARCHITECTURE-SPINE.md`) and HW-§n (`HARDWARE-INTERFACES.md`). UX items cite DESIGN.md / EXPERIENCE.md sections. UI vocabulary is Setlist / Live Set / Scene / Part / Timbre / Velora Studio; "program" and "performance" in ADs mean the Live Set.

## Requirements Inventory

### Functional Requirements

**Sample streaming and library (CAP-1)**

- FR1 (CAP-1): The sample engine starts every voice from a RAM-resident, mlocked head and continues from NVMe-streamed buffers that refill ahead of the playhead, so instruments from a library far larger than RAM are playable.
- FR2 (CAP-1): The unit indexes on-device libraries totalling up to 1 TB and resolves instruments from the index (`/data/lib/<library_id>/current`) without scanning library contents at boot or at Live Set load.
- FR3 (CAP-1, CAP-9): Library audio is stored losslessly (FLAC-style per-block coding for integer PCM, uncompressed float, up to 32-bit float / 96 kHz) and decodes bit-identical to the source; import never resamples or requantizes.
- FR4 (CAP-1): Heads are decoded to PCM into locked RAM when an instrument loads, so a note's attack never waits on decode or disk.

**Latency (CAP-2)**

- FR5 (CAP-2, CAP-18): Keybed, panel, MIDI DIN and USB MIDI events are timestamped in device time and applied by the engine at their exact in-block frame offset after a fixed scheduling delay, so the key-to-sound path is deterministic.

**Voice pool (CAP-3)**

- FR6 (CAP-3): All engines draw voices from one dynamic, CPU-budgeted shared pool; each engine is guaranteed its floor (sample ≥256 stereo, FM ≥128, VA ≥64, organ never stolen) and surplus is shared.
- FR7 (CAP-3): Voice stealing considers the whole Live Set, protects the lowest held note and pedal-sustained notes, and steals newer, quieter voices first; a stolen voice still fading out keeps counting against the budget.

**Tonewheel organ (CAP-4, CAP-18)**

- FR8 (CAP-4): The tonewheel organ engine plays all 73 keys with all 9 drawbars at full polyphony and is never voice-stolen (reserved ahead of engine floors).
- FR9 (CAP-4): The organ provides percussion, key click, vibrato/chorus scanner and tonewheel leakage controls.
- FR10 (CAP-4, CAP-13): The rotary speaker is part of the organ voice path with slow / fast / stop and ramping, driven by the ROTARY STOP and S/F panel buttons and shown as `ROTARY SLOW/FAST` in the Status Bar and Organ Card.
- FR11 (CAP-4, CAP-18): Organ onset follows the timing of each key's successive contacts (multi-contact onset, reference: Hammond M-solo Virtual Multi-Contact); the organ starts sound on the first contact.

**FM and VA engines (CAP-5, CAP-6, CAP-12)**

- FR12 (CAP-5): A clean-room 6-operator FM engine sustains ≥128 voices inside the shared pool and reaches shipped v1 units by firmware update.
- FR13 (CAP-6): A clean-room virtual-analog engine sustains ≥64 voices inside the shared pool and reaches shipped v1 units by firmware update.
- FR14 (CAP-5, CAP-6, CAP-12): The engine interface, voice pool, patch (Live Set) format and UI support all four engines from day one, so FM and VA need no format or UI migration when they ship.

**Parts, layers and splits (CAP-7)**

- FR15 (CAP-7): A Live Set holds up to 8 parts (16 on the Pro tier), each assigned one timbre from any engine.
- FR16 (CAP-7): Each part has a key zone (low key, high key), octave/transpose and keyboard on/off, so parts combine as layers and splits with correct zoning.
- FR17 (CAP-7): The tier (v1 / Pro) is read at boot from the board ID strap or OTP and sets part count (8 / 16), head budget, voice/cost budgets and enabled modules; the same image runs on both tiers (Pro: two banks A/B toggled by a bank button, details deferred).

**User import (CAP-8, CAP-14)**

- FR18 (CAP-8): The user imports SFZ + WAV libraries within the v1 opcode subset (`<global>`, `<master>`, `<group>`, `<region>`, `<control>`; key/velocity ranges, `pitch_keycenter`, tune/transpose, volume/pan, offset; round robin `seq_length`/`seq_position`, `lorand`/`hirand`; loops; `ampeg_*`; release triggers; keyswitches `sw_*`; CC crossfades `xfin`/`xfout`; `#define`/`#include`, `default_path`; basic filters) with key, velocity, round-robin and loop mapping intact. The subset is extended in Epic 13 by tiers (importer value rules; note-on scalars; trigger and release modes; CC at voice start; choke groups and polyphony; pitch bend and CC-triggered voices; per-voice filter), and region EQ and LFO/EG modulation are conditional on measured need; the verdict per tier is in `_bmad-output/planning-artifacts/sfz-scope-decision.md` (proposed, awaiting founder ratification). A file that needs a tier marked out of scope is skipped with `unsupported_features` (FR22).
- FR19 (CAP-8): The user imports SF2 libraries into the same internal instrument model.
- FR20 (CAP-8): Every library in the reference corpus (Salamander Grand Piano, VSCO2, Sonatina Symphonic Orchestra, Karoryfer libraries, a Pianobook selection) imports and plays with mapping intact for every file in a supported tier (FR18 and Epic 13); a file that needs a tier marked out of scope is skipped with a stated reason and listed in the corpus manifest, and nothing else is.
- FR21 (CAP-8): Every imported instrument carries exactly one of the 17 categories; the category is auto-suggested from filename or metadata, the user confirms or changes it (per row or "Apply one category to all"), and an instrument without a category cannot import.
- FR22 (CAP-8): Files that use features outside the v1 subset (SFZ scripting, effect buses) or are damaged are skipped with a stated reason, and the rest of the batch imports.
- FR23 (CAP-8): A library already imported is flagged `already imported` with Skip (default) or Import again; Import again replaces it and existing Live Sets keep pointing at it (stable `library_id` / `instrument_key`).
- FR24 (CAP-8, CAP-9): User-imported SFZ/SF2 content is never encrypted.
- FR25 (CAP-14): Before an import starts, the unit checks free space and refuses to start when the batch does not fit, stating required vs free space.
- FR26 (CAP-14): Import runs in the background (EXIT sends it there) while the player keeps playing; progress shows "Importing n of m · name · converting & verifying (lossless)".
- FR27 (CAP-14, CAP-17): Imports are atomic: a library becomes visible only after it is fully written and verified by a bit-exact decode round-trip; if the drive is removed, finished timbres are kept and the timbre in progress is discarded whole.
- FR28 (CAP-14): A 300 GB SFZ library on an external SSD imports through USB-A (drive mounted read-only, parsed only by the sandboxed librarian) and becomes playable.
- FR29 (CAP-14): Inserting a USB drive raises a non-modal Notice Toast "USB drive found · Import?" (or "Update n found · Open?" when the drive holds a firmware bundle); nothing opens automatically.

**Protected factory content (CAP-9)**

- FR30 (CAP-9): Factory and partner libraries are encrypted per block with AES-CTR under a per-library content key, which is wrapped to a per-device key held in RK3588 OTP / OP-TEE; a copied library does not play on another unit.
- FR31 (CAP-9): Every protected bundle carries an Ed25519-signed manifest and per-block hashes; a tampered file is rejected at install and at load.
- FR32 (CAP-9): Content keys are unwrapped only into engine memory while a library is loaded and are never written to disk; plaintext vendor content never exists on the unit's storage or on USB.
- FR33 (CAP-9): Secure boot prevents a custom OS from booting on the unit to read keys.
- FR34 (CAP-9, GTM): The founder-supplied factory library (including the factory piano and the organ presets used in the v1 success signal) is packed and signed off-device by the content-pack tool, carries vendor-assigned library and program IDs, and is pre-installed at the factory already wrapped for each unit.

**USB audio / MIDI interface (CAP-10)**

- FR35 (CAP-10): Over USB-C the unit enumerates without extra drivers on macOS, Windows and Linux as a class-compliant UAC2 audio device (async with feedback endpoint, following the codec clock), and a DAW records its outputs.
- FR36 (CAP-10): Over USB-C the unit enumerates as a class-compliant USB MIDI 2.0 / 1.0 device.
- FR37 (CAP-10, CAP-14): The USB-C gadget composition is fixed to UAC2 + MIDI + NCM (network for Velora Studio); it never exposes mass storage or a serial console.

**Stage I/O (CAP-11)**

- FR38 (CAP-11): 4 balanced ¼" TRS outputs (2 main, 2 aux) work, with per-part assignment to Main or Aux.
- FR39 (CAP-11): A headphone output with its own amplifier carries the headphone feed.
- FR40 (CAP-11): A stereo line input can be mixed into the master only, with a level control in Effects › Master.
- FR41 (CAP-11): MIDI DIN in and out work (in opto-isolated, 31,250 baud).
- FR42 (CAP-11): Pedal inputs work: continuous sustain with half-pedal calibration, expression pedal, and one switch; pedal polarity is auto-detected at boot.
- FR43 (CAP-11): Pitch and modulation wheels control the parts.
- FR44 (CAP-11, AD-17): Hardware mute is held from power-on until the software releases it, so there is no pop at power-on or power-off.

**Firmware updates (CAP-12, CAP-17)**

- FR45 (CAP-12): A signed firmware (RAUC) update that adds a new engine installs on a v1 unit, and the engine runs inside the shared voice pool.
- FR46 (CAP-12): Firmware bundles arrive only from Velora Studio or a USB-A stick, and only `sysd` installs them; library updates are never part of a firmware bundle.
- FR47 (CAP-12): The install flow states that sound stops until restart (Action Sheet "Install update? …" with `WRITE · Install` / `EXIT · Later`), shows `UPDATING TO n` with progress, then "Updated to n" on success, "Update failed: the file is damaged or not signed. Still on n" on failure, and "Update did not finish…" after a rollback.
- FR48 (CAP-12): Keybed and panel MCU firmware ship inside the A/B image and are reflashed at boot only on a version mismatch, before the engine starts.
- FR49 (CAP-12): Every release opens every Live Set saved by any earlier release; readers accept format versions N and N−1 and preserve unknown fields.

**Effects (CAP-13)**

- FR50 (CAP-13): Built-in effects are available: rotary speaker, reverb, delay, chorus/phaser, EP amp/drive, EQ and compressor.
- FR51 (CAP-13): Each part has an ordered insert chain (rotary speaker, chorus/phaser, EP amp/drive, EQ, compressor) where the user adds, removes, reorders and bypasses effects.
- FR52 (CAP-13): Two global sends exist: Reverb (type, time, level) and Delay (time, tempo sync, feedback, level), with per-part send levels; the REV fader in MIXER drives the reverb send.
- FR53 (CAP-13): A master section provides master EQ and compressor.

**Companion transfer (CAP-14)**

- FR54 (CAP-14): Velora Studio performs heavy SFZ conversion on the computer and transfers a library to the unit over the mediated ws.v1 protocol on USB NCM; the unit keeps playing, the library activates only when complete ("<name> ready in Library"), and a failed transfer installs nothing.
- FR55 (CAP-14): The internal NVMe is never exposed as mass storage on any path.

**Library browsing (CAP-15)**

- FR56 (CAP-15): The Library finds timbres by search (On-screen Keyboard), category chips (17), ★ Favorites and Recent, in a 3-column result grid.
- FR57 (CAP-15): With 1,000+ instruments, a favorite is reachable within 2 interactions from the Library and any instrument within 3 interactions through search or categories.
- FR58 (CAP-15): Every timbre name in the UI shows its category (short label ≤10 characters on Part Strips, full label elsewhere), never its engine type.
- FR59 (CAP-15): While browsing the Library, playing the keys auditions the highlighted timbre; WRITE assigns it to the target part ("CHOOSE TIMBRE FOR PART n").
- FR60 (CAP-15): Long press on a timbre Result Tile (or ★ on the Part surface) adds it to or removes it from Favorites.
- FR61 (CAP-15): Empty states read "No favorites yet. Long-press a timbre to add one." / "Nothing played yet." / "No matches for 'x'. Try another word or a category."

**Boot (CAP-16)**

- FR62 (CAP-16): A Settings option "Boot into" selects what is playable after power-on: Last sound used (default), Start of setlist, or Default Live Set.
- FR63 (CAP-16): The Boot-into target is playable ≤10 s after power-on with a 1 TB library: its heads load first, then favorites, and the rest indexes in the background.
- FR64 (CAP-16): The Boot-into pointer (last Live Set and scene) is written atomically to the journaled partition on a Live Set or scene change, deferred to an idle-safe moment.
- FR65 (CAP-16): The Boot Screen says what is being restored ("RESTORING · {Live Set} · SCENE n"); if the target or its library is missing, boot ends on Performance with the target loaded and Missing library shown on affected parts.

**Power-loss safety (CAP-17)**

- FR66 (CAP-17): Firmware uses A/B slots with rollback on failed boot: a power cut at any point of an update boots the previous or the new firmware.
- FR67 (CAP-17): A power cut during import never leaves a partially imported library; a startup reconcile pass removes orphan bundles and catalog rows.
- FR68 (CAP-17): On the POWER_FAIL signal the engine fades outputs to silence and `sysd` freezes writes and flushes the NVMe within the hold-up window.
- FR69 (CAP-17): Irreversible data migrations run only after the new firmware slot is marked good.

**Keybed expression (CAP-18)**

- FR70 (CAP-18): Attack velocity is derived on the SoC from raw per-contact times, with the velocity curve applied exactly once; the user selects the velocity curve in Settings › Keyboard & Pedals, and curves cover the full MIDI range evenly.
- FR71 (CAP-18): Release velocity is measured and changes release samples (release triggers).
- FR72 (CAP-18): Aftertouch is read and routed when the keybed has it, with a Settings row shown only on keybeds with aftertouch.
- FR73 (CAP-18): The keybed model is a calibration table on the SoC (2 or 3 contacts per key handled), so a different Fatar model needs no code change.

**Arpeggiator and clock (CAP-19)**

- FR74 (CAP-19): Any part can run an arpeggiator with Arp On/Off, Mode (Up, Down, Up-Down, Random, As played), Rate (1/4–1/32 incl. triplets), Octaves 1–4, Gate %, Swing %, Latch.
- FR75 (CAP-19): Arpeggiated notes land sample-accurately on the tempo grid of the single RT tempo clock (Live Set tempo when Clock source is Internal).
- FR76 (CAP-19): Clock source (Internal / DIN / USB) is a system setting; when following external clock, tempo is read-only with the source shown (`♩ 72 · DIN`), and jitter stays ≤1 ms against the incoming clock.
- FR77 (CAP-19): On external clock loss the arpeggiator keeps running at the last received tempo and the UI shows `⚠ CLOCK LOST` until clock returns.
- FR78 (CAP-19, CAP-21): The Live Set holds initial arpeggiator values; each scene stores Arp on/off and all arpeggiator parameters (tempo stays the Live Set's).

**Seamless switching (CAP-20)**

- FR79 (CAP-20): Switching scene, Live Set or setlist never cuts held, pedal-sustained or releasing notes (voices finish on their original generation).
- FR80 (CAP-20): The next Live Set of the loaded setlist is preloaded (heads resident, `● READY`) so an in-setlist switch is immediate.
- FR81 (CAP-20): A jump to a non-preloaded Live Set keeps the current sound playing until the target is ready, showing `Loading <name>… Ns`, then swaps.
- FR82 (CAP-20): Loading a setlist never stops the current sound.

**Setlists, Live Sets, scenes (CAP-21)**

- FR83 (CAP-21): Content is organised as Setlist → Live Set (one per song) → 8 Parts + up to 8 Scenes; the Live Set holds each part's timbre, and recalling a scene never changes a part's timbre.
- FR84 (CAP-21): On the device the user creates, chooses, renames and reorders setlists and their Live Sets, creates a new Live Set (8 empty parts, one scene), and creates/edits user setlist tags.
- FR85 (CAP-21): The Setlist Browser searches Live Sets by name across all setlists, filters by chips (★ Favorites, Recent, Setlists, All Live Sets, then user tags), loads a setlist or jumps to a Live Set with WRITE, and adds/removes Live Set favorites by long press.
- FR86 (CAP-21): After an off-script jump the setlist stays loaded and LIVE SET ◀ ▶ returns to it; with no setlist loaded LIVE SET ◀ ▶ steps through All Live Sets; at the end of a setlist NEXT reads "End of setlist" and LIVE SET ▶ does nothing.
- FR87 (CAP-21): The Scenes surface creates (copying the current scene into the next empty slot), renames, duplicates, moves and clears scenes; scene order drives SCENE ◀ ▶, scene buttons 1–8 and the footswitch.
- FR88 (CAP-21): A scene stores all Live Set parameters except timbre assignment (part volumes, keyboard on/off per part, FX, drawbars, arpeggiator state and parameters).
- FR89 (CAP-21): Holding WRITE (Write hold, default 0.5 s, 0.3–1.0 s) opens the Write Dialog listing what changed with scope Scene / Whole Live Set / Save as new…; Scene scope is unavailable when edits include a timbre change; Save as new suggests a free name; with no edits the Hint Bar says "Nothing to write".
- FR90 (CAP-21): Edits stay in memory across scene and Live Set changes until Write or power-off, marked by the ◆ Unsaved Indicator on the Live Set Hero, List Rows and Result Tiles; Write writes the current Live Set only; no confirmation dialog interrupts performance.
- FR91 (CAP-21): When storage is full, Write fails with "Not enough space to write…" and edits stay in memory with ◆.
- FR92 (CAP-21): Locally created Live Sets get UUIDv4 IDs; an imported/restored Live Set whose ID collides gets a new one.

**Backup, restore and Velora Studio (CAP-22)**

- FR93 (CAP-22): Velora Studio writes a versioned `.velorabackup` file of setlists and Live Sets to a user-chosen location and shows the last backup time.
- FR94 (CAP-22): Restore asks Merge (add and update, keep what the backup lacks) or Replace (device content becomes the backup), applied on the device only through control-thread commands; both produce the stated result.
- FR95 (CAP-22): With no unit connected, Velora Studio opens and edits backups and the computer-side library; live edit, transfer, firmware and Write to Velora wait for a device.
- FR96 (CAP-22, AD-13): Pairing starts on the device (Settings › Computer › Pair new computer) and shows a 6-digit code that the user types into Velora Studio; wrong codes are rejected; paired computers can be listed and forgotten.
- FR97 (CAP-22): Velora Studio shows the selected Live Set with its 8 scenes, 8 parts and a Part Editor with all parameters of the selected part at once (sound, envelope, EQ, inserts/sends, zone/keyboard, arpeggiator, ASSIGN map), plus Setlists, Library management and Device views.
- FR98 (CAP-22): Velora Studio edits play live on the unit (Status Bar `STUDIO`), mark ◆ like local edits, and persist only with "Write to Velora" using the device's scopes; success clears ◆ on both.
- FR99 (CAP-22): Velora Studio handles version mismatch (hides unsupported features, banner to update), disconnect mid-edit (edits stay on the unit with ◆; reload on reconnect), stale edits (rejected by `base_revision`, "Reload" banner) and write failure (banner, edits kept on the unit).
- FR100 (CAP-22, CAP-12): Velora Studio sends a firmware bundle to the unit with transfer progress, then the install proceeds per FR47.

**ASSIGN (CAP-23)**

- FR101 (CAP-23): In ASSIGN mode each of the 8 encoders controls its mapped parameter in the current Live Set; turning one shows the K1–K8 names in the ASSIGN Overlay for 3 s (restarting on each turn); sound is never blocked.
- FR102 (CAP-23): A new Live Set copies the global default ASSIGN map, which the user edits in Settings › Knobs · Global Assign.
- FR103 (CAP-23): The per-Live-Set ASSIGN map is editable on the device and in Velora Studio and survives Write and a power cycle.

**Panel control behaviour (Constraints, hardware-platform, CAP-2, CAP-18, CAP-23)**

- FR104 (Constraints): The DRAWBARS/MIXER toggle switches the 9 faders between the organ part's drawbars and part 1–8 volumes + REV send; with no organ in the Live Set, DRAWBARS does not change mode, its LED blinks twice and the screen says "No organ in this Live Set"; with two organs, the faders control the organ selected by long press of its part button.
- FR105 (Constraints): After any scene, Live Set or mode change, a non-motorized fader changes nothing until it crosses the stored value (pickup), with on-screen guidance (Pickup Ghost, Status Bar takeover `▲ FADER 3 → 65%`).
- FR106 (Constraints): The PARTS/SCENES toggle sets the 8 buttons to parts (short press part on/off, long press selects knob target) or scenes (recall 1–8; LED current solid strong / existing weak / empty off, 200 ms dip on recall).
- FR107 (Constraints): The PART/ALL/ASSIGN toggle cycles the 8 encoders between the selected part, all parts (relative) and the ASSIGN map; in PART/ALL the encoders keep fixed functions DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH; LED rings show the target value; encoders never need pickup.
- FR108 (Constraints): Navigation buttons follow the canonical rules: LIVE SET ◀ ▶ and SCENE ◀ ▶ step seamlessly from any surface; EXIT goes one level back; MENU toggles the Launcher; VALUE turns to scroll/edit and, on Performance, pushes to open the knob-target part's Part surface; WRITE confirms or (held) writes.
- FR109 (Constraints): The footswitch advances the scene by default and is configurable to previous scene, next Live Set or rotary speed.
- FR110 (Constraints): Accidental-press guards are configurable: Panel guard (Off / On, On = LIVE SET ◀ ▶ act only after a 300 ms hold), Long press threshold (400 / 500 / 800 ms), Write hold (0.3–1.0 s), Touch on Performance (Full / Safe / Off, default Safe).
- FR111 (Constraints): All panel LEDs are driven by the SoC; every toggle shows one LED per state, mirrored in the Top Bar.

**Device UI surfaces and settings (Constraints, CAP-7, CAP-13, CAP-15, CAP-21)**

- FR112 (CAP-7, CAP-13): The Part surface edits one part's timbre (Change timbre → Library), engine sound parameters, envelope and EQ, zone and keyboard, level and output (Main/Aux), insert chain and sends, and arpeggiator.
- FR113 (CAP-13): The Effects surface edits part inserts, sends, master and outputs.
- FR114 (CAP-1, CAP-8): A part whose library or instrument does not resolve plays silent, shows "Missing library" (never substituted), and offers Replace timbre or Import via an Action Sheet.
- FR115 (Constraints): Settings provides Display & Language (theme Stage/Studio, language, screen brightness 5–100%, panel LED brightness Low/Mid/High/Follow screen, Large text on Performance, Reduced motion, Touch on Performance, Write hold, Long press, Panel guard, Boot into), Keyboard & Pedals, MIDI & Clock, Knobs · Global Assign, Computer (Velora Studio), Storage & Library (free space, installed libraries), System & Update.
- FR116 (Constraints): Screen brightness can be changed on any surface by holding EXIT and turning VALUE, shown in a Notice Toast.
- FR117 (Constraints): The UI language is selectable among English (default), PT-BR and ES; more locales arrive by system update; user content (setlist, Live Set, scene, tag names) is never translated; panel legends stay English.
- FR118 (Constraints): The device offers two themes, Stage (dark) and Studio (light), selected manually (no light sensor); Velora Studio follows the OS or a user setting.
- FR119 (Storage, AD-11): From Settings › Storage & Library the user sees free space and installed libraries and can remove a library; a bundle is deleted only when no live generation holds a lease on it.

**Diagnostics (AD-29)**

- FR120 (AD-29): The unit keeps xrun, steal and stream-underrun counters and a latency histogram in RAM and exports them through Velora Studio on request.

**FM SysEx import (CAP-5)**

- FR121 (CAP-5): The 6-operator FM engine is parameter-compatible with the classic 6-operator FM SysEx format (32 algorithms, rate/level envelopes, keyboard scaling, operator output levels, LFO, pitch envelope) and imports user SysEx banks — 32-voice bulk dumps and single-voice dumps — from files on USB-A, from Velora Studio, and as SysEx received over USB MIDI/DIN; each imported voice becomes a timbre in the Library under a user-chosen category (default Synth Poly), labelled generically ('Import 6-OP SysEx'); no third-party patches ship.

**Partial backup and restore (CAP-22)**

- FR122 (CAP-22, AD-31): Velora Studio exports and imports partial content: individual or selected Live Sets and Live Set bundles (the selected Live Sets plus the timbres they use, to move a repertoire to another Velora) as a versioned `.velorasets` file, and individual or selected timbres as a versioned `.veloratimbre` file, each with a manifest declaring its scope; full backups stay `.velorabackup`. User-imported (SFZ/SF2) timbres are exported with their audio as lossless formatlib bundle data; factory and partner timbres are never exported (encrypted per device, AD-14) and travel only as `library_id`/`instrument_key` references, so on import a part whose library is not installed shows "Missing library". Studio shows a size estimate before export and warns when a bundle with large user timbres is big. Import merges per item on a collision: Skip, Replace, or Keep both (the copy gets a new ID, AD-10). Export reads published state and `librarian` commands; import reaches the device only as control-thread and `librarian` commands, never as direct file writes.

### NonFunctional Requirements

**Performance**

- NFR1 (CAP-2): Key-press-to-audio-output latency ≤5 ms under full polyphony load (64-frame buffer @ 48 kHz; keybed scan + transport <1 ms of the budget; keybed MCU contact-to-frame ≤200 µs).
- NFR2 (CAP-1): ≥256 simultaneous stereo sample voices streaming from NVMe — a floor, not a ceiling.
- NFR3 (CAP-1): Libraries up to 1 TB indexed on NVMe; RAM 16 GB (v1) / 32 GB (Pro) with ~4–6 GB reserved for heads in v1.
- NFR4 (CAP-1): Zero dropouts (xruns) in a 24 h stress run at 256 voices with the SSD near full; heads must cover the qualified SSD's measured p99.99 read latency plus margin.
- NFR5 (CAP-16): Boot-into target playable ≤10 s after power-on with a 1 TB library, for each of the 3 Boot-into options.
- NFR6 (CAP-13): All effects on an 8-part Live Set at full polyphony do not break NFR1 or NFR4.
- NFR7 (CAP-19): Arpeggiator sample-accurate on the internal grid; ≤1 ms jitter following external MIDI clock (DIN or USB).
- NFR8 (CAP-5, CAP-6): FM ≥128 voices and VA ≥64 voices sustained within the shared pool.
- NFR9 (CAP-15): Any favorite ≤2 interactions from the Library; any instrument ≤3 interactions via search or categories, with 1,000+ instruments.
- NFR10 (AD-5): Engine runs internally at 48 kHz; 3 Cortex-A76 cores isolated for audio RT (`isolcpus`, `nohz_full`, fixed governor).
- NFR11 (HW-§2): 24 h at full audio load without thermal throttling in a closed chassis at 40 °C ambient.

**Reliability and data integrity**

- NFR12 (CAP-17): 100 random power cuts during firmware update and 100 during library import: the unit boots every time (previous or new firmware) and holds no partially imported library.
- NFR13 (CAP-1, CAP-9): Lossless: sample data is never lossy-compressed; decoded output is bit-identical to source; import never resamples or requantizes.
- NFR14 (CAP-20): Automated renders of scene, in-setlist Live Set, setlist and non-preloaded Live Set switches during held, sustained and releasing notes show no audible gap or truncation.
- NFR15 (AD-18): During a performance the disk only reads (logs/metrics RAM-buffered, SQLite checkpoints and fstrim at idle); writes occur only on Write, import, update and the Boot-into pointer.
- NFR16 (Constraints, AD-4): A UI crash never interrupts sound (UI isolated from the engine process); no UI action, load or import cuts audio except an explicitly started firmware install.
- NFR17 (AD-3): Audio RT threads never allocate, lock, block, do disk I/O or take page faults; no swap, no core dumps.
- NFR18 (HW-§12): Power hold-up ≥150 ms after input loss; POWER_FAIL asserted ≥100 ms before rails drop.

**Sound fidelity (acceptance-tests.md)**

- NFR19 (CAP-1, CAP-8): Sample null test vs the reference renderer (sfizz) with residual below a defined dB tolerance, automated.
- NFR20 (CAP-4): Organ objective A/B vs Hammond M-solo renders (spectrum, key click, percussion decay, leakage) within reference-derived tolerances.
- NFR21 (CAP-4): Organ subjective blind ABX with 3–5 professional keyboardists: not reliably distinguishable, or mean rating ≥4/5.
- NFR22 (CAP-5): FM null test vs an unshipped reference oracle with residual below tolerance.

**Security and content protection**

- NFR23 (CAP-9, AD-14, AD-25): RK3588 secure boot fused at the factory; per-device key generated in OP-TEE; keys never on disk; dev / factory-test / production signing roots separate, production root offline in an HSM; OTP anti-rollback counter bumped only on security releases.
- NFR24 (Constraints, AD-13): Production firmware exposes no serial console, no SSH, no mass storage; JTAG/SWD disabled on production SoC and MCUs; nftables admits only the one ws.v1 port on USB NCM; USB-A drives mounted read-only `nosuid,nodev,noexec` and parsed only in the sandboxed librarian.
- NFR25 (Non-goals): Protection deters extraction but does not claim to be unbreakable; user imports are never encrypted.
- NFR26 (Constraints): The NVMe is sealed inside the unit and not user-accessible or replaceable.

**Licensing and IP**

- NFR27 (Constraints, AD-15): No GPL-3.0 or LGPL-3.0 code in the image (`INCOMPATIBLE_LICENSE`); GPLv2 kernel/BusyBox and GCC-exception runtimes allowed; flac/metaflac CLIs and libsndfile excluded; license manifest per build.
- NFR28 (Constraints, engines.md): Engines are clean-room or licensed; GPL software (Dexed/MSFA, setBfree) only as unshipped test oracles; permissive code (sfizz BSD-2, libFLAC BSD) allowed.
- NFR29 (Constraints): No third-party trademarks in marketing, UI or panel ("rotary speaker" / ROTARY, never "Leslie"; no "Hammond", no "DX7").

**Localization**

- NFR30 (Constraints, AD-20): English is the source and default locale; PT-BR and ES ship in v1; further locales ship by system update without a new image; no hard-coded user-facing strings; panel legends English only.

**Accessibility and stage usability (DESIGN.md, EXPERIENCE.md)**

- NFR31: Text contrast ≥4.5:1 and meaningful non-text boundaries ≥3:1 in both themes, in Velora Studio and on the darkest aluminum stop; no opacity on text.
- NFR32: Device touch targets ≥56 px, ≥12 px apart on Performance, no nested targets.
- NFR33: Stage legibility: Live Set name ≥10 arcmin at 1.5 m (≥43 px); pending-pickup takeover ≥12 arcmin at 0.7 m (≥24 px).
- NFR34: Never color-only state; reduced-motion option on device and OS-driven in Studio; all blinking ≤3 Hz; UI transitions ≤150 ms; nothing animates in the Live Set Hero.
- NFR35: Velora Studio is fully keyboard operable, screen-reader labelled (ARIA sliders, live regions), honours OS text size to 200% with reflow and no horizontal scroll, minimum window 1280×800.
- NFR36 (HW-§8, DESIGN Hardware asks): Display ≥1000 cd/m² with optically bonded AG/AR glass (≤2% reflectance), dimmable to ≤5 cd/m²; panel LEDs ≥10:1 on:off at 50 klx; legend cap height ≥2.5 mm.

**Platform and hardware constraints**

- NFR37 (Constraints): Engine is a 64-bit userspace process on mainline Linux with PREEMPT_RT; no RTAI or out-of-tree RT patches.
- NFR38 (Constraints): Rockchip RK3588 single SoC for v1 (16 GB) and Pro (32 GB); production on an industrial SoM with a written 7–10 year longevity commitment; CM5 / Intel N-series fallback only if Milestone-1 RT/display validation fails.
- NFR39 (Constraints, AD-27): One software base for v1 and Pro; the sequencer is a module, not a fork.
- NFR40 (Constraints, AD-17): Main audio leaves over I2S/TDM to the PCM3168A codec as clock master on its own 24.576 MHz crystal; USB is never in the main output path; the engine never runs on the USB clock.
- NFR41 (Constraints): v1 has no radio (no Wi-Fi, no Bluetooth) — avoids Anatel homologation.
- NFR42 (Constraints): Fatar 73SW keybed from the partner's ~100-unit stock; scan ≥10 kHz; scan + transport fits within the 5 ms budget.
- NFR43 (Constraints): 7-inch capacitive touchscreen, native 1024×600 landscape; fixed panel control set per hardware-platform.md; v1 faders non-motorized.
- NFR44 (AD-12): Every persisted and wire format is versioned; readers accept N and N−1; `ws.v1` grows only by addition.

**Certification and compliance**

- NFR45 (GTM): Off-the-shelf power supply compliant with Inmetro (exact requirement to confirm).
- NFR46 (GTM, HW-§12): Electrical safety and EMC designed and tested to IEC 62368-1 to ease later CE/FCC.
- NFR47 (Survey, LGPD): Any personal data collected (survey e-mails, waitlist) is collected with explicit LGPD consent and used only for the stated purpose.

**Business constraints**

- NFR48 (Constraints, GTM): v1 retail price R$ 17,990–19,990; BOM (est. US$ 1,200–1,600) and tax structure must leave margin at that price (landed cost est. R$ 11,000, unverified); Pro R$ 24,990+.
- NFR49 (Constraints): Scope must fit one developer (founder + AI build all software; partners cover only the physical build); first units may be hand-built but quality must scale.
- NFR50 (GTM): Timeline from work start: Milestone 1 investor demo at month 6 (dev board: 256 voices, 1 TB, ≤5 ms, 0 dropouts / 24 h); first playable units (samples, organ, effects on prototype hardware) at month 12; v1 launch at month 18 (~100 units, direct sales).
- NFR51 (Constraints): Release is date-driven: v1 requires sample engine, organ and effects; FM and VA may follow by firmware.
- NFR52 (Non-goals): v1 excludes: Kronos compatibility, sequencer, >8 parts, Bluetooth/Wi-Fi, user-accessible NVMe, x86_64 product, partner library store, retail distribution for the first ~100 units, SFZ scripting/effect buses, motorized faders, sampling.

### Additional Requirements

**Starter template:** none. The architecture specifies a greenfield build with no starter template: a custom Yocto 6.0 "Wrynose" LTS distribution (systemd, meta-rauc), Linux 6.18 LTS PREEMPT_RT mainline, C++20 services (`engine`, `librarian`, `sysd`, `formatlib`), an LVGL 9.6 DRM/KMS UI, and a Tauri 2.12 desktop app. Story 1.3 sets up the monorepo (after the Story 1.1 and 1.2 go/no-go spikes, whose files it keeps under `tools/spikes/`) per the Structural Seed (`proto/`, `engine/core|ports|host/`, `formatlib/`, `librarian/`, `sysd/`, `ui/`, `desktop/`, `mcu/keybed|panel|bootloader/`, `tools/offline-render/`, `tools/content-pack/`, `yocto/`) and the host build/CI.

**Build, CI and release**

- AR1 (Structural Seed, AD-19): Host build (macOS/Linux) of `engine/core`, offline renderer and desktop app; `engine/core` has no Linux/ALSA/OP-TEE/hardware dependency.
- AR2 (AD-29): CI pipeline with release gates: host unit tests, null tests, RealtimeSanitizer on host builds, Yocto build with license manifest, RAUC signing per root, 24 h soak on the dev board with zero xruns.
- AR3 (Structural Seed): Yocto layers and three image variants: dev (dev-signed, SSH + debug UART), factory-test (signed, port/MCU/keybed/codec-loopback/display/touch/NVMe tests), production (production-signed, secure boot, no SSH/console).
- AR4 (AD-15): `INCOMPATIBLE_LICENSE` policy for GPL-3/LGPL-3, flac CLIs and libsndfile excluded, license manifest reviewed per release.
- AR5 (Deferred): Kernel LTS migration — 6.18 EOL ≈ Dec 2027; start 6 months before EOL with a full RT soak.

**Milestone-1 spikes (Open Questions)**

- AR6 (Open Q, AD-5, CAP-2): PREEMPT_RT latency spike on RK3588 under streaming + render load (cyclictest/rtla); CM5/Intel fallback decision if it fails.
- AR7 (hardware-platform, HW-§8): MIPI DSI 7" panel bring-up on mainline RK3588; fallback to internal HDMI panel.
- AR8 (Open Q, AD-14, AD-25): Secure-boot chain spike — barebox (`rksecure`) vs U-Boot + Rockchip tools, TF-A source, OP-TEE hardware-unique-key; exit: a fused dev board boots a signed image and derives a per-device key.
- AR9 (Open Q, AD-17, CAP-10): Hardware bring-up — USB peripheral role in DT, I2S MCLK gating by boot firmware, PCM3168A in TDM, UAC2 + MIDI 2.0 + NCM composite on macOS/Windows/Linux.

**Process topology and core runtime**

- AR10 (AD-4): Four systemd services `engine`, `ui`, `librarian`, `sysd`; `engine` sole runtime owner of audio device, MCU links, MIDI DIN, USB MIDI and power-fail GPIO (with sysd); `librarian` sandboxed, low priority, off audio cores; `ui` client only (PanelEvent in, LED commands out).
- AR11 (AD-1): Engine control thread as single writer of structural state; immutable snapshot generations swapped by atomic pointer.
- AR12 (AD-2): Generated parameter registry in `proto/` classifying every `param_id` as structural or performance; RT-owned last-value table; Save waits on a control→RT fence.
- AR13 (AD-3): RT-safety discipline: lock-free SPSC queues, atomic swaps, mlocked pre-faulted memory, only syscall = ALSA period wait; RTSan in CI.
- AR14 (AD-5, AD-6): Block phase scheduler (serial events/arbiter → parallel shared generators → parallel voice render → barrier → sum/FX/sends/master) on 3 isolated A76 cores; voice contract (writes only own state/scratch).
- AR15 (AD-7): Single static engine interface with per-voice cost units and steal metadata; engines compiled in, no loadable plugins.
- AR16 (AD-8): MIDI 2.0 UMP as the only engine event format; device time CLOCK_MONOTONIC_RAW ns; MCU tick→device-time offset/drift tracking (TIME_SYNC); DLL mapping device time to frames on the ALSA hw pointer; keybed reader emits per-note `contact_k` controllers then Note On.
- AR17 (AD-21): Event-processor framework on the RT plane (arpeggiator now, Pro sequencer later); one tempo/transport clock; clock source config owned by sysd.
- AR18 (AD-22): Voice pool arbiter: reservation order FX budget → organ + rotary → engine floors → shared; sole allocator/stealer.
- AR19 (AD-23): Generation lifecycle: snapshot + heads + content keys; swap only when heads resident; free only when no live voice references it; release BundleLeases then.
- AR20 (AD-24): Streaming subsystem: io_uring + O_DIRECT reads on I/O edge threads, decode + block-hash + AES-CTR on edge threads, head sizing from SSD p99.99, tier head budget, boot loads Boot-into heads first then favorites.
- AR21 (AD-30): Preload of next in-setlist Live Set within head budget; non-preloaded jump keeps old generation until ready; scene recall = parameter-only snapshot swap.
- AR22 (AD-28): Fixed routing topology: per-part insert chain, 2 global sends, master FX, per-part Main/Aux; line-in to master only.
- AR23 (AD-17): ALSA machine driver for RK3588 I2S/TDM + PCM3168A; codec clock master; UAC2 gadget async with feedback endpoint.
- AR24 (AD-27): Tier profile read from board ID / OTP at boot.

**Data, formats and IDs**

- AR25 (AD-13): Protocol Buffers `ws.v1` schema over UDS (and framed TCP over USB NCM for desktop, bridged by sysd); version/feature handshake; `request_id=(client_id,seq)`, deltas `(epoch,seq)`, edits `base_revision`; closed error-code enum; full snapshot on connect then deltas.
- AR26 (AD-9): One writer per dataset: librarian (bundles, SQLite WAL catalog incl. favorites/categories/tags), engine control thread (setlists, Live Sets, scenes, ASSIGN maps, setlist tags), sysd (system config protobuf), OP-TEE (keys).
- AR27 (AD-10, AD-11): `formatlib`: ID minting (`library_id` UUIDv5 / vendor, `bundle_id` manifest hash, `instrument_key`), block-indexed container read/write, lossless codec (libFLAC), crypto (decrypt excluded in desktop build), vendored sfizz SFZ parser, SF2 reader.
- AR28 (AD-11): Librarian import pipeline: stage → bit-exact round-trip verify → fsync → rename → flip `current` → catalog row; startup reconcile; immutable published bundles; lease-aware GC.
- AR29 (AD-12, AD-31): Versioned persisted forms (bundle `format_version`, Live Sets, config, catalog schema, ws.v1, MCU protocol, `.velorabackup`, `.velorasets`, `.veloratimbre`); N/N−1 readers; migrations only after RAUC marks slot good; compatibility test corpus of every prior release's saved Live Sets.
- AR30 (AD-31): Live Set data model (8 parts, ≤8 scenes, ASSIGN map), global default ASSIGN map and setlist tags as control-thread state; backup reads published state, restore via control-thread commands; partial export/import (Live Set selections, Live Set bundles, timbre selections) routes Live Sets through the control thread and user timbres through `librarian`, and factory/partner timbres travel only as references (AD-14).

**Content pipeline, security and provisioning**

- AR31 (AD-14): `tools/content-pack` offline tool: AES-CTR per block, per-library content key, Ed25519-signed manifest + block hashes, vendor IDs; used for the founder's factory library.
- AR32 (AD-14): OP-TEE trusted application for device-key generation and content-key wrap/unwrap into engine memory.
- AR33 (AD-25): Factory provisioning station: fuse secure boot, OP-TEE device key generation, register device ID + public key in the vendor device registry (from unit #1), pre-install factory library wrapped per device, link serial ↔ SoM ID; SoM swap re-provisioning flow.
- AR34 (AD-25): Device registry service and signing-key custody (separate dev / factory-test / production roots, production root in offline HSM).
- AR35 (AD-25): OTP anti-rollback counter process for security releases.

**Updates, power and system services**

- AR36 (AD-18, AD-26): RAUC A/B with rollback, read-only rootfs, journaled data partition; sysd as sole installer from Velora Studio or USB-A stick.
- AR37 (AD-18): Power-fail handling: engine edge thread fades on the RT plane; sysd freezes writes and flushes NVMe; hardware mute control.
- AR38 (AD-13, AD-4): sysd USB gadget (UAC2 + MIDI + NCM), nftables single-port policy, ws.v1 bridge, pairing-code authentication, paired-computer store.
- AR39 (AD-9): sysd config service (velocity curves, calibration, MIDI, clock source, touch/hold/guard/brightness settings) with `schema_version`.

**MCU firmware**

- AR40 (AD-16, HW-§3–6): Keybed MCU firmware: keys only, ≥10 kHz scan, ≥1 MHz tick per contact change, raw contact times, aftertouch if present, ≤200 µs to wire.
- AR41 (AD-16, HW-§4–5): Panel MCU firmware: encoders (relative), VALUE push as BUTTON, faders/wheels/pedals 12-bit (MCU-ID-MAP; ≥10 bits minimum), debounced buttons, SoC-driven LEDs, pedal polarity detection.
- AR42 (HW-§5): Versioned frame protocol (sync 0xA5, version, type, len, seq, tick, payload, CRC-16/CCITT; HELLO, KEY_CONTACT, AFTERTOUCH, CONTROL, BUTTON, LED_SET, TIME_SYNC, ENTER_BOOTLOADER); software team publishes the control and LED ID maps.
- AR43 (AD-16, HW-§6): Immutable dual-bank bootloaders on both MCUs; sysd reflashes over the link (or BOOT/RESET GPIO) at boot on version mismatch only; no external field flashing.

**Observability and ops**

- AR44 (AD-29): RAM counters (xruns, steals, underruns, latency histogram) exported via desktop app on request; structured journald logging RAM-buffered during performance, RT records drained by control thread.
- AR45 (AD-20): Locale-catalog infrastructure shared by `ui` and `desktop`, English source, updatable by system update.

**Desktop**

- AR46 (Structural Seed, AD-13, AD-14): Tauri desktop app with FFI to the no-decrypt formatlib build, ws.v1 client over TCP on USB NCM.

**Deferred (track, not v1 scope)**

- AR47 (Deferred): Partner licensing/activation server (post-launch); Pro sequencer internals; sampling / line-in into engines; lossless float codec (when float >20% of library); multi-library dedup and quotas (when >500 GB test libraries); MCU family chosen by partners by month 7.

### External Dependencies & Business Tasks

Non-software work that gates or informs epics. Each item has an owner outside the software build or is a decision input.

- EXT1 (SPEC Open Q, NFR48): Tax advisor validation of margin at R$ 17,990–19,990 (II, IPI, ICMS, PIS/COFINS, ex-tarifário on imported parts) against the BOM and landed-cost estimates.
- EXT2 (SPEC Open Q, HW-§14 #2): Fatar OEM resupply terms (minimum order, lead time) beyond the initial ~100 keybeds; confirm 73SW contact count (2 or 3) and aftertouch presence.
- EXT3 (SPEC Open Q, GTM): Confirm which Inmetro requirement applies to the chosen power supply; select a compliant off-the-shelf PSU; plan IEC 62368-1 safety/EMC testing.
- EXT4 (SPEC Open Q, AR7): MIPI DSI on mainline RK3588 for the 7" panel (Milestone-1 spike); decide on the internal-HDMI fallback.
- EXT5 (SPEC Open Q, AD-15): License check of the vendored sfizz parser parts (sfizz 1.2.3, archived, BSD-2) and all their dependencies for closed-product use.
- EXT6 (SPEC Open Q): Obtain the Hammond M-solo drawbar MIDI CC/NRPN numbers (Drawbar Data List, manual pp. 64–65, is an image) for automated organ A/B renders.
- EXT7 (SPEC Open Q, acceptance-tests): Record a real tonewheel B-3 through a real rotary speaker cabinet as organ reference (recommended, not blocking); recruit the 3–5 keyboardist ABX panel.
- EXT8 (GTM): Formal trademark clearance search by a trademark attorney; INPI filing for Ladiani and Velora in Nice classes 15 and 9; later Madrid Protocol filing; register domains (ladiani.com, .com.br, .net, .io, .music, .audio) and handles (`@ladianimusic`, `@ladiani.instruments`) before branding spend.
- EXT9 (research): Run the Brazil keyboardist market survey (`pesquisa-tecladistas-brasil.md`: ~30 questions, ≥400 responses, pilot with 10, no brand names, LGPD consent) and feed results — Van Westendorp price range vs R$ 17,990–19,990, feature importance/top-3, pains, switching barriers, installment preference — into pricing and feature priority.
- EXT10 (HW-§14): Hardware partner deliverables: SoM vendor/part number and written longevity commitment; keybed contact count and aftertouch; MCU family and bootloader approach (by month 7); DSI panel + touch controller part numbers with measured luminance; encoder and LED part numbers with LED on:off contrast at 50 klx; measured POWER_FAIL timing and hold-up; codec reference design latency and noise; thermal test at full load; tier-ID strap; serial label + EEPROM; factory programming fixture and codec-loopback test points.
- EXT11 (HW-§11, AD-24): SSD qualification: measure p99.99 read latency under streaming load and endurance on candidate M.2 2280 NVMe SSDs; approve at least one (board must not depend on one model).
- EXT12 (DESIGN Deferred): Physical panel specifications with hardware partners: engraving vs silkscreen, LED colours, brushed aluminum finish that hides wear, MASTER control type (knob vs fader), legends on a matte band, tactile cues, dark-stage plan.
- EXT13 (DESIGN Deferred): Commission a type/logo designer for the final typeface (licensed for LVGL embedding and desktop), the final symbol/wordmark/model mark, the desktop app icon and social avatar (see UX-DR for acceptance criteria).
- EXT14 (Constraints, GTM): Founder produces the factory library content (factory piano, organ presets, other categories) — no sample-production partner for v1.
- EXT15 (GTM): Direct-sales channel for the first ~100 units (storefront, payment, logistics, warranty/service in Brazil); retail partners after scale.

### UX Design Requirements

**Design tokens (DESIGN.md frontmatter)**

- UX-DR1: Implement the Sálvia Stage (dark) color token set (`bg/surface/raised/line/line-strong/text/muted/accent/on-accent/warn/ok -dark`) as LVGL theme tokens shared with Velora Studio.
- UX-DR2: Implement the Sálvia Studio (light) color token set (`-light` twins incl. the darker muted/accent/warn set `#5a635c / #466853 / #725f1e`) as the second theme on device and desktop.
- UX-DR3: Implement theme-invariant drawbar tokens (brown/white/black heads, inks and edges) with the per-theme edge rule (black edge in Stage, white edge in Studio).
- UX-DR4: Deliver panel render-reference tokens (aluminum gradient, panel ink, legend, button cap/edge/lit edge, fader track, LED on/weak/lens-off) to the hardware partner; logo tokens (`logo-ink`, `logo-paper`; on screen text-dark on bg-dark).
- UX-DR5: Implement the device typography scale (live-set-name 53 px … meta-caps 12 px, mode-mirror 20 px bold, status-takeover 24 px; all lineHeight ≥1.2; tabular numerals for drawbar values and registration) as LVGL fonts built with ascent measured from Á/Ç/Ñ/Ê.
- UX-DR6: Implement the Large-text-on-Performance token variants (≈+25%: 66 / 28 / 20 / 21 / 18 / 30 px) and their rules (drop Scene Tile sub-labels and Part Strip categories, one-line names, skeleton unchanged).
- UX-DR7: Implement the Velora Studio rem typography (desktop-live-set-name 1.75 rem, part-name, body 13 px min, section-label 11 px/600 min) honouring OS text size.
- UX-DR8: Implement wordmark/boot typography tokens (wordmark-brand, wordmark-model, boot-wordmark, panel-legend) as stand-ins until the designer's face lands.
- UX-DR9: Implement radius tokens (sm 5, desktop-sm 6, md 8, lg 12, full) and spacing/layout tokens (spacing 1–5, screen-gutter 22, desktop-gutter 18, sidebar 220, device bar heights 56/118/72/298/56/56, touch-target-min 56, selection 3/2 px, focus-ring 2, mode-underline 2).
- UX-DR10: Implement the device screen skeletons: Performance (Top Bar 56 · Hero 118 · Scene strip 72 · Part area 298 · Status 56) and all other surfaces (Top Bar 56 · content 488 · Hint Bar 56), plus the overlay layering rules (Toast never over Hero/Scenes; ASSIGN Overlay over part area with precedence; OSK never over Top Bar or edited field; Action Sheet/Write Dialog centred over 35%-dimmed surface).
- UX-DR11: Implement tonal elevation (bg → surface → raised) with selection always adding an accent border; shadows only on Write Dialog, Action Sheet (device) and Studio Dialog (desktop).

**Device components (DESIGN.md Components + EXPERIENCE.md Component Patterns)**

- UX-DR12: Top Bar — context (setlist · position or `NO SETLIST`, middle-ellipsized), 20 px bold mode mirror of all three toggles with accent underline (no fill), tempo + clock source, `⚠ CLOCK LOST` in warn; setlist context opens the Setlist Browser (tap in Full, long press in Safe/Off).
- UX-DR13: Live Set Hero — `LIVE SET nn` label, Live Set name (1 line, end ellipsis) + ◆ when unsaved, NEXT title with `● READY` / `Loading <name>… Ns` / `End of setlist`; nothing animates.
- UX-DR14: Scene Tile — number, name, muted sub-label; current is the only accent-filled tile with on-accent text at 100%; empty dashed `line-strong`; tap recalls (per Touch on Performance), long press opens Scenes; reused in Studio.
- UX-DR15: Part Strip — part number + LED dot, ⏻ (56 px hit area), timbre name (2 lines), category short label, Volume Bar; selected, off (muted + hollow ring + OFF tag), empty, Missing library (warn) and pickup (dashed warn outline) states; card body → Part surface, ⏻ → on/off, no other targets.
- UX-DR16: Volume Bar — `line-strong` track, accent fill showing stored (sounding) volume; changes only after fader takeover.
- UX-DR17: Pickup Ghost — 3 px dashed warn line with 2 px bg keylines and ▲/▼ chevron at the physical control position, identical on every screen; disappears on takeover.
- UX-DR18: Status Bar — voices, `● STREAMING OK`, ROTARY, REV, STUDIO; pending pickup / warning takeover line (24 px, glyph-prefixed) replacing all items; REV opens Effects.
- UX-DR19: Hint Bar — keycap-outlined physical control names (English) + localized verb, only controls active on the surface, right-side reassurance or blocking warning.
- UX-DR20: Organ Card — `PART n · DRAWBARS`, organ name, `ORGAN` category, percussion/vibrato/rotary meta, numeric registration (`88 8600 000`), raised with accent border; follows the organ selected by part-button long press.
- UX-DR21: Drawbar Column (×9) — footage label 16'…1', slot with rod and coloured head 0–8, numeric value; mirrors fader with pickup ghost; registration updates live.
- UX-DR22: List Row — name/meta, selected raised + accent border, ▶ NOW marker, ◆ pending edits, drag (dashed accent) and hold ▲▼ reorder; VALUE scrolls, touch selects.
- UX-DR23: Search Field — ⌕, query, accent caret, `line-strong` border, muted scope hint; typing via On-screen Keyboard.
- UX-DR24: Filter Chip — pill, `line-strong` outline, one active accent-filled chip; fixed chips then categories or user tags.
- UX-DR25: Result Tile — name, meta caps line, ★ favorite, ◆ pending edits, 3-column grid; Library: key-play preview, WRITE assigns; Setlist Browser: WRITE loads/jumps; long press → Add/Remove Favorites Action Sheet.
- UX-DR26: Import Row — file name + size/format, Category Chip, status (`ready` / `needs category` / `skipped: …` / `already imported`) with problem states in warn + text.
- UX-DR27: Category Chip — `E.Piano ✓ suggested` in accent or `Choose category ▾` with warn border; opens the Category Picker.
- UX-DR28: Progress Bar — thin pill, accent fill; used for import, firmware update, transfer and boot.
- UX-DR29: Write Dialog — "Write changes?", scope options (Scene / Whole Live Set / Save as new… with disabled-reason line), name field, "Changed: …", `EXIT · Cancel`, accent `WRITE · Yes`; name-in-use and storage-full messages.
- UX-DR30: Unsaved Indicator — warn ◆ immediately after the Live Set name (Hero, List Rows, Result Tiles, Studio header); no text, no dialog.
- UX-DR31: Notice Toast — non-modal pill, message · accent action, never takes focus, 8 s auto-dismiss paused while touched, EXIT dismisses; hosts the brightness overlay; waits while ASSIGN Overlay is open.
- UX-DR32: Launcher Grid — 7 tiles (Setlist Browser, Setlist, Library, Effects, Scenes, Import, Settings) in 4 + 3 rows, ≥56 px, VALUE highlight, WRITE/touch open, MENU/EXIT close to origin.
- UX-DR33: ASSIGN Overlay — card over the part area with K1–K8 and parameter names, turned knob's slot raised + accent border; 3 s, restarts per turn; informational only.
- UX-DR34: Segmented Control — inline options with `line-strong` outline and accent-filled active segment; VALUE changes, touch selects; used for every on/off and multi-choice setting.
- UX-DR35: On-screen Keyboard — locale layouts (EN / PT-BR / ES) with accented letters, keys ≥56 px, accent Done; WRITE/Done confirms, EXIT closes keeping text; keys still sound while typing.
- UX-DR36: Category Picker — raised panel, grid of 17 full localized labels, accent-bordered highlight, VALUE + WRITE or touch, "Apply one category to all" row in Import.
- UX-DR37: Action Sheet — raised card, title, 2–4 action rows ≥56 px with their control (`WRITE · …`, `EXIT · Cancel`); used for missing library, pairing code, firmware install, favorites; never unprompted during performance.
- UX-DR38: Boot Screen — centred symbol + VELORA on bg, drawbar pull-out animation (static under reduced motion), thin progress bar, muted status (`RESTORING · … · SCENE n`, `UPDATING TO n`).

**Velora Studio components**

- UX-DR39: Studio Title Bar — app name, connection state in ok (`● VELORA 73 · CONNECTED VIA USB · PAIRED`), Import, Backup, Primary Button "Write to Velora".
- UX-DR40: Studio Sidebar — SETLISTS (nested Live Sets), LIBRARY (by category), DEVICE (storage, firmware, backups); active item raised with accent left edge; selecting a Live Set loads it.
- UX-DR41: Studio Part Strip — `PART n` + LED, name, category in accent, level meter, `VOL n`, key range, organ registration; "+ Add timbre" on empty parts; click fills the Part Editor.
- UX-DR42: Part Editor — four hairline-divided sections (Sound · Amp envelope + EQ · Effects inserts + sends · Zone & keyboard + Arpeggiator + Assign) with accent envelope polyline, insert/send rows and dashed "+ Add insert"; reflows 4 → 2 → 1 columns at ≥150% / 200%.
- UX-DR43: Studio Knob — raised disc, accent value arc, caps label, value; drag, type, scroll, keyboard; ARIA slider with `aria-valuetext`; 2 px accent focus ring at 2 px offset; edits play live.
- UX-DR44: Primary Button — accent fill, on-accent text, one per window ("Write to Velora"), disabled while writing.
- UX-DR45: Studio Footer — unsaved scope (`UNSAVED: SCENE 2` with warn ◆), `● LIVE EDIT ON`, write/transfer progress, last backup time; polite live region.
- UX-DR46: Studio Dialog — modal on raised, lg radius; pairing-code entry, Write to Velora scope, Restore Merge/Replace; Enter confirms, Esc cancels, focus trapped and returned.
- UX-DR47: Studio Banner — non-modal strip with 4 px warn left edge, message + one action (Reconnect, Update, Reload) for disconnect, version mismatch, write failure, stale edit; stays until resolved.

**Physical panel components (render references and behaviour)**

- UX-DR48: MENU Button — cap/edge as navigation buttons, no LED, `MENU` legend, centred between EXIT and WRITE, centre-dot tactile cue; toggles the Launcher, no long press.
- UX-DR49: Navigation Buttons — EXIT, WRITE (concave cap / raised ring), LIVE SET ◀ ▶, SCENE ◀ ▶; no LED; behaviour per the Navigation rules table.
- UX-DR50: Panel Toggle Buttons — single button with a legend + LED per state (DRAWBARS/MIXER, PARTS/SCENES, PART/ALL/ASSIGN 3-state), only active LED lit, slightly taller than momentary buttons.
- UX-DR51: Part/Scene Buttons — cap with LED strip: strong / weak (~15% PWM) / off; nubs on buttons 1 and 5; meaning per PARTS/SCENES mode.
- UX-DR52: Encoders — endless with LED ring value arc (rest unlit, never weak-lit), pointer line; VALUE encoder larger (≥30 mm), knurled, with push switch.
- UX-DR53: Faders — dark cap on grey track, non-motorized; REV cap distinct from faders 1–8.

**Device surfaces**

- UX-DR54: Performance surface, MIXER mode — Top Bar, Hero, 8 Scene Tiles, 8 Part Strips, Status Bar (per key-screen-performance mock, with the DESIGN.md-listed mock deviations corrected: category not engine type, warn keylined ghost, muted off parts, ⏻ icon, new bar heights, ◆, 20 px mode mirror).
- UX-DR55: Performance surface, DRAWBARS mode — Organ Card + 9 Drawbar Columns in the part area.
- UX-DR56: Launcher surface (7 tiles, opened by MENU from any surface).
- UX-DR57: Setlist surface — choose, create, reorder setlists and Live Sets, + New Live Set, edit setlist tags; first-run "No setlists yet." with "+ New setlist" highlighted.
- UX-DR58: Setlist Browser surface — search across setlists and Live Sets, chips (★ Favorites, Recent, Setlists, All Live Sets, user tags), 3-column results, jump off-script, long-press favorites.
- UX-DR59: Library surface — search, category/Favorites/Recent chips, 3-column results with preview, "CHOOSE TIMBRE FOR PART n" mode.
- UX-DR60: Part surface — header `PART n · timbre · CATEGORY`, ⏻, ★; list-left/fields-right sections Timbre, Sound, Envelope & EQ, Zone & keyboard, Level & output, Effects, Arpeggiator; VALUE edits highlighted field, encoders keep fixed functions.
- UX-DR61: Effects surface — Part inserts (per part, add/remove/reorder/bypass), Sends (reverb, delay, per-part levels), Master (EQ, compressor, line-in level), Outputs (Main/Aux per part).
- UX-DR62: Scenes surface — slots 1–8 in Scene Tile style with ▶ current and dashed empty; actions New / Rename / Duplicate / Move ◀ ▶ / Clear; saving via Write (Scene).
- UX-DR63: Write surface (Write Dialog flow from WRITE hold on Performance and editing surfaces).
- UX-DR64: Import surface — full-width Import Rows, category handling, batch apply, background import, insufficient storage, drive removed, unsupported/corrupt, duplicate states.
- UX-DR65: Settings surface — seven sections (Display & Language, Keyboard & Pedals, MIDI & Clock, Knobs · Global Assign, Computer, Storage & Library, System & Update) with all rows listed in EXPERIENCE.md (incl. theme guidance text, Reduced motion and Display rows missing from the mock).
- UX-DR66: Boot surface and firmware-install screen (Boot Screen layout with `UPDATING TO n`, "If power is lost, the Velora starts on n").

**Velora Studio surfaces**

- UX-DR67: Studio main window — title bar, sidebar + main (Live Set header with ◆, 8 Scene Tiles, 8 Studio Part Strips, Part Editor), footer; minimum 1280×800; dark/light following OS or user.
- UX-DR68: Studio Setlists view — setlists, their Live Sets, setlist tags, keyboard reorder (Alt+↑/↓, "Move to…").
- UX-DR69: Studio Library management view — browse by category, import/convert, transfer to device with progress and failure states.
- UX-DR70: Studio Device view — storage, firmware (send update), backups, diagnostics export.
- UX-DR71: Studio Backup & Restore — Backup to user-chosen `.velorabackup`, Restore with Merge/Replace dialog, last backup time; Export… (Live Sets, Timbres, Bundle) to `.velorasets` / `.veloratimbre` and Import… of partial files.

**State groups (EXPERIENCE.md State Patterns)**

- UX-DR72: Performance states — pickup pending, ready/preloaded, loading (not preloaded), end of setlist, no setlist loaded, missing library, part off, no organ (LED blinks twice), two organs, clock following, clock lost, Studio live edit, unsaved, USB drive inserted, empty scene/part, highlighted/focused.
- UX-DR73: Lists and search states — no results, first run no setlists, empty Favorites / Recent.
- UX-DR74: Write states — nothing to write, name in use (suggested name), storage full.
- UX-DR75: Import states — needs category, background, insufficient storage, drive removed, unsupported/corrupt, duplicate (Skip / Import again).
- UX-DR76: Firmware update states — ready to install, installing (sound unavailable), success toast, failure, rolled back after power cut.
- UX-DR77: Velora Studio states — first-time pairing (wrong code), connected, offline, version mismatch, live edit, unsaved, write to Velora, write failed, stale edit, disconnect mid-edit, library transfer, transfer failed, firmware send, restore.
- UX-DR78: Boot states — restoring, target missing.

**Interaction primitives**

- UX-DR79: Implement the canonical Navigation rules table (WRITE press/hold per surface, EXIT one-level-back incl. Launcher origin rules, MENU toggle, VALUE turn/push, LIVE SET ◀ ▶ with Panel guard, SCENE ◀ ▶).
- UX-DR80: Implement Touch on Performance (Full / Safe / Off) per-target matrix with armed state on press-down and Safe = ≥150 ms contact, ≤10 px movement, act on release.
- UX-DR81: Implement canonical timings (long press 400/500/800, Write hold 0.3–1.0 s, panel guard 300 ms, ASSIGN 3 s, Toast 8 s, scene LED 200 ms dip, blink 2×250/250 ms, ≤3 Hz, transitions ≤150 ms, reduced-motion ≤80 ms or none).
- UX-DR82: Touch equivalence — every panel-reachable action reachable by touch and Performance actions vice versa, subject to Touch on Performance; knob-target selection panel-only in Safe/Off.
- UX-DR83: Brightness quick path (hold EXIT + turn VALUE, Toast level, cancels back action) and LED brightness (3 steps + Follow screen).

**Panel ↔ screen mapping**

- UX-DR84: Toggle LEDs mirrored in the Top Bar and part-area mode (DRAWBARS/MIXER ↔ Part Strips vs Organ Card; PARTS/SCENES ↔ button LED meaning; PART/ALL/ASSIGN ↔ `KNOBS PART n / ALL / ASSIGN` + selected Part Strip outline).
- UX-DR85: Fader position ↔ Volume Bar / Drawbar Column + Pickup Ghost + Status Bar takeover; encoder ring ↔ target value (relative in ALL); encoder turned in ASSIGN ↔ ASSIGN Overlay.
- UX-DR86: Part button (PARTS) ↔ Part Strip LED dot / ⏻ / OFF; long press ↔ outlined Part Strip; VALUE push ↔ Part surface of the outlined part; ROTARY S/F ↔ Status Bar + Organ Card; LIVE SET/SCENE/footswitch ↔ Hero / current Scene Tile; VALUE/WRITE/EXIT ↔ Hint Bar; MENU ↔ Launcher; Pro bank button ↔ active bank (deferred).

**Accessibility**

- UX-DR87: Contrast audit of every token pair in both themes, Studio and panel against the measured-contrast table (text ≥4.5:1, boundaries ≥3:1, no opacity on text).
- UX-DR88: Never-color-only cues implemented everywhere (modes LED + legend + text; current scene fill + position / solid LED + pulse; ▶ NOW; off = muted + hollow + OFF; pickup keyline + chevron + text; warnings glyph + text; ◆ distinct from ●).
- UX-DR89: Device reduced motion (boot static frame, ≤80 ms cross-fades, Toast in place, instant LED pulse) and Large text on Performance.
- UX-DR90: Velora Studio keyboard model — Tab in visual order, F6 region cycling, arrow keys in scene/part grids, Alt+↑/↓ reorder and "Move to…", visible focus ring never removed.
- UX-DR91: Velora Studio screen-reader support — ARIA sliders, live regions for connection/unsaved/write/transfer/backup, `aria-label="Unsaved changes"` + tooltip on ◆.
- UX-DR92: Velora Studio text-size and contrast preferences — rem sizes, OS text size and ⌘/Ctrl +/− to 200% with Part Editor reflow and no horizontal scroll; OS Increase contrast swaps hairlines for `line-strong`; OS reduced motion.

**Localization**

- UX-DR93: All device and Studio strings as locale-catalog keys (EN source; PT-BR, ES); Studio follows OS language with override.
- UX-DR94: 17-category label table (EN / PT-BR / ES full and ≤10-char short labels) used by Part Strips (short) and Library/Import/Picker (full).
- UX-DR95: Truncation rules (Top Bar middle ellipsis; timbre end ellipsis 2 lines / 1 in Large; Live Set 1 line end ellipsis; Status Bar takeover only) and CLDR number/unit formatting per locale (`65 %`, `18,4 GB`, `18.4 GB`).
- UX-DR96: Hint Bar control names stay English, verbs localized with +40% budget; panel legends English in every locale.
- UX-DR97: Localization QA: pseudo-localization at +40% with accented capitals and the clipping string `ÁÇÊÕÑ Ação` on every surface review.

**Voice and tone**

- UX-DR98: Microcopy follows the Voice and Tone rules (name control/direction/target, plain facts, what still holds, sound reassurance, short questions, control-paired actions, fixed vocabulary Setlist/Live Set/Scene/Part/Timbre/Library/Category/Write, ROTARY), reviewed per surface.

**Design stories for undrawn surfaces (DESIGN.md Open Questions › Design backlog — each drawn before its surface is implemented)**

- UX-DR99: Design story — Part surface mockup (all sections incl. Arpeggiator, Stage and Studio themes, Large-text-independent, pseudo-localized).
- UX-DR100: Design story — Effects surface mockup (inserts, sends, master, outputs).
- UX-DR101: Design story — Scenes surface mockup (slots, actions).
- UX-DR102: Design story — Launcher Grid mockup (7 tiles, 4 + 3).
- UX-DR103: Design story — On-screen Keyboard mockup (EN / PT-BR / ES layouts, accented letters, coverage rule).
- UX-DR104: Design story — Category Picker mockup (17 categories, localized, Apply-to-all row).
- UX-DR105: Design story — Action Sheet mockup (missing library, pairing code, install update, favorites variants).
- UX-DR106: Design story — Velora Studio Footer, Studio Dialog (pairing, Write scope, Restore) and Studio Banner mockups, plus Backup & Restore (spine-only).
- UX-DR107: Design story — ASSIGN Overlay mockup.
- UX-DR108: Design story — Notice Toast mockup (USB drive, update found, brightness overlay, updated/rolled back variants).
- UX-DR109: Update existing mockups to remove the listed known deviations (engine type sub-labels, text-coloured ghost, 42% opacity off parts, missing ⏻, old bar heights, ●→◆, 20 px mode mirror, Studio ◆, `88 8600 000`, Settings rows, "songs"→"Live Sets", panel-v2 3-state knob toggle) so mocks match the spines.

**Brand handoff**

- UX-DR110: Final typeface handoff (designer) meeting acceptance criteria: distinct I/l/1 and 0/O, x-height ≥0.5 em, open apertures, tabular numerals and ⅓ ⅔ ⅗ glyphs, weight ≥400 for device UI (300 only for wordmark), Latin-1 + Latin Extended-A with accented capitals fitting a 1.2 line box, licensed for LVGL embedding and desktop; then swap stand-in fonts.
- UX-DR111: Final logo handoff (designer): symbol S1 (drawbar "L", simplified at 32 px and 16 px), wordmark LADIANI (T3), model mark VELORA (V1), panel lockup, boot-screen artwork and animation frames; works engraved/silkscreened, on the boot screen, as tiny icon and round avatar.
- UX-DR112: Desktop app icon and social avatar renderings of the symbol (designer), integrated into Velora Studio builds.
- UX-DR113: Physical panel layout A render package and hardware asks to partners (display, LEDs, legends, tactile cues) — tracked with EXT12.

**Partial backup and restore**

- UX-DR114: Studio Selection List (DESIGN.md `studio-selection-list`) and partial export/import states (EXPERIENCE.md) — checkbox rows with name, category and size, header select-all, Bundle scope listing used timbres locked with factory/partner rows tagged "Reference only"; export size estimate and large-bundle warning; export/import progress and results in the Footer; per-item Skip / Replace / Keep both with "Apply to all"; missing library listed after import; import failure messages; live regions for export and import results.

### FR Coverage Map

- FR1: Epic 1 - Deep library with stage latency
- FR2: Epic 1 - Deep library with stage latency
- FR3: Epic 1 - Deep library with stage latency
- FR4: Epic 1 - Deep library with stage latency
- FR5: Epic 1 - Deep library with stage latency
- FR6: Epic 1 - Deep library with stage latency
- FR7: Epic 1 - Deep library with stage latency
- FR8: Epic 2 - Organ and effects
- FR9: Epic 2 - Organ and effects
- FR10: Epic 2 - Organ and effects
- FR11: Epic 2 - Organ and effects
- FR12: Epic 12 - FM and VA engines (post-launch)
- FR13: Epic 12 - FM and VA engines (post-launch)
- FR14: Epic 12 - FM and VA engines (post-launch); v1 groundwork in Epic 1 (Story 1.8) and Epic 4 (Story 4.2)
- FR15: Epic 4 - Complete show: Live Sets, scenes and panel
- FR16: Epic 4 - Complete show: Live Sets, scenes and panel
- FR17: Epic 4 - Complete show: Live Sets, scenes and panel
- FR18: Epic 5 - Library: import and find timbres
- FR19: Epic 5 - Library: import and find timbres
- FR20: Epic 5 - Library: import and find timbres
- FR21: Epic 5 - Library: import and find timbres
- FR22: Epic 5 - Library: import and find timbres
- FR23: Epic 5 - Library: import and find timbres
- FR24: Epic 5 - Library: import and find timbres
- FR25: Epic 5 - Library: import and find timbres
- FR26: Epic 5 - Library: import and find timbres
- FR27: Epic 5 - Library: import and find timbres
- FR28: Epic 5 - Library: import and find timbres
- FR29: Epic 5 - Library: import and find timbres
- FR30: Epic 6 - Protected factory content
- FR31: Epic 6 - Protected factory content
- FR32: Epic 6 - Protected factory content
- FR33: Epic 6 - Protected factory content
- FR34: Epic 6 - Protected factory content
- FR35: Epic 9 - Velora Studio and USB
- FR36: Epic 9 - Velora Studio and USB
- FR37: Epic 9 - Velora Studio and USB
- FR38: Epic 3 - Physical instrument
- FR39: Epic 3 - Physical instrument
- FR40: Epic 3 - Physical instrument
- FR41: Epic 3 - Physical instrument
- FR42: Epic 3 - Physical instrument
- FR43: Epic 3 - Physical instrument
- FR44: Epic 3 - Physical instrument
- FR45: Epic 8 - Reliability: boot, updates and power loss
- FR46: Epic 8 - Reliability: boot, updates and power loss
- FR47: Epic 8 - Reliability: boot, updates and power loss
- FR48: Epic 8 - Reliability: boot, updates and power loss
- FR49: Epic 8 - Reliability: boot, updates and power loss
- FR50: Epic 2 - Organ and effects
- FR51: Epic 2 - Organ and effects
- FR52: Epic 2 - Organ and effects
- FR53: Epic 2 - Organ and effects
- FR54: Epic 9 - Velora Studio and USB
- FR55: Epic 9 - Velora Studio and USB
- FR56: Epic 5 - Library: import and find timbres
- FR57: Epic 5 - Library: import and find timbres
- FR58: Epic 5 - Library: import and find timbres
- FR59: Epic 5 - Library: import and find timbres
- FR60: Epic 5 - Library: import and find timbres
- FR61: Epic 5 - Library: import and find timbres
- FR62: Epic 8 - Reliability: boot, updates and power loss
- FR63: Epic 8 - Reliability: boot, updates and power loss
- FR64: Epic 8 - Reliability: boot, updates and power loss
- FR65: Epic 8 - Reliability: boot, updates and power loss
- FR66: Epic 6 - Protected factory content (A/B slots with rollback, Story 6.12); Epic 8 - Reliability: boot, updates and power loss (power-cut acceptance, Story 8.13)
- FR67: Epic 8 - Reliability: boot, updates and power loss
- FR68: Epic 8 - Reliability: boot, updates and power loss
- FR69: Epic 8 - Reliability: boot, updates and power loss
- FR70: Epic 3 - Physical instrument; Epic 10 - Settings, languages and themes (Velocity curve row, Story 10.2)
- FR71: Epic 3 - Physical instrument
- FR72: Epic 3 - Physical instrument; Epic 10 - Settings, languages and themes (Aftertouch row, Story 10.2)
- FR73: Epic 3 - Physical instrument
- FR74: Epic 7 - Arpeggiator and clock
- FR75: Epic 7 - Arpeggiator and clock
- FR76: Epic 7 - Arpeggiator and clock
- FR77: Epic 7 - Arpeggiator and clock
- FR78: Epic 7 - Arpeggiator and clock
- FR79: Epic 4 - Complete show: Live Sets, scenes and panel
- FR80: Epic 4 - Complete show: Live Sets, scenes and panel
- FR81: Epic 4 - Complete show: Live Sets, scenes and panel
- FR82: Epic 4 - Complete show: Live Sets, scenes and panel
- FR83: Epic 4 - Complete show: Live Sets, scenes and panel
- FR84: Epic 4 - Complete show: Live Sets, scenes and panel
- FR85: Epic 4 - Complete show: Live Sets, scenes and panel
- FR86: Epic 4 - Complete show: Live Sets, scenes and panel
- FR87: Epic 4 - Complete show: Live Sets, scenes and panel
- FR88: Epic 4 - Complete show: Live Sets, scenes and panel
- FR89: Epic 4 - Complete show: Live Sets, scenes and panel
- FR90: Epic 4 - Complete show: Live Sets, scenes and panel
- FR91: Epic 4 - Complete show: Live Sets, scenes and panel
- FR92: Epic 4 - Complete show: Live Sets, scenes and panel
- FR93: Epic 9 - Velora Studio and USB
- FR94: Epic 9 - Velora Studio and USB
- FR95: Epic 9 - Velora Studio and USB
- FR96: Epic 9 - Velora Studio and USB
- FR97: Epic 9 - Velora Studio and USB
- FR98: Epic 9 - Velora Studio and USB
- FR99: Epic 9 - Velora Studio and USB
- FR100: Epic 9 - Velora Studio and USB
- FR101: Epic 4 - Complete show: Live Sets, scenes and panel
- FR102: Epic 4 - Complete show: Live Sets, scenes and panel
- FR103: Epic 4 - Complete show: Live Sets, scenes and panel
- FR104: Epic 4 - Complete show: Live Sets, scenes and panel
- FR105: Epic 4 - Complete show: Live Sets, scenes and panel
- FR106: Epic 4 - Complete show: Live Sets, scenes and panel
- FR107: Epic 4 - Complete show: Live Sets, scenes and panel
- FR108: Epic 4 - Complete show: Live Sets, scenes and panel
- FR109: Epic 4 - Complete show: Live Sets, scenes and panel
- FR110: Epic 4 - Complete show: Live Sets, scenes and panel
- FR111: Epic 3 - Physical instrument
- FR112: Epic 4 - Complete show: Live Sets, scenes and panel; Epic 7 - Arpeggiator and clock (Arpeggiator section, Story 7.5)
- FR113: Epic 2 - Organ and effects
- FR114: Epic 5 - Library: import and find timbres
- FR115: Epic 10 - Settings, languages and themes (rows added earlier by Epics 4, 5, 7, 8 and 9 are consolidated there)
- FR116: Epic 10 - Settings, languages and themes
- FR117: Epic 10 - Settings, languages and themes
- FR118: Epic 10 - Settings, languages and themes
- FR119: Epic 5 - Library: import and find timbres
- FR120: Epic 1 - Deep library with stage latency
- FR121: Epic 12 - FM and VA engines (post-launch)
- FR122: Epic 9 - Velora Studio and USB


## Epic List

### Epic 1: Deep library with stage latency

A keyboardist plays, on the RK3588 dev board with a MIDI keyboard, a sample library far larger than RAM with 256 voices, ≤5 ms latency and zero dropouts over 24 h. Starts with the PREEMPT_RT latency, MIPI DSI and secure-boot spikes. Milestone 1 (month 6).

**FRs covered:** FR1, FR2, FR3, FR4, FR5, FR6, FR7, FR120

### Epic 2: Organ and effects

A keyboardist plays the tonewheel organ with drawbars, percussion, multi-contact onset and rotary speaker, and shapes sounds with per-part inserts, reverb and delay sends and the master section.

**FRs covered:** FR8, FR9, FR10, FR11, FR50, FR51, FR52, FR53, FR113

### Epic 3: Physical instrument

A keyboardist plays the real prototype: keybed with attack/release velocity and multi-contact timing, panel MCU with LEDs, balanced outputs, headphones, MIDI DIN, pedals, wheels, pop-free mute and power-fail protection.

**FRs covered:** FR38, FR39, FR40, FR41, FR42, FR43, FR44, FR70, FR71, FR72, FR73, FR111

### Epic 4: Complete show: Live Sets, scenes and panel

A keyboardist builds and plays a whole show: setlists, Live Sets with 8 parts and 8 scenes, seamless switching, Setlist Browser, Write, fader pickup, the three toggles, ASSIGN, footswitch and guards. Target: first playable units (month 12).

**FRs covered:** FR15, FR16, FR17, FR79, FR80, FR81, FR82, FR83, FR84, FR85, FR86, FR87, FR88, FR89, FR90, FR91, FR92, FR101, FR102, FR103, FR104, FR105, FR106, FR107, FR108, FR109, FR110, FR112

### Epic 5: Library: import and find timbres

A keyboardist imports SFZ/SF2 from a pen drive or SSD with suggested categories, searches and filters by category, keeps favorites, recovers from missing libraries and removes libraries.

**FRs covered:** FR18, FR19, FR20, FR21, FR22, FR23, FR24, FR25, FR26, FR27, FR28, FR29, FR56, FR57, FR58, FR59, FR60, FR61, FR114, FR119

### Epic 6: Protected factory content

A keyboardist receives the unit with the encrypted, signed factory library that plays only on that unit; includes the content-pack tool, key custody, the read-only root filesystem with RAUC A/B slots, the dev, factory-test and production images, anti-rollback, the factory provisioning station and the device registry.

**FRs covered:** FR30, FR31, FR32, FR33, FR34, FR66 (partial: A/B slots with rollback)

### Epic 7: Arpeggiator and clock

A keyboardist uses a per-part arpeggiator, also stored in scenes, synced to the internal tempo or external MIDI clock, with a clock-lost state.

**FRs covered:** FR74, FR75, FR76, FR77, FR78

### Epic 8: Reliability: boot, updates and power loss

A keyboardist powers on and plays the Boot-into target in ≤10 s (with the Boot into setting), updates firmware safely through `sysd` on the Epic 6 A/B slots, and never loses data on power loss.

**FRs covered:** FR45, FR46, FR47, FR48, FR49, FR62, FR63, FR64, FR65, FR66, FR67, FR68, FR69

### Epic 9: Velora Studio and USB

A keyboardist uses the unit as a driverless USB audio/MIDI interface, pairs Velora Studio, edits everything at a glance, transfers libraries, backs up and restores setlists and Live Sets, and exports and imports selected Live Sets, timbres or bundles.

**FRs covered:** FR35, FR36, FR37, FR54, FR55, FR93, FR94, FR95, FR96, FR97, FR98, FR99, FR100, FR122

### Epic 10: Settings, languages and themes

A keyboardist sets the velocity curve, pedal calibration, aftertouch and MIDI rows, picks Stage/Studio theme, brightness (with the quick path) and language (EN, PT-BR, ES), and finds every earlier row (incl. Boot into from Epic 8) in one canonical Settings surface.

**FRs covered:** FR115, FR116, FR117, FR118, FR70 (Settings row), FR72 (Settings row)

### Epic 11: Pre-launch: brand, certification and suppliers

The business is ready to launch: trademark and domains, tax validation, Inmetro PSU, Fatar resupply, hardware partner deliverables, SSD qualification, the Brazil keyboardist survey, and final brand assets integrated into the product. Runs in parallel from month 0; stories in other epics that need one of its deliverables carry a **Blocked by:** line.

**FRs covered:** EXT1–EXT15 (no FRs)

### Epic 12: FM and VA engines (post-launch)

A keyboardist receives the 6-operator FM and virtual-analog engines by firmware update, running inside the shared voice pool, and imports 6-operator FM SysEx banks and single voices.

**FRs covered:** FR12, FR13, FR14, FR121

### Epic 13: SFZ coverage for the reference corpus

The reference corpus (Salamander, VSCO2, Sonatina, Karoryfer, a Pianobook selection) imports and passes the sfizz null test, by widening the SFZ opcode coverage of the importer and the sample engine in measured steps. Follows the Story 5.4 corpus findings: only VSCO 2 imports today, and the Pianobook, Salamander, Sonatina and Karoryfer libraries are rejected on specific opcodes. Starts with an explicit scope decision so the v1 subset (FR18) and the corpus requirement (FR20) agree.

Scope decision (Story 13.1, proposed, awaiting founder ratification; `_bmad-output/planning-artifacts/sfz-scope-decision.md`): tiers of Stories 13.2-13.8 and 13.10 in scope; the region EQ and LFO/EG tier (Story 13.9) deferred and conditional on measured need after 13.8. No story is cancelled by the proposal; if 13.9 ends out of scope it is cancelled and its files stay skipped in the manifest with that reason.

**FRs covered:** FR18 (extension), FR20, FR22

## Epic 1: Deep library with stage latency

A keyboardist plays, on the RK3588 dev board with a USB MIDI keyboard, a sample library far larger than RAM with 256 voices, ≤5 ms latency and zero dropouts over 24 h. The epic starts with the PREEMPT_RT latency, audio output, MIPI DSI and secure-boot spikes, and ends with the Milestone 1 investor demo (month 6). Development is Mac-first: stories tagged **Runs on: Mac** start on the developer's Apple-silicon Mac (same ARM64/NEON ISA) before the dev board arrives; latency and xrun budgets are only accepted on the dev board.

### Story 1.1: PREEMPT_RT latency spike on mainline RK3588 under load

As a firmware developer,
I want measured worst-case scheduling latency of a mainline PREEMPT_RT kernel on the RK3588 dev board under streaming and render load,
So that the founder can make a go/no-go call on the RK3588 before any engine work depends on it, with the CM5 / Intel N-series fallback decided on data.

**Covers:** AR6, NFR10, NFR37, NFR38

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** a ROCK 5B+ class RK3588 dev board (16 GB) with an NVMe SSD
**When** a mainline Linux 6.18 LTS kernel is built with `CONFIG_PREEMPT_RT`, no out-of-tree RT patches, and booted with `isolcpus`, `nohz_full` and `rcu_nocbs` on 3 Cortex-A76 cores and a fixed `performance` governor
**Then** the kernel boots on the dev board and `/sys/kernel/realtime` reads 1
**And** the kernel config fragment and boot arguments are committed under `tools/spikes/rt-latency/` so a later image build can reuse them unchanged

**Given** the RT kernel is running
**When** `cyclictest` (SCHED_FIFO, one thread per isolated core, 1.333 ms interval matching a 64-frame period at 48 kHz) and `rtla timerlat` run for ≥12 h while a load script runs at the same time: synthetic DSP load on the 3 isolated cores (~70% of each period), `fio` with `O_DIRECT` random 64 KiB reads on the NVMe at ≥400 MB/s, and `stress-ng` CPU/memory/IO load on the remaining cores
**Then** the max, p99.99 and histogram of wakeup latency per isolated core are recorded
**And** the run passes when max latency on every isolated core is ≤100 µs (≤7.5% of the period)

**Given** the run results
**When** the spike report is written in `tools/spikes/rt-latency/REPORT.md`
**Then** it states GO (RK3588 stays) or NO-GO (open the CM5 / Intel N-series fallback per NFR38), with the measured numbers, the load profile, and any `rtla` traces of outliers above 50 µs with their identified source (driver, IRQ, firmware)
**And** a NO-GO lists which mitigations were tried (IRQ affinity, disabling specific drivers, DVFS off) before giving up

### Story 1.2: Audio output bring-up on the dev board

As a founder preparing the investor demo,
I want the dev board to play audio cleanly at 48 kHz with 64-frame periods through a codec output,
So that the demo has a real stage-quality output path and the output part of the latency budget is measured early.

**Covers:** AR9 (partial: I2S MCLK behaviour on the dev board), NFR1 (output-side budget measurement)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** the RT kernel from Story 1.1 on the dev board
**When** the onboard I2S codec output is enabled through the mainline device tree (or a DT overlay committed under `tools/spikes/audio-bringup/`)
**Then** ALSA exposes a playback device that opens at 48 kHz, S32_LE or S24_LE, period size 64 frames, 2 periods
**And** a SCHED_FIFO test player on an isolated core plays a 1 kHz sine for 1 h with zero xruns reported by ALSA

**Given** the onboard codec cannot meet 48 kHz / 64 frames / 2 periods xrun-free, or its analog output is not usable for the demo
**When** the fallback path is tried
**Then** a class-compliant USB audio interface is brought up with the same parameters and the same 1 h zero-xrun test
**And** the report states that USB output is a demo-only path and never the product main output (NFR40)

**Given** a working output path
**When** output latency is measured with a loopback (GPIO pulse at the moment a frame is written vs analog onset on a scope, or a loopback cable into a second interface)
**Then** the write-to-analog latency is recorded in `tools/spikes/audio-bringup/REPORT.md`
**And** the report records whether boot firmware gates the I2S MCLK and how the DT handles it, as input to AR9

### Story 1.3: Monorepo skeleton with host build and CI

As a firmware developer,
I want the monorepo laid out per the Structural Seed with a host build and a CI pipeline that runs unit tests and RealtimeSanitizer,
So that every following story lands in the right place and is tested automatically from the first commit.

**Covers:** AR1, AR2 (partial: host unit tests, RTSan), AR13 (partial: RTSan in CI)

**Runs on:** Mac + CI

**Acceptance Criteria:**

**Given** the repository
**When** the skeleton is created
**Then** it contains only the directories the next stories need, `engine/core/`, `engine/ports/`, `engine/host/`, `tools/offline-render/`, and `tools/spikes/` (kept from Stories 1.1 and 1.2), with a README line per directory stating its role and dependency rule from the architecture spine
**And** a top-level CMake (C++20) build configures and builds on macOS (Apple clang) and Linux (gcc and clang) hosts

**Given** the host build
**When** a dependency check runs in CI
**Then** the build fails if any target under `engine/core/` links or includes anything outside `engine/core/`, `engine/ports/` and the C++ standard library (no Linux, ALSA, OP-TEE or hardware headers)

**Given** a push or pull request
**When** CI runs
**Then** it builds on macOS and Linux, runs the unit test suite, and runs a RealtimeSanitizer build of the unit tests
**And** a deliberately RT-unsafe test (heap allocation inside a function marked `[[clang::nonblocking]]`) makes the RTSan job fail, proving the gate works, and is then removed or marked expected-fail

### Story 1.4: Minimal Yocto dev image for the dev board

As a firmware developer,
I want a reproducible Yocto dev image for the RK3588 dev board with the validated RT kernel and audio configuration,
So that engine work runs on a known, license-clean, CI-built system instead of a hand-configured board.

**Covers:** AR3 (partial: dev variant), AR4, AR2 (partial: Yocto build + license manifest), NFR10, NFR17 (partial: no swap, no core dumps), NFR27, NFR37

**Runs on:** Linux build host or ARM64 Linux VM on the Mac; image runs on the dev board

**Acceptance Criteria:**

**Given** the `yocto/` directory with a distro layer on Yocto 6.0 "Wrynose" LTS and `INIT_MANAGER = "systemd"`
**When** the dev image is built
**Then** it boots on the dev board with the Linux 6.18 LTS PREEMPT_RT kernel using the config fragment and boot arguments from Story 1.1, and the audio configuration from Story 1.2
**And** the image has SSH and the debug UART enabled, is marked dev in `/etc/os-release`, has no swap and has core dumps disabled

**Given** the distro configuration
**When** a recipe with a GPL-3.0 or LGPL-3.0 license is added to the image
**Then** the build fails through `INCOMPATIBLE_LICENSE`, while GPLv2 kernel/BusyBox and GCC-exception runtimes are allowed
**And** the flac/metaflac CLIs and libsndfile are not in the image

**Given** CI
**When** the Yocto job runs on the main branch
**Then** it builds the dev image and publishes the image and its license manifest as build artifacts

**Given** the booted dev image
**When** `cyclictest` from Story 1.1 runs for 1 h under the same load profile
**Then** max latency on the isolated cores stays within the Story 1.1 pass threshold, so the image did not regress the spike result

### Story 1.5: MIPI DSI 7" panel spike on mainline RK3588

As a firmware developer,
I want a 7" 1024×600 MIPI DSI panel with capacitive touch driven by the mainline kernel on the dev board,
So that the founder knows by Milestone 1 whether the product display works on mainline or the internal-HDMI fallback is needed.

**Covers:** AR7, NFR43 (display part), NFR38 (display validation half of the fallback decision), EXT4 (software side)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** the dev image from Story 1.4 and a candidate 7" 1024×600 DSI panel with an I2C touch controller connected to the dev board's DSI connector
**When** the panel is enabled through mainline DT and drivers (overlay committed under `yocto/`)
**Then** DRM/KMS exposes a 1024×600 landscape mode and a test client (`modetest` or a minimal LVGL DRM demo) shows a full-screen test pattern without tearing or artifacts
**And** the touch controller reports multi-touch events through evdev with coordinates mapped to the panel

**Given** the panel is running a continuously animating test client
**When** the Story 1.1 load profile runs for 1 h
**Then** cyclictest max latency on the isolated cores stays within the Story 1.1 threshold (display does not break RT)

**Given** the spike result
**When** `tools/spikes/dsi/REPORT.md` is written
**Then** it states GO (DSI on mainline) or FALLBACK (internal HDMI panel), the kernel version and drivers used, any patches needed and whether they are upstream
**And** a FALLBACK verifies a 1024×600 HDMI panel the same way so the board footprint decision can be passed to the hardware partner

### Story 1.6: Secure-boot chain selection and signed image on an unfused board

As a firmware developer,
I want a chosen secure-boot chain for mainline RK3588 that produces a fully signed boot path,
So that the production content-protection design rests on a chain that is proven to build before any board is fused.

**Covers:** AR8 (partial: chain selection and signing), NFR23 (partial: separate dev signing root)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** the two candidates, barebox (`rksecure`) and U-Boot + Rockchip tools, each with a TF-A source and OP-TEE ≥4.9.0
**When** both are evaluated on the dev board
**Then** a comparison in `tools/spikes/secure-boot/REPORT.md` covers mainline status, TF-A source (upstream vs Rockchip blob), OP-TEE integration, signing tooling, license fit with AD-15, and Yocto integration effort
**And** one chain is selected with the reasons

**Given** the selected chain
**When** the Yocto build signs every boot stage (loader, TF-A, OP-TEE, bootloader, kernel FIT) with a dev signing key generated for this purpose and kept out of the repository
**Then** the unfused dev board boots the signed image and each stage logs a successful signature check where the stage supports verification without fuses
**And** an image with one byte flipped in the signed kernel FIT fails the bootloader's verification

**Given** the signing setup
**When** keys are listed
**Then** the dev root is labelled dev-only and the report documents how factory-test and production roots stay separate (production root offline in an HSM is out of scope for this spike)

### Story 1.7: Fused dev board boots signed and derives a per-device key

As a firmware developer,
I want a sacrificial dev board fused with the dev root to boot only the signed image and derive a per-device key in OP-TEE,
So that the secure-boot exit criterion of Milestone 1 is met and content protection can be built on it later.

**Covers:** AR8 (exit criterion), NFR23 (partial: per-device key in OP-TEE, keys never on disk)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** a dedicated dev board marked sacrificial (fusing is irreversible) and the signed chain from Story 1.6
**When** the secure-boot fuses are programmed with the dev root key hash following the documented procedure
**Then** the board boots the signed dev image
**And** an unsigned image or an image signed with a different key does not boot

**Given** the fused board running OP-TEE
**When** a test trusted application derives a device key from the OP-TEE hardware unique key implementation
**Then** the same key is derived across reboots on that board, a different key is derived on a second board, and the key never leaves the secure world (the test TA returns only a public value or a MAC over a challenge)
**And** no key material is written to any filesystem

**Given** the spike
**When** the report is updated
**Then** it records the exact fusing steps, the HUK source used, any blockers for production, and GO / NO-GO for the chain

### Story 1.8: Engine core block loop and engine interface

As a firmware developer,
I want a platform-independent engine core that renders 64-frame blocks from timestamped UMP events through the single engine interface,
So that all DSP is built and tested on the host from day one and engines plug into one contract.

**Covers:** AR1 (engine/core), AR13 (partial: SPSC queues, no allocation on RT path), AR14 (partial: serial event phase and sum), AR15, AR16 (partial: UMP-only events, in-block frame offset), NFR17 (partial), FR14 (partial: one engine interface and engine type IDs for all four engines from day one)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** `engine/core` and `engine/ports`
**When** the block loop is implemented
**Then** it renders stereo float blocks of 64 frames at 48 kHz, consumes MIDI 2.0 UMP events carrying a target frame position, and applies each event at its exact in-block frame offset
**And** UMP is the only event type accepted by the core

**Given** the single static engine interface
**When** a test-tone engine (sine voices with a linear envelope) implements it
**Then** the engine declares a per-voice cost in cost units and exposes steal metadata (start time, current level, held/sustained/releasing state), and voices are started and stopped only through calls from the core
**And** engines are compiled in, with no plugin loading

**Given** a lock-free SPSC queue and a pre-allocated voice/scratch memory arena in `engine/core`
**When** the RTSan build of the unit tests renders 10,000 blocks with note on/off traffic
**Then** RTSan reports no allocation, lock or blocking call inside the render path

**Given** a note-on scheduled at frame 37 of a block
**When** the block renders
**Then** unit tests show silence before frame 37 and the voice onset starting exactly at frame 37

**Given** the engine type registry in `engine/core`
**When** it is defined
**Then** it reserves stable engine type IDs for Sample, Organ, FM and VA from day one, and the single engine interface carries everything each of the four engines needs (per-voice cost, steal metadata, sound parameters addressed by `param_id`), so FM and VA later plug in with no interface change
**And** a voice request for a reserved type with no compiled engine (FM and VA in v1) yields silence with a typed `ENGINE_NOT_AVAILABLE` status and is never served by another engine, checked by a unit test

### Story 1.9: Offline renderer and null-test harness

As a firmware developer,
I want an offline renderer that drives the engine core from a MIDI file into a WAV file, and a harness that null-tests two renders against a tolerance,
So that sound fidelity is checked automatically in CI against reference renders.

**Covers:** AR1 (offline renderer), AR2 (partial: null tests in CI), NFR19 (harness only), NFR28 (oracle unshipped)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** `tools/offline-render`
**When** it is run with a Standard MIDI File, an engine configuration and an output path on a macOS or Linux host
**Then** it converts MIDI to UMP with frame positions using the same device-time-to-frame rule as the live engine (fixed tempo map, 48 kHz) and writes a 32-bit float WAV of the engine core output
**And** two runs with the same inputs produce bit-identical files

**Given** a null-test harness
**When** it compares a candidate WAV with a reference WAV
**Then** it aligns them (no alignment search beyond a configured max offset), computes the residual RMS relative to the reference RMS in dB, and passes or fails against a per-test tolerance stored with the test case
**And** a report lists per-file residual dB and the worst 100 ms window

**Given** CI
**When** the null-test job runs
**Then** a test-tone engine render is null-tested against a committed golden file and a deliberately detuned render fails the test
**And** reference oracles (e.g. sfizz) are built only in the test job and never linked into a shipped target

### Story 1.10: Engine plays live on the developer's Mac (macOS host adapters)

As a firmware developer working on an Apple-silicon Mac before any board arrives,
I want the platform-independent engine core to play live through CoreAudio and receive a USB MIDI keyboard through CoreMIDI,
So that engine, format and sound work can start immediately on the same ARM64 instruction set (NEON) and later move to the board unchanged.

**Covers:** AR1 (host build), AR13 (partial: RT-safe render on the host audio thread), AR14 (partial), AR16 (partial: host timestamping), NFR17 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the host build from Story 1.3 and the engine core from Story 1.8
**When** the developer starts the `engine-host` dev app on macOS
**Then** it renders the core through a CoreAudio adapter at 48 kHz with a 64-frame buffer and plays the test-tone engine
**And** the same engine core sources compile unchanged for the board (only adapters under `engine/host/` differ)

**Given** a USB MIDI keyboard connected to the Mac
**When** keys are played
**Then** a CoreMIDI adapter converts input to timestamped UMP and notes sound at their in-block offsets
**And** RealtimeSanitizer reports no allocation, lock or blocking call on the audio thread during 10 min of playing

**Given** the macOS host differs from the target (OS, scheduler, much faster cores)
**When** results are reported
**Then** Mac timing figures are labelled "host, not representative"; latency and xrun budgets are only accepted on the dev board (not on the Mac)

### Story 1.11: Engine runs live on the dev board

As a firmware developer,
I want the engine running as a systemd service on the dev board, rendering the core through ALSA on the isolated cores,
So that everything built in the core is heard and measured on the real target.

**Covers:** AR10 (partial: `engine` service, sole owner of the audio device), AR13 (partial: mlocked pre-faulted memory, only syscall ALSA period wait), AR14 (partial: RT thread on an isolated core), NFR10, NFR17

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** the dev image from Story 1.4 and the output path from Story 1.2
**When** the `engine` systemd service starts
**Then** it opens the ALSA playback device at 48 kHz, 64-frame period, 2 periods, runs the audio thread SCHED_FIFO on an isolated A76 core, calls `mlockall` and pre-faults its arenas before opening audio
**And** it is the only process that opens the audio device

**Given** the engine is running
**When** a scripted UMP sequence (a committed test file played by a dev tool through a local socket into the engine's SPSC queue) plays test-tone chords for 1 h
**Then** the audio is heard on the output and the engine reports zero xruns
**And** `strace -f` on the audio thread for 60 s shows only the ALSA period-wait syscalls

**Given** an xrun is forced (for example by stalling the audio thread in a debug build)
**When** the engine recovers
**Then** audio resumes without restarting the service and the xrun is logged through a non-RT path

### Story 1.12: formatlib lossless block-indexed bundle

As a keyboardist,
I want my sample libraries stored losslessly in a block-indexed bundle that decodes bit-identical to the source,
So that the instrument sounds exactly like the original recording while streaming from any point of any sample.

**Covers:** FR3, NFR13, AR27 (partial: IDs, block-indexed container read/write, lossless codec; no crypto, no SF2)

**Runs on:** Mac (bundle format); verified on the board later

**Acceptance Criteria:**

**Given** `formatlib/` with libFLAC 1.5.0 (library only)
**When** a bundle is written from WAV sources at 16-bit or 24-bit integer PCM, mono or stereo, 44.1 / 48 / 88.2 / 96 kHz
**Then** each sample is split into fixed-size blocks coded FLAC-style, and an index table maps every sample to its blocks so a reader can seek to any frame by decoding one block
**And** 32-bit float sources are stored uncompressed in the same container

**Given** any bundle written by formatlib
**When** every sample is decoded
**Then** the decoded PCM is bit-identical to the source for every supported format, verified by unit tests over a fixture set including silence, full-scale noise, and odd lengths
**And** no resampling or requantization happens on any path

**Given** the bundle manifest
**When** a bundle is written
**Then** it carries `format_version`, a `bundle_id` equal to the manifest hash, a `library_id` (UUIDv5 of a source fingerprint for user imports) and an `instrument_key` per instrument (normalized `.sfz` relative path), all minted only by formatlib
**And** a reader rejects a bundle with an unknown major `format_version` with a typed error, and accepts version N and N−1

**Given** a bundle with a truncated block or corrupted index
**When** it is opened
**Then** formatlib returns a typed error instead of crashing or returning garbage audio

### Story 1.13: Import the demo SFZ library subset into bundles

As a founder preparing the investor demo,
I want the demo SFZ libraries converted into bundles and published atomically under `/data/lib`,
So that the demo plays real, deep sample libraries without waiting for the full import feature.

**Covers:** FR3, AR27 (partial: vendored sfizz SFZ parser), AR28 (partial: stage → round-trip verify → fsync → rename → flip `current`), FR18 (demo subset only; full subset and user flow in Epic 5)

**Runs on:** Mac

**Blocked by:** Story 11.12 (target month 5; done). Full closure also depends on Epic 13 (SFZ coverage), the founder's three remaining Pianobook packs (>=5 total) and the licence confirmation of the Sonatina and Pianobook packs

**Acceptance Criteria:**

**Given** the sfizz 1.2.3 parser vendored into `formatlib/` with its license recorded (EXT5 license check tracked as a dependency)
**When** a command-line importer (`librarian import`, runnable on host and on the dev board) is given an SFZ file
**Then** it maps headers `<global>`, `<master>`, `<group>`, `<region>`, `<control>`, key and velocity ranges, `pitch_keycenter`, tune/transpose, volume/pan, offset, `seq_length`/`seq_position`, `lorand`/`hirand`, loops, `ampeg_*`, release triggers, `#define`/`#include` and `default_path` into the internal instrument model
**And** opcodes outside this demo subset are reported per file as unsupported and the file is skipped with that reason

**Given** Salamander Grand Piano and at least one VSCO2 instrument
**When** they are imported
**Then** each produces a bundle with all regions mapped and an instrument model whose key, velocity, round-robin and loop mapping matches the SFZ source, checked by a test that compares the parsed region table with the bundle's instrument table

**Given** an import in progress
**When** the bundle is written
**Then** it is staged in a temporary directory on the same filesystem, every sample is decode-verified bit-exact against the source, fsynced, renamed to `/data/lib/<library_id>/<bundle_id>/`, and only then is `/data/lib/<library_id>/current` atomically switched to it
**And** killing the importer at any point leaves either the previous `current` or the new one, never a partial bundle under `current`

### Story 1.14: 1 TB library resolved from the index without scanning

As a keyboardist,
I want the unit to find any instrument in up to 1 TB of libraries straight from the index,
So that loading an instrument never waits on scanning the library contents.

**Covers:** FR2, NFR3 (partial: 1 TB indexed on NVMe), AR20 (partial: open `current` links and indexes, never scan)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a test tool that fills an NVMe with ≥1 TB of valid bundles (distinct `library_id`s built from the demo bundles plus generated material)
**When** the engine resolves an instrument by `(library_id, instrument_key)`
**Then** it opens only `/data/lib/<library_id>/current`, the bundle manifest and index, and reads no sample data until a note or head load needs it
**And** resolve time for one instrument is ≤50 ms and does not grow with total library size (measured at 10 GB, 100 GB and 1 TB)

**Given** an engine startup on the 1 TB disk
**When** the engine starts
**Then** it does not walk library directories beyond the requested `current` links (verified by counting filesystem operations)

**Given** a reference to a `library_id` or `instrument_key` that does not resolve
**When** an instrument load is requested
**Then** the engine returns a typed "missing library/instrument" result, the part plays silent, and no other instrument is substituted

### Story 1.15: Sample engine plays RAM-resident instruments and passes the sfizz null test

As a keyboardist,
I want the sample engine to play an instrument with correct velocity layers, round robin, envelopes, loops and release triggers,
So that sampled instruments sound like the original library.

**Covers:** NFR19, NFR13, NFR28, AR15 (sample engine implementation)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** an instrument from a Story 1.13 bundle loaded fully into RAM through a sample-source port (host and offline renderer)
**When** notes are rendered
**Then** the engine selects regions by key, velocity, round robin (`seq_*`, `lorand`/`hirand`), plays with pitch from `pitch_keycenter` and tune, applies `ampeg_*`, loops per `loop_mode`, and fires release-trigger regions on note off
**And** the engine runs in `engine/core` with no OS dependency and passes the RTSan render test

**Given** a committed MIDI test suite (single notes across the range and velocity layers, chords, round-robin repeats, sustained loops, releases) for Salamander Grand Piano and one VSCO2 instrument
**When** the null-test harness compares the engine's offline render with sfizz rendering the original SFZ/WAV
**Then** the residual is below the per-test tolerance (initial proposal −60 dB relative to reference RMS; the chosen value and its reasoning are recorded with the tests)
**And** the null tests run in CI on every change to the sample engine

**Given** a pitched note far from its key center
**When** it renders
**Then** interpolation stays within the null-test tolerance against the oracle, and the cost per voice declared to the engine interface reflects the interpolation quality used

### Story 1.16: Instrument heads decoded into locked RAM at load

As a keyboardist,
I want every note's attack to play from memory that is already decoded and locked,
So that an attack never waits on decode or disk, even with a library far larger than RAM.

**Covers:** FR4, AR19 (partial: heads resident before a generation is used), AR20 (partial: head sizing from SSD p99.99, head budget), NFR3 (partial: 4–6 GB head budget), NFR4 (partial: head sizing), EXT11 (measurement on the demo SSD)

**Runs on:** Dev board (p99.99 measured on the target SSD); logic prototyped on Mac

**Acceptance Criteria:**

**Given** the demo NVMe SSD on the dev board
**When** a measurement tool reads random `O_DIRECT` blocks from bundles at the streaming rate of 256 voices for ≥1 h with the SSD ≥90% full
**Then** it reports the p99.99 read latency, which is recorded as the qualified value for this SSD

**Given** that p99.99 value and a configured margin
**When** an instrument loads on the dev board
**Then** for every sample the engine decodes a head long enough to cover p99.99 + margin at the sample's rate and the highest playback pitch the mapping allows, into mlocked, pre-faulted memory, on a non-RT thread
**And** the instrument becomes playable only after all of its heads are resident

**Given** a head budget configured at 5 GB (v1 profile)
**When** a load would exceed the budget
**Then** the load is refused with a typed error stating required vs available head memory, and already-loaded instruments keep playing

**Given** heads are resident
**When** a note starts
**Then** the first audio block of the note reads only head memory, verified by a test that drops the page cache and blocks disk reads during note onset

### Story 1.17: Streaming from NVMe ahead of the playhead

As a keyboardist,
I want long samples to continue seamlessly from NVMe after the head,
So that I can play instruments from a library far larger than RAM.

**Covers:** FR1, AR20 (io_uring, O_DIRECT, decode on I/O edge threads), AR13 (partial), NFR2 (partial), NFR17

**Runs on:** Dev board (io_uring/O_DIRECT); a portable reader prototypes on Mac

**Acceptance Criteria:**

**Given** a voice playing from its head
**When** the playhead advances
**Then** I/O edge threads (not RT threads) read the following blocks with io_uring and `O_DIRECT`, decode them, and fill a per-voice ring buffer far enough ahead that the voice switches from head to stream without a discontinuity
**And** the rendered output for a long sustained note matches the fully-in-RAM render from Story 1.15 bit-for-bit

**Given** the RT thread needs stream data that has not arrived
**When** a stream underrun happens
**Then** the RT thread does not wait; the voice outputs silence with a short fade for the missing frames, an underrun counter increments, and playback continues

**Given** 256 stereo voices sustaining different samples on the dev board
**When** they stream for 1 h with the SSD ≥90% full
**Then** there are zero stream underruns and zero xruns
**And** I/O edge threads run on non-isolated cores at an RT priority below the audio thread

### Story 1.18: Parallel voice render on the three isolated cores

As a keyboardist,
I want the engine to spread voice rendering over the three audio cores,
So that 256 sample voices play within each 64-frame period.

**Covers:** AR14 (block phases with parallel voice render and barrier), NFR2, NFR10

**Runs on:** Mac (correctness), dev board (timing budget)

**Acceptance Criteria:**

**Given** the block loop from Story 1.8
**When** the parallel phases are added
**Then** each block runs serial event processing, then parallel voice render on 3 worker threads pinned to the 3 isolated A76 cores writing to per-worker scratch buffers, then a barrier, then summation
**And** voices write only their own state and their worker's scratch buffer (voice contract), and the workers synchronize without locks or syscalls other than the ALSA period wait on the main audio thread

**Given** the same input
**When** rendered with 1 worker and with 3 workers in the offline renderer
**Then** the outputs are bit-identical (deterministic summation order)

**Given** 256 stereo sample voices on the dev board
**When** the per-block render time is measured for 1 h
**Then** the worst block finishes within 70% of the 1.333 ms period, and the histogram is recorded

### Story 1.19: Voice pool arbiter with the sample floor

As a keyboardist,
I want voices allocated from one shared pool that guarantees the sample engine at least 256 voices and steals sensibly when the pool is full,
So that dense playing never drops the notes that matter.

**Covers:** FR6, FR7, AR18, AR15 (cost units and steal metadata in use), NFR2

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a pool with a budget in cost units
**When** a Live Set loads
**Then** the arbiter reserves in order: the FX budget, the organ and rotary reservation (zero when no organ is present), each engine's floor (sample ≥256 stereo voices), and shares the remainder
**And** the arbiter is the only code that allocates or steals voices and runs in the serial phase on the RT plane

**Given** the pool is full
**When** a new note arrives
**Then** the arbiter steals across the whole Live Set, never steals the lowest held note or pedal-sustained notes while other candidates exist, and prefers newer and quieter voices
**And** a stolen voice fades out over a short ramp and keeps counting against the budget until it is silent

**Given** unit tests in `engine/core`
**When** scripted scenarios run (full pool with held bass note, sustain pedal down, mixed velocities)
**Then** the chosen victims match the expected order in every scenario
**And** each steal increments a steal counter

### Story 1.20: Timestamped event path from a USB MIDI keyboard

As a keyboardist,
I want my USB MIDI keyboard's notes applied at a fixed, deterministic delay after they arrive,
So that timing feels tight and identical every time I play.

**Covers:** FR5 (USB MIDI path; keybed, panel and DIN in Epic 3), AR16 (device time, DLL to frames, fixed scheduling delay), AR10 (partial: engine owns USB MIDI), NFR1 (partial)

**Runs on:** Dev board (ALSA DLL); CoreMIDI path already on Mac from 1.10

**Acceptance Criteria:**

**Given** a class-compliant USB MIDI keyboard on the dev board's USB host port
**When** the engine starts
**Then** an input edge thread owned by `engine` opens it through ALSA (UMP where available, MIDI 1.0 converted to UMP otherwise) and stamps every event on arrival with `CLOCK_MONOTONIC_RAW` nanoseconds
**And** events reach the RT plane only through an SPSC queue

**Given** timestamped events
**When** the RT plane maps device time to frames
**Then** a DLL on the ALSA hardware pointer converts device time to a frame position, and each event is applied at its exact in-block offset after a fixed scheduling delay (configured, initially one period)
**And** the offline renderer uses the same mapping code

**Given** a test that injects events with known timestamps at random sub-block positions for 1 h
**When** onset positions in the output are measured
**Then** the jitter between event time + fixed delay and audio onset is ≤1 frame
**And** the DLL stays locked without drift-induced reordering over the hour

**Given** the keyboard is unplugged and replugged while the engine runs
**When** the device returns
**Then** the input thread reopens it without disturbing audio, and notes held at unplug are released

### Story 1.21: In-RAM diagnostics counters and latency histogram

As a firmware developer,
I want the engine to keep xrun, steal and stream-underrun counters and a latency histogram in RAM and export them on request,
So that soak runs, the demo and later field units can prove or diagnose audio health.

**Covers:** FR120 (export through a dev CLI now; Velora Studio export in Epic 9), AR44, NFR15 (partial: no disk writes during performance)

**Runs on:** Mac + dev board

**Acceptance Criteria:**

**Given** the running engine
**When** an xrun, a steal or a stream underrun happens
**Then** the matching counter increments on the RT thread with an atomic operation and no syscall
**And** a block-render-time histogram and an event-to-onset latency histogram (from Story 1.20 timestamps) are updated in RAM

**Given** a minimal `proto/` with a versioned `ws.v1` Diagnostics request and response (only the messages this story needs)
**When** a dev CLI (`velora-diag`) sends the request over the engine's Unix domain socket
**Then** it receives all counters, both histograms and the engine uptime
**And** the control thread, not an RT thread, serializes the response

**Given** a 1 h performance run
**When** disk writes are monitored
**Then** the engine writes nothing to disk; counters and RT log records stay in RAM and RT log records are drained by the control thread

### Story 1.22: 24-hour soak gate on the dev board

As a founder,
I want an automated 24-hour stress run at 256 voices with the SSD near full that must report zero dropouts,
So that every release candidate proves stage reliability before it ships to the demo or to units.

**Covers:** NFR4, AR2 (24 h soak gate), FR120 (used as the soak oracle)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** the dev board with the CI-built dev image, the SSD filled to ≥90% with bundles, and active cooling
**When** the soak job starts
**Then** a stress player sends a scripted MIDI pattern that keeps ≥256 stereo sample voices sounding across at least 100 different instruments and forces steals, through the same USB MIDI path as Story 1.20 (USB MIDI loopback from a second host or a USB gadget-capable sender)
**And** it runs for 24 h

**Given** the run finished
**When** the job reads the counters from Story 1.21
**Then** it passes only with zero xruns and zero stream underruns, and attaches the histograms, steal count, SoC temperatures and the image version to the CI run
**And** any non-zero counter fails the release gate

**Given** a failed run
**When** the job reports
**Then** it includes the time of each xrun or underrun and the block-time histogram around it so the cause can be investigated

### Story 1.23: Milestone 1 investor demo proving NFR1–NFR4

As a founder demoing to investors,
I want a repeatable demo on the dev board where a keyboardist plays a deep library from a USB MIDI keyboard with measured ≤5 ms latency at full polyphony,
So that investors see the core claim proven with numbers, not promises.

**Covers:** NFR1, NFR2, NFR3, NFR4, NFR50 (Milestone 1), FR1, FR2, FR4, FR5, FR6, FR7, FR120

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** a latency rig: a sender that toggles a GPIO at the instant it transmits a USB MIDI Note On to the dev board, and a scope or second interface capturing the GPIO and the analog output
**When** 1,000 notes are measured while 256 other voices are sounding
**Then** USB-MIDI-arrival-to-analog-output latency is ≤5 ms for every note, and the report gives min/median/p99/max with a budget breakdown (USB poll, scheduling delay, render, output buffering, codec)
**And** the keyboard-inclusive latency of the demo keyboard is measured separately and reported for information

**Given** the dev board with ≥1 TB of indexed bundles on NVMe and 16 GB RAM
**When** the demo script runs (load piano and VSCO2 instruments, play dense passages, show the engine's live voice count and counters from `velora-diag`)
**Then** instruments load from the index without scanning, ≥256 stereo voices stream at once, and counters show zero xruns and zero underruns during the demo
**And** the most recent passing 24 h soak report from Story 1.22 for the same image is part of the demo pack

**Given** the demo pack
**When** it is assembled
**Then** it contains the RT spike, audio, DSI and secure-boot reports, the latency report, the soak report and a one-page result sheet stating NFR1–NFR4 pass/fail with numbers
**And** the demo can be reproduced from a clean flash of the tagged image by following a written runbook

## Epic 2: Organ and effects

A keyboardist plays the tonewheel organ with 9 drawbars, percussion, key click, vibrato/chorus, leakage, multi-contact onset and the rotary speaker, never voice-stolen, and shapes every part with an ordered insert chain, reverb and delay sends and a master section, edited on the device's Effects surface. Organ fidelity is accepted against Hammond M-solo renders and a blind ABX panel. Development is Mac-first: all DSP lives in `engine/core` and is built, null-tested and played live on the developer's Apple-silicon Mac through the Epic 1 host adapters; the device UI is built in LVGL's SDL simulator at 1024×600 on the Mac. CPU budgets are accepted only on the dev board. Until the panel (Epic 3) and the Live Set format (Epic 4) exist, drawbars, rotary and FX parameters are driven by MIDI CC from a USB MIDI controller and parts come from an in-memory engine configuration loaded by the control thread.

### Story 2.1: Parameter registry and control-thread snapshots for engine settings

As a firmware developer,
I want a generated parameter registry and a control thread that is the only writer of structural engine settings, handing them to the RT threads as immutable snapshot generations,
So that organ and effect parameters have exactly one owner and every later client (UI, panel, Velora Studio) edits them the same way.

**Covers:** AR11 (partial: control thread as single writer, snapshot generations swapped by atomic pointer), AR12 (partial: registry generator, structural vs performance classes, RT last-value table), AR13 (partial), AR25 (partial: `ws.v1` command/state messages for parameters), NFR17, NFR44

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a parameter definition file in `proto/` (id, name key, class structural or performance, type, range, default, unit)
**When** the build runs
**Then** a generator emits C++ tables for `engine/core` and `engine/host` and the matching `ws.v1` enums, and the build fails if a `param_id` is declared twice or without a class
**And** only the parameters this epic needs are declared (organ, rotary, inserts, sends, master, per-part output), each with a stable numeric id

**Given** the engine running on the Mac (`engine-host` from Story 1.10)
**When** a client sends a `ws.v1` structural command (for example "set part 2 insert chain") over the engine's Unix domain socket
**Then** the control thread validates it against the registry, builds a new immutable snapshot generation, swaps it in with one atomic pointer store, and replies with `request_id` and the new revision
**And** an invalid command returns a closed-enum error code and changes nothing

**Given** a performance parameter (for example drawbar 3 of part 4)
**When** it changes from a MIDI CC or a `ws.v1` command
**Then** it reaches the RT threads only as a timestamped UMP event, the RT threads own its live value, and they publish it in a last-value table that the control thread reads without locks
**And** the RTSan build shows no allocation, lock or blocking call on the RT path while 10,000 parameter changes and 100 snapshot swaps are applied

**Given** an in-memory engine configuration of up to 8 parts (timbre reference, insert chain, send levels, output) loaded from a committed dev file at engine start
**When** the engine starts on the Mac or the dev board
**Then** the control thread publishes it as the first generation, and a later swap never frees a generation while a voice still references it (lifetime rule per AD-23, tested with a held note across a swap)

### Story 2.2: Tonewheel generator and drawbar voices at full polyphony

As a keyboardist,
I want a tonewheel organ that plays all 73 keys with all 9 drawbars at once and is never voice-stolen,
So that I can play full organ chords and glissandos without any note dropping out.

**Covers:** FR8, FR6 (organ floor), AR14 (phase 2: per-part shared generator), AR15 (organ engine behind the single interface), AR18 (organ reservation never stolen), NFR28, NFR29

**Runs on:** Mac (DSP, null and unit tests); dev board (CPU cost)

**Acceptance Criteria:**

**Given** the organ engine in `engine/core`, implemented clean-room (no setBfree or other GPL source consulted for code; setBfree may run only as an unshipped oracle)
**When** a part is assigned the organ
**Then** a per-part shared tonewheel generator runs in block phase 2 producing the full wheel set with the tonewheel frequency ratios of the reference organ, and each key voice in phase 3 sums its 9 drawbar footages (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1') from the shared wheels with foldback at the top and bottom of the range
**And** voices write only their own state and their worker's scratch buffer (AD-6)

**Given** drawbar values 0–8 as performance parameters mapped to a configurable MIDI CC set
**When** a drawbar CC arrives while keys are held
**Then** the drawbar level changes without zipper noise (smoothed per block) at the event's in-block offset

**Given** a Live Set configuration with an organ part
**When** it loads
**Then** the arbiter reserves the organ and rotary cost for 73 simultaneous keys ahead of every engine floor, and an organ key voice is never chosen as a steal victim, verified by a test that fills the pool with sample voices while all 73 organ keys are held
**And** the organ's declared cost per voice and per shared generator is measured on the dev board and recorded with the engine

**Given** the engine and UI strings
**When** they are reviewed
**Then** no user-visible or log string uses "Hammond", "Leslie" or "B-3"; the engine is named "tonewheel organ"

### Story 2.3: Organ percussion and key click

As a keyboardist,
I want the organ's percussion (2nd/3rd harmonic, fast/slow decay, normal/soft) and key click,
So that the organ has the attack and articulation of a real tonewheel console.

**Covers:** FR9 (percussion, key click), NFR28

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the organ engine from Story 2.2
**When** percussion is on with harmonic 2nd or 3rd, decay fast or slow and volume normal or soft
**Then** percussion is single-triggered: it sounds on a key press only when no other organ key is held (legato playing does not retrigger it), and its decay follows the selected time constant
**And** with percussion on, the 1' drawbar is cancelled and normal volume lowers the drawbar level as on the reference organ, both behaviours unit-tested

**Given** key click level as a parameter (0 = off)
**When** a key closes and opens
**Then** a short broadband click is generated per contact event at the key voice's onset and release, scaled by the click level and varying per key so repeated notes are not identical

**Given** the offline renderer
**When** a committed MIDI test file exercises percussion settings and key click
**Then** the render is deterministic (bit-identical across runs) and stored as a golden file for regression

### Story 2.4: Vibrato/chorus scanner and tonewheel leakage

As a keyboardist,
I want the vibrato/chorus scanner (V1–V3, C1–C3) and adjustable tonewheel leakage,
So that the organ has the motion and the slightly impure tone of the real instrument.

**Covers:** FR9 (vibrato/chorus scanner, leakage), NFR28

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the organ part's summed output
**When** vibrato/chorus is set to V1, V2, V3, C1, C2 or C3
**Then** a scanner modelled as a tapped delay line swept at the reference rate modulates the organ signal, with chorus settings mixing the dry and scanned signals
**And** switching settings while keys are held produces no click

**Given** leakage as a parameter (0 = off)
**When** keys are held
**Then** each sounding key adds low-level contributions from neighbouring wheels with a fixed per-unit random pattern (seeded, reproducible), scaled by the leakage parameter
**And** with leakage 0 a single drawbar on a single key renders as a pure tone within the null-test tolerance against a synthetic sine reference

**Given** the RTSan build
**When** all scanner settings are cycled during a 73-key chord
**Then** no allocation, lock or blocking call occurs on the RT path

### Story 2.5: Multi-contact onset hook in the organ

As a keyboardist,
I want the organ to start sounding on the first key contact and follow the timing of each successive contact,
So that fast organ playing, smears and half-pressed keys feel like a real multi-contact keyboard.

**Covers:** FR11, AR16 (partial: organ consumes per-note `contact_k` controllers), NFR1 (organ onset on first contact)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** UMP events carrying a vendor per-note `contact_k` controller per contact closure followed by a Note On (the format the keybed reader in Epic 3 will emit)
**When** the organ receives a `contact_k` for contact 1 of a key
**Then** the key voice starts sound at that event's in-block offset, before any Note On
**And** each later contact closure brings in its drawbar busbar group at that contact's time, following the reference behaviour of the Hammond M-solo Virtual Multi-Contact (documented mapping of contacts to footages in the engine README)

**Given** a key released contact by contact
**When** `contact_k` open events arrive
**Then** busbar groups drop out in reverse order at each contact's time, and the voice ends when the last contact opens

**Given** an input that sends only Note On/Off (USB MIDI keyboard, DIN)
**When** a note plays on the organ
**Then** all busbars start together at the Note On and the organ behaves normally, so the hook is optional per source

**Given** synthetic `contact_k` sequences with 1 ms, 5 ms and 20 ms spacing in a unit test
**When** they render offline
**Then** the onset of each busbar group lands on the exact frame of its contact event

### Story 2.6: Rotary speaker in the organ voice path

As a keyboardist,
I want the organ to play through a rotary speaker with slow, fast and stop and realistic speed ramps,
So that I can switch the rotary speed mid-phrase like on a real cabinet.

**Covers:** FR10 (partial: DSP, speed control by UMP and the reported state; ROTARY buttons arrive in Epic 3, Status Bar and Organ Card display in Epic 4), FR50 (rotary speaker), NFR28, NFR29

**Runs on:** Mac

**Acceptance Criteria:**

**Given** an organ part
**When** it renders
**Then** its output passes through a rotary speaker stage modelled clean-room as a horn and a drum with separate crossover, rotation (amplitude, Doppler, stereo spread) and independent acceleration and braking times
**And** the rotary cost is part of the organ reservation from Story 2.2

**Given** rotary speed as a performance parameter with values slow, fast and stop
**When** it changes (MIDI CC from a controller or a `ws.v1` command)
**Then** horn and drum ramp to the new speed with their own time constants, and stop brakes both to a standstill without a click
**And** the current speed state (`SLOW`, `FAST`, `STOP`) is published in the engine state so the UI can show `ROTARY SLOW/FAST`

**Given** the engine identifiers and user strings
**When** they are reviewed
**Then** the effect is named "rotary speaker" (UI label ROTARY), never "Leslie"

### Story 2.7: Fixed routing topology with insert chains, sends, master and output buses

As a keyboardist,
I want every part to pass through its own ordered insert chain, two global sends and a master section to the Main or Aux bus,
So that effects behave the same on every part and I can add, remove, reorder and bypass them without clicks or dropouts.

**Covers:** AR22, FR51 (chain mechanics: add, remove, reorder, bypass), AR18 (FX budget reserved first), AR14 (phase 5: sum, per-part FX, sends, master), FR40 (partial: line-in input port mixed into the master only), FR38 (partial: per-part Main/Aux buses; physical outputs in Epic 3)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the block loop from Epic 1
**When** phase 5 runs
**Then** each part's voices are summed into its part bus, processed by its insert chain in order, sent at per-part send levels to the Reverb and Delay buses, and the part bus plus send returns are summed into the Main bus or the Aux bus per the part's output setting, then through the master section
**And** a line-in source port (CoreAudio input on the Mac, silent on the dev board until Epic 3) is mixed into the master only, at a line-in level parameter

**Given** a part's insert chain as structural state (up to 5 slots, each one of rotary speaker, chorus/phaser, EP amp/drive, EQ, compressor, with a bypass flag)
**When** a command adds, removes, reorders or bypasses an effect while notes sound
**Then** the control thread publishes a new generation with the effect instances pre-allocated off the RT thread, and RT cross-fades from the old chain to the new one over a short ramp, with no click and no allocation on the RT path
**And** the effect types available are only those listed (no plugin loading)

**Given** a Live Set configuration
**When** the arbiter reserves its budget
**Then** the FX budget for the configured inserts, sends and master is reserved before the organ reservation and engine floors, and a command that would exceed the FX budget is refused with a typed error

**Given** a placeholder pass-through effect in every slot type until its DSP lands
**When** the routing tests run in the offline renderer
**Then** Main/Aux assignment, send levels and line-in-to-master are verified by null tests against analytically mixed references

### Story 2.8: EQ and compressor as inserts and in the master section

As a keyboardist,
I want a parametric EQ and a compressor on any part and on the master,
So that I can fit each part into the mix and control the overall level on stage.

**Covers:** FR50 (EQ, compressor), FR51 (EQ and compressor inserts), FR53

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the EQ effect
**When** it is placed in an insert slot or the master section
**Then** it provides Low shelf, Mid peak with Mid freq, and High shelf bands (gain ±15 dB), smoothed per block, with a magnitude response within ±0.5 dB of the design curve at test frequencies
**And** the part EQ bands are the same parameters as the fixed encoder functions LOW, MID, MID FREQ, HIGH so the panel can drive them later

**Given** the compressor effect
**When** it is placed in an insert slot or the master section
**Then** it provides threshold, ratio, attack, release, makeup gain and a gain-reduction value published in the engine state
**And** a step-input unit test shows the attack and release times within 10% of their settings

**Given** the master section
**When** a Live Set is active
**Then** master EQ and master compressor are always present in that order after the send returns, each with its own bypass

### Story 2.9: Chorus/phaser, EP amp/drive and rotary speaker inserts

As a keyboardist,
I want chorus/phaser, an electric-piano amp/drive and the rotary speaker as insert effects on any part,
So that electric pianos, pads and non-organ parts get their classic treatments.

**Covers:** FR50 (chorus/phaser, EP amp/drive, rotary speaker as an effect), FR51 (full insert set), NFR28, NFR29

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the chorus/phaser effect
**When** it is inserted on a part
**Then** it offers a chorus mode and a phaser mode with rate, depth, feedback (phaser) and mix, and mode changes cross-fade without a click

**Given** the EP amp/drive effect
**When** it is inserted on a part
**Then** it applies drive, tone and an amp/cabinet colouring with output level, using oversampling where the nonlinearity needs it, and declares its cost to the FX budget

**Given** the rotary speaker from Story 2.6
**When** it is inserted on a non-organ part
**Then** the same DSP runs as an insert with its own speed parameter, and an organ part keeps its built-in rotary without a second instance unless the user inserts one

**Given** each new effect
**When** the RTSan build and the routing null tests run
**Then** they pass, and each effect has a committed golden render

### Story 2.10: Reverb send

As a keyboardist,
I want a global reverb with type, time and level and a reverb send on every part,
So that I can place all parts in one room, and later ride the reverb with the REV fader.

**Covers:** FR52 (Reverb: type, time, level, per-part send), FR50 (reverb)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the Reverb send bus
**When** a reverb type is chosen (at least Room, Hall, Plate)
**Then** the reverb renders a stereo tail whose RT60 tracks the time parameter within 15% at 1 kHz, measured by an impulse-response test
**And** changing type while a tail rings cross-fades without a click

**Given** per-part reverb send levels as performance parameters
**When** a send level changes
**Then** only that part's contribution changes, smoothed per block
**And** the reverb send of all parts together is exposed as one REV master send level parameter so the 9th fader in MIXER can drive it later

**Given** the reverb at its longest time on all 8 parts
**When** the dev-board cost is measured
**Then** the reverb's declared FX cost covers the measured worst block time

### Story 2.11: Delay send with tempo sync

As a keyboardist,
I want a global delay with time, tempo sync, feedback and level and a delay send on every part,
So that echoes follow the song tempo.

**Covers:** FR52 (Delay: time, tempo sync, feedback, level, per-part send), FR50 (delay)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the Delay send bus
**When** tempo sync is off
**Then** delay time is set in milliseconds (up to at least 2 s) and changes glide without clicks

**Given** tempo sync on with a note value (1/4, 1/8, dotted and triplet values)
**When** the RT plane's current tempo value changes
**Then** the delay time follows the tempo at the next block, and the tempo is read from the RT plane's single tempo value (internal tempo now; external clock following arrives in Epic 7)

**Given** feedback set near maximum
**When** the delay runs for 10 min
**Then** the output never exceeds full scale and never runs away (feedback is limited below unity)

**Given** per-part delay send levels as performance parameters
**When** the fixed encoder function DELAY is later bound to a part's delay send
**Then** the parameter id used is the registry id defined here

### Story 2.12: Full effects load within the latency and dropout budget

As a founder,
I want proof on the dev board that an 8-part Live Set with every effect active at full polyphony still meets the latency and dropout targets,
So that effects never become the reason a show drops audio.

**Covers:** NFR6, NFR1 (partial: render budget with FX), NFR4 (partial: soak with FX), AR2 (soak gate extended)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** a worst-case configuration: 8 parts, each with a full 5-slot insert chain, reverb and delay sends on every part, master EQ and compressor, an organ part with all 73 keys held, and ≥256 stereo sample voices streaming
**When** the per-block render time is measured for 1 h
**Then** the worst block finishes within the Story 1.18 limit (70% of the 1.333 ms period)
**And** if it does not, the report identifies the effect costs and the FX budget values are lowered or effects optimised until it does

**Given** the 24 h soak job from Story 1.22
**When** it runs with this worst-case configuration
**Then** it reports zero xruns and zero stream underruns, and the soak gate uses this configuration from now on

### Story 2.13: Organ objective A/B against Hammond M-solo renders

As a founder,
I want automated renders that compare the organ with a Hammond M-solo playing the same MIDI sequences,
So that organ fidelity is measured against a reference, not judged by ear alone.

**Covers:** NFR20, EXT6 (dependency: M-solo drawbar CC/NRPN numbers), NFR28

**Runs on:** Mac (renders and analysis) + M-solo recording rig

**Blocked by:** Story 11.13 (target month 8)

**Acceptance Criteria:**

**Given** the M-solo drawbar, percussion, vibrato/chorus and rotary CC/NRPN map (EXT6) transcribed into a committed map file
**When** a script plays a committed MIDI test suite (single keys across the range per drawbar, registrations, percussion settings, key click, leakage, vibrato/chorus settings, rotary slow/fast/stop) into the M-solo and records its output through a reference interface at 48 kHz
**Then** reference recordings are stored with their settings, and the same suite renders through the offline renderer with matching settings

**Given** both render sets
**When** the analysis runs
**Then** it compares long-term spectrum per registration, key-click energy and duration, percussion decay time, leakage level and rotary modulation rate, against tolerances derived from the variation between repeated M-solo takes
**And** a report lists pass/fail per metric and per test case

**Given** the analysis passes
**When** it is committed
**Then** it runs in CI on every change to the organ engine using the stored reference recordings (the M-solo itself is not needed in CI)

### Story 2.14: Organ blind ABX listening test with professional keyboardists

As a founder,
I want a blind ABX test where 3 to 5 professional keyboardists compare the organ with the reference,
So that the organ is accepted by the players who will judge it on stage.

**Covers:** NFR21, EXT7 (dependency: panel recruitment; optional real B-3 + rotary recording), NFR20 (input material)

**Runs on:** Mac

**Blocked by:** Story 11.14 (target month 11)

**Acceptance Criteria:**

**Given** matched excerpts from Story 2.13 (and real B-3 through rotary cabinet recordings when EXT7 provides them), loudness-matched
**When** a written ABX protocol is followed (randomised order, listener blind to source, at least 16 trials per listener, same playback system for all)
**Then** each listener's correct-identification rate and a 1–5 rating per excerpt are recorded

**Given** the results of 3–5 professional keyboardists
**When** they are analysed
**Then** the test passes if the panel cannot reliably distinguish the organ from the reference (no listener above the p < 0.05 binomial threshold) or the mean rating is ≥4/5
**And** a report records the protocol, listeners (anonymised, with LGPD consent), raw results and the verdict, and lists the excerpts that failed for engine follow-up

### Story 2.15: Design story — Effects surface mockup

As a keyboardist,
I want the Effects surface drawn before it is built,
So that inserts, sends, master and outputs are laid out legibly and consistently with the rest of the device.

**Covers:** UX-DR100, UX-DR61 (design), UX-DR10 (other-surface skeleton), UX-DR19, UX-DR22, UX-DR34, UX-DR87, UX-DR88, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockup in `mockups/`)

**Acceptance Criteria:**

**Given** DESIGN.md tokens and components and EXPERIENCE.md › Effects
**When** the mockup `mockups/effects-surface.html` is drawn at 1024×600
**Then** it shows the skeleton Top Bar 56 · content 488 · Hint Bar 56 and four sections: Part inserts (part selector 1–8, ordered chain rows with add, remove, reorder by hold ▲▼ and bypass), Sends (Reverb type/time/level, Delay time/tempo sync/feedback/level, per-part send levels), Master (EQ, compressor, line-in level), Outputs (Main/Aux per part as Segmented Controls)
**And** the Hint Bar lists only the controls active on each section (`VALUE`, `WRITE`, `EXIT`) with localized verbs

**Given** the mockup
**When** it is reviewed
**Then** it is shown in Stage and Studio themes, with pseudo-localization at +40% and the string `ÁÇÊÕÑ Ação`, every text pair meets ≥4.5:1 and control boundaries ≥3:1, touch targets are ≥56 px, and no state relies on colour alone (bypass shown by text and shape)
**And** microcopy follows Voice and Tone (ROTARY, fixed vocabulary) and EXPERIENCE.md is updated to link the mockup

### Story 2.16: Design story — organ section of the Part surface

As a keyboardist,
I want the organ's sound section of the Part surface drawn before the Part surface is built,
So that drawbars, percussion, key click, vibrato/chorus, leakage and rotary read clearly on one screen.

**Covers:** UX-DR99 (partial: organ Sound section; the full Part surface design is in Epic 4), UX-DR3, UX-DR21, UX-DR60 (design, organ fields), UX-DR87, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockup)

**Acceptance Criteria:**

**Given** EXPERIENCE.md › Part (list-left / fields-right) and the Drawbar Column tokens
**When** the mockup `mockups/part-surface-organ.html` is drawn at 1024×600
**Then** the Sound section for an organ part shows the 9 drawbars (theme-invariant head colours, per-theme edge rule, tabular numerals and the numeric registration `88 8600 000`), percussion (on/off, 2nd/3rd, fast/slow, normal/soft), key click level, vibrato/chorus (V1–V3, C1–C3, off), leakage level and rotary (SLOW/FAST/STOP) as Segmented Controls and value fields
**And** the header reads `PART n · timbre · ORGAN` with ⏻ and ★

**Given** the mockup
**When** it is reviewed
**Then** it passes the same theme, contrast, pseudo-localization and never-colour-only checks as Story 2.15, and uses ROTARY wording only

### Story 2.17: Device UI shell on the LVGL simulator and the dev board

As a firmware developer,
I want the `ui` process running in LVGL's SDL simulator on the Mac and on DRM/KMS on the dev board, connected to the engine over `ws.v1`,
So that every device surface is built and reviewed on the Mac first and runs unchanged on the board.

**Covers:** AR10 (partial: `ui` as client only), AR45 (partial: locale catalog with EN source), UX-DR93 (partial), NFR30 (partial), NFR16 (partial)

**Runs on:** Mac (LVGL SDL simulator 1024×600); dev board (DRM/KMS on the Story 1.5 panel)

**Acceptance Criteria:**

**Given** `ui/` with LVGL 9.6
**When** it builds for macOS with the SDL backend and for the dev image with the DRM/KMS + evdev backend
**Then** the same UI sources render a 1024×600 window on the Mac and full screen on the dev board panel, and only the display/input adapters differ

**Given** the `ui` process
**When** it connects to the engine over the `ws.v1` Unix domain socket (engine-host on the Mac, `engine` on the board)
**Then** it receives a full state snapshot then deltas, sends only commands, and every user-facing string is a locale-catalog key (a lint fails the build on hard-coded strings)
**And** killing the `ui` process while notes sound does not interrupt audio, and the restarted `ui` resyncs from the snapshot

### Story 2.18: Design tokens, Stage and Studio themes and the contrast gate

As a firmware developer,
I want one token generator that turns the DESIGN.md frontmatter into the LVGL theme, with a CI contrast gate,
So that every surface uses the same colours, type and spacing and no legibility regression can ship.

**Covers:** UX-DR1, UX-DR2, UX-DR5, UX-DR9, UX-DR11, UX-DR87, NFR31

**Runs on:** Mac (generator, simulator and CI); dev board (visual check on the Story 1.5 panel)

**Acceptance Criteria:**

**Given** the DESIGN.md frontmatter tokens
**When** the token generator under `tools/tokens/` runs in the build
**Then** it is the project's single token generator: it emits the LVGL theme module for `ui` with the Stage (dark) and Studio (light) colour sets, the theme-invariant drawbar, LED and logo tokens, the typography scale (fonts built with ascent measured from Á/Ç/Ñ/Ê, tabular numerals), radius and spacing tokens and tonal elevation as LVGL styles
**And** its outputs are pluggable, so later targets (such as the Velora Studio CSS output) are added to this generator rather than written separately

**Given** the generated styles
**When** a theme switch is commanded at runtime (developer entry point)
**Then** every widget restyles without restart
**And** no colour literal exists in `ui` source outside the generated files (checked by a lint rule in CI)

**Given** the generated token pairs
**When** the CI contrast check runs over both themes
**Then** it fails the build below 4.5:1 for any text pair or 3:1 for any meaningful boundary, and on any style that applies opacity to text

### Story 2.19: Hint Bar, List Row, Segmented Control and the Mac virtual panel

As a firmware developer,
I want the non-Performance surface skeleton with the Hint Bar, List Row and Segmented Control widgets, and a virtual panel on the Mac,
So that every list-style surface is built from the same parts and driven like the real panel before hardware exists.

**Covers:** UX-DR10 (other-surface skeleton), UX-DR19, UX-DR22 (partial), UX-DR34, NFR32

**Runs on:** Mac (LVGL SDL simulator 1024×600); dev board

**Acceptance Criteria:**

**Given** the skeleton for non-Performance surfaces (Top Bar 56 · content 488 · Hint Bar 56)
**When** a surface declares its active controls
**Then** the Hint Bar shows keycap-outlined English control names with localized verbs, and List Row and Segmented Control widgets support VALUE turn to move/change, WRITE to confirm and touch to select, with the selected item raised and accent-bordered
**And** every touch target in these widgets is ≥56 px

**Given** no physical panel yet
**When** the simulator runs on the Mac
**Then** a virtual panel maps keyboard keys to EXIT, MENU, WRITE, VALUE turn/push, LIVE SET ◀ ▶ and SCENE ◀ ▶ and delivers them to `ui` as the same `PanelEvent` messages the real panel will produce

### Story 2.20: Effects surface — part inserts

As a keyboardist,
I want to add, remove, reorder and bypass each part's insert effects on the device screen,
So that I can shape every part's sound without a computer.

**Covers:** FR113 (part inserts), FR51, UX-DR61 (Part inserts), UX-DR22, UX-DR34, UX-DR19, UX-DR79 (partial: VALUE, WRITE, EXIT on this surface), UX-DR88, UX-DR97, UX-DR98

**Runs on:** Mac (simulator + engine-host); dev board

**Acceptance Criteria:**

**Given** the Effects surface built from the Story 2.15 mockup (opened by a developer entry point until the Launcher and the `REV` shortcut arrive in Epic 4)
**When** the Part inserts section is shown
**Then** the player selects part 1–8 and sees its chain as ordered List Rows with effect name and a bypass Segmented Control, plus an "add effect" row offering only the 5 insert types

**Given** a selected chain row
**When** the player bypasses, removes, or reorders it (hold ▲▼ or drag) by VALUE/WRITE or touch
**Then** a `ws.v1` command is sent, the change is heard immediately without a click, and the screen reflects the engine's published state (not a local guess)
**And** an edit the engine refuses (for example FX budget exceeded) shows its reason in the Hint Bar and the chain is unchanged

**Given** an insert row is opened
**When** its parameters are shown
**Then** each parameter is a field VALUE edits, with units formatted per locale, and the values come from the parameter registry ranges

**Given** EXIT on this surface
**When** it is pressed
**Then** it goes one level back (parameter view → chain → the surface it was opened from) and never discards the edits, which stay in memory

### Story 2.21: Effects surface — sends, master and outputs

As a keyboardist,
I want to set the reverb and delay, each part's send levels, the master EQ and compressor, the line-in level and each part's Main/Aux output on the device,
So that the whole mix is controllable from the instrument.

**Covers:** FR113 (sends, master, outputs), FR52, FR53, FR40 (partial: line-in level control in Effects › Master), FR38 (partial: per-part Main/Aux selection), UX-DR61 (Sends, Master, Outputs), UX-DR34, UX-DR88, UX-DR95 (CLDR number/unit formatting), UX-DR97, UX-DR98

**Runs on:** Mac (simulator + engine-host); dev board

**Acceptance Criteria:**

**Given** the Sends section
**When** the player edits Reverb type, time and level, Delay time, tempo sync, feedback and level, and per-part reverb and delay send levels
**Then** each change is sent as a `ws.v1` command and heard immediately, and values display with CLDR formatting per locale (`35 %` in ES, `35%` in EN)

**Given** the Master section
**When** the player edits master EQ bands, compressor settings and bypasses, and the line-in level
**Then** the changes are heard immediately, and the compressor's gain reduction is shown as a live value

**Given** the Outputs section
**When** the player sets a part to Main or Aux with a Segmented Control
**Then** the part's output bus changes without a click and the selection is shown by the filled segment and its text label

**Given** the whole Effects surface
**When** it is reviewed in both themes with pseudo-localization at +40%
**Then** no text clips, all touch targets are ≥56 px, and WRITE held past the Write hold is reserved for the Write Dialog (wired in Epic 4) and does nothing else here

## Epic 3: Physical instrument

A keyboardist plays the real prototype: the Fatar keybed with attack and release velocity, aftertouch where present and multi-contact timing; the panel with faders, encoders with LED rings, toggles, buttons and SoC-driven LEDs; four balanced outputs, headphones and line in through the PCM3168A codec as clock master; MIDI DIN; pedals; wheels; pop-free mute and power-fail protection; 24 h in the closed chassis without throttling. Development is Mac-first where the work is portable: the frame protocol, the SoC-side readers and the velocity/calibration logic are built and tested on the Mac against recorded and simulated MCU frames. MCU firmware starts on a dev kit of the candidate MCU family wired to a keybed or panel harness; codec, power and thermal work need the prototype board. Several stories depend on hardware partner deliverables (EXT10: MCU family by month 7, keybed contact count and aftertouch, POWER_FAIL timing, codec reference design, thermal result) and the physical panel specification (EXT12); each such story names the dependency.

### Story 3.1: Versioned MCU frame protocol library and control/LED ID maps

As a firmware developer,
I want one frame protocol implementation shared by both MCUs and the SoC, with published control and LED ID maps,
So that the keybed, the panel and the engine agree on every byte from the first firmware.

**Covers:** AR42, NFR44 (MCU protocol versioned), AR29 (partial: MCU protocol in the versioned forms), EXT10 (published ID maps handed to partners)

**Runs on:** Mac (host tests, fuzzing); compiles for the MCU target

**Acceptance Criteria:**

**Given** a portable C library under `mcu/common/` (no OS, no heap) and a C++ wrapper for `engine/host`
**When** a frame is encoded
**Then** it has sync `0xA5`, protocol version, type, length, 16-bit seq, 32-bit tick, payload and CRC-16/CCITT over version-to-payload, all little-endian
**And** the types `HELLO`, `KEY_CONTACT`, `AFTERTOUCH`, `CONTROL`, `BUTTON`, `LED_SET`, `TIME_SYNC` and `ENTER_BOOTLOADER` encode and decode round-trip in unit tests

**Given** a byte stream with noise, truncated frames, bad CRCs and seq gaps
**When** the streaming decoder runs (including a 1-hour fuzz run on the Mac)
**Then** it resynchronises on the next valid sync byte, never crashes or reads out of bounds, reports CRC errors and seq gaps as counters, and never delivers a corrupt frame

**Given** a frame with an unknown protocol major version
**When** it is decoded
**Then** it is rejected with a typed error and a version-mismatch counter increments, and N−1 frames are accepted

**Given** the panel control set in HARDWARE-INTERFACES §4
**When** the ID maps are generated from one source file
**Then** every control (8 encoders, VALUE encoder, 9 faders, 2 wheels, 3 pedals), button (8 part/scene, LIVE SET ◀ ▶, SCENE ◀ ▶, EXIT, MENU, WRITE, VALUE push, ROTARY STOP, ROTARY S/F, 3 toggles) and LED (toggle state LEDs, part/scene LEDs, 8 encoder rings) has one ID, and the maps are published as a versioned document for the hardware partners

### Story 3.2: Prototype board bring-up

As a firmware developer,
I want the prototype board booting the Yocto dev image with display, touch, NVMe, USB, both MCU links and MIDI UART working,
So that all hardware-bound stories have a real board to run on.

**Covers:** AR9 (partial: USB peripheral role in the DT), AR3 (partial: dev image for the prototype machine), AR24 (partial: tier ID strap readable), NFR38, NFR41, NFR43, EXT10 (dependency: SoM part number, board files, tier-ID strap, serial EEPROM)

**Runs on:** Linux build host or ARM64 Linux VM (image); prototype board

**Blocked by:** Story 11.7 (target month 7)

**Acceptance Criteria:**

**Given** the partner's board files and the industrial SoM
**When** a Yocto machine for the prototype board is added and the dev image is built
**Then** it boots the Linux 6.18 LTS PREEMPT_RT kernel with the Epic 1 RT configuration on mainline drivers for audio, USB, PCIe and display paths, and a check script lists each peripheral as present or missing

**Given** the booted board
**When** the check script runs
**Then** the 7" 1024×600 panel and touch work, the NVMe is detected at PCIe Gen3 x4, USB-C enumerates in device role, USB-A host supplies ≥900 mA, both MCU links (UART or SPI + IRQ) pass a frame loopback test with the Story 3.1 frame library, the MIDI UART is present, and the tier ID strap and serial EEPROM are readable
**And** no Wi-Fi or Bluetooth interface exists

**Given** the board
**When** the Story 1.1 cyclictest load profile runs for 1 h
**Then** max latency on the isolated cores stays within the Story 1.1 threshold

### Story 3.3: Keybed MCU firmware with raw per-contact timestamps

As a keyboardist,
I want the keybed scanned at ≥10 kHz with every contact change timestamped and sent to the SoC within 200 µs,
So that the SoC can measure velocity and multi-contact timing precisely.

**Covers:** AR40, NFR42, NFR1 (partial: contact-to-frame ≤200 µs), EXT10 / EXT2 (dependency: MCU family, contact count and aftertouch of the 73SW stock)

**Runs on:** MCU dev kit of the candidate family wired to a 73SW keybed harness; then the prototype board

**Blocked by:** Story 11.6 (target month 7), Story 11.7 (target month 7)

**Acceptance Criteria:**

**Given** firmware under `mcu/keybed/` for the MCU family chosen by the partners (a candidate dev kit until EXT10 confirms it)
**When** it runs with a 73SW keybed connected
**Then** it scans every contact (2 or 3 per key, as the keybed has) at ≥10 kHz, timestamps each change with a free-running tick of ≥1 MHz, and sends one `KEY_CONTACT` frame per change (key index, contact index, closed/open)
**And** the MCU computes no velocity and holds no calibration

**Given** power-up
**When** the link comes up
**Then** the MCU sends `HELLO` with firmware version, protocol version, board ID and keybed contact count, and answers every `TIME_SYNC` with its current tick

**Given** a logic analyser on a contact line and the link
**When** 10,000 contact changes are measured, including all keys pressed at once
**Then** contact change to frame complete on the wire is ≤200 µs for every event, and no event is lost (seq has no gaps)

**Given** the MCU firmware
**When** it is reviewed
**Then** it runs no duty other than key scanning (aftertouch, if present, is handled in its own story)

### Story 3.4: MCU dual-bank bootloader and reflash from the SoC

As a firmware developer,
I want an immutable dual-bank bootloader on both MCUs that the SoC reflashes over the link only on a version mismatch,
So that MCU firmware ships inside the A/B image and a power cut while flashing never bricks a unit.

**Covers:** AR43, FR48 (partial: owns the dual-bank bootloader and the SoC reflash mechanism; the image manifest and boot-time check build on it in Story 8.4), NFR24 (partial: no external field flashing), EXT10 (dependency: bootloader approach for the chosen MCU family)

**Runs on:** MCU dev kit + dev board (link over UART); then the prototype board

**Blocked by:** Story 11.7 (target month 7)

**Acceptance Criteria:**

**Given** a bootloader under `mcu/bootloader/` that is write-protected after factory programming
**When** an MCU boots
**Then** the bootloader starts the newest bank whose image passes its integrity check, and falls back to the other bank if it does not

**Given** a `sysd` flashing tool on the SoC
**When** it reads the MCU's `HELLO` version and it differs from the firmware packaged in the image
**Then** it sends `ENTER_BOOTLOADER` (or uses the BOOT/RESET GPIO pair), writes the image to the inactive bank, verifies it, marks it active and resets the MCU
**And** when versions match it does nothing

**Given** 50 power cuts at random points during flashing on the dev kit
**When** power returns
**Then** the MCU boots the old or the new firmware every time and the next flash attempt succeeds

**Given** the production configuration
**When** the MCU is inspected
**Then** no external connector can flash it and SWD/JTAG is disabled per the production option bytes

### Story 3.5: Keybed reader on the SoC with drift-tracked device time and contact_k events

As a keyboardist,
I want every key contact turned into device-time UMP events with drift tracking, `contact_k` controllers and Note On/Off,
So that notes land at exactly the right sample and the organ follows each contact.

**Covers:** FR5 (keybed path), FR11 (real contacts drive the organ hook), FR73, AR16 (MCU tick→device time offset/drift with TIME_SYNC, `contact_k` then Note On), AR10 (partial: `engine` owns the MCU links), NFR1 (partial)

**Runs on:** Mac (reader core against recorded frames and a simulated MCU); dev board / prototype board with the keybed MCU

**Acceptance Criteria:**

**Given** an input edge thread in `engine/host` that owns the keybed link (UART or SPI + IRQ)
**When** frames arrive
**Then** it converts each MCU tick to `CLOCK_MONOTONIC_RAW` nanoseconds using an offset and drift estimate refreshed by periodic `TIME_SYNC` round trips, and pushes UMP to the RT plane only through its SPSC queue

**Given** a 12-hour run with a simulated MCU whose clock drifts by ±200 ppm
**When** event times are compared with the simulator's true times
**Then** the converted times stay within 20 µs and events are never reordered

**Given** a calibration table on the SoC describing the keybed model (contact count 2 or 3, which contact pair measures velocity, key index mapping)
**When** a key closes contact by contact
**Then** the reader emits a vendor per-note `contact_k` controller at each closure and a Note On at the contact that completes the velocity measurement, and the matching opens produce `contact_k` open events and a Note Off
**And** switching to a different Fatar model's table changes behaviour with no code change (tested with a 2-contact and a 3-contact table)

**Given** the organ engine from Epic 2 on a part
**When** keys are played on the real keybed
**Then** the organ starts sound on the first contact, and a recorded slow key press shows busbar groups entering at each contact time

**Given** a frame CRC error or seq gap
**When** it is detected
**Then** the reader counts it, never emits a stuck note (a key whose open event was lost is released when the next frame for that key or a periodic key-state check shows it open)

### Story 3.6: Attack and release velocity with selectable curves

As a keyboardist,
I want attack velocity measured from contact times with my chosen curve applied exactly once, and release velocity that changes release samples,
So that the keybed responds evenly across its range and releases sound natural.

**Covers:** FR70, FR71, AR39 (partial: velocity curves and calibration in `sysd` config), NFR44 (config `schema_version`)

**Runs on:** Mac (curve math, tests); prototype board (real keybed)

**Acceptance Criteria:**

**Given** the time between the velocity-measuring contacts for a press
**When** the reader computes attack velocity
**Then** it maps the time through the keybed calibration table and then through the selected velocity curve once, producing a MIDI 2.0 16-bit velocity
**And** no other stage (MCU, engine, sample engine) applies a curve, verified by a test that injects a known contact time and checks the voice's velocity

**Given** the shipped curves (at least Light, Normal, Heavy and Fixed)
**When** each curve is evaluated over the keybed's full measured time range
**Then** it reaches MIDI velocity 1 and 127 (and the 16-bit equivalents) and the 127 steps are spread without gaps or plateaus, checked by a unit test

**Given** the velocity curve stored in the `sysd` config protobuf with `schema_version`
**When** it is changed through a `ws.v1` config command (a CLI for now; the Settings › Keyboard & Pedals › Velocity curve row is added in Story 10.2)
**Then** the reader uses the new curve for the next note without restart, and the setting survives a reboot

**Given** a key release
**When** the release contact times are measured
**Then** the Note Off carries a release velocity, and a sample instrument with release triggers plays a louder release sample for a fast release than for a slow one (render test)

### Story 3.7: Aftertouch read and routed when the keybed has it

As a keyboardist,
I want aftertouch read and routed to the parts when my keybed has it,
So that I can use pressure for expression.

**Covers:** FR72, AR40 (aftertouch), EXT2 / EXT10 (dependency: aftertouch presence on the 73SW stock)

**Runs on:** Mac (routing tests with simulated frames); prototype board if the keybed has aftertouch

**Blocked by:** Story 11.6 (target month 7)

**Acceptance Criteria:**

**Given** a keybed whose `HELLO` reports aftertouch
**When** pressure changes
**Then** the keybed MCU sends `AFTERTOUCH` frames (channel or per-key, 16-bit) without delaying `KEY_CONTACT` frames, and the reader emits UMP channel or poly pressure in device time

**Given** a keybed without aftertouch
**When** the engine starts
**Then** no aftertouch events are produced and the engine state publishes `aftertouch_available = false`, which Story 10.2 uses to show the Aftertouch row only when it is true

**Given** aftertouch events
**When** they reach a part whose timbre maps pressure (for example a sample instrument mapping it to volume or filter)
**Then** they are routed to every part with the keyboard on, and a render test shows the mapped parameter following pressure

### Story 3.8: Panel MCU firmware — inputs

As a keyboardist,
I want every fader, encoder, wheel, pedal and button on the panel reported reliably to the SoC,
So that the panel controls the instrument without jumps, misses or double presses.

**Covers:** AR41 (inputs), FR42 (partial: pedal ADC and polarity detection at boot), FR43 (partial: wheel ADC), EXT10 / EXT12 (dependency: MCU family, encoder and fader part numbers)

**Runs on:** MCU dev kit wired to a panel harness (encoders, faders, buttons, pedals); then the prototype board

**Blocked by:** Story 11.7 (target month 7), Story 11.8 (target month 9)

**Acceptance Criteria:**

**Given** firmware under `mcu/panel/` on its own link
**When** an encoder turns
**Then** it sends a `CONTROL` frame with the encoder ID and a signed relative step delta, with no missed or reversed steps at fast turns (tested at the encoder's rated maximum speed)

**Given** the 9 faders, 2 wheels and 3 pedal inputs
**When** they move
**Then** each is sampled at 12 bits (MCU-ID-MAP; never below 10 bits), filtered and reported on change at ≥500 Hz with a deadband that suppresses idle jitter

**Given** every button including the VALUE push switch
**When** it is pressed and released
**Then** a debounced `BUTTON` frame (ID, pressed/released) is sent for each edge, with no double events under a 5,000-press bounce test

**Given** power-up with pedals plugged in
**When** the MCU starts
**Then** it detects each pedal's polarity (normally open / normally closed, and continuous pedal direction) and reports it in `HELLO`

### Story 3.9: Panel MCU firmware — SoC-driven LEDs

As a keyboardist,
I want every panel LED driven only by the SoC, with strong, weak and off levels and adjustable brightness,
So that the LEDs always show the instrument's true state and stay readable on bright and dark stages.

**Covers:** AR41 (LEDs), FR111 (partial: MCU side), UX-DR50, UX-DR51, UX-DR52 (LED behaviour), NFR36 (partial: firmware side of LED contrast), EXT10 / EXT12 (dependency: LED part numbers, measured on:off contrast)

**Runs on:** MCU dev kit with LED harness; then the prototype board

**Blocked by:** Story 11.7 (target month 7), Story 11.8 (target month 9)

**Acceptance Criteria:**

**Given** `LED_SET` frames from the SoC
**When** they arrive
**Then** the MCU sets the addressed LED (toggle state LEDs, part/scene LED strips, encoder ring segments) to the commanded brightness, and never changes any LED on its own, including at boot (all LEDs off until the first command)

**Given** the brightness scheme
**When** the SoC selects strong, weak (~15% PWM) or off per LED and a global user brightness of Low, Mid or High
**Then** the PWM levels apply without visible flicker (PWM ≥20 kHz or equivalent)

**Given** an encoder ring
**When** the SoC sends a value arc
**Then** the lit segments show the arc and the rest are fully off, never weak-lit

**Given** the prototype panel under a 50 klx lamp
**When** LED on:off contrast is measured at High brightness
**Then** the result is recorded against the ≥10:1 target and passed to the hardware partner if it fails

### Story 3.10: Panel reader on the SoC and the Mac virtual panel

As a firmware developer,
I want the SoC to turn panel frames into performance events for the engine and `PanelEvent` messages for the UI, with the same path fed by a virtual panel on the Mac,
So that wheels, pedals, faders and encoders act on sound at the right time and the UI gets every button.

**Covers:** FR5 (panel path), FR43, AR16 (partial: panel tick → device time with TIME_SYNC), AR10 (partial: `engine` owns the panel link; `ui` receives `PanelEvent`), AR41

**Runs on:** Mac (reader core + virtual panel from keyboard and a USB MIDI control surface); dev board / prototype board with the panel MCU

**Acceptance Criteria:**

**Given** an input edge thread owning the panel link
**When** `CONTROL` frames for wheels and pedals arrive
**Then** they are converted to device time (TIME_SYNC drift tracking as in Story 3.5) and sent to the RT plane as UMP: pitch wheel as pitch bend and mod wheel as modulation to every part with the keyboard on, applied at their in-block offset
**And** a render test shows pitch bend and modulation reaching all keyboard-on parts

**Given** fader and encoder `CONTROL` frames
**When** they arrive
**Then** they are published to the RT plane as raw panel control events with control ID and value or delta (their binding to drawbars, volumes, parameters and pickup comes in Epic 4)

**Given** `BUTTON` frames
**When** they arrive
**Then** each is forwarded to `ui` as a `PanelEvent` (button ID, pressed/released, device time) and never relayed through `sysd`

**Given** the Mac
**When** the virtual panel runs
**Then** a mapping file turns keyboard keys and a USB MIDI control surface's faders, knobs and buttons into the same frames, so Epic 4 panel behaviour can be developed and tested without hardware

### Story 3.11: Pedals with half-pedal calibration and expression

As a keyboardist,
I want continuous sustain with half-pedalling, an expression pedal and a switch pedal that work whatever pedal I plug in,
So that I can play with my own pedals and get proper half-pedal on pianos.

**Covers:** FR42, AR39 (partial: pedal calibration in `sysd` config)

**Runs on:** Mac (calibration math, simulated input); prototype board (real pedals)

**Acceptance Criteria:**

**Given** the polarity detected at boot (Story 3.8)
**When** a pedal is pressed
**Then** sustain, expression and switch report the same direction for any supported pedal, tested with at least one normally-open and one normally-closed switch pedal and two continuous sustain pedals

**Given** a calibration routine run by command (CLI now; the Settings › Keyboard & Pedals › Sustain calibration row is added in Story 10.2)
**When** the player presses the sustain pedal fully and releases it
**Then** the measured min/max are stored in the `sysd` config and the pedal maps to the full continuous sustain range, so a sample instrument plays half-pedal damping at mid travel (render test)

**Given** the expression pedal
**When** it moves
**Then** it sends a continuous expression controller (UMP) to keyboard-on parts in device time

**Given** the switch pedal
**When** it is pressed
**Then** it is published as a pedal event that later functions (footswitch actions, Epic 4) consume

### Story 3.12: SoC-driven LED state and toggle state with LED per state

As a keyboardist,
I want each toggle button to cycle its states with one LED lit per state, and all panel LEDs driven from the SoC through the UI,
So that the panel always shows which mode I am in.

**Covers:** FR111, UX-DR50, UX-DR51, UX-DR81 (partial: LED blink 2 × 250/250 ms, ≤3 Hz, 200 ms dip), AR10 (partial: `ui` drives LEDs through commands to `engine`)

**Runs on:** Mac (simulator + virtual panel with on-screen LED view); prototype board

**Acceptance Criteria:**

**Given** a `ws.v1` LED command from `ui` to `engine`
**When** it is received
**Then** the engine's panel edge thread sends the matching `LED_SET` frames, and only `engine` writes to the panel link

**Given** the DRAWBARS/MIXER and PARTS/SCENES toggles (2 states) and PART/ALL/ASSIGN (3 states, cycling PART → ALL → ASSIGN)
**When** a toggle is pressed
**Then** `ui` advances its state and lights only the LED for the active state
**And** the toggle state is published so the Top Bar mirror and mode behaviours (Epic 4) read it from one place

**Given** LED patterns
**When** a blink-twice or a 200 ms recall dip is commanded
**Then** timing follows 2 × (250 ms on, 250 ms off) and a single 200 ms dip, and no LED pattern exceeds 3 Hz

**Given** a `ui` crash
**When** `ui` restarts
**Then** it re-sends the full LED state, and sound was not interrupted

### Story 3.13: PCM3168A codec as clock master with the ALSA machine driver

As a keyboardist,
I want the instrument's audio clocked by the codec's own crystal through a TDM link,
So that the output has low jitter and the engine runs on a stable stage-grade clock.

**Covers:** AR23, AR9 (partial: I2S MCLK gating, PCM3168A in TDM), NFR40, EXT10 (dependency: codec reference design latency and noise)

**Runs on:** Prototype board

**Blocked by:** Story 11.9 (target month 10)

**Acceptance Criteria:**

**Given** the mainline `rockchip-i2s-tdm` and `pcm3168a` drivers and a board machine driver (upstreamable, under `yocto/`)
**When** the board boots
**Then** the PCM3168A is the bit and frame clock master on its 24.576 MHz crystal, the SoC I2S/TDM is the slave, the SoC PLL generates no audio clock, and ALSA exposes a 6-channel playback device and a 2-channel capture device at 48 kHz, 64-frame periods

**Given** the `engine` service
**When** it opens the codec device instead of the dev-board output
**Then** it plays for 1 h with zero xruns and the DLL from Story 1.20 locks to the codec clock

**Given** a loopback from DAC to ADC on the factory test points
**When** latency and noise are measured
**Then** codec round-trip latency and output noise floor are recorded and compared with the partner's reference design figures

### Story 3.14: Balanced outputs and headphone output

As a keyboardist,
I want 4 balanced outputs (Main and Aux pairs) and a headphone output on its own amplifier,
So that I can send parts to separate PA channels and monitor on headphones.

**Covers:** FR38, FR39, AR22 (Main/Aux buses mapped to codec channels)

**Runs on:** Prototype board

**Acceptance Criteria:**

**Given** the Main and Aux buses from Epic 2
**When** the engine renders
**Then** Main goes to DAC 1–2, Aux to DAC 3–4 and the headphone feed (the Main mix) to DAC 5–6, per HARDWARE-INTERFACES §7

**Given** a part assigned to Aux in Effects › Outputs
**When** it plays
**Then** it is heard on the Aux pair only and not on Main, and the change is click-free

**Given** each balanced output and the headphone output
**When** a test tone and silence are measured
**Then** level, balance, crosstalk between Main and Aux, and noise are recorded, and the headphone amp drives 32 Ω and 250 Ω headphones without clipping at the reference level

### Story 3.15: Stereo line input mixed into the master

As a keyboardist,
I want to plug a phone or another keyboard into the line input and hear it in the master mix,
So that I can play along with a backing track or a second instrument through one PA feed.

**Covers:** FR40, AR22 (line-in to master only)

**Runs on:** Prototype board

**Acceptance Criteria:**

**Given** the codec ADC 1–2 capture device
**When** the engine runs
**Then** the line input is captured in the same period as playback and fed to the line-in port from Story 2.7, mixed into the master only, at the line-in level set in Effects › Master

**Given** the line input
**When** a reference signal is measured through to Main out
**Then** the round-trip latency and noise are recorded, and the line input never reaches parts, inserts or sends

### Story 3.16: Hardware mute for pop-free power-on and power-off

As a keyboardist,
I want the outputs held silent from power-on until the software is ready, and muted before power-off,
So that the PA never pops when I switch the instrument on or off.

**Covers:** FR44, AR37 (partial: hardware mute control)

**Runs on:** Prototype board

**Acceptance Criteria:**

**Given** the hardware mute line held muted by default
**When** the unit powers on
**Then** the outputs stay muted until the engine has opened the codec, rendered at least one period of silence and released the mute through the GPIO it owns
**And** a scope on Main out shows no transient above the noise floor at power-on across 20 power cycles

**Given** a soft power-off (power button)
**When** shutdown starts
**Then** the engine fades the outputs to silence, asserts mute, and only then stops audio, with no transient at power-off across 20 cycles

**Given** the engine crashes or is restarted
**When** audio stops unexpectedly
**Then** mute is asserted (watchdog or GPIO default) before the codec stops clocking

### Story 3.17: MIDI DIN in and out

As a keyboardist,
I want MIDI DIN in and out,
So that I can play the Velora from another keyboard and control older gear.

**Covers:** FR41, FR5 (MIDI DIN path), AR10 (partial: `engine` owns MIDI DIN), AR39 (partial: MIDI settings in `sysd` config)

**Runs on:** Dev board with a DIN breakout (opto-isolated) on a UART; then the prototype board

**Acceptance Criteria:**

**Given** the opto-isolated MIDI IN on a dedicated UART at 31,250 baud
**When** MIDI 1.0 messages arrive (including running status and real-time bytes interleaved)
**Then** an input edge thread owned by `engine` parses them, stamps each on arrival with device time and converts it to UMP for the RT plane

**Given** a hardware MIDI sender with known timing
**When** 10,000 notes are sent
**Then** onset jitter relative to the fixed scheduling delay is ≤1 frame and no message is lost

**Given** the MIDI settings in the `sysd` config with `schema_version` (this story adds `midi_out_channel` 1–16 default 1, `keybed_to_midi_out` On/Off default On, and `midi_thru` Off/On default Off, set through a `ws.v1` config command; the Settings › MIDI & Clock rows are added in Story 10.2)
**When** the engine has DIN output events
**Then** keybed notes are sent on the chosen channel only when `keybed_to_midi_out` is On, DIN IN is echoed to DIN OUT only when `midi_thru` is On, all at 31,250 baud in order
**And** a loopback test from OUT to IN receives them intact for each setting combination

**Given** MIDI clock bytes on DIN IN
**When** they arrive
**Then** they are timestamped and made available to the clock follower (used in Epic 7) without disturbing note timing

### Story 3.18: Power-fail fade and write freeze within the hold-up window

As a keyboardist,
I want the instrument to fade to silence and protect its storage when power is cut,
So that pulling the plug never pops the PA or corrupts the SSD.

**Covers:** AR37, NFR18 (verification on the board), FR68 (partial: owns the fade and write-freeze mechanism; brown-out recovery and acceptance build on it in Story 8.11), AR10 (partial: power-fail GPIO owned by `engine` with `sysd`), EXT10 (dependency: measured POWER_FAIL timing and hold-up)

**Runs on:** Prototype board

**Blocked by:** Story 11.9 (target month 10)

**Acceptance Criteria:**

**Given** the POWER_FAIL GPIO
**When** it asserts
**Then** an engine edge thread signals the RT plane, which fades all outputs to silence within one fade ramp (≤20 ms) and then asserts the hardware mute
**And** `sysd` freezes the journaled data partition against new writes, completes in-flight writes and flushes the NVMe cache
**And** the freeze is published as one signal that every later writer subscribes to, so no writer implements its own POWER_FAIL handling

**Given** a programmable supply that cuts input power at random times
**When** 50 cuts are applied while audio plays and while a test writer writes files
**Then** the scope shows no output transient, the hold-up lasts ≥150 ms and POWER_FAIL precedes rail drop by ≥100 ms (measured, recorded and compared with the partner's figures), and the journaled partition mounts clean on every reboot

**Given** POWER_FAIL asserts and input power returns within the hold-up window
**When** the rails never dropped
**Then** the unit restarts cleanly through a defined interim path (reboot), and the log records the event (Story 8.11 replaces this with brown-out recovery without a reboot)

### Story 3.19: Key-to-sound latency on the prototype

As a founder,
I want the full key-to-analog-output latency measured on the prototype with the real keybed at full polyphony,
So that the ≤5 ms promise is proven on the real instrument, not only with a USB keyboard.

**Covers:** NFR1, NFR42 (scan + transport within budget)

**Runs on:** Prototype board

**Acceptance Criteria:**

**Given** a latency rig that senses the velocity-measuring contact closing (or a solenoid pressing a key with a timing reference) and captures Main out
**When** 1,000 notes are measured while 256 other sample voices sound
**Then** contact-to-analog latency is ≤5 ms for every note, and the report gives min/median/p99/max with a breakdown (scan + transport, TIME_SYNC conversion, scheduling delay, render, codec)
**And** scan + transport is <1 ms of the budget

**Given** an organ part
**When** the first contact closes
**Then** the organ's onset latency from the first contact is measured and recorded separately

### Story 3.20: 24-hour thermal run in the closed chassis

As a founder,
I want the prototype to run 24 h at full audio load in its closed chassis at 40 °C ambient without throttling,
So that the instrument stays reliable in a hot venue.

**Covers:** NFR11, NFR4 (soak on the prototype), EXT10 (dependency: chassis and thermal design, partner thermal test result)

**Runs on:** Prototype board in the closed chassis, climate chamber or controlled room at 40 °C

**Blocked by:** Story 11.9 (target month 10)

**Acceptance Criteria:**

**Given** the prototype in its closed chassis at 40 °C ambient
**When** the Story 2.12 worst-case soak runs for 24 h through the codec outputs
**Then** SoC, NVMe and SoM temperatures and CPU frequencies are logged every 10 s, no thermal throttling occurs on any core, and the counters show zero xruns and zero stream underruns

**Given** the results
**When** the report is written
**Then** it states the thermal margin to the throttle point, compares with the partner's thermal test, and passes or fails NFR11

## Epic 4: Complete show: Live Sets, scenes and panel

A keyboardist builds and plays a whole show on the instrument: setlists of Live Sets, each with 8 parts (zones, layers, splits) and up to 8 scenes; seamless scene, Live Set and setlist switching with the next Live Set preloaded; the Performance screen; the Setlist and Setlist Browser with tags and favorites; Write with its hold guard and the ◆ Unsaved Indicator; fader pickup; the three panel toggles; ASSIGN with a global default; navigation, MENU and the Launcher; the footswitch; accidental-press guards; and the Part surface. Target: first playable units (month 12). Development is Mac-first: the Live Set model, control thread, switching and Write run in `engine-host` on the Mac and are verified with the offline renderer; every device surface is built in the LVGL SDL simulator at 1024×600 against the Mac virtual panel from Story 3.10; head-budget, preload timing and the final run-through are accepted on the dev board and the prototype. Undrawn surfaces (Launcher, On-screen Keyboard, Scenes, Part, Action Sheet, ASSIGN Overlay) get a design story before their implementation.

### Story 4.1: Tier profile read at boot

As a founder,
I want the unit to read its tier (v1 or Pro) at boot and set part count, head budget, voice/cost budgets and enabled modules from it,
So that one image runs on both tiers without a code fork.

**Covers:** FR17, AR24, NFR39

**Runs on:** Mac (profile logic, override flag); prototype board (strap / OTP)

**Acceptance Criteria:**

**Given** the tier ID strap or OTP value readable on the board (Story 3.2)
**When** `engine` starts
**Then** it reads the tier once and selects a tier profile: v1 = 8 parts, 4–6 GB head budget (configured 5 GB), v1 voice and cost budgets; Pro = 16 parts, Pro head and cost budgets, sequencer module enabled (no sequencer code yet)
**And** the profile is published in the engine state for `ui` and `sysd`

**Given** a board with no readable strap (dev board) or the Mac host
**When** `engine` starts
**Then** it uses the v1 profile, and a dev-only override selects Pro for testing

**Given** the same image
**When** it boots with each profile
**Then** part count, head budget and cost budget differ as specified, and no code path is compiled per tier
**And** the Pro bank A/B behaviour is recorded as deferred (only the 16-part data model is enabled)

### Story 4.2: Versioned Live Set format with parts, scenes and ASSIGN map

As a keyboardist,
I want my Live Sets saved in a versioned format that every later firmware opens,
So that a show built today still loads after any update.

**Covers:** AR29 (Live Set persisted form), AR30 (partial: Live Set data model), FR92, FR49 (partial: N/N−1 reader, unknown fields preserved, compatibility corpus seeded), NFR44, AR26 (partial: engine control thread sole writer of Live Sets), FR14 (partial: format holds any engine's part)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a protobuf `LiveSet` message in `proto/` with `format_version`
**When** it is defined
**Then** it holds id, name, tempo, the tier's part count of parts (each: timbre reference `(library_id, instrument_key)` or empty, low key, high key, octave, transpose, keyboard on/off, volume, output Main/Aux, insert chain, send levels, engine sound parameters, envelope, EQ), up to 8 scenes (name and every Live Set parameter except timbre assignment) and an 8-slot ASSIGN map of `param_id`s
**And** the arpeggiator fields that Epic 7 uses are defined now and left unset, so adding the arpeggiator needs neither a format version bump nor a migration

**Given** a part's engine data
**When** it is stored
**Then** it holds an engine type ID (Story 1.8) plus its sound parameters keyed by `param_id`, with no engine-specific message per engine, so FM and VA parts fit the v1 format unchanged
**And** a test Live Set holding a part of a reserved engine type with no compiled engine (FM or VA in v1) round-trips through read and write unchanged and loads with that part silent, never substituted

**Given** a new Live Set created on the device
**When** the control thread mints its ID
**Then** the ID is a UUIDv4, and a Live Set loaded from an import or restore whose ID collides with an existing one is given a new UUIDv4 and keeps its content

**Given** the reader
**When** it opens files of version N and N−1, and a file with unknown fields
**Then** both versions load, unknown fields are preserved on the next write, and a file of an unknown major version is refused with a typed error
**And** a compatibility corpus directory is created with Live Sets saved by this release, and CI opens every file in it on every build

**Given** a Live Set write
**When** the control thread persists it
**Then** it writes to a temporary file on the journaled partition, fsyncs and renames atomically, and only the control thread writes Live Set files

### Story 4.3: Live Set load with 8 parts, zones, layers and splits

As a keyboardist,
I want a Live Set of up to 8 parts, each with its own timbre from any engine, key zone, octave/transpose and keyboard on/off,
So that I can layer and split sounds across the keyboard.

**Covers:** FR15, FR16, FR83 (partial: Live Set holds each part's timbre), AR11 (Live Set as structural state), AR19 (generation = snapshot + heads), FR14 (partial: part model accepts any engine)

**Runs on:** Mac (render tests); dev board

**Acceptance Criteria:**

**Given** a Live Set file
**When** the control thread loads it
**Then** it resolves each part's timbre, loads its heads, builds a snapshot generation with all parts, and swaps it in only when every head is resident
**And** an unresolved timbre leaves that part silent with a "missing library" state, never a substitute

**Given** parts with low key and high key, octave and transpose
**When** a key is played
**Then** each part with the keyboard on sounds only if the key (after the part's octave/transpose is applied to pitch, not to the zone test) is inside its zone, so overlapping zones layer and adjacent zones split
**And** render tests cover a piano + pad layer, a bass/piano split at B2/C3 and a part with keyboard off

**Given** a part with the keyboard off
**When** keys are played
**Then** that part receives no notes, while its sound keeps releasing any notes held before

**Given** parts from the sample engine and the organ in one Live Set
**When** it loads
**Then** the arbiter reservations from Epics 1 and 2 apply to the whole Live Set

### Story 4.4: Scene recall as a parameter-only snapshot swap

As a keyboardist,
I want to recall up to 8 scenes per Live Set that change volumes, keyboard on/off, effects, drawbars and other settings but never the timbres,
So that I can move from verse to chorus instantly without any note being cut.

**Covers:** FR88, FR83 (scenes never change timbre), FR79 (scene switch), AR21 (scene recall = parameter-only snapshot swap), AR12 (partial: initial performance values tagged with `program_generation`)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a Live Set with several scenes
**When** a scene recall command arrives
**Then** the control thread builds a snapshot that changes only scene-stored parameters, swaps it in, and loads no heads
**And** performance parameters carried by the scene (volumes, drawbars, sends) are applied by RT once, at the new generation

**Given** notes held, sustained by the pedal or releasing at the moment of recall
**When** the scene changes
**Then** they continue without a gap or truncation, and only new notes follow the new keyboard on/off states

**Given** a scene recall command that would change a part's timbre
**When** it is built
**Then** the timbre is not part of the scene and stays as the Live Set defines it

**Given** scene order in the Live Set
**When** "next scene" and "previous scene" commands arrive
**Then** they step in that order and stop at the first and last existing scene

### Story 4.5: Seamless Live Set switching with background load

As a keyboardist,
I want to jump to any Live Set while the current sound keeps playing until the new one is ready,
So that changing songs never cuts a note, even when the target was not preloaded.

**Covers:** FR79 (Live Set switch), FR81, AR19, AR21 (non-preloaded jump keeps old generation), NFR16 (partial)

**Runs on:** Mac (logic, renders); dev board (load times)

**Acceptance Criteria:**

**Given** a Live Set playing
**When** a jump to a Live Set that is not preloaded is requested
**Then** the current generation keeps playing and responding to the keys while the control thread loads the target's heads on non-RT threads
**And** the engine publishes `loading` with the target name and an estimated remaining time in seconds, updated at least once per second

**Given** the target's heads are resident
**When** the swap happens
**Then** notes held, sustained or releasing on the old generation finish on it, new notes play on the new one, and the old generation is freed only after RT reports no voice references it (its BundleLeases released then)

**Given** a second jump request while a load is in progress
**When** it arrives
**Then** the first load is cancelled, its partial heads freed, and the new target loads, without affecting the sound

**Given** a target that cannot fit in the head budget
**When** the load is refused
**Then** the current Live Set keeps playing and the engine publishes a typed error for the UI

### Story 4.6: Setlists, setlist tags and Live Set stepping

As a keyboardist,
I want setlists that order my Live Sets, user tags on setlists, and LIVE SET stepping that follows the loaded setlist,
So that a show runs in order and I can go off-script and come back.

**Covers:** FR82, FR84 (partial: create, rename, reorder setlists and Live Sets, new Live Set, tags, as commands), FR86, AR30 (setlists and setlist tags as control-thread state), AR26 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** versioned protobuf forms for setlists (id, name, ordered Live Set ids, tag ids) and setlist tags (id, name)
**When** the control thread receives commands to create, rename, reorder and delete setlists, add/remove/reorder Live Sets in a setlist, create a new Live Set (8 empty parts, one scene, ASSIGN map copied from the global default) and create/rename/delete tags
**Then** it applies them as the sole writer and publishes the new state

**Given** a setlist is loaded
**When** a load-setlist command for another setlist arrives
**Then** the current generation keeps playing; the new setlist becomes the loaded one and its first Live Set is not loaded until the player steps or chooses

**Given** a loaded setlist and an off-script jump to a Live Set outside it
**When** "next Live Set" or "previous Live Set" is issued
**Then** stepping returns into the setlist relative to the last setlist position, with no setlist loaded stepping walks All Live Sets in name order, and at the end of a setlist "next" does nothing and the state reports `end_of_setlist`

### Story 4.7: Next Live Set preload within the head budget

As a keyboardist,
I want the next Live Set in my setlist already loaded,
So that stepping to the next song is immediate.

**Covers:** FR80, AR21 (preload within head budget), AR20 (partial: tier head budget shared by current and preloaded generations), NFR3 (partial)

**Runs on:** Dev board (head budget, timing); logic on Mac

**Acceptance Criteria:**

**Given** a loaded setlist and a current Live Set
**When** the current Live Set becomes active
**Then** the control thread loads the heads of the next Live Set in the setlist within the remaining head budget, and publishes `ready` when all of them are resident

**Given** the next Live Set is ready
**When** "next Live Set" is issued
**Then** the swap happens at the next block with no load wait, measured as <10 ms from command to swap on the dev board, and the preload of the following Live Set starts

**Given** current + next would exceed the head budget
**When** the preload is attempted
**Then** heads shared by both are counted once, and if the total still does not fit the preload is skipped, the state reports not ready, and stepping falls back to the Story 4.5 background load

### Story 4.8: Seamless switching render suite

As a founder,
I want automated renders that switch scenes, Live Sets and setlists during held, sustained and releasing notes and prove nothing is cut,
So that seamless switching stays true on every build.

**Covers:** NFR14, FR79, FR80, FR81, FR82, AR2 (partial: added to the release gate)

**Runs on:** Mac (offline renderer, CI); dev board (live run)

**Acceptance Criteria:**

**Given** scripted cases in the offline renderer: scene change, in-setlist Live Set step (preloaded), setlist load, and a jump to a non-preloaded Live Set, each during held notes, pedal-sustained notes and releasing notes
**When** they render
**Then** a gap/truncation detector shows no drop in envelope and no early end for every note started before the switch, compared with a render without the switch

**Given** CI
**When** the switching suite runs on every change to the control thread, generations or streaming
**Then** any detected gap fails the build

**Given** the dev board
**When** the same cases are played live through the engine for 1 h in a loop
**Then** counters show zero xruns and zero underruns

### Story 4.9: Edits in memory and Write scopes

As a keyboardist,
I want my edits kept in memory across scene and Live Set changes until I write them, with Write saving a scene, the whole Live Set or a new Live Set,
So that I never lose an edit on stage and never get interrupted by a confirmation.

**Covers:** FR89 (engine: scopes, Scene unavailable with timbre change, name suggestion), FR90 (engine: edits kept until Write or power-off, Write writes the current Live Set only), FR91, AR12 (Save waits on a control→RT fence), NFR15

**Runs on:** Mac

**Acceptance Criteria:**

**Given** edits to a Live Set (structural commands or performance parameter changes)
**When** the player changes scene or Live Set and returns
**Then** the edits are still present, and the engine publishes a dirty flag per Live Set and a change summary for the current one (for example "Part 2 volume, Reverb send")

**Given** a Write command with scope Scene, Whole Live Set or Save as new
**When** it runs
**Then** the control thread raises a fence to RT, reads the last-value table after the fence, writes per scope (Scene: the current scene's parameters; Whole Live Set: everything; Save as new: a new Live Set with a new UUIDv4 added after the current one in the setlist) and clears the dirty flag only for what was written
**And** Scene scope is refused when the edits include a timbre change, with reason "Timbre changes are saved with the Live Set"

**Given** Save as new with a name already used in the setlist
**When** the command is validated
**Then** it is refused with "name in use" and a suggested free name ("Perfect 2")

**Given** storage is full
**When** Write runs
**Then** it fails with a storage-full error, nothing partial is written, and the edits stay in memory with the dirty flag set

**Given** a performance with no Write
**When** disk writes are monitored for 1 h of playing, scene and Live Set changes
**Then** no Live Set or setlist file is written

### Story 4.10: ASSIGN maps with a global default

As a keyboardist,
I want each Live Set to have its own map of the 8 encoders in ASSIGN mode, seeded from a global default I can edit,
So that every song has the knobs I need and new songs start from my usual layout.

**Covers:** FR102, FR103, FR101 (partial: engine-side routing of encoders in ASSIGN to mapped parameters), AR30 (global default ASSIGN map)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the global default ASSIGN map as control-thread state in its own versioned file
**When** a new Live Set is created
**Then** it copies the global default at creation, and later edits to the global default do not change existing Live Sets

**Given** commands to set or clear a slot K1–K8 of the current Live Set's map, or of the global default
**When** they are applied
**Then** the map changes, the Live Set is marked dirty (Live Set map) and the map survives Write and a power cycle

**Given** ASSIGN mode is active
**When** encoder n turns
**Then** its relative steps change the parameter mapped to slot n in the current Live Set (no pickup), and an empty slot does nothing

### Story 4.11: Live Set favorites and Recent in the catalog

As a keyboardist,
I want to mark Live Sets as favorites and see the ones I played recently,
So that I can find the songs I use most in the Setlist Browser.

**Covers:** FR85 (partial: favorites and Recent data), AR26 (partial: librarian catalog, Live Set favorites/recent tables only), AR10 (partial: minimal `librarian` service), AR25 (partial: Library API handshake and favorites messages), NFR15

**Runs on:** Mac (host process); packaged as a systemd unit for the board

**Acceptance Criteria:**

**Given** no `librarian` service yet
**When** this story creates it
**Then** a minimal `librarian/` C++20 service runs (host process on the Mac, systemd unit on the board) and serves a `ws.v1` Library API over a Unix domain socket (handshake and the favorites and recent messages of this story)
**And** it opens a SQLite WAL catalog with a `schema_version` holding only the tables this story needs (Live Set favorites, Live Set recent plays), and only `librarian` writes it

**Given** add/remove favorite commands for a Live Set id via the librarian API
**When** they are applied
**Then** the favorite set changes and is published to `ui`

**Given** a Live Set becomes active during a performance
**When** Recent is updated
**Then** the update is kept in RAM and written at an idle-safe moment (no write during playing), and Recent lists the last 20 Live Sets, most recent first

**Given** a Live Set is deleted
**When** the catalog reconciles
**Then** its favorite and recent rows are removed

### Story 4.12: Fader bindings with pickup

As a keyboardist,
I want the 9 faders to control part volumes and the reverb send in MIXER and the selected organ's drawbars in DRAWBARS, never changing anything until they cross the stored value after a recall,
So that moving a fader never makes a value jump.

**Covers:** FR105 (engine: pickup rule and state), FR104 (engine: fader binding per mode, organ selection), FR52 (REV fader drives the reverb send), AR16 (partial: non-motorized faders, pickup after recall)

**Runs on:** Mac (virtual panel); prototype board

**Acceptance Criteria:**

**Given** the fader mode MIXER
**When** faders 1–8 and the 9th fader move
**Then** they drive part 1–8 volumes and the REV send level

**Given** the fader mode DRAWBARS and an organ part selected (the only organ, or with two organs the one chosen by part-button long press)
**When** faders move
**Then** they drive the 9 drawbars of that organ part

**Given** a scene, Live Set or fader-mode change, or a change of the selected organ
**When** a fader's physical position differs from the stored value of its new target
**Then** the fader changes nothing until its position crosses the stored value; from then on it takes over and follows
**And** the engine publishes per fader: target, stored value, physical position and pending/taken-over, updated on every change

**Given** a request for DRAWBARS with no organ part in the Live Set
**When** it is evaluated
**Then** the engine refuses the mode change with a "no organ" result and the faders stay in MIXER

### Story 4.13: Encoder bindings for PART, ALL and ASSIGN

As a keyboardist,
I want the 8 encoders to act on the selected part, on all parts at once, or on the ASSIGN map, with their LED rings showing the target value,
So that I can shape one part or the whole Live Set without value jumps.

**Covers:** FR107 (engine: fixed functions DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH; relative in ALL; no pickup), FR106 (partial: knob target selected by part-button long press), FR101 (partial), UX-DR52 (ring value), UX-DR85 (encoder ring ↔ target value)

**Runs on:** Mac (virtual panel); prototype board

**Acceptance Criteria:**

**Given** the encoder mode PART and a knob-target part
**When** encoders 1–8 turn
**Then** they change that part's delay send, brightness, attack, release, EQ low, mid, mid freq and high by relative steps, and the engine publishes each ring value (0–100% of range) for the LED rings

**Given** the encoder mode ALL
**When** an encoder turns
**Then** the same function changes on every part by the same relative amount (each clamped to its range), and the ring shows the relative offset from the values at the moment ALL was entered

**Given** the encoder mode ASSIGN
**When** an encoder turns
**Then** the Story 4.10 mapping applies, and the ring shows the mapped parameter's value

**Given** any mode
**When** encoders turn after a scene or Live Set change
**Then** no pickup applies; values move from the stored value

### Story 4.14: Design story — bring existing mockups in line with the spines

As a keyboardist,
I want the Performance, drawbars, Setlist and Studio mockups corrected before they are built,
So that implementation follows the agreed design, not the known mock deviations.

**Covers:** UX-DR109, UX-DR54 (design), UX-DR55 (design), UX-DR57 (design), UX-DR58 (design), UX-DR97

**Runs on:** Mac (HTML mockups)

**Acceptance Criteria:**

**Given** the deviations listed in DESIGN.md Brand & Style
**When** `key-screen-performance.html`, `key-screen-drawbars-categories.html`, `key-screens-device.html`, `key-screen-studio.html` and `panel-v2.html` are updated
**Then** Part Strips show category short labels instead of engine types, the Pickup Ghost is warn with bg keylines and a chevron, off parts use muted text with a hollow ring and OFF tag (no opacity), ⏻ is present, bar heights are 56/118/72/298/56, unsaved uses ◆, the mode mirror is 20 px bold with an accent underline, the drawbar registration reads `88 8600 000`, Settings rows match EXPERIENCE.md, "songs" reads "Live Sets", and the knob toggle is 3-state
**And** each corrected mock passes pseudo-localization at +40% with `ÁÇÊÕÑ Ação`

### Story 4.15: Design story — Launcher Grid mockup

As a keyboardist,
I want the Launcher drawn before it is built,
So that the 7 destinations are easy to reach by VALUE or touch.

**Covers:** UX-DR102, UX-DR32 (design), UX-DR56 (design), UX-DR87, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockup)

**Acceptance Criteria:**

**Given** EXPERIENCE.md › Launcher and DESIGN.md › Launcher Grid
**When** `mockups/launcher.html` is drawn at 1024×600
**Then** it shows the 7 tiles Setlist Browser, Setlist, Library, Effects, Scenes, Import, Settings in rows of 4 + 3, each ≥56 px with ≥12 px gaps, the highlighted tile raised with an accent border, and a Hint Bar `VALUE move · WRITE open · MENU close · EXIT close`
**And** a disabled-tile state is drawn for surfaces not yet available, with text, not colour alone

**Given** the mockup
**When** it is reviewed in Stage and Studio themes with pseudo-localization
**Then** contrast and target-size checks pass and labels do not clip

### Story 4.16: Design story — On-screen Keyboard mockup

As a keyboardist,
I want the On-screen Keyboard drawn for English, Portuguese and Spanish before it is built,
So that I can type song and setlist names with accents quickly.

**Covers:** UX-DR103, UX-DR35 (design), UX-DR10 (OSK layering rule), UX-DR87, UX-DR97

**Runs on:** Mac (HTML mockup)

**Acceptance Criteria:**

**Given** DESIGN.md › On-screen Keyboard and the layering rule (never over the Top Bar or the edited field)
**When** `mockups/on-screen-keyboard.html` is drawn at 1024×600
**Then** it shows EN, PT-BR and ES layouts with keys ≥56 px, reachable accented letters (á à â ã é ê í ó ô õ ú ç ñ ü and capitals), shift, space, backspace and an accent-filled Done key, over a search field and over a name field
**And** a coverage rule states which glyphs each layout reaches directly and which by long press

**Given** the mockup
**When** it is reviewed
**Then** it passes both themes, contrast and the `ÁÇÊÕÑ Ação` clipping check

### Story 4.17: Design story — Scenes surface mockup

As a keyboardist,
I want the Scenes surface drawn before it is built,
So that creating, naming and ordering scenes is clear.

**Covers:** UX-DR101, UX-DR62 (design), UX-DR14, UX-DR87, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockup)

**Acceptance Criteria:**

**Given** EXPERIENCE.md › Scenes
**When** `mockups/scenes-surface.html` is drawn at 1024×600
**Then** it shows slots 1–8 in Scene Tile style with the current marked ▶ and empty slots dashed, the actions New scene · Rename · Duplicate · Move ◀ ▶ · Clear, the line "Edits become permanent with Write (Scene)", and the Hint Bar for this surface

**Given** the mockup
**When** it is reviewed
**Then** it passes both themes, contrast, target size and pseudo-localization, and current is shown by ▶ and fill, not colour alone

### Story 4.18: Design story — Part surface mockup

As a keyboardist,
I want the full Part surface drawn before it is built,
So that one part's timbre, sound, zone, level, effects and arpeggiator fit one clear screen.

**Covers:** UX-DR99, UX-DR60 (design), UX-DR87, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockup)

**Acceptance Criteria:**

**Given** EXPERIENCE.md › Part and the organ section drawn in Story 2.16
**When** `mockups/part-surface.html` is drawn at 1024×600
**Then** it shows the header `PART n · timbre · CATEGORY` with ⏻ and ★, and list-left / fields-right sections Timbre (Change timbre), Sound (sample engine set and the organ set from Story 2.16), Envelope & EQ, Zone & keyboard (low key, high key, octave/transpose, keyboard on/off), Level & output (volume, Main/Aux), Effects (insert chain link, reverb send, delay send) and Arpeggiator (all fields from EXPERIENCE.md)
**And** the Hint Bar shows VALUE edits the highlighted field and the encoders keep their fixed functions

**Given** the mockup
**When** it is reviewed
**Then** it is shown in Stage and Studio themes, independent of Large text, pseudo-localized at +40%, with contrast and target size checks passing

### Story 4.19: Design story — Action Sheet and ASSIGN Overlay mockups

As a keyboardist,
I want the Action Sheet and the ASSIGN Overlay drawn before they are built,
So that short choices and knob names read clearly without blocking play.

**Covers:** UX-DR105 (partial: owns the base Action Sheet design and the favorites variant; library, pairing and install variants are refined in Story 5.18), UX-DR107, UX-DR33 (design), UX-DR37 (design), UX-DR10 (layering), UX-DR87, UX-DR97, UX-DR98

**Runs on:** Mac (HTML mockups)

**Acceptance Criteria:**

**Given** DESIGN.md › Action Sheet
**When** `mockups/action-sheet.html` is drawn
**Then** it shows a raised card centred over a 35%-dimmed surface with a title and 2–4 rows ≥56 px paired with controls, in the variants `★ Add to Favorites` / Remove from Favorites, Missing library (Replace timbre / Import), Pairing code and Install update

**Given** DESIGN.md › ASSIGN Overlay
**When** `mockups/assign-overlay.html` is drawn over the Performance part area
**Then** it shows K1–K8 in meta caps with parameter names, the turned knob's slot raised with an accent border, and leaves the Live Set Hero and Scene strip visible

**Given** both mockups
**When** they are reviewed
**Then** they pass both themes, contrast, target size and pseudo-localization

### Story 4.20: Performance surface — Top Bar, Live Set Hero and Scene strip

As a keyboardist,
I want the Performance screen to show my setlist position, the three modes, tempo, the Live Set name large with what comes next, and the scenes,
So that I can read where I am in the show at a glance from 1.5 m.

**Covers:** UX-DR12, UX-DR13, UX-DR14, UX-DR54 (partial), UX-DR10 (Performance skeleton), UX-DR72 (ready, loading, end of setlist, no setlist, unsaved ◆ position, empty scene), UX-DR84 (Top Bar mirror), FR111 (Top Bar mirror of toggle LEDs), FR80 (`● READY`), FR81 (`Loading <name>… Ns`), FR86 (`End of setlist`, `NO SETLIST`), UX-DR88, UX-DR95, NFR33, NFR34

**Runs on:** Mac (simulator + engine-host); dev board

**Acceptance Criteria:**

**Given** the Performance skeleton (Top Bar 56 · Hero 118 · Scene strip 72 · Part area 298 · Status 56)
**When** a Live Set from a setlist is active
**Then** the Top Bar shows `SETLIST · <name> · n / m` (middle ellipsis), the mode mirror of all three toggles in 20 px bold with the active state in accent text and an accent underline, and the tempo `♩ 72`
**And** with no setlist loaded the context reads `NO SETLIST · <Live Set>`

**Given** the Live Set Hero
**When** it renders
**Then** it shows `LIVE SET nn`, the Live Set name in the 53 px token on one line with end ellipsis (≥43 px for 1.5 m legibility), the ◆ placeholder position after the name, and NEXT with the next title and `● READY` in ok, `Loading <name>… Ns` in muted, or `End of setlist`
**And** nothing in the Hero animates

**Given** the Scene strip
**When** the Live Set has scenes
**Then** 8 Scene Tiles show number, name and muted sub-label, the current scene is the only accent-filled tile with on-accent text, and empty slots are dashed with a muted number

**Given** a scene, Live Set or toggle change from any source
**When** the engine publishes it
**Then** the screen updates within one UI frame, UI transitions are ≤150 ms, and the screen never shows a state the engine did not publish

### Story 4.21: Performance surface — Part Strips, Volume Bars and Status Bar

As a keyboardist,
I want 8 Part Strips showing each part's timbre, category, on/off and volume, and a Status Bar with voices, streaming, rotary and reverb,
So that I see what every part is doing while I play.

**Covers:** UX-DR15, UX-DR16, UX-DR18, UX-DR54, UX-DR72 (part off, empty part, missing library display), UX-DR94 (short labels on Part Strips), FR10 (partial: `ROTARY SLOW/FAST` in the Status Bar), UX-DR88, UX-DR95, NFR32

**Runs on:** Mac (simulator + engine-host); dev board

**Acceptance Criteria:**

**Given** the part area in MIXER mode
**When** a Live Set is active
**Then** 8 Part Strips show part number + LED dot, ⏻ with a ≥56 px hit area, the timbre name (2 lines, end ellipsis), the category short label (≤10 characters, localized) and a Volume Bar whose accent fill shows the stored (sounding) volume
**And** an off part shows name and category in muted at full opacity, a hollow ring and an `OFF` tag; an empty part shows "— empty"; a missing library shows "Missing library" in warn in place of the category

**Given** the Status Bar
**When** nothing is pending
**Then** it shows `VOICES n / max`, `● STREAMING OK`, `ROTARY SLOW/FAST/STOP` (when an organ or rotary insert is present) and `REV n%`
**And** `REV` opens the Effects surface from Epic 2 (tap; touch guards are added later in this epic)

**Given** a warning (missing library on a part)
**When** it is active
**Then** a 24 px warn takeover line prefixed with `⚠` names the part and replaces all Status Bar items until resolved

**Given** ⏻ on a Part Strip
**When** it is activated
**Then** the part toggles on/off through a command, the strip shows the new state, and touch targets stay ≥12 px apart with no nested targets

### Story 4.22: Navigation rules, MENU and the Launcher

As a keyboardist,
I want EXIT, WRITE, VALUE, MENU, LIVE SET ◀ ▶ and SCENE ◀ ▶ to behave the same everywhere, and MENU to open a Launcher of all surfaces,
So that I never get lost on the instrument.

**Covers:** FR108, UX-DR79, UX-DR32, UX-DR56, UX-DR48, UX-DR49, UX-DR86 (MENU ↔ Launcher, LIVE SET/SCENE ↔ Hero/Scene Tile), FR79 (stepping from any surface)

**Runs on:** Mac (simulator + virtual panel); prototype board

**Acceptance Criteria:**

**Given** any surface
**When** LIVE SET ◀ ▶ or SCENE ◀ ▶ is pressed
**Then** the Live Set or scene steps seamlessly (Stories 4.4–4.6) without leaving the current surface

**Given** the Launcher built from the Story 4.15 mockup
**When** MENU is pressed on any surface
**Then** the Launcher opens; VALUE moves the highlight; WRITE or touch opens the tile (replacing the Launcher); MENU or EXIT closes it back to the surface it was opened from
**And** tiles whose surface is not yet available (Library, Import until Epic 5) are shown disabled with text and do nothing; Effects opens the Epic 2 surface

**Given** a surface opened from the Launcher
**When** EXIT is pressed
**Then** it returns to the surface the Launcher was opened from; EXIT always acts on release and goes one level back; dialogs and sheets cancel; on Performance EXIT does nothing

**Given** WRITE and VALUE
**When** they are used on Performance and on list surfaces
**Then** a short WRITE press on Performance does nothing and the Hint Bar elsewhere names what WRITE confirms; VALUE turn scrolls or changes the highlighted item

### Story 4.23: PARTS/SCENES toggle and the part/scene buttons

As a keyboardist,
I want the 8 buttons to switch parts on/off and select the knob target in PARTS, and recall scenes 1–8 in SCENES, with LEDs that show the state,
So that parts and scenes are under my fingers.

**Covers:** FR106, UX-DR51, UX-DR84 (PARTS/SCENES ↔ button LED meaning), UX-DR86 (part button ↔ Part Strip LED dot / ⏻ / OFF; long press ↔ outlined strip), UX-DR81 (long press, 200 ms dip), UX-DR88

**Runs on:** Mac (simulator + virtual panel); prototype board

**Acceptance Criteria:**

**Given** PARTS mode
**When** a part button is pressed briefly
**Then** that part toggles on/off, its LED is strong when on and off when off, and its Part Strip shows the same state
**And** a press held past the long-press threshold selects that part as knob target (Part Strip outlined, Top Bar `KNOBS PART n`) without toggling it

**Given** SCENES mode
**When** a scene button is pressed
**Then** that scene recalls; LEDs show current = solid strong, existing = weak, empty = off, and the current LED dips once for 200 ms on recall
**And** pressing an empty scene's button does nothing

**Given** the PARTS/SCENES toggle
**When** it is pressed
**Then** the button meanings and LEDs switch immediately, and the Top Bar mirror changes with the toggle LED

### Story 4.24: Pickup Ghost and the Status Bar takeover

As a keyboardist,
I want to see exactly which way to move a fader to take over after a recall,
So that I can match it mid-song without guessing.

**Covers:** FR105 (UI), UX-DR17, UX-DR85, UX-DR72 (pickup pending), UX-DR18 (pickup takeover), UX-DR88, NFR33 (takeover ≥24 px)

**Runs on:** Mac (simulator + virtual panel faders); prototype board

**Acceptance Criteria:**

**Given** a fader whose pickup is pending (Story 4.12 state)
**When** the Performance surface shows its Part Strip
**Then** a 3 px dashed warn line with 2 px bg keylines marks the physical position across the Volume Bar, a ▲ or ▼ chevron at the strip edge shows the direction, and the strip has a 2 px dashed warn outline
**And** the Status Bar takeover reads `▲ FADER 3 → 65%` (24 px, warn, glyph-prefixed) replacing every other item

**Given** several faders pending
**When** the takeover is shown
**Then** it names the most recently moved pending fader

**Given** the fader crosses the stored value
**When** it takes over
**Then** the ghost, outline and takeover disappear and the Volume Bar follows the fader

### Story 4.25: DRAWBARS/MIXER toggle with the Organ Card and Drawbar Columns

As a keyboardist,
I want the DRAWBARS button to turn the faders into the organ's drawbars, shown on screen as an Organ Card with nine drawbars,
So that I can register the organ live like on a real console.

**Covers:** FR104, FR10 (partial: ROTARY meta in the Organ Card), UX-DR20, UX-DR21, UX-DR55, UX-DR3, UX-DR72 (no organ, two organs), UX-DR84 (DRAWBARS/MIXER ↔ part area), UX-DR86 (ROTARY S/F ↔ Organ Card), UX-DR81 (blink twice)

**Runs on:** Mac (simulator + virtual panel); prototype board

**Acceptance Criteria:**

**Given** a Live Set with an organ part
**When** DRAWBARS is selected
**Then** the part area shows the Organ Card (`PART n · DRAWBARS`, organ name, `ORGAN` in accent, percussion/vibrato/rotary meta, registration like `88 8600 000` in tabular numerals, raised with accent border) and 9 Drawbar Columns (16'…1', coloured heads 0–8 with the per-theme edge rule, numeric value) that mirror the drawbar values live
**And** a pending drawbar pickup shows the same Pickup Ghost and takeover treatment as Story 4.24 ("DRAWBAR 2⅔': MOVE TO 6 TO TAKE OVER (NOW 3)")

**Given** a Live Set without an organ
**When** DRAWBARS is pressed
**Then** the mode does not change, the DRAWBARS LED blinks twice (2 × 250/250 ms) and the screen says "No organ in this Live Set"

**Given** two organ parts
**When** the player long-presses a part button of one organ (PARTS mode)
**Then** the faders and the Organ Card follow that organ

**Given** ROTARY STOP or S/F on the panel
**When** pressed
**Then** the organ's rotary speed changes (STOP toggles stop, S/F toggles slow/fast) and both the Organ Card meta and the Status Bar show it

### Story 4.26: PART/ALL/ASSIGN toggle and encoder LED rings

As a keyboardist,
I want the knob toggle to cycle PART, ALL and ASSIGN with the LED rings and Top Bar showing what the knobs control,
So that I always know what a knob turn will change.

**Covers:** FR107 (UI and LEDs), UX-DR52, UX-DR84 (PART/ALL/ASSIGN ↔ `KNOBS PART n / ALL / ASSIGN` + outlined strip), UX-DR85, UX-DR88

**Runs on:** Mac (simulator + virtual panel); prototype board

**Acceptance Criteria:**

**Given** the PART/ALL/ASSIGN toggle
**When** it is pressed
**Then** it cycles PART → ALL → ASSIGN → PART, one LED per state, and the Top Bar shows `KNOBS PART n`, `KNOBS ALL` or `KNOBS ASSIGN`, with the knob-target Part Strip outlined in PART

**Given** any mode
**When** encoder values change (from a turn, a recall or a mode change)
**Then** each LED ring shows the target's value arc (relative offset in ALL), lit segments only, never weak-lit

**Given** the encoders
**When** they are turned on any surface
**Then** they act per the current mode (Story 4.13), including while the Part, Effects or Setlist surfaces are open

### Story 4.27: ASSIGN Overlay

As a keyboardist,
I want to see the names of K1–K8 when I turn a knob in ASSIGN,
So that I know what each assigned knob does without leaving Performance.

**Covers:** FR101 (overlay, 3 s, restart per turn, never blocks sound), UX-DR33, UX-DR10 (overlay precedence over the part area), UX-DR81 (3 s)

**Runs on:** Mac (simulator + virtual panel)

**Acceptance Criteria:**

**Given** ASSIGN mode
**When** any encoder turns
**Then** the ASSIGN Overlay from the Story 4.19 mockup appears over the part area with K1–K8 and their localized parameter names (empty slots shown as "—"), the turned knob's slot raised with an accent border

**Given** the overlay is shown
**When** 3 s pass without a turn
**Then** it closes; each new turn restarts the 3 s timer

**Given** the overlay
**When** it is visible
**Then** it is informational only: keys, panel and scene/Live Set changes work normally, the Hero and Scene strip stay visible, and it takes no focus

### Story 4.28: On-screen Keyboard

As a keyboardist,
I want an on-screen keyboard with my language's accented letters,
So that I can type names and searches on the instrument.

**Covers:** UX-DR35, UX-DR10 (OSK never over the Top Bar or the edited field), UX-DR93 (partial: EN / PT-BR / ES layouts as catalog data)

**Runs on:** Mac (simulator)

**Acceptance Criteria:**

**Given** a text field (search or name)
**When** it receives focus
**Then** the On-screen Keyboard from the Story 4.16 mockup opens in the current UI language's layout (EN, PT-BR or ES), never covering the Top Bar or the edited field, with keys ≥56 px and the accented letters of the coverage rule

**Given** the keyboard is open
**When** the player types, confirms with WRITE or Done, or presses EXIT
**Then** text edits in place; WRITE/Done confirms; EXIT closes the keyboard keeping the typed text

**Given** the keyboard is open
**When** keys on the keybed are played
**Then** they still sound normally

### Story 4.29: Setlist surface

As a keyboardist,
I want to choose, create, rename and reorder setlists and their Live Sets, create a new Live Set and edit setlist tags on the instrument,
So that I can prepare a whole show without a computer.

**Covers:** FR84, UX-DR57, UX-DR22, UX-DR73 (first run no setlists), UX-DR72 (▶ NOW, ◆ on rows), UX-DR30 (◆ on List Rows), UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** the Setlist surface opened from the Launcher
**When** it shows setlists and the selected setlist's Live Sets as List Rows
**Then** VALUE scrolls, touch selects, WRITE on a setlist loads it (current sound keeps playing, Hint Bar says so), WRITE on a Live Set jumps to it, the now-playing row shows ▶ `NOW`, and Live Sets with edits show ◆ after the name

**Given** the actions on this surface
**When** the player creates or renames a setlist, adds a Live Set, chooses "+ New Live Set" (named with the On-screen Keyboard), reorders rows by drag or hold ▲▼, or creates/renames/assigns setlist tags
**Then** each change is a control-thread command, the list reflects the published state, and a new Live Set has 8 empty parts and one scene and becomes the current Live Set

**Given** no setlists exist
**When** the surface opens
**Then** it reads "No setlists yet." with "+ New setlist" highlighted

### Story 4.30: Setlist Browser with search, chips, tags and favorites

As a keyboardist,
I want to search every Live Set by name, filter by favorites, recent, setlists, all Live Sets and my tags, and jump off-script without losing my setlist,
So that I can play any song the band calls.

**Covers:** FR85, FR86 (off-script jump keeps setlist), UX-DR58, UX-DR23, UX-DR24, UX-DR25 (Setlist Browser use), UX-DR37 (favorites variant), UX-DR12 (setlist context → Setlist Browser), UX-DR73 (no results, empty favorites/recent), UX-DR98

**Runs on:** Mac (simulator + engine-host + librarian)

**Acceptance Criteria:**

**Given** the Setlist Browser opened from the Launcher or from the Top Bar setlist context on Performance
**When** it opens
**Then** it shows a Search Field ("searches setlists and Live Sets"), chips ★ Favorites, Recent, Setlists, All Live Sets, then user tags (one active, accent-filled), and a 3-column grid of Result Tiles with name, meta line (`LIVE SET · IN SUNDAY SERVICE`), ★ for favorites and ◆ for pending edits

**Given** a query typed on the On-screen Keyboard
**When** results update
**Then** Live Sets and setlists matching by name across all setlists are listed, and with no match it reads "No matches for 'x'. Try another word or a category."

**Given** a highlighted Live Set tile
**When** WRITE is pressed
**Then** the unit jumps to it seamlessly (preloaded or Loading state), the loaded setlist stays loaded, and LIVE SET ◀ ▶ returns into it; WRITE on a setlist tile loads that setlist

**Given** a long press on a Live Set tile
**When** the Action Sheet opens (this story builds the base Action Sheet component from the Story 4.19 design; it is its first use)
**Then** it offers `★ Add to Favorites` (or Remove from Favorites), WRITE confirms, EXIT cancels, and the change is stored through the librarian

**Given** an empty ★ Favorites or Recent chip
**When** it is selected
**Then** it reads "No favorites yet. Long-press a Live Set to add one." or "Nothing played yet." and the other chips stay available

### Story 4.31: Scenes surface

As a keyboardist,
I want to create, rename, duplicate, move and clear scenes on the instrument,
So that I can build each song's sections myself.

**Covers:** FR87, UX-DR62, UX-DR14 (long press → Scenes), UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** the Scenes surface from the Story 4.17 mockup, opened from the Launcher or by long press on a Scene Tile
**When** it shows slots 1–8
**Then** the current scene is marked ▶ and filled, existing scenes show their names and empty slots are dashed

**Given** the actions
**When** the player chooses New scene, Rename (On-screen Keyboard), Duplicate, Move ◀ ▶ or Clear
**Then** New copies the current scene into the next empty slot, Duplicate copies the highlighted scene into the next empty slot, Move changes its position, Clear empties it (the current scene cannot be cleared while it is the only scene), and the Live Set is marked ◆

**Given** the new scene order
**When** SCENE ◀ ▶, scene buttons 1–8 or the footswitch are used
**Then** they follow it

### Story 4.32: Write Dialog, Write hold and the Unsaved Indicator

As a keyboardist,
I want to hold WRITE to save my edits as a scene, the whole Live Set or a new Live Set, with ◆ showing anything not yet written,
So that I save on purpose and never by accident.

**Covers:** FR89, FR90, FR91, UX-DR29, UX-DR30, UX-DR63, UX-DR74, UX-DR10 (dialog over 35%-dimmed surface), UX-DR81 (Write hold), UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** pending edits on the current Live Set
**When** the player holds WRITE past the Write hold (default 0.5 s) on Performance, Part, Effects or Scenes
**Then** the Write Dialog opens over the dimmed surface: "Write changes?", scope options Scene / Whole Live Set / Save as new…, a name field for Save as new, a muted "Changed: …" line, `EXIT · Cancel` and accent `WRITE · Yes`
**And** Scene is shown unavailable in muted with "Timbre changes are saved with the Live Set" when the edits include a timbre change

**Given** no pending edits
**When** WRITE is held
**Then** no dialog opens and the Hint Bar shows "Nothing to write"

**Given** Save as new with a name already in the setlist
**When** WRITE is pressed
**Then** "A Live Set named 'x' is already in this setlist" is shown with the suggested free name filled in

**Given** storage full
**When** the write fails
**Then** "Not enough space to write. Free space in Settings › Storage & Library." is shown and ◆ stays

**Given** any Live Set with unwritten edits
**When** it appears in the Live Set Hero, List Rows or Result Tiles
**Then** a warn ◆ follows its name, with no text and no dialog; a successful Write clears it only for what was written

### Story 4.33: Part surface — timbre, zone and keyboard, level and output

As a keyboardist,
I want to open a part from Performance and change its timbre, zone, octave/transpose, keyboard on/off, volume and output,
So that I can build layers and splits on the instrument.

**Covers:** FR112 (timbre, zone and keyboard, level and output), FR16 (UI), UX-DR60, UX-DR82 (Part opens by touch or VALUE push), UX-DR86 (VALUE push ↔ Part surface of the outlined part), UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** Performance
**When** the player touches a Part Strip's card body or pushes VALUE (opens the knob-target part)
**Then** the Part surface from the Story 4.18 mockup opens with header `PART n · timbre · CATEGORY`, ⏻ and ★ (★ stores the timbre favorite once the Library catalog exists in Epic 5; until then it is hidden)

**Given** the Timbre section
**When** the player chooses Change timbre
**Then** a basic timbre list of installed timbres (List Rows, VALUE + WRITE) titled "CHOOSE TIMBRE FOR PART n" assigns the chosen timbre and marks the Live Set ◆; EXIT cancels without change (Epic 5 replaces this list with the full Library)

**Given** the Zone & keyboard and Level & output sections
**When** the player edits low key, high key, octave, transpose, keyboard on/off, volume and Main/Aux with VALUE or touch
**Then** each change is heard immediately and marks the Live Set ◆
**And** low key and high key can also be set by pressing a key on the keybed while the field is highlighted

**Given** EXIT on the Part surface
**When** pressed
**Then** it returns to Performance (or to the surface that opened it), keeping the edits

### Story 4.34: Part surface — sound, envelope and EQ, effects

As a keyboardist,
I want the part's engine sound parameters, envelope, EQ and effect sends on its Part surface,
So that I can shape one sound completely on the instrument.

**Covers:** FR112 (sound, envelope and EQ, insert chain and sends), FR9 (UI: organ percussion, key click, vibrato/chorus, leakage), FR10 (partial: rotary in the organ Sound section), UX-DR60, UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** a sample part
**When** the Sound and Envelope & EQ sections are shown
**Then** the sample engine's parameters, Brightness, Attack, Release and the other envelope stages, and EQ Low, Mid, Mid freq and High are editable with VALUE, and the encoders in PART mode change the same values on this part

**Given** an organ part
**When** the Sound section is shown
**Then** it matches the Story 2.16 design: drawbars with the numeric registration, percussion, key click, vibrato/chorus, leakage and rotary, all editable and heard immediately

**Given** the Effects section
**When** the player opens the insert chain link
**Then** the Epic 2 Effects surface opens on this part's inserts, and reverb and delay send levels are editable here directly

**Given** the Arpeggiator section
**When** the Part surface is shown before Epic 7
**Then** the section is not shown (it is added in Epic 7)

### Story 4.35: Settings surface with Knobs · Global Assign

As a keyboardist,
I want a Settings surface where I edit the global default ASSIGN map,
So that every new Live Set starts with my preferred knob layout.

**Covers:** FR102 (UI), UX-DR65 (partial: Settings surface frame and Knobs · Global Assign section), UX-DR34, UX-DR98

**Runs on:** Mac (simulator + engine-host)

**Acceptance Criteria:**

**Given** the Settings tile in the Launcher
**When** it opens
**Then** a Settings surface shows the seven section names of EXPERIENCE.md in order, with only the sections whose rows exist enabled (Knobs · Global Assign now; others added by later stories and epics)

**Given** Knobs · Global Assign
**When** the player highlights K1–K8 and picks a parameter from the registry's assignable list (or clears the slot)
**Then** the global default map changes through a control-thread command and is used by the next new Live Set

**Given** the per-Live-Set map
**When** the player opens "ASSIGN · this Live Set" from the same section
**Then** the current Live Set's map is edited the same way and marks it ◆

### Story 4.36: Footswitch function

As a keyboardist,
I want the footswitch to advance the scene by default, or go to the previous scene, the next Live Set or toggle rotary speed,
So that I can change sections hands-free.

**Covers:** FR109, AR39 (partial: footswitch function in `sysd` config), UX-DR65 (partial: Keyboard & Pedals › Footswitch function row), UX-DR86 (footswitch ↔ Hero / current Scene Tile)

**Runs on:** Mac (virtual panel); prototype board

**Acceptance Criteria:**

**Given** the switch pedal event from Story 3.11
**When** it is pressed with the default setting
**Then** the scene advances (same result as SCENE ▶), seamlessly

**Given** the Settings › Keyboard & Pedals › Footswitch function row (Segmented Control: Next scene / Previous scene / Next Live Set / Rotary speed)
**When** the player changes it
**Then** the value is stored in the `sysd` config and the footswitch follows it immediately; Rotary speed toggles slow/fast on the organ

**Given** Next Live Set at the end of a setlist
**When** the footswitch is pressed
**Then** nothing happens, as with LIVE SET ▶

### Story 4.37: Accidental-press guards and Touch on Performance

As a keyboardist,
I want guards against accidental presses: touch modes for the Performance screen, an adjustable long press, a configurable Write hold and an optional panel guard on LIVE SET ◀ ▶,
So that a brushed screen or a stray press never changes my show.

**Covers:** FR110, UX-DR80, UX-DR81, UX-DR82, AR39 (partial: guard settings in `sysd` config), UX-DR65 (partial: Display & Language rows Touch on Performance, Write hold, Long press, Panel guard), FR115 (Touch on Performance, Write hold, Long press, Panel guard rows)

**Runs on:** Mac (simulator + virtual panel, touch emulation); prototype board (touch panel)

**Acceptance Criteria:**

**Given** Settings › Display & Language
**When** the guard rows render
**Then** they show Touch on Performance (Full / Safe / Off, default Safe), Write hold (0.3–1.0 s in 0.1 s steps, default 0.5 s, never 0), Long press (400 / 500 / 800 ms, default 500 ms) and Panel guard (Off / On, default Off, with the line "On: LIVE SET ◀ ▶ act only after a 300 ms hold")

**Given** Touch on Performance = Safe (default)
**When** the player touches ⏻ or a Scene Tile
**Then** it shows an armed state on press-down and acts on release only after ≥150 ms contact with ≤10 px movement; the Part Strip card body, setlist context and `REV` act on long press

**Given** Touch on Performance = Full or Off
**When** the same targets are touched
**Then** Full acts on tap (card body tap → Part, long press → knob target), and Off ignores ⏻ and Scene Tile touches while long-press targets still work, per the EXPERIENCE.md matrix
**And** knob-target selection by touch is available only in Full

**Given** Long press (400 / 500 / 800 ms) and Write hold (0.3–1.0 s, never 0)
**When** they are changed in Settings › Display & Language
**Then** part buttons, touch long presses and WRITE hold use the new values immediately

**Given** Panel guard = On
**When** LIVE SET ◀ or ▶ is pressed briefly
**Then** nothing happens, and a hold of 300 ms steps the Live Set; with Panel guard Off a press steps

**Given** all four rows
**When** they are stored
**Then** they live in the `sysd` config with `schema_version` and survive a reboot
**And** on a new unit's first boot the defaults above apply

**Given** each row is changed
**When** automated UI tests run in the SDL simulator
**Then** they check each value against the EXPERIENCE.md Touch on Performance matrix and timings table (Full / Safe ≥150 ms and ≤10 px acting on release / Off; Write hold before the Write Dialog; the long-press threshold for touch and part buttons; the Panel guard hold on LIVE SET ◀ ▶)

### Story 4.38: First playable unit — full show run-through

As a founder,
I want a keyboardist to build and play a full show on the prototype using only the instrument,
So that the month-12 first-playable-unit milestone is proven end to end.

**Covers:** NFR50 (month 12: samples, organ, effects on prototype hardware), NFR14, NFR16, NFR15, FR79–FR91 (end-to-end), FR104–FR110 (end-to-end)

**Runs on:** Prototype board

**Acceptance Criteria:**

**Given** the prototype with keybed, panel, codec outputs, the Epic 1 demo libraries (the protected factory library arrives in Epic 6) and no computer connected
**When** a keyboardist follows Flow 4 (build "Goodness of God": new Live Set, timbres on 3 parts as layer and split, reverb sends, a compressor insert, two scenes, Write Whole Live Set)
**Then** every step works on the instrument and sounds as it goes

**Given** a setlist of at least 6 Live Sets including an organ song
**When** the keyboardist follows Flow 1 (load the setlist, play through with footswitch scene changes, fader pickup, DRAWBARS with rotary S/F, LIVE SET ▶ steps, an off-script jump via the Setlist Browser and back, an edit and Write Scene)
**Then** no note is cut on any switch, the next Live Set shows `● READY`, the engine counters show zero xruns and zero underruns, and no disk write occurs except the Write

**Given** the run
**When** it is reported
**Then** the report lists each flow step as pass/fail with any issue filed, and states the milestone result

## Epic 5: Library: import and find timbres

A keyboardist imports SFZ/SF2 libraries from a pen drive or SSD with suggested categories, finds timbres by search, category, favorites and recent, auditions them while browsing, recovers from missing libraries and removes libraries to free space. The epic builds on Epic 1's formatlib lossless bundle, demo SFZ importer, atomic publish and index/`current` resolution, and on the device UI shell, Part surface, Live Set state and Setlist Browser components (Search Field, Filter Chip, Result Tile, On-screen Keyboard) delivered in Epic 4. Development is Mac-first: formatlib, librarian and importer logic run on the Mac; device UI runs in the LVGL SDL simulator at 1024×600 against a host-built librarian and engine; USB-A host mounting, the sandbox and throughput acceptance need the dev board or prototype.

### Story 5.1: Full SFZ v1 opcode subset in the importer

As a keyboardist,
I want every SFZ file that stays within the v1 opcode subset to import with its keyswitches, crossfades, loops and filters intact,
So that my libraries keep the articulations and expression they were built with.

**Covers:** FR18 (full subset), FR22 (partial: per-file skip reasons), AR27 (partial: instrument model extension), NFR44 (partial: bundle `format_version`)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the Epic 1 importer and instrument model in `formatlib/`
**When** the subset is extended
**Then** the model and importer also map keyswitches (`sw_lokey`, `sw_hikey`, `sw_last`, `sw_down`, `sw_up`, `sw_previous`, `sw_default`), CC crossfades (`xfin_locc`/`xfin_hicc`, `xfout_locc`/`xfout_hicc`, `xf_cccurve`), loops (`loop_mode` no_loop / one_shot / loop_continuous / loop_sustain, `loop_start`/`loop_end`) and basic filters (`fil_type` lpf_1p / lpf_2p / hpf_1p / hpf_2p / bpf_2p, `cutoff`, `resonance`)
**And** a unit test per opcode compares the parsed region table with the bundle's instrument table for a fixture SFZ using it

**Given** the extended model
**When** bundles are written
**Then** the bundle `format_version` minor is bumped, the reader accepts N and N−1, and every Epic 1 demo bundle still opens and renders bit-identically

**Given** an SFZ file using an opcode outside the subset (scripting, effect buses, any unlisted opcode that affects sound)
**When** it is imported
**Then** the file is skipped with reason code `unsupported_features` and the list of offending opcodes in the import report, and the other files of the batch import
**And** an explicit allow-list of non-sounding opcodes (for example `label_cc`, `image`) is accepted and logged instead of causing a skip

**Given** an SFZ file that cannot be parsed, references a missing sample, or points at an unreadable or truncated WAV
**When** it is imported
**Then** the file is skipped with reason code `damaged` and the failing path, and nothing of that file is written

### Story 5.2: Sample engine plays keyswitches, CC crossfades and basic filters

As a keyboardist,
I want imported instruments to respond to keyswitches, crossfade controllers and their filters exactly as the library intends,
So that an imported instrument plays like it does in the reference player.

**Covers:** FR18 (playback of the full subset), NFR19, NFR13, AR13 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** an instrument with keyswitches loaded in the sample engine
**When** a keyswitch key is pressed (or held, for `sw_down`/`sw_up`)
**Then** region selection follows the active keyswitch, `sw_default` applies after load, the keyswitch state is per part, and keyswitch keys themselves make no sound unless the SFZ maps regions to them

**Given** an instrument with `xfin_*`/`xfout_*` on a controller
**When** that controller (mod wheel, expression pedal or any CC routed to the part) moves
**Then** the region gains follow the crossfade ranges and `xf_cccurve` (gain or power) and change without zipper noise (per-block smoothing)

**Given** regions with `fil_type`, `cutoff` and `resonance`
**When** they render
**Then** each voice runs its filter inside the voice render, with coefficients computed off the hot path or per block, and the RTSan render test reports no allocation, lock or blocking call

**Given** a committed MIDI test suite exercising keyswitch changes, crossfade sweeps and filtered regions on at least one Karoryfer instrument and one fixture instrument per feature
**When** the null-test harness compares the engine render with sfizz rendering the original SFZ
**Then** the residual is below the per-test tolerance recorded with the tests, and the tests run in CI on every sample-engine change

### Story 5.3: SF2 import into the same instrument model

As a keyboardist,
I want to import SoundFont 2 files,
So that my SF2 collection plays on the Velora alongside my SFZ libraries.

**Covers:** FR19, FR3, NFR13, AR27 (partial: SF2 reader)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** an SF2 reader in `formatlib/` (RIFF `sfbk`, `INFO`, `sdta` with 16-bit samples and the optional `sm24` chunk, `pdta`)
**When** an SF2 file is imported
**Then** each preset becomes one instrument with `instrument_key = bank:preset`, and zones map key range, velocity range, root key (`overridingRootKey` or sample pitch), coarse/fine tune, `sampleModes` loops, attenuation, pan, volume envelope (delay, attack, hold, decay, sustain, release) and `initialFilterFc`/`initialFilterQ` into the internal model
**And** sample data is stored at its source bit depth and rate and decodes bit-identical to the SF2 sample data

**Given** a preset that uses SF2 features outside the model (non-default modulators, LFOs, modulation envelope)
**When** it is imported
**Then** the preset imports with those modulators and generators ignored (never skipped for them), and the import report lists each ignored item per preset as a warning; the rule is documented in the importer and covered by a test

**Given** a damaged SF2 (bad chunk sizes, sample offsets outside `sdta`)
**When** it is imported
**Then** the file is skipped with reason code `damaged` and the batch continues

**Given** two SF2 fixtures (one General MIDI style bank, one single-instrument file)
**When** they are imported and rendered by the offline renderer
**Then** a structural test matches every zone to the source, and a render comparison against an unshipped reference player (built only in the test job) stays within a tolerance recorded with the tests

### Story 5.4: Reference corpus imports and plays with mapping intact

As a founder,
I want every library of the reference corpus imported and null-tested automatically,
So that "imports and plays with mapping intact" is proven on real libraries and protected against regressions.

**Covers:** FR20, NFR19, AR2 (partial: null tests), EXT5 (dependency: sfizz license check)

**Runs on:** Mac + CI (nightly)

**Blocked by:** Story 11.12 (target month 5)

**Acceptance Criteria:**

**Given** a corpus manifest under `tools/corpus/` pinning Salamander Grand Piano, VSCO2, Sonatina Symphonic Orchestra, the Karoryfer libraries and a named Pianobook selection (≥5 instruments chosen by the founder), each with source URL, version and content hash
**When** the corpus job runs
**Then** it fetches (or reuses a cached copy of) each library, verifies the hash, and imports it with the host importer
**And** no corpus file is skipped except entries explicitly listed with a reason in the manifest

**Given** the imported corpus
**When** a per-library MIDI test suite (range, velocity layers, round robin, keyswitches, releases, sustained loops) is rendered
**Then** each render passes the sfizz null test within its recorded tolerance, and a report lists per-library results and residuals

**Given** CI
**When** the nightly corpus job finishes
**Then** any import failure or null-test failure fails the job, and corpus audio is never committed to the repository

### Story 5.5: Category model, localized labels and auto-suggestion

As a keyboardist,
I want every timbre to carry one of 17 categories, with a sensible suggestion when I import,
So that I can find timbres by what they sound like in my own language.

**Covers:** FR21 (category model and suggestion), FR58 (label source), UX-DR94, NFR30, AR45 (partial: category keys)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** `proto/`
**When** the category type is added
**Then** it defines the 17 categories (Piano, E.Piano, Organ, Keys, Mallet & Bells, Strings, Brass, Woodwind, Choir & Vocal, Guitar, Bass, Synth Lead, Synth Pad, Synth Poly, Drums & Perc, World, FX & Ambient) with stable numeric IDs that only grow by addition, plus an explicit "none" value that can never be stored on an imported instrument

**Given** the locale catalog
**When** the category keys are added
**Then** each category has a full and a short label in EN, PT-BR and ES exactly as in the EXPERIENCE.md Categories table, every short label is ≤10 characters, and a test fails if any key is missing in a shipped locale or a short label exceeds 10 characters

**Given** a suggestion module in `librarian/`
**When** it receives an instrument's file name, folder names and available metadata (SFZ `<control>` labels and comments, SF2 preset and bank names)
**Then** it returns one suggested category or none, using a versioned keyword table in EN, PT-BR and ES (for example `rhodes`, `wurli` → E.Piano; `viola`, `violino`, `strings`, `cordas` → Strings; `upright`, `baixo` → Bass)
**And** a fixture test over at least 60 real-world file names from the reference corpus and the Flow 2 examples (`Rhodes_Mk1` → E.Piano, `Upright_Bass_Jazz` → Bass, `Viola_Caipira` → none) matches the expected suggestion for each

### Story 5.6: Librarian libraries catalog, import API and atomic publish

As a keyboardist,
I want imported libraries to appear only when they are complete and verified, and to stay listed correctly across restarts,
So that I never play a half-imported or broken library.

**Covers:** FR27 (atomic publish), FR24, AR26 (librarian datasets), AR28 (publish order, startup reconcile), AR29 (partial: catalog `schema_version`), AR25 (partial: Library API messages), AR10 (partial: extends the Story 4.11 `librarian` service)

**Runs on:** Mac (service runs as a host process); packaged as a systemd unit for the board

**Acceptance Criteria:**

**Given** the minimal `librarian` service and catalog from Story 4.11
**When** this story extends it with the Epic 1 importer and publish pipeline
**Then** the catalog gains only the tables this story needs (libraries, instruments with name, category and source info) with its `schema_version` incremented, and the Library API gains import request, import progress events, list libraries and list instruments
**And** it is the only process that writes under `/data/lib` and to the catalog

**Given** an import request for a source folder with a category per instrument
**When** the import runs
**Then** each instrument is converted, staged, verified by bit-exact decode round-trip, fsynced, renamed and `current` is flipped, and only then the catalog rows are written, in that order
**And** an import request with any instrument lacking a category is rejected with a typed error before anything is written

**Given** the bundle manifest
**When** a user library is published
**Then** the manifest records each instrument's name and category, so the catalog can be rebuilt from bundles alone
**And** the bundle carries no encryption and the librarian links no encryption code path (verified by a symbol check in CI)

**Given** the librarian is killed at random points during 200 scripted imports
**When** it restarts
**Then** the startup reconcile pass deletes staging leftovers, removes catalog rows whose library has no `current`, and recreates missing catalog rows from the manifests of bundles under `current`, so catalog and `/data/lib` always agree and no partial library is listed

### Story 5.7: Import preflight: batch scan, free space and duplicates

As a keyboardist,
I want the unit to show me what is on the drive, what will be skipped and whether it fits before anything is written,
So that an import never surprises me halfway through.

**Covers:** FR22, FR23 (detection), FR25, FR21 (mandatory category check), AR10 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a mounted source volume
**When** the librarian receives a preflight request for it
**Then** it scans for `.sfz` and `.sf2` files without converting anything, groups them into libraries by top-level folder (files at the volume root form one library named after the volume label), and returns one row per timbre with name, size, format, suggested category (Story 5.5) and status `ready`, `needs category`, `skipped: unsupported_features`, `skipped: damaged` or `already imported`

**Given** a library whose `library_id` (minted by formatlib from the source fingerprint) is already in the catalog
**When** preflight runs
**Then** its timbres already present are marked `already imported` with the action defaulting to Skip, and the API accepts Import again per row

**Given** the rows selected to import
**When** preflight computes space
**Then** it reports required bytes (worst-case: uncompressed PCM size plus staging and a fixed reserve) and free bytes on `/data`, and an import request whose required space exceeds free space is refused with a typed error carrying both numbers

**Given** a 1,000-file test volume on the Mac
**When** preflight runs
**Then** it completes without reading sample data beyond headers needed for size and damage checks, and its duration is reported (host figures labelled not representative)

### Story 5.8: Import execution survives drive removal and power loss

As a keyboardist,
I want finished timbres kept and the timbre in progress discarded whole if the drive is pulled or power fails,
So that a mistake at the USB port never leaves a broken library.

**Covers:** FR27 (drive removal), FR23 (Import again replaces), AR28 (partial), NFR12 (partial: import side, power-cut campaign in Epic 8)

**Runs on:** Mac (disk-image detach); verified on the dev board with a physical drive

**Acceptance Criteria:**

**Given** an import of several timbres from one library
**When** timbres complete
**Then** a finished timbre becomes visible by publishing a new bundle for that library that contains all finished timbres, reusing already verified sample data from the previous bundle without copying it again (for example hard links into the new bundle directory), so published bundles stay immutable
**And** the librarian publishes at a checkpoint interval (configurable, default every 10 timbres or 20 GB) rather than after every tiny file

**Given** the source volume disappears mid-import (disk image detached on the Mac, USB drive pulled on the board)
**When** the librarian detects I/O errors or the unmount event
**Then** it publishes the timbres finished so far, discards the timbre in progress whole, ends the job with status `drive_removed` and the counts imported vs total, and leaves earlier libraries untouched

**Given** Import again on a row marked `already imported`
**When** the import finishes
**Then** the library's `current` points at a new bundle with the re-converted timbre, the `library_id` and `instrument_key` are unchanged, and a Live Set referencing that timbre resolves it without edits

**Given** 50 kills of the librarian process at random points of an import on the Mac
**When** it restarts each time
**Then** the reconcile pass leaves only fully verified published bundles and matching catalog rows

### Story 5.9: Sandboxed librarian and read-only USB-A drives on the board

As a keyboardist,
I want drives I plug in to be read safely and imported without disturbing the sound,
So that a strange file on a pen drive can never harm the instrument or interrupt a show.

**Covers:** FR28 (partial: read-only mount, sandboxed parsing), NFR24 (partial: USB-A mount options, librarian sandbox), NFR16, AR10 (librarian sandboxed, low priority, off audio cores)

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** a USB mass-storage drive (FAT32, exFAT or ext4) inserted in a USB-A host port
**When** udev and the mount helper handle it
**Then** it is mounted read-only with `nosuid,nodev,noexec` under a fixed mount root, NTFS and other filesystems are reported as unsupported, and no process other than the librarian reads its contents

**Given** the `librarian` systemd unit
**When** it runs
**Then** it is confined with systemd sandboxing (own user, `ProtectSystem=strict`, write access only to `/data/lib`, the catalog and its staging dir, no network, restricted syscalls via `SystemCallFilter`, `NoNewPrivileges`), pinned off the isolated audio cores, with low CPU and I/O priority (`Nice`, `IOSchedulingClass=idle` or equivalent)
**And** a test that tries to write outside the allowed paths, open a socket or exec a binary from the librarian fails with a permission error

**Given** the engine playing 128 sustained stereo sample voices
**When** a 20 GB import runs from a USB SSD
**Then** the engine reports zero xruns and zero stream underruns for the whole import, and the import throughput is recorded

### Story 5.10: Design story — Notice Toast mockup

As a keyboardist,
I want a drawn, reviewed design for the Notice Toast before it is built,
So that non-modal notices look and behave consistently across USB, update, brightness and status messages.

**Covers:** UX-DR108, UX-DR31 (spec input), UX-DR97, UX-DR10 (overlay layering)

**Runs on:** Design (HTML mockup in the UX folder)

**Acceptance Criteria:**

**Given** the DESIGN.md tokens and the existing device mockups
**When** the Notice Toast mockup is drawn
**Then** it shows the variants "USB drive found · Import?", "Update n found · Open?", the brightness overlay (label + Progress Bar), "Updated to n" and the rolled-back notice, in Stage and Studio themes, on Performance and on a non-Performance surface
**And** the toast sits above the Status/Hint Bar and never covers the Live Set Hero or Scene strip

**Given** the mockup
**When** it is reviewed
**Then** it passes pseudo-localization at +40% with accented capitals and `ÁÇÊÕÑ Ação`, meets the ≥4.5:1 text and ≥3:1 boundary contrast floor, and records touch, auto-dismiss (8 s, paused while touched), EXIT and ASSIGN-Overlay waiting rules in its annotations

### Story 5.11: Design story — Category Picker mockup

As a keyboardist,
I want a drawn, reviewed design for the Category Picker before it is built,
So that choosing a category is fast with VALUE or touch in every language.

**Covers:** UX-DR104, UX-DR36 (spec input), UX-DR94, UX-DR97

**Runs on:** Design (HTML mockup in the UX folder)

**Acceptance Criteria:**

**Given** the DESIGN.md Category Picker spec and the 17-category label table
**When** the mockup is drawn
**Then** it shows the raised panel with a grid of the 17 full localized labels, the accent-bordered highlight, the "Apply one category to all" row as it appears in Import, and the Hint Bar (`VALUE` choose, `WRITE` pick, `EXIT` cancel), in Stage and Studio themes
**And** every tile is ≥56 px with ≥12 px spacing

**Given** the mockup
**When** it is reviewed
**Then** it is drawn in EN, PT-BR and ES (the longest full labels "Efeitos & Ambiências", "Bateria & Percussão", "Percusión Melódica" fit without clipping) and passes pseudo-localization at +40% with `ÁÇÊÕÑ Ação`

### Story 5.12: Import surface: rows, categories and apply to all

As a keyboardist,
I want to see every timbre on the drive with its suggested category and fix the ones that need one,
So that each timbre lands in the right category with as few touches as possible.

**Covers:** FR21, FR22 (row display), FR23 (row display), UX-DR26, UX-DR27, UX-DR36, UX-DR64 (rows and category handling), UX-DR75 (needs category, unsupported/corrupt, duplicate), UX-DR19, UX-DR98

**Runs on:** Mac (LVGL simulator against the host librarian)

**Acceptance Criteria:**

**Given** the Import surface opened from the Launcher (MENU › Import) with a drive selected
**When** the librarian preflight (Story 5.7) returns
**Then** the surface lists full-width Import Rows with file name, size and format, a Category Chip and a status, with a header stating the drive name, timbre count and total size
**And** problem statuses (`needs category`, `skipped: uses SFZ features Velora doesn't support`, `skipped: file is damaged`, `already imported`) are in warn with text, never colour alone

**Given** a row with a suggestion
**When** it is shown
**Then** its Category Chip reads the full localized label with `✓ suggested` in accent; a row without one reads `Choose category ▾` with a warn border

**Given** the Category Picker built from the Story 5.11 design
**When** the player opens it from a row and picks with VALUE + WRITE or touch
**Then** the row takes the category and becomes `ready`; choosing "Apply one category to all" sets every non-skipped row of the batch

**Given** any non-skipped row without a category
**When** the player presses WRITE to start
**Then** the import does not start and the Hint Bar reads "⚠ n timbre(s) need a category before import"

**Given** a row marked `already imported`
**When** it is highlighted
**Then** the player chooses Skip (default) or Import again on that row

### Story 5.13: Import surface: progress, background and failure states

As a keyboardist,
I want to start an import, keep playing while it runs and be told plainly when space, the drive or a file is a problem,
So that importing never costs me sound or leaves me guessing.

**Covers:** FR25, FR26, FR27 (drive removed message), FR22 (skipped summary), UX-DR28, UX-DR31 (partial: Notice Toast component), UX-DR64 (background and failure states), UX-DR75 (background, insufficient storage, drive removed), UX-DR95 (CLDR sizes), NFR16

**Runs on:** Mac (LVGL simulator against the host librarian)

**Acceptance Criteria:**

**Given** all rows ready
**When** the player presses WRITE and the batch does not fit
**Then** the import does not start and the surface reads "Needs 18.4 GB, 9.2 GB free. Free space in Settings › Storage & Library or import fewer files." with sizes formatted per locale (`18.4 GB` EN, `18,4 GB` PT-BR)

**Given** the batch fits
**When** the import starts
**Then** a Progress Bar shows "Importing n of m · name · converting & verifying (lossless)", and EXIT sends the import to the background while keys keep playing; returning to Import shows current progress

**Given** the import finishes
**When** the player is on another surface
**Then** a Notice Toast (the component is built in this story from the Story 5.10 design) reports how many timbres were imported and skipped, and the imported timbres appear in the catalog with their categories

**Given** the drive is removed mid-import
**When** the librarian reports `drive_removed`
**Then** the surface reads "USB drive removed. 3 of 7 timbres imported; the rest were not changed. Reconnect to import them." (with the real counts)

**Given** an import running in the background in the simulator with a host engine playing
**When** the UI process is killed and restarted
**Then** the import continues in the librarian, the engine keeps sounding, and the restarted Import surface shows the current progress

### Story 5.14: "USB drive found" notice

As a keyboardist,
I want a quiet notice when I plug in a drive, with one touch to start importing,
So that importing is easy without anything popping open on stage.

**Covers:** FR29, UX-DR31, UX-DR72 (USB drive inserted), NFR16

**Runs on:** Mac (LVGL simulator with a simulated drive event); dev board (real hotplug)

**Acceptance Criteria:**

**Given** the Notice Toast component from Story 5.13
**When** the librarian reports a newly mounted drive containing at least one `.sfz` or `.sf2` file
**Then** a Notice Toast "USB drive found · Import?" appears; it never takes focus, never opens a surface by itself, auto-dismisses after 8 s (paused while touched) and EXIT dismisses it
**And** touching the action opens the Import surface (Stories 5.12 and 5.13) for that drive, while MENU › Import stays the persistent path

**Given** a drive whose root holds a firmware bundle
**When** it is mounted
**Then** the toast reads "Update n found · Open?" instead (the action target is wired by Epic 8; until then it opens nothing and the toast says only "Update n found")

**Given** the ASSIGN Overlay is open
**When** a drive is inserted
**Then** the toast waits until the overlay closes, and it never covers the Live Set Hero or Scene strip

**Given** the dev board
**When** drives are inserted and removed 50 times during playback
**Then** each insertion raises at most one toast, sound is never interrupted and the engine reports zero xruns

### Story 5.15: 300 GB SSD library imports and plays end to end

As a keyboardist,
I want to import a 300 GB SFZ library from an external SSD and play it,
So that my biggest libraries work on the Velora without a computer.

**Covers:** FR28, FR27, FR26, NFR16, NFR24 (partial)

**Runs on:** Dev board or prototype

**Acceptance Criteria:**

**Given** a USB 3 SSD holding ≥300 GB of SFZ + WAV content within the v1 subset, and ≥600 GB free on the internal NVMe
**When** the player imports it from the Import surface
**Then** the import completes, every timbre is published and listed in the Library with its category, and the measured throughput and total time are recorded in the story report

**Given** the import running in the background
**When** a scripted player keeps 128 stereo voices sounding and switches scenes during the whole import
**Then** the engine reports zero xruns and zero stream underruns and no switch cuts sound

**Given** the import finished
**When** instruments from the start, middle and end of the library are assigned and played
**Then** they play with mapping intact, and their renders on the board match the Mac importer's bundles bit-for-bit

### Story 5.16: Library surface: search, categories, favorites and recent

As a keyboardist,
I want to find any timbre by typing, by category, in my favorites or among recent ones,
So that I reach the sound I want in seconds even with thousands of timbres.

**Covers:** FR56, FR58, FR61, UX-DR59, UX-DR23, UX-DR24, UX-DR25, UX-DR73, UX-DR94, UX-DR95, NFR9 (partial)

**Runs on:** Mac (LVGL simulator against the host librarian)

**Acceptance Criteria:**

**Given** the catalog
**When** this story adds search
**Then** the librarian builds an FTS5 index over timbre name, library name and localized category labels, adds a recent table and a timbre favorites table (empty until the starring UI later in this epic writes to it), and exposes search, filter-by-category, favorites and recent queries through the Library API

**Given** the Library surface opened from the Launcher
**When** it renders
**Then** it shows a Search Field (typing through the On-screen Keyboard, keys still sounding), Filter Chips in order ★ Favorites, Recent, then the 17 categories with full localized labels, and a 3-column grid of Result Tiles with name and a meta caps line `CATEGORY · FACTORY` or `CATEGORY · IMPORTED`
**And** exactly one chip is active and accent-filled, and VALUE scrolls the grid while touch selects

**Given** a query or chip with no results
**When** the grid is empty
**Then** it reads "No matches for 'x'. Try another word or a category.", "No favorites yet. Long-press a timbre to add one." or "Nothing played yet." as applicable, and the other chips stay available

**Given** a catalog with 1,200 generated instruments
**When** the player types a query
**Then** results update per keystroke within 100 ms on the Mac (host figure, not representative of the board)

**Given** a timbre name anywhere in the UI (Result Tile, Part Strip, Part surface header)
**When** it is shown
**Then** it carries its category from the catalog, the short label on Part Strips and the full label elsewhere, in the selected language, and never the engine type

### Story 5.17: Choose a timbre for a part with audition

As a keyboardist,
I want to hear the highlighted timbre when I play the keys and assign it with WRITE,
So that I choose by ear without my hands leaving the keyboard.

**Covers:** FR59, FR58, UX-DR25 (key-play preview, WRITE assigns), UX-DR59 ("CHOOSE TIMBRE FOR PART n"), UX-DR79 (partial), NFR16

**Runs on:** Mac (LVGL simulator + host engine)

**Acceptance Criteria:**

**Given** Part › Change timbre on part n (or the Library opened from the Launcher, targeting the knob-target part)
**When** the Library opens
**Then** its header reads "LIBRARY · CHOOSE TIMBRE FOR PART n"

**Given** a highlighted Result Tile
**When** the highlight rests for 150 ms
**Then** the engine control thread prepares a preview generation with that timbre on part n, swaps it in when its heads are resident, and keys played on part n audition it; the previous timbre keeps sounding until the preview is ready, and held notes finish on their original generation

**Given** a preview that would exceed the head budget
**When** it is requested
**Then** the preview is refused, the current timbre keeps playing and the Hint Bar states that there is not enough memory to preview

**Given** the previewed timbre
**When** the player presses WRITE
**Then** it is assigned to part n as a Live Set edit (◆ on the Live Set), the librarian records it in Recent, and EXIT returns to the Part surface showing the new timbre and category

**Given** the Library in choose mode
**When** the player presses EXIT without WRITE
**Then** part n returns to its previous timbre without a cut and no edit is recorded

### Story 5.18: Design story — Action Sheet library variants (missing library, remove library, install, pairing)

As a keyboardist,
I want the library-related Action Sheet variants drawn and reviewed on top of the existing Action Sheet design,
So that every short choice (missing library, removal, install, pairing) looks and works like the favorites sheet I already use.

**Covers:** UX-DR105 (complete: library, removal, install and pairing variants; base design from Story 4.19), UX-DR37 (spec input), UX-DR10 (centred over 35%-dimmed surface), UX-DR97

**Runs on:** Design (HTML mockup in the UX folder)

**Acceptance Criteria:**

**Given** the base Action Sheet design in `mockups/action-sheet.html` from Story 4.19 (raised card, lg radius, centred over the surface dimmed to 35%, 2–4 rows ≥56 px, favorites variant)
**When** the library variants are added to the same mockup without changing the base anatomy
**Then** it shows Missing library (`WRITE · Replace timbre`, Import, `EXIT · Cancel`), Remove library confirmation (names the library and states that Live Sets using it will show Missing library), pairing code, and "Install update? Sound stops until the Velora restarts." (`WRITE · Install`, `EXIT · Later`), in Stage and Studio themes

**Given** the mockup
**When** it is reviewed
**Then** it passes pseudo-localization at +40% with `ÁÇÊÕÑ Ação`, meets the contrast floor, and its annotations state VALUE highlight, WRITE confirms, EXIT cancels, and "never unprompted during performance"

### Story 5.19: Favorites by long press and from the Part surface

As a keyboardist,
I want to star timbres with a long press or the ★ on the Part surface,
So that my go-to sounds are always two touches away.

**Covers:** FR60, FR57, NFR9, UX-DR37, UX-DR25 (★ and long press), UX-DR60 (partial: ★ button)

**Runs on:** Mac (LVGL simulator); board timing check on the dev board or prototype

**Acceptance Criteria:**

**Given** the existing Action Sheet component built in Story 4.30 (base design from Story 4.19), extended in this story with the library variants drawn in Story 5.18 (no second sheet component)
**When** the player long-presses a Result Tile (threshold from the Long press setting)
**Then** the Action Sheet offers `★ Add to Favorites`, or Remove from Favorites when already starred; WRITE confirms, EXIT cancels
**And** the librarian stores the favorite in the favorites table from Story 5.16, and the tile shows ★ in accent

**Given** the Part surface
**When** the player presses ★
**Then** the current timbre is added to or removed from Favorites, and the ★ Favorites chip in the Library reflects it immediately

**Given** a catalog of 1,200 instruments with 20 favorites
**When** an automated UI test drives the Library
**Then** every favorite is highlighted within 2 interactions (★ Favorites chip, tile) and every instrument is assignable within 3 interactions through a category or a search query (query entry counted as one interaction, per the EXPERIENCE.md Flow 3 counting rule)
**And** on the dev board search results update within 100 ms per keystroke

### Story 5.20: Missing library state and recovery

As a keyboardist,
I want a part whose library is gone to stay silent, say so, and offer to replace the timbre or import the library,
So that I never get a wrong sound on stage and can fix it in a couple of touches.

**Covers:** FR114, UX-DR15 (Missing library state), UX-DR18 (warn takeover), UX-DR72 (missing library), UX-DR37

**Runs on:** Mac (LVGL simulator + host engine)

**Acceptance Criteria:**

**Given** a Live Set whose part references a `library_id` or `instrument_key` that does not resolve
**When** it loads
**Then** the part plays silent and is never substituted, its Part Strip shows "Missing library" in warn with text, the Part surface header shows the stored timbre name with "Missing library", and the Status Bar warn takeover names the part

**Given** the Missing library part
**When** the player touches its Part Strip card or presses WRITE on its Timbre section
**Then** an Action Sheet offers `WRITE · Replace timbre` (opens the Library in "CHOOSE TIMBRE FOR PART n") or Import (opens the Import surface), and `EXIT · Cancel`; it is never raised unprompted

**Given** the missing library is imported later with the same `library_id`
**When** the import publishes
**Then** the part resolves on the next generation without any Live Set edit and the Missing library state clears

### Story 5.21: Storage & Library: free space, installed libraries and removal

As a keyboardist,
I want to see free space and installed libraries and remove one I no longer need,
So that I can make room for new imports without risking the sound that is playing.

**Covers:** FR119, AR28 (lease-aware GC), AR19 (partial: BundleLease), NFR15 (partial), UX-DR65 (partial: Storage & Library section), UX-DR95

**Runs on:** Mac (LVGL simulator + host librarian and engine); dev board check

**Acceptance Criteria:**

**Given** the Settings surface (this story adds the Storage & Library section to the Settings surface from Story 4.35)
**When** the player opens Storage & Library
**Then** it shows free and used space on `/data` (CLDR formatted) and the installed libraries grouped by library with name, kind (Factory / Imported), size and number of timbres

**Given** the engine
**When** a generation references a bundle
**Then** the engine holds a `BundleLease` on it (a shared lock on the bundle that the librarian can test) until the generation is freed after its last voice ends

**Given** a factory library
**When** it is highlighted
**Then** it is listed but offers no Remove action in v1 (it cannot be reinstalled without factory re-provisioning)

**Given** an imported library
**When** the player removes it and confirms in an Action Sheet that names the library and states that Live Sets using it will show Missing library
**Then** the librarian flips nothing for other libraries, removes the catalog rows and `current`, and deletes each bundle only when no lease holds it; leased bundles are deleted by a later GC pass once released

**Given** a library in use by the playing Live Set
**When** it is removed
**Then** sound continues on the current generation until the next generation is built, after which the affected parts show Missing library, and no audio cut or xrun occurs

**Given** superseded bundles left by Import again or checkpoint publishing
**When** the GC pass runs at idle
**Then** every unleased bundle that is not a library's `current` is deleted, and GC never runs during an active import, and runs deletions only at idle so the disk stays read-only during performance (NFR15)

## Epic 6: Protected factory content

A keyboardist receives the unit with the encrypted, signed factory library already installed and wrapped for that unit, so it plays there and nowhere else, while their own imports stay unencrypted. The epic delivers the content-pack tool, the protected bundle format, engine decryption, the OP-TEE key path, the device registry, signing-key custody, production secure boot, anti-rollback, the factory provisioning station and the SoM repair flow. It builds on Epic 1's formatlib bundle, the secure-boot chain selected in Story 1.6 and the fused-board per-device key derivation of Story 1.7, and on Epic 5's librarian. Before the production image, the epic lays the read-only root filesystem, the RAUC A/B slots and the dev and factory-test image variants (Stories 6.11–6.13) that anti-rollback and factory provisioning need; Epic 8 builds the update flow on them. Format, packing and decrypt logic run on the Mac with dev keys; OP-TEE, fusing and the factory station need the fused dev board or prototype.

### Story 6.1: Signing and content-key hierarchy with dev roots

As a founder,
I want a documented key hierarchy with separate dev, factory-test and production roots and working dev keys,
So that every signing and wrapping step can be built and tested now without any production key ever touching a developer machine.

**Covers:** AR34 (partial: key hierarchy, dev root), NFR23 (partial: separate roots)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the Story 1.6 boot-signing setup
**When** the key hierarchy document is written in `tools/keys/KEY-HIERARCHY.md`
**Then** it defines, per root (dev, factory-test, production): the boot-image signing key, the RAUC bundle signing key, the content-manifest signing key (Ed25519) and the content-key vault key-encryption key, with who holds each, where it is stored, how it rotates and which device images trust which public keys
**And** it states that production private keys exist only in an offline HSM (the ceremony is a later story of this epic)

**Given** a dev key tool in `tools/keys/`
**When** a developer runs it
**Then** it generates the dev content-manifest key, a dev KEK and a dev device key pair into a directory outside the repository, labelled dev-only
**And** a CI check fails if any private key material (PEM, raw Ed25519 seed patterns, or files from the dev key directory) is committed

**Given** the device image build
**When** trusted content-signing public keys are embedded
**Then** dev images trust only dev keys and the build refuses to embed a dev public key into a production image variant

### Story 6.2: Protected bundle format in formatlib

As a keyboardist,
I want factory libraries encrypted per block and signed so they still stream from any point,
So that protected content plays as smoothly as my own libraries while resisting copying and tampering.

**Covers:** FR30 (format), FR31 (format), AR27 (crypto), NFR13, NFR44 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the Epic 1 block-indexed bundle
**When** the protected variant is added to formatlib
**Then** each coded block is encrypted with AES-CTR under the per-library content key with a counter block derived from a per-bundle nonce and the global block index (never reused within a key), and the per-block hash of the ciphertext is stored in the index
**And** the manifest carries the vendor `library_id`, `bundle_id`, `format_version`, the index hash and the hash list root, and an Ed25519 signature with the signing key ID

**Given** a protected bundle and the content key
**When** any frame is read
**Then** the reader verifies the block hash, decrypts and decodes only the blocks it needs, and the decoded PCM is bit-identical to the source
**And** random seeks to 10,000 frame positions in a test bundle decode correctly

**Given** a bundle with a modified block, index or manifest, or a signature from an untrusted key
**When** it is opened or the block is read
**Then** formatlib returns a typed error (`signature_invalid`, `block_hash_mismatch`), never plaintext from the bad block

**Given** the desktop build target of formatlib
**When** it is built
**Then** it contains no decrypt path (verified by a symbol check in CI), while signature verification stays available

### Story 6.3: Content-pack tool

As a founder,
I want an offline tool that turns source libraries into encrypted, signed bundles with vendor IDs,
So that factory and future partner content is protected before it ever reaches a unit.

**Covers:** AR31, FR30, FR31, FR34 (partial: packing), FR24 (user imports untouched)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** `tools/content-pack` and a source SFZ/WAV library with a vendor `library_id` and per-instrument categories in a pack description file
**When** the tool runs with a signing key
**Then** it imports the library through the same formatlib importer as user imports, generates a fresh random 256-bit content key, writes a protected bundle (Story 6.2) and a manifest signed with the given Ed25519 key
**And** it writes the content key only into the key vault format (encrypted under the KEK of the chosen root), never in plaintext to disk

**Given** the same source and the same content key
**When** the tool runs twice
**Then** the bundle bytes are identical (reproducible build), so a release can be verified by rebuilding

**Given** a pack description that reuses a `library_id` already assigned to another library in the vendor ID register (`tools/content-pack/vendor-ids`)
**When** the tool runs
**Then** it refuses with an error

**Given** a packed bundle
**When** it is decrypted with the vaulted key on the Mac and rendered by the offline renderer
**Then** the render matches the render of the same library imported unencrypted, bit-for-bit

### Story 6.4: Librarian verifies and installs protected bundles

As a keyboardist,
I want the unit to accept only genuine, untampered factory bundles,
So that a damaged or modified file never installs and never plays.

**Covers:** FR31 (install), FR32 (librarian holds no keys), AR28 (partial: install path), NFR25

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a protected bundle and its wrapped-key blob delivered to the librarian install API
**When** it is installed
**Then** the librarian verifies the manifest signature against the trusted content-signing keys and every block hash, stages, fsyncs, renames to `/data/lib/<vendor library_id>/<bundle_id>/`, flips `current`, stores the wrapped-key blob next to the bundle, and writes catalog rows marked Factory
**And** the librarian never decrypts any block and never handles an unwrapped content key (its build links no decrypt path)

**Given** a bundle with one flipped bit anywhere, or signed by an untrusted key
**When** install is attempted
**Then** it is rejected with a typed reason, nothing is published, and the reconcile pass leaves no staging files

**Given** an installed protected library
**When** the user import path runs
**Then** user-imported bundles are still written unencrypted (FR24), and protected and user libraries coexist in the catalog with their kind shown

### Story 6.5: Engine decrypts protected bundles on I/O edge threads

As a keyboardist,
I want protected instruments to stream and play exactly like my own,
So that protection costs me no polyphony, latency or reliability.

**Covers:** FR30, FR31 (load), FR32, AR20 (decrypt and block-hash on edge threads), AR19 (content keys in the generation), NFR17, NFR4 (partial)

**Runs on:** Mac (dev key-provider port)

**Acceptance Criteria:**

**Given** a key-provider port in `engine/ports` with a dev implementation in `engine/host` that unwraps dev-wrapped content keys with the dev device key
**When** a generation that references a protected library is prepared
**Then** the control thread obtains the content key through the port, keeps it only in mlocked memory marked non-dumpable, and attaches it to the generation
**And** when the generation is freed after its last voice ends, the key memory is zeroized

**Given** a protected instrument
**When** heads load and streaming runs
**Then** block-hash checks and AES-CTR decryption happen on non-RT threads (head loader and I/O edge threads), the RT threads only read decoded PCM, and the RTSan render test passes

**Given** a block whose hash does not match during streaming
**When** it is read
**Then** the voice outputs silence with a short fade for that block, a `content_integrity` counter increments, and the library is reported damaged; at instrument load the same failure refuses the load with a typed error

**Given** the offline renderer
**When** a protected and an unprotected bundle of the same library render the same MIDI suite
**Then** the outputs are bit-identical
**And** no plaintext block or key is written to disk on any path (verified by a filesystem write monitor during the test)

### Story 6.6: OP-TEE content-key trusted application on the board

As a keyboardist,
I want content keys unwrapped only inside the secure world with my unit's own key,
So that the factory library is bound to my instrument.

**Covers:** AR32, FR30, FR32, NFR23 (partial: device key in OP-TEE, keys never on disk), AR8 (builds on the exit criterion)

**Runs on:** Dev board (fused, from Story 1.7)

**Acceptance Criteria:**

**Given** the fused dev board and the Story 1.7 HUK-derived device key
**When** the content-key TA is built and installed in the signed image
**Then** it derives a device key pair (X25519 or the scheme recorded in the story) from the HUK, exports only the public key and a device ID, and never exposes the private key to the normal world

**Given** a content key wrapped to this device's public key (sealed box with HKDF and an AEAD, bound to the `library_id`)
**When** the engine's OP-TEE key-provider implementation asks the TA to unwrap it
**Then** the TA returns the content key only to the `engine` process (checked by the TA client identity), and the engine handles it as in Story 6.5

**Given** a wrapped key for a different `library_id`, a different device, or a modified blob
**When** unwrap is requested
**Then** the TA refuses with a typed error and returns no key material

**Given** the engine playing a protected library on the board
**When** 256 stereo voices stream from it for 1 h
**Then** the engine reports zero xruns and zero underruns, and decrypt CPU time on the I/O edge threads is recorded

### Story 6.7: A copied or tampered library does not play elsewhere

As a founder,
I want proof that the factory library plays only on the unit it was wrapped for and that tampering is caught,
So that the content-protection promise is verified, not assumed.

**Covers:** FR30, FR31, FR32, NFR25

**Runs on:** Dev board (two fused boards)

**Acceptance Criteria:**

**Given** two fused dev boards A and B, and a protected library installed on A with a key wrapped for A
**When** the library directory and wrapped-key blob are copied from A's NVMe to B
**Then** B refuses to load it (unwrap fails), the parts using it play silent with Missing library, and nothing else on B is affected

**Given** board A
**When** one byte of a block, the index or the manifest is modified on disk
**Then** the install check (if reinstalled) or the load check rejects it, and no audio from the modified block is played

**Given** board A after a 1 h playing session with the protected library
**When** the NVMe and any USB drive attached during the session are scanned for plaintext (decoded PCM fingerprints of the source library and the content key bytes)
**Then** no match is found

**Given** the results
**When** the report is written
**Then** it states what the protection deters and what it does not claim (NFR25)

### Story 6.8: Device registry and content-key vault

As a founder,
I want a vendor registry that records every unit's device key and wraps content keys for it,
So that each unit can receive factory and future partner content, and repaired units can be re-provisioned.

**Covers:** AR34 (device registry), AR33 (partial: registration from unit #1, serial ↔ SoM link)

**Runs on:** Vendor server (developed and tested on the Mac)

**Acceptance Criteria:**

**Given** a registry service in `tools/registry/`
**When** it is deployed
**Then** it stores per unit: device ID, device public key, SoM ID, serial number, tier, provisioning date, station ID and status (active / revoked), in a database with an append-only audit log of every write

**Given** an authenticated station (mutual TLS with a station certificate issued under the factory-test root)
**When** it registers a device or requests wrapped keys
**Then** the registry rejects duplicates of an active device ID or serial, and returns content keys for the requested libraries wrapped to that device's public key, unwrapping vaulted keys with the KEK only inside the wrap operation and never logging or storing them in plaintext

**Given** an unauthenticated client or a revoked device
**When** it calls any registry API
**Then** the call is refused and logged

**Given** the registry database
**When** the nightly backup runs
**Then** an encrypted backup is written to a second location and a restore test recovers it, so losing the server never orphans provisioned units

### Story 6.9: Factory library packaging

As a founder,
I want the factory library I produce packed, signed and versioned as one reproducible release,
So that every unit ships with the same genuine factory piano, organ presets and other categories.

**Covers:** FR34, AR31 (in use), EXT14 (dependency: founder-supplied content)

**Runs on:** Mac

**Blocked by:** Story 11.17 (target month 12)

**Acceptance Criteria:**

**Given** founder-supplied source content (factory piano and the other factory instruments as SFZ/WAV) and a release description listing vendor `library_id`s, categories and names
**When** the factory release pipeline runs
**Then** it packs every library with content-pack under the chosen root's keys, and outputs a versioned factory release (bundles, signed manifests, a release index with hashes, and vaulted content keys for the registry)

**Given** the organ presets and other factory Live Sets used in the v1 success signal
**When** they are added to the release
**Then** they carry vendor-assigned program IDs, reference instruments only by `(library_id, instrument_key)` or engine parameters, and are delivered as a signed factory Live Set file that the engine control thread loads on first boot (single writer)

**Given** the release
**When** it is rebuilt from the same sources and keys
**Then** every bundle and the release index are byte-identical, and a verification command checks a release against its index

**Given** a dev-root build of the release
**When** it is installed on the Mac host librarian and engine
**Then** every factory instrument plays, every factory Live Set loads without Missing library, and each instrument shows its category

### Story 6.10: Production signing root ceremony in an offline HSM

As a founder,
I want the production signing keys generated and held only in an offline HSM, with a written ceremony,
So that a compromised laptop or a leaked dev key can never sign firmware or content for customers' units.

**Covers:** AR34 (signing-key custody), NFR23 (production root offline in an HSM)

**Runs on:** Offline signing laptop + HSM

**Acceptance Criteria:**

**Given** two HSM tokens of the same model and an air-gapped laptop with a pinned, verified toolchain
**When** the key ceremony runs following `tools/keys/CEREMONY.md`
**Then** the production boot-signing, RAUC-signing and content-manifest keys and the production KEK are generated inside the HSM (non-exportable, or exported only as an encrypted backup to the second token), and only the public keys and their hashes leave the ceremony
**And** the ceremony log records date, participants, device serials and key fingerprints, and is signed

**Given** a release to sign
**When** the signing procedure runs on the offline laptop
**Then** artifacts move in and out only on removable media, signing requires the HSM PIN, and a signed artifact verifies against the published production public key

**Given** the backup token
**When** a recovery drill runs
**Then** a test artifact is signed from the backup token and verifies, and the drill is logged

### Story 6.11: Read-only root filesystem and journaled data partition

As a keyboardist,
I want the system software to sit on a read-only filesystem and my data on a separate journaled partition,
So that a power cut or a crash can never corrupt the operating system and my Live Sets survive intact.

**Covers:** AR36 (partial: read-only rootfs, journaled data partition), NFR15 (partial: write discipline), NFR26 (partial: sealed NVMe layout)

**Runs on:** Linux build host or ARM64 Linux VM on the Mac (image build); dev board (verification)

**Acceptance Criteria:**

**Given** the Yocto dev image from Story 1.4
**When** the partition layout is changed
**Then** the NVMe holds a boot area, two root slots sized for A/B (only slot A populated at this point), and one `/data` partition on a journaling filesystem (ext4 with `data=ordered` or the chosen equivalent, recorded with its reason in `yocto/README`)
**And** the layout is defined in a committed `wks` file so every image builds the same layout

**Given** the booted dev image
**When** the root filesystem is inspected
**Then** `/` is mounted read-only, writable runtime paths (`/var/volatile`, `/tmp`, `/run`) are tmpfs, and journald is configured for volatile (RAM) storage with a bounded size
**And** an attempt to write under `/usr` or `/etc` fails with EROFS

**Given** services that persist data (`engine` control thread, `librarian`, `sysd`)
**When** they write
**Then** they write only under `/data` in their own subdirectory (`/data/lib`, `/data/catalog`, `/data/livesets`, `/data/config`), owned by their service user, matching the AD-9 single-writer table
**And** `/data` is mounted `nodev,nosuid`

**Given** the dev board running the engine
**When** `fsck` is forced after 20 hard power removals at idle (no write in progress)
**Then** `/data` mounts cleanly every time and the root slot verifies unchanged against its build hash

### Story 6.12: RAUC A/B slots with rollback on failed boot

As a keyboardist,
I want firmware to install into the inactive slot and fall back to the previous one if the new one fails to boot,
So that an update can never leave my instrument unable to start.

**Covers:** FR66, AR36 (partial: RAUC A/B with rollback), AR2 (partial: RAUC signing per root, dev root here)

**Runs on:** Linux build host or ARM64 Linux VM (bundle build); dev board (install and rollback)

**Acceptance Criteria:**

**Given** meta-rauc (`wrynose`, RAUC ≥1.15.2) in the distro and the layout from Story 6.11
**When** the image builds
**Then** a RAUC system config declares slots `rootfs.0` / `rootfs.1` with the bootloader backend of the chain selected in Story 1.6, and CI produces a RAUC bundle signed with the dev signing key alongside the image
**And** the dev image carries only the dev keyring

**Given** a running slot A
**When** `rauc install` of a valid dev-signed bundle runs from the dev shell
**Then** the bundle is written to slot B, the boot order switches to B with a boot-attempt counter (3 attempts), and after reboot the system runs from slot B
**And** a systemd unit marks the slot good only after `engine`, `ui` and `sysd` reached their ready state, and not before

**Given** a bundle whose kernel deliberately panics or whose `engine` fails to reach ready
**When** it is installed and booted
**Then** after the attempt counter is exhausted the bootloader boots the previous slot, which RAUC reports as booted with the failed slot marked bad
**And** `sysd` records the rollback (from-version, to-version) in a small state file under `/data/config` so the UI can report it later

**Given** a bundle signed with a key outside the dev keyring, or a bundle with one byte flipped
**When** it is installed
**Then** RAUC rejects it before writing any slot and the running slot is unchanged

### Story 6.13: Dev and factory-test image variants with per-root keyrings

As a founder,
I want the dev and factory-test images built from one source, each with its own signing root,
So that development stays convenient and factory tests run on real units before any unit is fused for production.

**Covers:** AR3 (partial: dev and factory-test variants), NFR23 (partial: separate dev / factory-test roots), AR2 (partial: RAUC signing per root), AR34 (partial: dev and factory-test roots used by CI)

**Runs on:** Linux build host or ARM64 Linux VM + CI; dev board and prototype (unfused)

**Acceptance Criteria:**

**Given** the Yocto build, the layout from Story 6.11, the RAUC slots from Story 6.12 and the key hierarchy from Story 6.1
**When** CI builds a release
**Then** it produces two images and two RAUC bundles from the same layer set: `dev` (dev-signed, SSH and debug UART on) and `factory-test` (factory-test-signed, SSH off, factory test suite installed), with the layer set structured so the production image is a third variant of it
**And** each image ships only its own root's RAUC keyring, so a dev bundle is rejected by `rauc install` on a factory-test unit and vice versa

**Given** the factory-test variant
**When** it boots on a prototype
**Then** it runs a test runner with one test per subsystem available at this point (MCU links, keybed scan, panel controls and LEDs, codec loopback, display and touch, NVMe read/write speed), each reporting pass/fail with a summary on screen and a machine-readable report on the factory station link
**And** missing hardware makes the related test report `not present` rather than crash the runner

**Given** either variant
**When** `/etc/os-release` is read
**Then** it states the variant, so the UI and Velora Studio can display it

### Story 6.14: Production image with secure boot and debug lockdown

As a keyboardist,
I want my unit to boot only genuine Velora firmware with every debug door closed,
So that nobody can boot a custom OS to read the keys that protect the factory library.

**Covers:** FR33, NFR23, NFR24 (partial: no serial console, no SSH, JTAG/SWD disabled on the SoC), AR3 (partial: production image variant, sole owner of the production image build), AR8 (production use)

**Runs on:** Prototype (sacrificial units for fusing tests)

**Acceptance Criteria:**

**Given** the Story 1.6 chain and the production public keys from Story 6.10
**When** the production image variant is built as the third variant of the Story 6.13 layer set
**Then** every boot stage is signed with the production boot key, the image has no SSH, no getty on any UART, no debug UART output after the loader, kernel `console=` is empty, it is marked production in `/etc/os-release`, and it ships only the production RAUC keyring

**Given** a prototype fused with the production root key hash and with SoC JTAG disabled by its fuse
**When** it is powered
**Then** it boots the production image, and an unsigned image, a dev-signed image or an image with one flipped byte does not boot
**And** a JTAG probe cannot attach, and the bootloader offers no interactive console or alternative boot source

**Given** the production unit
**When** a USB-A drive with a bootable OS, or a USB-C host trying to enter maskrom download, is connected
**Then** the unit does not boot from it and maskrom loading refuses unsigned loaders

**Given** the production variant on a unit
**When** it is probed from outside
**Then** USB-C exposes only UAC2 + MIDI + NCM (or nothing if the gadget is not yet enabled), the NCM interface accepts no SSH, and no UART emits a login prompt or kernel log

### Story 6.15: OTP anti-rollback counter for security releases

As a founder,
I want an OTP version counter that blocks booting firmware older than the last security release,
So that a fixed vulnerability cannot be reopened by flashing an old image.

**Covers:** AR35, NFR23 (anti-rollback)

**Runs on:** Fused dev board (sacrificial) or prototype

**Acceptance Criteria:**

**Given** a security version field in the signed image metadata and the bootloader's check against the OTP counter
**When** an image with security version below the OTP counter is presented
**Then** it does not boot, and the A/B slot logic falls back to the other slot if it is valid

**Given** a new security release
**When** it has booted and its boot-confirmed hook runs (called from the Story 6.12 mark-good unit)
**Then** the OTP counter is bumped to the image's security version, and never before a successful boot of that image
**And** non-security releases never bump the counter

**Given** the counter procedure
**When** it is documented in `tools/keys/ANTI-ROLLBACK.md`
**Then** it states the OTP capacity (number of possible bumps), the release policy for using one, and the test results on the sacrificial board

### Story 6.16: Factory provisioning station

As a founder,
I want a factory station that fuses each unit, provisions its device key, registers it and installs the factory library wrapped for it,
So that every unit from #1 leaves the factory secure, registered and playable.

**Covers:** AR33, FR34 (pre-installed wrapped), FR30, NFR23 (fused at the factory, device key generated in OP-TEE)

**Runs on:** Factory station (Linux PC with the partner's programming fixture) + prototype units

**Blocked by:** Story 11.7 (target month 7)

**Acceptance Criteria:**

**Given** a unit that passed the factory-test image checks (Story 6.13) unfused, and the station connected to the registry
**When** the provisioning run starts
**Then** the station reads the serial from the label/EEPROM and the SoM ID, fuses the production root key hash and debug lockdown, flashes the production image (Story 6.14), and starts the single-purpose provisioning agent (production-signed, no shell, refuses to run once the unit is marked provisioned)
**And** each step is idempotent or refuses safely if repeated, and the station log records every step per serial

**Given** the provisioning agent
**When** it asks the content-key TA for the device public key and ID
**Then** the station registers device ID, public key, SoM ID, serial and tier in the registry, receives the factory content keys wrapped for that device, and installs the factory release bundles and the wrapped-key blobs through the librarian install path

**Given** the provisioned unit
**When** the station's final check runs
**Then** the unit boots production, a factory instrument renders a test note matching the expected checksum on the unit, the provisioning agent is disabled for good, and the registry status is active

**Given** a failure at any step (fuse error, registry unreachable, install error)
**When** it happens
**Then** the station stops, shows the step and reason, never marks the unit active, and the unit can be resumed or quarantined

### Story 6.17: SoM swap re-provisioning

As a founder,
I want a repair flow that re-provisions a unit after its SoM is replaced,
So that a repaired keyboard plays its factory library again and the old SoM can never be used to decrypt it.

**Covers:** AR33 (SoM swap re-provisioning)

**Runs on:** Factory station (repair mode) + prototype

**Acceptance Criteria:**

**Given** a unit whose SoM was replaced, identified by its serial
**When** the station runs in repair mode
**Then** it fuses and provisions the new SoM as in Story 6.16, the registry links the serial to the new SoM ID and device key, and marks the old device ID revoked with the reason and date

**Given** the new device key
**When** the registry wraps the factory content keys for it
**Then** the station installs the new wrapped-key blobs, the existing encrypted bundles on the NVMe are reused without copying, and the factory library plays

**Given** the user's data on the NVMe (setlists, Live Sets, imported libraries)
**When** the repair finishes
**Then** it is preserved and opens unchanged

**Given** the revoked old SoM
**When** it is placed in another unit with the original wrapped-key blobs
**Then** the registry refuses any further wrap for it, and the blobs wrapped for the new SoM do not unwrap on it

## Epic 7: Arpeggiator and clock

A keyboardist runs an arpeggiator on any part, with its on/off state and settings stored per scene, locked to the Live Set tempo or to external MIDI clock from DIN or USB, and keeps playing at the last tempo when the external clock drops out. The epic builds on Epic 1's engine core (UMP events, in-block frame offsets, offline renderer, null-test harness), Epic 3's MIDI DIN input and Epic 4's Live Sets, scenes, parameter registry, Part surface and Top Bar. Engine logic runs on the Mac and in the offline renderer; DIN clock and the ≤1 ms jitter acceptance need the dev board or prototype.

### Story 7.1: Single tempo and transport clock on the RT plane

As a keyboardist,
I want one tempo clock in the engine set by the Live Set tempo,
So that everything tempo-based in the instrument shares the same grid.

**Covers:** FR75 (internal clock), AR17 (one tempo/transport clock), NFR7 (partial: internal grid)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** `engine/core`
**When** the clock is added
**Then** the RT plane owns exactly one tempo/transport clock that advances per block and exposes, for any frame in the block, the musical position in ticks (960 PPQN or the value recorded in the story) without drift: positions are computed from an integer frame count and the tempo, not accumulated floating-point increments

**Given** a Live Set with a tempo (20–300 BPM, 0.1 BPM resolution) stored as a structural parameter in the parameter registry
**When** the Live Set loads or the tempo changes
**Then** the clock follows the new tempo from the next block, continuous in musical position, and tempo is performance state on the RT plane

**Given** the offline renderer
**When** a 10-minute run at 120 BPM and at 97.3 BPM computes the frame of every sixteenth note
**Then** every computed grid frame equals the exact rational position rounded to the nearest frame (no cumulative drift)

### Story 7.2: Event-processor framework and arpeggiator note generation

As a keyboardist,
I want an arpeggiator on any part with mode, rate, octaves and gate,
So that held chords turn into rhythmic patterns locked to the tempo.

**Covers:** FR74 (Arp On/Off, Mode, Rate, Octaves, Gate), FR75, AR17 (event-processor framework), AR14 (partial: serial phase), NFR7 (internal), NFR17

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the serial phase of the block loop
**When** the event-processor framework is added
**Then** each part has an optional processor slot that consumes the part's UMP input and emits UMP with exact frame positions before voice allocation, runs without allocation or locks, and is designed so a later sequencer processor uses the same slot type

**Given** an arpeggiator on a part with Arp On and notes held
**When** blocks render
**Then** notes are generated in Mode Up, Down, Up-Down (end notes not repeated), Random (seeded per part for reproducible tests) or As played, across Octaves 1–4, at Rate 1/4, 1/8, 1/16, 1/32 or their triplets, with note length = Gate % of the step (minimum one frame)
**And** each generated Note On lands on the exact frame of its grid position from the Story 7.1 clock

**Given** Arp Off
**When** notes are played
**Then** they pass through unchanged, and switching Arp Off while notes are held ends arpeggiated notes and sounds the held notes normally

**Given** the offline renderer
**When** committed MIDI test files render each mode, rate and octave setting at two tempos, with the arpeggiator active on 2 parts
**Then** the emitted note frames match expected frame lists exactly, and a golden render null-tests in CI

### Story 7.3: Swing, latch and arpeggiator performance behaviour

As a keyboardist,
I want swing, latch and predictable behaviour with the sustain pedal and when switching sounds,
So that the arpeggiator feels musical and never glitches on stage.

**Covers:** FR74 (Swing, Latch), FR79 (partial: arpeggiated notes and seamless switching), NFR7 (internal), NFR14 (partial)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** Swing % (50% = straight, up to 75%)
**When** the arpeggiator runs
**Then** every second step of the rate is delayed by the swing amount of the step pair, exactly on the computed frame

**Given** Latch On
**When** the player releases all keys
**Then** the pattern keeps running with the last held notes; a new chord played after all keys were released replaces the latched notes; Latch Off ends latched notes at the next step

**Given** the sustain pedal down with Latch Off
**When** keys are released
**Then** the arpeggiator keeps the sustained notes in its pattern until the pedal is released (behaviour documented and tested)

**Given** an arpeggiator running on a part
**When** a scene or Live Set switch happens
**Then** notes already sounding finish on their original generation with their gate length, the pattern continues on the musical grid without a gap or double trigger, and the seamless-switching render test shows no truncation

### Story 7.4: Arpeggiator settings in Live Sets and scenes

As a keyboardist,
I want each scene to remember whether the arpeggiator is on and how it is set,
So that one button press can start, stop or reshape the arpeggio in the middle of a song.

**Covers:** FR78, FR88 (partial: arpeggiator state in scenes), AR12 (arpeggiator params in the registry), AR29 (partial: uses the arpeggiator fields defined in Story 4.2, no format bump)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the parameter registry in `proto/`
**When** the arpeggiator parameters are added (on/off, mode, rate, octaves, gate, swing, latch per part)
**Then** they are classified as structural, scene-scoped, and the Live Set tempo is Live-Set-scoped (never stored in a scene)

**Given** a new Live Set
**When** it is created
**Then** each part holds initial arpeggiator values (Off, Up, 1/8, 1 octave, Gate 50%, Swing 50%, Latch Off) and every scene copies them

**Given** scene 1 with Arp Off on part 2 and scene 2 with Arp On, Up-Down, 1/16T on part 2
**When** the player recalls scene 2 then scene 1
**Then** the arpeggiator starts and stops accordingly as a parameter-only snapshot swap, and the tempo does not change

**Given** a Live Set saved before this story, whose arpeggiator fields (defined in Story 4.2) are unset
**When** it is opened
**Then** it loads with the initial values above, no format version bump or migration is needed, unknown fields are preserved, and Write produces a file that N and N−1 readers open

### Story 7.5: Part surface Arpeggiator section and Live Set tempo

As a keyboardist,
I want to set the arpeggiator and the tempo from the Part surface,
So that I can build arpeggiated parts on the keyboard without a computer.

**Covers:** FR74 (UI), FR112 (Arpeggiator section), UX-DR60 (Arpeggiator section), UX-DR34, UX-DR93, UX-DR95, UX-DR98

**Runs on:** Mac (LVGL simulator + host engine)

**Acceptance Criteria:**

**Given** the Part surface (per the Part surface design including its Arpeggiator section)
**When** the player opens the Arpeggiator section
**Then** it lists Arp (On/Off), Mode, Rate (1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T, 1/32T), Octaves 1–4, Gate %, Swing %, Latch (On/Off) and Tempo, using Segmented Controls for on/off and multi-choice fields, VALUE editing the highlighted field and touch selecting
**And** every label is a locale-catalog key in EN, PT-BR and ES and values use CLDR formatting (`65 %` in ES)

**Given** the internal clock (the default)
**When** the player edits Tempo
**Then** the Live Set tempo changes for all parts, plays immediately, and marks ◆ like any edit

**Given** arpeggiator edits
**When** the player holds WRITE
**Then** the Write Dialog lists them in "Changed: …", and Scene scope is available because arpeggiator parameters are scene-scoped

### Story 7.6: External MIDI clock follower and clock-lost behaviour

As a keyboardist playing with a drummer's sequencer,
I want the arpeggiator to lock to incoming MIDI clock and keep going at the last tempo if the clock drops out,
So that the band stays in time and a cable fault never stops my part.

**Covers:** FR76 (following), FR77, NFR7 (partial: follower accuracy), AR17

**Runs on:** Mac (engine core and offline renderer with synthetic clock streams)

**Acceptance Criteria:**

**Given** timestamped MIDI clock (0xF8), Start, Continue and Stop UMP events in device time
**When** the clock source is external
**Then** a follower on the RT plane estimates tempo and phase from the clock timestamps with a filter (DLL or equivalent) that tracks tempo changes within 2 beats, and drives the single tempo/transport clock in place of the Live Set tempo

**Given** a synthetic clock stream at 72, 120 and 174 BPM with ±0.5 ms timestamp jitter, plus a ramp from 90 to 130 BPM
**When** the arpeggiator runs following it in the offline renderer
**Then** every arpeggiated Note On lies within ±1 ms of the ideal position of the clock tick it belongs to (after the fixed scheduling delay), and the results are written to a test report

**Given** clock ticks stop arriving for longer than 3 ticks at the last tempo (minimum 150 ms) without a Stop message
**When** the follower detects it
**Then** the clock keeps running at the last received tempo, the engine publishes a `clock_lost` state with that tempo, and the state clears when clock resumes, re-locking phase without a double trigger

**Given** MIDI Start or Continue
**When** it arrives
**Then** the arpeggiator step grid aligns to the next clock tick; after a Stop the arpeggiator keeps running at the last tempo without raising `clock_lost`, so no CLOCK LOST is shown (decided: transport Stop keeps the last tempo)

### Story 7.7: Clock source setting and DIN/USB clock inputs

As a keyboardist,
I want to choose Internal, DIN or USB as the clock source in Settings,
So that the Velora follows whichever device leads the band.

**Covers:** FR76 (clock source system setting, DIN and USB), AR39 (partial: clock source in sysd config), AR17 (clock source owned by sysd), UX-DR65 (partial: MIDI & Clock › Clock source)

**Runs on:** Mac (sysd config and CoreMIDI USB clock); dev board or prototype (DIN clock)

**Acceptance Criteria:**

**Given** the sysd config service
**When** the clock source setting is added
**Then** it stores Clock source (Internal / DIN / USB, default Internal) with the config `schema_version`, and the engine receives it at start and on change through the published config

**Given** the Settings surface
**When** the player opens MIDI & Clock (this story adds the Clock source row, creating the section if it does not exist yet)
**Then** Clock source is a Segmented Control (Internal / DIN / USB) whose change takes effect without interrupting sound

**Given** Clock source DIN on the board (or USB, on the Mac via CoreMIDI and on the board via the USB MIDI input)
**When** clock messages arrive on that port
**Then** the input edge thread stamps them on arrival in device time and forwards only that port's clock to the follower; clock on the other port is ignored

**Given** a hardware MIDI clock source (drum machine or DAW) at 120 BPM on DIN and on USB
**When** the arpeggiator runs following it on the board
**Then** it stays in time by ear over 10 minutes (the formal NFR7 jitter measurement is a separate acceptance story)

### Story 7.8: Top Bar tempo, clock source and CLOCK LOST display

As a keyboardist,
I want the Top Bar to show the tempo, where it comes from and a clear warning when external clock is lost,
So that I know at a glance whether I am locked to the band.

**Covers:** FR76 (`♩ 72 · DIN` display in the Top Bar and read-only Part surface Tempo when following), UX-DR60 (partial: read-only Tempo field), FR77 (`⚠ CLOCK LOST`), UX-DR12 (tempo + clock source, CLOCK LOST), UX-DR18 (warn takeover), UX-DR72 (clock following, clock lost), UX-DR88

**Runs on:** Mac (LVGL simulator + host engine with synthetic clock)

**Acceptance Criteria:**

**Given** Clock source Internal
**When** Performance is shown
**Then** the Top Bar shows `♩ 72` with the Live Set tempo

**Given** Clock source DIN or USB with clock arriving
**When** Performance is shown
**Then** the Top Bar shows the followed tempo rounded to whole BPM with its source (`♩ 72 · DIN`, `♩ 72 · USB`), updated no faster than twice per second to avoid flicker

**Given** a clock source other than Internal set in Story 7.7
**When** the Part surface Tempo field from Story 7.5 is shown
**Then** it is read-only and displays the followed tempo with its source (`♩ 72 · DIN`)

**Given** the engine publishes `clock_lost`
**When** the state is active
**Then** the Top Bar shows `⚠ CLOCK LOST` in warn and the Status Bar warn takeover reads "⚠ CLOCK LOST · ARP AT ♩ 72", both with glyph and text, never colour alone, and nothing blinks faster than 3 Hz
**And** both clear when clock returns

**Given** the three locales
**When** the strings render
**Then** CLOCK LOST and the takeover line are catalog keys translated in PT-BR and ES and pass the pseudo-localization clipping check in the Top Bar

### Story 7.9: Arpeggiator timing acceptance on the board

As a founder,
I want automated proof that the arpeggiator is sample-accurate on the internal clock and within 1 ms of external clock on the real hardware,
So that the CAP-19 success signal is measured, not assumed.

**Covers:** NFR7, FR75, FR76, AR2 (partial: timing tests in CI)

**Runs on:** Mac + CI (offline tests); dev board or prototype (external-clock rig)

**Acceptance Criteria:**

**Given** CI
**When** the arpeggiator timing suite runs in the offline renderer
**Then** with the arpeggiator active on 2 parts at several tempos, rates (including triplets) and swing values, every arpeggiated onset lands on its exact computed grid frame, and any deviation fails the build

**Given** a rig that sends MIDI clock to the board over DIN and over USB while recording, on a second interface, the clock signal (a GPIO pulse or the DIN line itself) and the board's analog output
**When** an arpeggiator at 1/16 runs for 10 minutes at 72, 120 and 174 BPM on each source
**Then** the onset-to-clock-tick offset, after removing the fixed latency, has jitter ≤1 ms (max deviation from the mean), and the report gives min, median, p99 and max per run

**Given** the rig
**When** the clock cable is pulled for 5 s and reconnected
**Then** the arpeggiator continues at the last tempo with `⚠ CLOCK LOST` shown, re-locks after reconnection without a double trigger, and the engine reports zero xruns throughout

## Epic 8: Reliability: boot, updates and power loss

A keyboardist powers on and plays the Boot-into target in ≤10 s, updates firmware safely from Velora Studio or a USB-A stick (A/B with rollback, signed bundles, `sysd` as sole installer) and never loses data or bricks the unit on power loss. It builds on the read-only root filesystem, RAUC A/B slots, image variants and production image delivered in Epic 6 (Stories 6.11–6.14), and also delivers the production release signing pipeline, the MCU firmware manifest and boot-time version check (building on the Epic 3 reflash mechanism), N/N−1 format compatibility with post-mark-good migrations, and the kernel LTS migration plan. Development is Mac-first: device UI work (Boot Screen, install flow) runs on the Mac in the LVGL SDL simulator against a mocked `sysd`; image, partition and RAUC work builds in an ARM64 Linux VM or Linux build host; boot timing, MCU reflash and power-cut tests are only accepted on the dev board or prototype.

### Story 8.1: sysd as the sole installer, from a USB-A stick

As a keyboardist,
I want to update the firmware from a USB stick without any computer, with the unit checking the file before it touches anything,
So that I can update anywhere and a damaged or foreign file can never be installed.

**Covers:** FR46, AR36 (partial: sysd as sole installer), NFR24 (partial: USB-A mounted read-only `nosuid,nodev,noexec`), AD-26

**Runs on:** Dev board (Mac for sysd unit tests with a mocked RAUC D-Bus)

**Acceptance Criteria:**

**Given** `sysd` with an installer module
**When** any process other than `sysd` tries to start an install (RAUC D-Bus `Install` call)
**Then** the D-Bus policy denies it; `sysd` is the only service allowed to call RAUC
**And** `rauc` CLI access is removed from non-root users in the image

**Given** a USB-A stick with a `.raucb` file in its root
**When** it is inserted
**Then** the stick is mounted read-only `nosuid,nodev,noexec` (the same mount policy as imports), `sysd` lists firmware bundles and verifies each bundle's signature and compatible string with RAUC's info call before announcing it
**And** `sysd` publishes a ws.v1 `UpdateAvailable {version, source: USB}` state, which the existing USB drive Notice Toast from Epic 5 shows as "Update n found · Open?"

**Given** a ws.v1 `InstallUpdate` command from the UI with a `request_id`
**When** `sysd` installs
**Then** it copies the bundle to a staging file under `/data/update` first (so removing the stick mid-install cannot corrupt the slot), then calls RAUC and publishes progress as ws.v1 `UpdateProgress {phase, percent}` deltas
**And** errors use the closed ws.v1 error enum (`UPDATE_BAD_SIGNATURE`, `UPDATE_INCOMPATIBLE`, `UPDATE_NO_SPACE`, `UPDATE_IO`)

**Given** a bundle that contains library content (a `/data/lib` payload or a library bundle manifest)
**When** it is offered
**Then** `sysd` refuses it with `UPDATE_INCOMPATIBLE`; library updates travel only as librarian bundles (AD-26)

**Given** the installer API
**When** a firmware file arrives later by another path (Velora Studio, Epic 9)
**Then** that path only stages the file and calls the same `InstallUpdate` command, so verification and install code exist once

### Story 8.2: Production release signing and per-root update bundles

As a founder,
I want every production firmware release signed on the offline HSM host and installable only on units of its own root,
So that customers' units accept only genuine production updates and no production key is ever reachable from CI.

**Covers:** AR3 (partial: production RAUC bundle in the release pipeline; the production image itself is built by Story 6.14), NFR23 (partial: production root offline), AR2 (partial: RAUC signing per root), AR34 (partial: signing roots used by CI)

**Runs on:** Linux build host or ARM64 Linux VM + CI; offline HSM host; fused production unit

**Acceptance Criteria:**

**Given** the production image variant from Story 6.14
**When** CI builds a release
**Then** it produces the production RAUC bundle from the same layer set as the dev and factory-test bundles of Story 6.13, as an unsigned artifact plus a signing request; signing happens on the offline HSM host per the Story 6.10 procedure (AR34), the signed result is attached back to the release, and no production private key is reachable from CI

**Given** a fused production unit running the Story 8.1 installer
**When** a dev-signed or factory-test-signed bundle is offered on a USB-A stick
**Then** `sysd` rejects it with `UPDATE_BAD_SIGNATURE` before any slot is written, and a production-signed bundle of the same release installs

**Given** a published release
**When** its record is inspected
**Then** it lists, per variant, the image and bundle hashes and the signing key fingerprints, so a field unit's installed version can be traced to its signing event

### Story 8.3: Install flow on the device

As a keyboardist,
I want the unit to tell me plainly that sound stops during an update, show progress, and tell me afterwards whether it worked,
So that I never start an update by accident before a show and always know which version I am on.

**Covers:** FR47, UX-DR76, UX-DR66, UX-DR37 (install update variant), UX-DR28 (firmware use), UX-DR31 (Updated / rolled back toasts), NFR16 (only the explicitly started install cuts audio)

**Runs on:** Mac (LVGL SDL simulator with a mocked `sysd`); dev board (end-to-end)

**Acceptance Criteria:**

**Given** the Settings surface reachable from the Launcher
**When** this story adds the System & Update section
**Then** it shows the firmware version and variant, and when `sysd` announces a verified bundle it shows "1.3 ready to install" with an Install row; with no bundle it shows "No update found. Insert a USB drive or send one from Velora Studio."
**And** the "Update n found · Open?" Notice Toast opens this section

**Given** the user chooses Install
**When** the Action Sheet opens
**Then** it reads "Install update? Sound stops until the Velora restarts." with `WRITE · Install` and `EXIT · Later`; EXIT closes it and nothing changes
**And** the sheet never opens unprompted

**Given** the user confirms with WRITE
**When** the install starts
**Then** the engine fades the outputs to silence and engages the hardware mute in the pop-free order from Epic 3, the screen switches to the Boot Screen layout with `UPDATING TO 1.3`, a Progress Bar fed by `UpdateProgress`, and the line "If power is lost, the Velora starts on 1.2."
**And** panel and touch input are ignored during install (no control cancels a slot write in progress)

**Given** the install finished and the unit restarted on the new slot marked good
**When** Performance appears
**Then** a Notice Toast reads "Updated to 1.3"

**Given** `sysd` rejects the bundle
**When** the error returns
**Then** the screen shows "Update failed: the file is damaged or not signed. Still on 1.2." and sound returns without a restart if the slot was never written

**Given** the rollback state file from Story 6.12 is present at boot
**When** Performance appears
**Then** a Notice Toast reads "Update did not finish. Still on 1.2 · Try again in Settings › System & Update." and the state file is cleared after it is shown once

**Given** all strings in this flow
**When** they are added
**Then** they are locale-catalog keys (EN source) and pass the 1024×600 layout at +40% pseudo-localized length

### Story 8.4: MCU firmware manifest in the A/B image and boot-time version check

As a keyboardist,
I want the keybed and panel microcontroller firmware to travel inside the system image and be checked at every boot,
So that the whole instrument always runs one matching version and I never flash anything by hand.

**Covers:** FR48 (complete: builds on the dual-bank bootloader and reflash mechanism of Story 3.4), AR43, AD-16 (MCU firmware ships inside the A/B image)

**Runs on:** Prototype (keybed and panel MCUs with the Story 3.4 bootloader); Mac for the boot-ordering and failure-reporting logic against a simulated MCU

**Acceptance Criteria:**

**Given** the image
**When** it builds
**Then** it includes the keybed and panel MCU firmware images with their versions and hashes in a manifest under `/usr/lib/velora/mcu/`, so they switch with the A/B slot
**And** the build fails if any manifest hash does not match its image

**Given** boot, before `engine` starts
**When** `sysd` reads each MCU's HELLO version over its link and compares it with the manifest of the booted slot
**Then** if the version matches it does nothing; if it differs, it calls the Story 3.4 reflash mechanism with the manifest's image (no second flashing implementation)
**And** `engine.service` is ordered after this step, so the engine never talks to an MCU being flashed

**Given** an A/B rollback to the previous slot (Story 6.12)
**When** the unit boots the older slot
**Then** the version check finds the MCU firmware differs from that slot's manifest and reflashes the older version, so MCU and system firmware always match

**Given** a flash interrupted by a power cut or a CRC failure
**When** the unit boots again
**Then** the MCU's immutable bootloader still runs the previous bank, `sysd` retries the flash once per boot, and after a second failure `sysd` publishes "Keyboard controller update failed · Contact support" for the UI while the previous MCU firmware keeps running

**Given** an MCU flash at boot
**When** it runs
**Then** `sysd` publishes the boot status "UPDATING CONTROLS" with progress for the Boot Screen, and the flash time is excluded from the ≤10 s boot target only on boots that flash

### Story 8.5: Format versions N/N−1 and migrations only after the slot is marked good

As a keyboardist,
I want every new firmware to open all my saved Live Sets and to change stored data only once it is sure it will stay,
So that an update or a rollback never makes my show unreadable.

**Covers:** FR49, FR69, AR29, NFR44, AD-12

**Runs on:** Mac + CI (readers, corpus); dev board (mark-good hook)

**Acceptance Criteria:**

**Given** every persisted form at this point (bundle `format_version`, Live Set and setlist files, `sysd` config `schema_version`, catalog schema, ws.v1 handshake version, MCU protocol version)
**When** a reader in `formatlib`, the control thread, `librarian` or `sysd` opens data
**Then** it accepts version N and N−1, rejects anything older or with an unknown major version with a typed error, and round-trips unknown fields unchanged when writing back
**And** a unit test per format proves an unknown field written by a newer version survives a read-modify-write

**Given** a compatibility corpus `tests/compat/` holding Live Sets, setlists and config files saved by every tagged release (starting with the current one)
**When** CI runs
**Then** every corpus file opens on the current build, and the release job adds the new release's saved files to the corpus
**And** a release whose build fails to open any corpus file fails the gate

**Given** a new release whose data needs an irreversible migration (for example a catalog schema change)
**When** it boots the first time
**Then** it reads the old data through its N−1 reader and runs read-only until RAUC marks the slot good; only then does a migration unit run, and it writes the migrated form atomically (new file, fsync, rename)
**And** if the slot rolls back before mark-good, the previous firmware finds its data unchanged

**Given** a migration that has already run
**When** the user installs an older release that only reads N−1
**Then** the data written by the migration is still readable by that release because the new release wrote N, never N+1 (verified by booting N−1 on migrated data in a test)

### Story 8.6: A firmware update adds a new engine on a v1 unit

As a keyboardist who already owns a unit,
I want a firmware update to bring a new engine that plays inside the same voice pool,
So that my instrument gains sounds over time without new hardware or broken Live Sets.

**Covers:** FR45, AD-7 (engines compiled in, switched with the image), AD-12

**Runs on:** Dev board

**Acceptance Criteria:**

**Given** two dev-signed releases built from the same branch, N with the engines shipped at this point and N+1 adding a small test engine (for example the test-tone engine from Story 1.8 registered as a fifth engine type behind a build flag)
**When** N+1 is installed over N through the Story 8.1 installer
**Then** the unit boots N+1, the new engine is listed in the engine registry, a Live Set can assign a part to it, and its voices are allocated by the pool arbiter with its own floor and cost units
**And** the 1 h engine run from Story 1.11 shows zero xruns with the new engine sounding alongside 200 sample voices

**Given** Live Sets saved on N
**When** they open on N+1
**Then** they play identically (offline-render null test against N's render) and save back in a form N can still read

**Given** a rollback from N+1 to N
**When** a Live Set that uses the new engine is opened on N
**Then** the part is silent and shows the warn state "Timbre needs a newer firmware" with its stored parameters preserved (never substituted), and the rest of the Live Set plays

### Story 8.7: Boot Screen with drawbar animation and reduced motion

As a keyboardist,
I want a calm boot screen that tells me what is being restored,
So that I know the unit is starting and which Live Set I will play first.

**Covers:** UX-DR38, UX-DR78 (restoring), UX-DR8 (boot wordmark stand-in), UX-DR89 (partial: static boot frame under reduced motion), NFR34 (partial), AR39 (partial: `reduced_motion` config key)

**Runs on:** Mac (LVGL SDL simulator); dev board (DRM/KMS timing)

**Acceptance Criteria:**

**Given** the `ui` process started by systemd
**When** it draws its first frame
**Then** it shows the Boot Screen on `bg`: the centred drawbar symbol and VELORA in the stand-in boot wordmark, a thin Progress Bar and a muted status line
**And** the first frame appears within 2 s of power-on on the dev board (measured), or the bootloader splash keeps the same artwork until it does

**Given** Reduced motion is off
**When** the boot runs
**Then** the drawbars pull out one by one (≈0.7 s each, ease-out, staggered 0.1–0.25 s), then VELORA fades in; the animation never delays the switch to Performance once the target is playable
**And** with Reduced motion on (the `reduced_motion` key this story adds to the `sysd` config, default Off, set through a `ws.v1` config command and read at start) the screen shows the final static frame

**Given** the engine publishes boot progress (resolving target, loading heads n of m, favorites)
**When** the status updates
**Then** the line reads `RESTORING · {Live Set} · SCENE n` with the Live Set name in user content (untranslated), and the Progress Bar follows the heads-loaded fraction of the target

**Given** the target is playable
**When** the engine reports ready
**Then** the UI switches to Performance with a ≤150 ms transition (≤80 ms or none under reduced motion)

### Story 8.8: Journaled Boot-into pointer

As a keyboardist,
I want the unit to remember the Live Set and scene I was on without writing to disk while I play,
So that the next power-on restores my place and the show never risks a disk write mid-song.

**Covers:** FR64, NFR15 (Boot-into exception), AD-18

**Runs on:** Mac (control-thread logic); dev board (write monitoring)

**Acceptance Criteria:**

**Given** the control thread
**When** the current Live Set or scene changes
**Then** it marks the Boot-into pointer `{setlist_id, live_set_id, scene_index, schema_version}` dirty in memory and does not write immediately

**Given** a dirty pointer
**When** an idle-safe moment arrives (no note held and no pedal down for ≥2 s, or 30 s since the change, whichever first, and never during an import's verify phase)
**Then** the control thread writes the pointer atomically: write to a temp file on `/data`, fsync, rename over the old file, fsync the directory
**And** a pointer identical to the stored one is not rewritten

**Given** a 1 h performance run on the dev board with 40 scene and Live Set changes
**When** disk writes are traced (`blktrace` or `/proc/diskstats`)
**Then** the only writes are pointer writes, each ≤4 KiB, each at an idle-safe moment
**And** no write happens while a note is held

**Given** a power cut at any time during a pointer write (simulated by killing the process between write steps, 1,000 times)
**When** the pointer is read at the next start
**Then** it is either the previous or the new pointer, never corrupt; a corrupt or missing file falls back to the Default Live Set target

### Story 8.9: Boot into setting and restoring the target

As a keyboardist,
I want to choose what is playable after power-on, the last sound I used, the start of my setlist or a default Live Set,
So that the unit starts where my show needs it to start.

**Covers:** FR62, FR65, UX-DR78 (target missing), AR39 (partial: Boot into in sysd config)

**Runs on:** Mac (SDL simulator + host engine); dev board

**Acceptance Criteria:**

**Given** the `sysd` config schema
**When** this story adds `boot_into` (`LAST_SOUND` default, `START_OF_SETLIST`, `DEFAULT_LIVE_SET` + `default_live_set_id`)
**Then** the field is versioned per AD-12 and published to `engine` and `ui` at start and on change

**Given** the Settings surface
**When** this story adds the Boot into row to Display & Language (the section exists since Story 4.37)
**Then** it shows a Segmented Control "Last sound used / Start of setlist / Default Live Set"; choosing Default Live Set opens a List Row picker of All Live Sets
**And** the choice is saved by `sysd` immediately

**Given** each option at boot
**When** the engine resolves the target
**Then** Last sound used loads the Live Set and scene from the Story 8.8 pointer with its setlist loaded; Start of setlist loads the first Live Set of the last loaded setlist at scene 1; Default Live Set loads the chosen Live Set at scene 1 with no setlist loaded (`NO SETLIST`)

**Given** the target Live Set was deleted, or a library it uses is missing
**When** boot finishes
**Then** boot still ends on Performance: a deleted target falls back to the first Live Set of All Live Sets (or an empty new Live Set when none exist), and a missing library shows Missing library on the affected parts, silent, never substituted
**And** the Boot Screen never stalls waiting for a missing target

### Story 8.10: Boot to playable in ≤10 s with a 1 TB library

As a keyboardist,
I want to play my first sound within ten seconds of switching on, even with a full terabyte of libraries,
So that a power glitch on stage costs me seconds, not the song.

**Covers:** FR63, NFR5, AR20 (partial: boot loads Boot-into heads first, then favorites), AR19 (partial)

**Runs on:** Dev board (acceptance); prototype later re-runs it

**Acceptance Criteria:**

**Given** the boot sequence
**When** systemd starts services
**Then** the order is: MCU check (Story 8.4), `sysd` config, `engine` (resolves the Boot-into target and loads its heads first), `ui` in parallel with the engine; `librarian` and catalog indexing start only after the engine reports the target playable
**And** non-essential units (fstrim, SQLite checkpoints, log flush) are deferred to idle

**Given** the target is playable
**When** the engine continues
**Then** it loads heads of ★ favorite timbres next within the head budget, then the preload of the next in-setlist Live Set, all on non-RT threads at low priority, with no effect on the playable target (zero xruns during the first 60 s)

**Given** the dev board with ≥1 TB of indexed bundles and a Boot-into target of an 8-part Live Set using deep sample instruments
**When** power-on to first audible note is measured 20 times per Boot-into option (power relay + scripted note + audio capture)
**Then** every run is ≤10 s, and the report gives the time per boot phase (bootloader, kernel, systemd, engine heads, UI first frame)
**And** a CI-callable boot benchmark script stores the result with the image version so regressions fail the release gate

### Story 8.11: Power-fail acceptance: brown-out recovery and scope verification

As a keyboardist,
I want the power-fail protection proven on the finished software stack and short brown-outs survived without a reboot,
So that the PA does not pop, my data is safe when someone trips over the cable, and a flicker in the venue's power does not stop the show.

**Covers:** FR68 (acceptance; the fade and write-freeze mechanism is owned by Story 3.18), NFR18, AR37 (acceptance)

**Runs on:** Prototype (POWER_FAIL GPIO and supercap hold-up) with a scope and a programmable supply

**Acceptance Criteria:**

**Given** the fade and write-freeze mechanism from Story 3.18
**When** the writers delivered since then (import, Write, Boot-into pointer, update staging) receive its freeze signal during an operation
**Then** each abandons the operation in its atomic-staging state (no rename after POWER_FAIL), so recovery at next boot sees either the old or the new state
**And** no writer adds its own POWER_FAIL handling; each subscribes to the Story 3.18 signal, checked by a code review item and a fault-injection test per writer

**Given** the prototype on a scope running the release image
**When** input power is removed 50 times at random points while a dense Live Set plays and while an import and a Write are in progress
**Then** silence and mute are reached before the rails drop every time, the NVMe flush completes within the measured hold-up (≥150 ms, with POWER_FAIL ≥100 ms before rail drop), no audible pop is recorded on the outputs above the defined threshold, and the journaled partition mounts clean with every data item in its old or new state
**And** the measured margins are recorded in a report and compared with the Story 3.18 bring-up figures

**Given** POWER_FAIL de-asserts within the hold-up window (a brief brown-out) and the rails never dropped
**When** power returns
**Then** the unit does not reboot (replacing the interim reboot path of Story 3.18); writers resume, mute releases, sound returns with a fade-in, and the event is logged

**Given** 20 brown-outs of varying length up to and beyond the hold-up window
**When** each ends
**Then** every brown-out within the window recovers without a reboot and without an audible pop, and every one beyond it ends in the same clean power-down as the 50-cut test

### Story 8.12: Startup reconcile of interrupted imports: fault injection and boot bound

As a keyboardist,
I want the unit to clean up after an import interrupted by a power cut,
So that my library never shows a half-imported instrument or wastes space on leftovers.

**Covers:** FR67, AR28 (partial: reconcile fault injection, time bound and boot ordering; the reconcile rule is owned by Story 5.6), AD-11

**Runs on:** Mac (librarian on host with a temp `/data`); dev board

**Acceptance Criteria:**

**Given** the startup reconcile rule from Story 5.6 (delete staging leftovers, remove catalog rows whose library has no `current`, recreate missing catalog rows from the manifests of bundles under `current`) and the lease-aware GC from Story 5.21 for unreferenced bundles
**When** `librarian` starts on the board
**Then** the reconcile pass runs with that rule unchanged and before the librarian accepts commands; this story adds no reconcile rule of its own
**And** the pass logs a summary (counts per action) and never touches a bundle referenced by `current`

**Given** a fault-injection test that kills `librarian` at each step of the import pipeline (stage, verify, fsync, rename, flip `current`, catalog row) 100 times on random steps
**When** `librarian` restarts each time
**Then** the library set and catalog equal either the state before that import or after it, and an interrupted re-import (Import again) leaves the previous bundle playing

**Given** a large library
**When** reconcile runs on a 1 TB `/data`
**Then** it completes without scanning sample contents (directory and manifest level only), in ≤5 s, and does not delay the Boot-into target (it starts after the engine reports playable)

### Story 8.13: Power-cut rig: 100 cuts during update and 100 during import

As a founder,
I want an automated rig that cuts power at random points during a firmware update and during an import, 100 times each,
So that the CAP-17 promise is proven on hardware before units ship.

**Covers:** NFR12, FR66, FR67, AR2 (partial: release evidence)

**Runs on:** Prototype or dev board with a controllable power relay and serial/network observation (dev image)

**Acceptance Criteria:**

**Given** a rig with a relay on the unit's DC input driven by a test host, and the dev image reporting boot slot, version and library state over the dev network
**When** the update campaign runs
**Then** the host starts a USB-stick update (Story 8.1), waits a random delay uniformly across the whole install window (staging, slot write, reboot, first boot before mark-good), cuts power for 5 s, restores it, and records which firmware booted
**And** after 100 cycles the unit booted every time into either the previous or the new firmware, with the slot states consistent with the outcome

**Given** the import campaign
**When** the host starts an import of a multi-GB SFZ batch from a USB SSD and cuts power at a random moment, 100 times
**Then** after every boot the catalog and `/data/lib` hold no partially imported library, every previously imported library still plays (a scripted note per library is rendered and checked non-silent), and reconcile counts are logged

**Given** a campaign finished
**When** the report is written
**Then** it lists each cycle (delay, phase hit, outcome) and states pass/fail; any single failure fails the release gate

### Story 8.14: Kernel LTS migration plan

As a founder,
I want a dated plan to move from Linux 6.18 LTS to the next LTS before it reaches end of life,
So that shipped units keep receiving security fixes without risking stage latency.

**Covers:** AR5

**Runs on:** Documentation; later execution on the dev board and prototype

**Acceptance Criteria:**

**Given** the 6.18 LTS end of life (≈ Dec 2027)
**When** the plan is written in `yocto/KERNEL-LTS-PLAN.md`
**Then** it states the start date (6 months before EOL), the candidate next LTS, the inventory of kernel dependencies to re-validate (PREEMPT_RT config fragment, DT for DSI, I2S/TDM machine driver, USB gadget functions f_uac2/f_midi2/f_ncm, RAUC bootloader backend, OP-TEE interface)
**And** it lists the acceptance gates: Story 1.1 latency threshold, Story 1.22 24 h soak with zero xruns, Story 8.10 boot ≤10 s, Story 8.13 power-cut campaigns, and the USB enumeration matrix

**Given** the plan
**When** it is reviewed
**Then** it includes a rollback approach (the new kernel ships as a normal RAUC update, so A/B rollback covers a failed boot) and a calendar reminder or CI check that fails a build after the start date if the plan is not started

## Epic 9: Velora Studio and USB

A keyboardist uses the unit as a driverless USB audio/MIDI interface, pairs Velora Studio, edits everything at a glance with live playback on the unit, writes with the device's scopes, transfers libraries converted on the computer, sends firmware, exports diagnostics, backs up and restores setlists and Live Sets, and exports and imports selected Live Sets, timbres or Live Set bundles, also offline. Development is Mac-first: Velora Studio (Tauri 2.12) is native on macOS, Windows and Linux and is developed on the Mac against the host engine (`engine-host` from Story 1.10) exposing the same ws.v1 framing on localhost; the USB gadget, the `sysd` NCM bridge and cross-OS enumeration need the dev board (USB-C peripheral role) or the prototype.

### Story 9.1: USB-C gadget composition with class-compliant MIDI

As a keyboardist,
I want to plug the unit into any Mac, Windows or Linux computer with one USB-C cable and see it as a MIDI device without installing drivers,
So that my DAW can play and record the Velora straight away.

**Covers:** FR36, FR37, FR55, AR9 (partial: USB peripheral role, composite on three OSes), AR38 (partial: gadget composition), NFR24 (partial: never mass storage or console), AD-13

**Runs on:** Dev board (USB-C OTG/peripheral port); Mac, Windows and Linux hosts for enumeration tests

**Acceptance Criteria:**

**Given** the dev image with the USB-C controller in peripheral role in the device tree
**When** `sysd` sets up the gadget through configfs at boot
**Then** the composite exposes exactly three functions, UAC2, MIDI (f_midi2 with MIDI 2.0 UMP and a MIDI 1.0 alternate setting) and CDC-NCM, with a fixed VID/PID, the product string "Velora 73" and a per-unit serial
**And** no mass-storage, ACM/serial or other function exists in the image's gadget config, checked by a CI test that parses the configfs script

**Given** the unit connected to macOS (current and previous major), Windows 11 and a current Ubuntu LTS host
**When** it enumerates
**Then** each OS loads its in-box class drivers with no extra installation, the MIDI port appears in the OS MIDI setup (MIDI 2.0 where the OS supports it, MIDI 1.0 otherwise), and the results are recorded in an enumeration matrix in `sysd/USB-MATRIX.md`

**Given** a DAW sending notes to the Velora MIDI port
**When** notes arrive
**Then** `engine` (sole owner of USB MIDI, AD-4) reads them on an input edge thread, timestamps them in device time and applies them through the same path as Story 1.20, so they sound with the same fixed scheduling delay
**And** keybed and panel events from the unit are sent out the same port as UMP (MIDI 1.0 converted when the host uses the 1.0 setting)

**Given** any USB-C connection
**When** the host lists storage devices
**Then** no block device from the unit appears; the internal NVMe is never exposed on any path

### Story 9.2: USB audio interface that follows the codec clock

As a keyboardist,
I want my computer to record the Velora's outputs over USB with no drivers,
So that I can capture my playing in a DAW without extra audio gear.

**Covers:** FR35, AR23 (partial: UAC2 gadget async with feedback endpoint), NFR40, AD-17

**Runs on:** Dev board with the I2S codec path (prototype with PCM3168A for final acceptance); Mac, Windows, Linux hosts

**Acceptance Criteria:**

**Given** the UAC2 function from Story 9.1
**When** it is configured
**Then** it offers a 48 kHz, 24-bit capture stream (device → host) carrying the main stereo mix and the aux stereo pair (4 channels), in asynchronous mode with an explicit feedback endpoint
**And** the gadget's sample clock follows the codec's crystal; the engine keeps rendering on the codec clock and never on USB timing

**Given** the engine output
**When** USB audio is active
**Then** an edge thread copies rendered blocks into the gadget's buffer through a lock-free ring, and a USB stall or host disconnect never blocks the RT plane (the ring drops on overflow and counts drops)
**And** the main I2S output is unaffected (zero xruns) while a host starts, stops and reconfigures the USB stream 100 times

**Given** a DAW on each of the three OSes
**When** it records 30 min from the Velora while the unit plays a scripted sequence
**Then** the recording has no dropouts or clicks, and the drift between recorded length and expected length is zero samples beyond the feedback loop's settling (verified by a pilot tone)
**And** the result is added to the enumeration matrix

### Story 9.3: sysd ws.v1 bridge on USB NCM with a single open port

As a keyboardist,
I want Velora Studio to reach the unit over the same USB cable through one locked-down connection,
So that the computer link is convenient but nothing else on the unit is reachable.

**Covers:** AR25 (partial: framed TCP over USB NCM, version and feature handshake), AR38 (partial: nftables single-port policy, ws.v1 bridge), NFR24 (partial), NFR44 (partial: ws.v1 grows by addition)

**Runs on:** Dev board; Mac (bridge logic tested against the host engine on localhost)

**Acceptance Criteria:**

**Given** the NCM interface from Story 9.1
**When** a host connects
**Then** `sysd` assigns a link-local address pair on the NCM link (DHCP server or fixed link-local, recorded in the design), and listens on one TCP port that speaks length-prefixed ws.v1 frames
**And** nftables on the NCM interface drops everything except that port (no ICMP beyond what link setup needs, no SSH, no DNS), checked by a port scan in the test suite

**Given** a new TCP connection
**When** the client sends `Hello {client_version, features[], client_id}`
**Then** `sysd` replies with `{device_version, firmware_version, features[], device_id, model}` and the negotiated feature set; messages for features not in the set are rejected with the closed error code `UNSUPPORTED_FEATURE`
**And** an unpaired client may only call `Hello`; everything else returns `NOT_PAIRED` (the unpaired allow-list is a table, so pairing messages can be added to it without changing the bridge)

**Given** a paired session
**When** the client sends commands
**Then** `sysd` forwards them to the owning process over UDS (control thread for Live Set data, `librarian` for library and catalog, `sysd` itself for config and updates) and relays state snapshots and `(epoch, seq)` deltas back, never interpreting Live Set content itself
**And** `request_id = (client_id, seq)` is preserved end to end

**Given** Velora Studio development on the Mac
**When** `engine-host` runs with `--studio-port`
**Then** it serves the same framed ws.v1 protocol on localhost with pairing bypassed and a "DEV HOST" device model, so Studio work does not need the board

### Story 9.4: Design story — Velora Studio footer, dialog, banner and Backup & Restore

As the developer about to build Velora Studio,
I want mockups of the Studio Footer, Studio Dialog (pairing, Write to Velora scope, Restore), Studio Banner and the Backup & Restore surface,
So that implementation follows a drawn, reviewed design instead of improvising the undrawn parts.

**Covers:** UX-DR106, UX-DR45, UX-DR46, UX-DR47, UX-DR71 (design), UX-DR114 (design), UX-DR97 (applied to these mocks)

**Runs on:** Mac (HTML mockups next to `mockups/key-screen-studio.html`)

**Acceptance Criteria:**

**Given** DESIGN.md tokens and the Studio mock
**When** the mockups are drawn in `ux-designs/.../mockups/studio-footer-dialog-banner.html` and `studio-backup-restore.html`
**Then** they show, in both Stage (dark) and Studio (light) themes: the Footer states (idle with last backup, `● LIVE EDIT ON`, `UNSAVED: SCENE 2` with warn ◆, `WRITING TO VELORA…`, transfer progress), the Studio Dialog variants (pairing-code entry with the wrong-code message, Write to Velora scopes Scene / Whole Live Set / Save as new… with the disabled-reason line, Restore Merge / Replace with the result of each stated), and the Studio Banner variants (disconnect · Reconnect, version mismatch · Update firmware / Update app, write failed, stale edit · Reload)
**And** Backup & Restore shows the backup location choice, last backup time, restore file pick, and the Merge / Replace dialog
**And** `studio-backup-restore.html` also shows the Export dialog (scope Live Sets / Timbres / Bundle, Studio Selection List with sizes, "Reference only" rows, total size estimate, large-bundle warning) and the Import dialog with per-item Skip / Replace / Keep both

**Given** the mockups
**When** they are reviewed
**Then** each passes a contrast check (text ≥4.5:1, boundaries ≥3:1), shows the 2 px focus ring on the focused control, shows a 200% text-size variant with no horizontal scroll, and is shown once pseudo-localized at +40% with `ÁÇÊÕÑ Ação`
**And** DESIGN.md and EXPERIENCE.md reference the new mocks, and any decision taken while drawing is logged in the UX `.memlog.md`

### Story 9.5: Velora Studio app shell, offline

As a keyboardist,
I want Velora Studio to open on my computer with the familiar Velora look, in my language and theme, even with no unit connected,
So that I can start organising my sounds anywhere.

**Covers:** AR46 (partial: Tauri app), FR95 (partial: opens offline), FR118 (Studio follows OS theme), UX-DR1 (partial: CSS output of the shared token generator), UX-DR39, UX-DR40, UX-DR44, UX-DR45, UX-DR67 (shell), UX-DR7, UX-DR90 (partial: Tab order, F6 regions, focus ring), UX-DR92 (partial: rem sizes, OS reduced motion), UX-DR93 (partial: Studio strings as catalog keys, follows OS language)

**Runs on:** Mac (also builds on Windows and Linux in CI)

**Acceptance Criteria:**

**Given** `desktop/` with a Tauri 2.12 project
**When** it builds in CI on macOS, Windows and Linux
**Then** it produces a runnable app per OS, and the Rust side links the `proto/` ws.v1 types (generated) with no device-only code

**Given** the app launches with no device
**When** the main window opens
**Then** it shows the Studio Title Bar (app name, connection state "No Velora connected" in muted, Import, Backup, the Primary Button "Write to Velora" disabled), the Studio Sidebar (SETLISTS, LIBRARY, DEVICE), an empty main area with a short offline explanation, and the Studio Footer with the last backup time or "No backup yet"
**And** the window enforces a 1280×800 minimum size

**Given** the DESIGN.md tokens
**When** the UI renders
**Then** colours come from a CSS custom-properties output that this story adds to the Story 2.18 token generator (both `-dark` and `-light` sets, the same source the device uses), the Story 2.18 contrast gate and colour-literal lint are extended to `desktop`, and the theme follows the OS appearance live, with an override (System / Stage / Studio) in the app settings
**And** all sizes are rem with body ≥13 px and labels ≥11 px at 600 weight

**Given** every user-facing string in the shell
**When** it is rendered
**Then** it comes from the shared locale catalog (EN source) via keys; the app picks the OS language when a catalog exists for it, falls back to EN, and offers a language override in its settings

**Given** keyboard use
**When** the user presses Tab and F6
**Then** Tab visits controls in visual order, F6 cycles sidebar → main → footer, and the focused control always shows the 2 px accent ring at 2 px offset
**And** with the OS reduced-motion preference on, the app uses no transitions longer than 80 ms

### Story 9.6: Pairing Velora Studio with the unit

As a keyboardist,
I want to pair my computer by reading a code on the Velora and typing it into Velora Studio,
So that only computers I approved can change my instrument.

**Covers:** FR96, AR38 (partial: pairing-code authentication, paired-computer store), UX-DR37 (pairing code variant), UX-DR46 (pairing dialog), UX-DR77 (first-time pairing, connected), NFR16

**Runs on:** Mac (Studio + device UI in the SDL simulator against host services); dev board (end-to-end over USB)

**Acceptance Criteria:**

**Given** the Settings surface on the device
**When** this story adds the Computer (Velora Studio) section
**Then** it shows Pair new computer, the list of paired computers (name, last connected) each with Forget, and a USB audio/MIDI status line

**Given** the user selects Pair new computer
**When** `sysd` starts a pairing window
**Then** an Action Sheet shows a random 6-digit code, the computer name once a client asks, and `EXIT · Cancel`; the window closes after 2 min or on EXIT
**And** sound is never interrupted

**Given** Velora Studio connected over NCM but unpaired
**When** it detects the unit
**Then** a Studio Dialog "Enter the code shown on Velora 73" opens with a 6-digit field, Enter confirms, Esc cancels, focus is trapped in the dialog and returned afterwards
**And** a wrong code shows "That code doesn't match. Check the Velora screen."; after 5 wrong codes the pairing window closes and must be restarted on the device

**Given** a correct code
**When** pairing completes
**Then** the client and `sysd` exchange long-term keys (a key-agreement bound to the code, design recorded in `sysd/PAIRING.md`), `sysd` stores the paired computer in its config, the device Action Sheet closes, and the Studio Title Bar reads "● VELORA 73 · CONNECTED VIA USB · PAIRED" in ok
**And** later connections from that computer authenticate without a code

**Given** a paired computer forgotten on the device
**When** it connects again
**Then** it is treated as unpaired and gets `NOT_PAIRED`

### Story 9.7: Connection states: version mismatch, disconnect and reconnect

As a keyboardist,
I want Velora Studio to tell me clearly when versions differ or the cable is pulled, and to reload the unit's state when it returns,
So that I never edit stale data or wonder whether my changes reached the instrument.

**Covers:** FR99 (partial: version mismatch, disconnect and reconnect), UX-DR47, UX-DR77 (connected, offline, version mismatch), UX-DR91 (partial: live region for connection state), AR25 (partial: full snapshot on connect)

**Runs on:** Mac (against host engine with version overrides); dev board

**Acceptance Criteria:**

**Given** the handshake from Story 9.3
**When** device and app feature sets differ
**Then** features the device lacks are hidden in the app (not shown disabled), and a Studio Banner reads "Velora 73 runs 1.2; this Velora Studio needs 1.3 for some features. Update firmware / Update app."
**And** live edit is disabled only when the major protocol versions cannot talk, with the banner saying so

**Given** a connected session
**When** the connection is lost (cable pulled, unit restarted)
**Then** within 2 s the Title Bar changes to "Velora 73 disconnected" and a Studio Banner "Velora 73 disconnected · Reconnect" stays until resolved; the app keeps working in offline mode
**And** the change is announced through a polite live region

**Given** the unit returns
**When** the app reconnects (automatically or via Reconnect)
**Then** it repeats the handshake, requests a full snapshot, replaces its view of device state with it, removes the banner and announces "Connected"

### Story 9.8: Live Set view: scenes and part strips

As a keyboardist,
I want to see a whole Live Set on my computer, its scenes and its eight parts at a glance,
So that I understand and navigate my show faster than on the 7-inch screen.

**Covers:** FR97 (partial: selected Live Set with 8 scenes and 8 parts), UX-DR14 (reused in Studio), UX-DR30 (Studio header ◆), UX-DR40, UX-DR41, UX-DR67, UX-DR90 (partial: arrow keys in scene and part grids), UX-DR91 (partial: ◆ label)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** a paired connection and the published state snapshot
**When** the user expands SETLISTS in the sidebar and selects a Live Set
**Then** the main area shows the Live Set header (name in `desktop-live-set-name`, ◆ in warn when the device has unsaved edits for it), 8 Scene Tiles (current filled, empty dashed) and 8 Studio Part Strips (`PART n` + LED, timbre name, category in accent, level meter, `VOL n`, key range, organ registration on organ parts, "+ Add timbre" on empty parts)
**And** selecting a Live Set in Studio does not change what the unit is playing

**Given** state deltas from the device
**When** the player changes scene, part on/off or volume on the unit
**Then** the Studio view updates within 100 ms of the delta arriving, without a full reload

**Given** keyboard use
**When** focus is in the scene or part grid
**Then** arrow keys move within the grid, Enter on a part strip selects it, and the ◆ has `aria-label="Unsaved changes"` and a tooltip

**Given** a part whose library is missing on the device
**When** it is shown
**Then** the strip shows "Missing library" in warn with a glyph, never another timbre's name

### Story 9.9: Part Editor layout with Studio Knobs

As a keyboardist,
I want every parameter of a part on one screen, editable with mouse or keyboard and playing on the unit as I change it,
So that I shape sounds quickly with my hands still on the keys.

**Covers:** FR97 (partial: Part Editor with all parameters at once incl. ASSIGN map), FR98 (partial: edits play on the unit immediately), FR103 (Studio side of ASSIGN editing), UX-DR42, UX-DR43, UX-DR91 (partial: ARIA sliders)

**Runs on:** Mac (against host engine); dev board (end-to-end)

**Acceptance Criteria:**

**Given** a selected part
**When** the Part Editor opens below the part strips
**Then** it shows four hairline-divided sections with no menus or tabs: Sound (engine parameters, organ drawbars and controls), Amp envelope + EQ (accent envelope polyline), Effects (insert rows with bypass and reorder, dashed "+ Add insert", reverb and delay sends), Zone & keyboard + Arpeggiator + ASSIGN · KNOBS (THIS LIVE SET)
**And** the parameters shown come from the generated parameter registry in `proto/`, so a parameter added to the registry appears without hand-written UI per field

**Given** a Studio Knob
**When** the user drags, scrolls, types a value or uses ↑/↓ (1), Shift (±10), PgUp/PgDn, Home/End, Enter (value entry)
**Then** the value changes and is exposed as an ARIA `slider` with name, unit and `aria-valuetext`
**And** each change is sent as a ws.v1 edit carrying `base_revision` and plays on the unit immediately

**Given** text size 150% or 200% (OS or ⌘/Ctrl +/−)
**When** the Part Editor renders
**Then** it reflows from 4 to 2 to 1 columns with no horizontal scrolling

### Story 9.10: Live edit state, stale edits and disconnect mid-edit

As a keyboardist,
I want the unit and Velora Studio to show that edits are coming from the computer and which are unsaved, and to be protected from stale edits and pulled cables,
So that I always know what is playing and never overwrite newer changes made on the instrument.

**Covers:** FR98 (partial: live edit, `STUDIO`, ◆), FR99 (partial: disconnect mid-edit, stale edits), UX-DR45 (`● LIVE EDIT ON`, `UNSAVED: …`), UX-DR72 (Studio live edit state on device)

**Runs on:** Mac (against host engine); dev board (end-to-end)

**Acceptance Criteria:**

**Given** Part Editor edits arriving from Story 9.9
**When** the control thread accepts them
**Then** the device Status Bar shows `STUDIO` while edits arrive, as it does for any Velora Studio live edit

**Given** an edit accepted by the control thread
**When** state is published
**Then** the Live Set shows ◆ on the device (Hero, List Rows, Result Tiles) and in Studio, and the Footer reads `● LIVE EDIT ON · CHANGES PLAY ON THE VELORA` and `UNSAVED: SCENE n` or `UNSAVED: LIVE SET` (a timbre change)

**Given** the same Live Set was changed on the device after Studio loaded it
**When** a Studio edit with an old `base_revision` arrives
**Then** the control thread rejects it with `STALE_REVISION`, and Studio shows the Banner "Changed on the Velora since you opened it · Reload"; Reload applies the current snapshot

**Given** the cable is pulled mid-edit
**When** the connection drops
**Then** edits already applied stay on the unit in memory with ◆ until Write or power-off; after reconnect Studio reloads the device state and shows those edits as unsaved

### Story 9.11: Add a timbre to an empty part from Velora Studio

As a keyboardist,
I want to fill an empty part from Velora Studio by picking any timbre installed on the unit,
So that I can build a Live Set on the computer without going back to the instrument.

**Covers:** FR97 (partial: "+ Add timbre" on empty parts), FR98 (partial: structural live edit marked ◆), UX-DR41 (partial: empty Part Strip action)

**Runs on:** Mac (against host engine and librarian); dev board

**Acceptance Criteria:**

**Given** an empty part
**When** the user clicks "+ Add timbre"
**Then** a picker lists device-installed timbres (from the Library API through the `sysd` bridge) by category with search, fully operable by keyboard with Esc closing it without change, and choosing one assigns it live (a structural change marked ◆ for Whole Live Set scope)

**Given** the chosen timbre's library is later missing on the device
**When** the Live Set is shown again
**Then** the strip shows "Missing library" in warn as in Story 9.8, never another timbre

### Story 9.12: Write to Velora with the device's scopes

As a keyboardist,
I want to save my Studio edits on the unit with the same choices I know from the panel,
So that what I built on the computer is permanent on the instrument.

**Covers:** FR98 (partial: persist only with Write to Velora, success clears ◆ on both), FR99 (partial: write failure), UX-DR44, UX-DR46 (Write scope dialog), UX-DR77 (write to Velora, write failed), UX-DR91 (partial: live region for write result)

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** unsaved edits on the device for the selected Live Set
**When** the user clicks "Write to Velora"
**Then** a Studio Dialog lists what changed and offers Scene / Whole Live Set / Save as new… with the device's rules: Scene is disabled with its reason when edits include a timbre change; Save as new suggests a free name and rejects a name in use with the device message
**And** with no edits, the button is disabled with the tooltip "Nothing to write"

**Given** the user confirms
**When** the write runs
**Then** Studio sends the device's Write command (the same control-thread command the device Write Dialog uses), the Footer reads "WRITING TO VELORA…", the Primary Button is disabled, and on success ◆ clears on device and Studio and a polite live region announces "Written to Velora"

**Given** the write fails (connection lost, storage full)
**When** the error returns or the connection drops
**Then** a Studio Banner explains it ("Couldn't write to Velora 73: connection lost. Your edits are still on the Velora (◆). Reconnect and write again." or the storage-full message), and edits stay on the unit with ◆

### Story 9.13: Setlists view in Velora Studio

As a keyboardist,
I want to organise setlists, their Live Sets and setlist tags on the computer, including reordering by keyboard,
So that preparing a show's running order is quick and accessible.

**Covers:** FR97 (partial: Setlists view), UX-DR68, UX-DR90 (partial: Alt+↑/↓ and "Move to…")

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the SETLISTS section
**When** the Setlists view opens
**Then** it lists setlists with their Live Sets in order and the setlist tags, and supports the same operations as the device Setlist surface: create, rename and reorder setlists and their Live Sets, create a new Live Set (8 empty parts, one scene), and create/edit setlist tags
**And** every change is sent as the same control-thread command the device uses, so it is applied and persisted exactly as the device applies it

**Given** keyboard use
**When** a row is focused
**Then** Alt+↑/↓ moves it, and a "Move to…" menu moves a Live Set to another setlist or position, with the result announced through a live region
**And** drag-and-drop works with the mouse as an equivalent

### Story 9.14: Library management and heavy SFZ conversion on the computer

As a keyboardist,
I want Velora Studio to convert large SFZ libraries on my computer, with categories suggested just like on the unit,
So that big imports are fast and the unit keeps playing while I work.

**Covers:** FR54 (partial: heavy SFZ conversion on the computer), AR27 (partial: desktop no-decrypt formatlib build), AR46 (partial: FFI to formatlib), FR24 (Studio path: user content never encrypted), UX-DR69 (partial: browse, import/convert)

**Runs on:** Mac (formatlib FFI builds on Windows and Linux in CI)

**Acceptance Criteria:**

**Given** formatlib built in its desktop configuration
**When** the desktop build links it through FFI
**Then** the build has no decrypt symbols (checked in CI by symbol inspection) and cannot open protected factory bundles beyond their public manifest

**Given** the LIBRARY section
**When** the user opens Library management
**Then** it lists device-installed libraries by category (from the librarian catalog via ws.v1, when connected) and computer-side converted libraries (from a local cache directory), each with size and source

**Given** the user imports an SFZ or SF2 folder
**When** conversion runs on the computer
**Then** formatlib produces bundles with the same IDs, lossless coding, round-trip verification and category rules as the device importer (one of 17 categories required, suggested from names, apply-to-all available), skipping unsupported or damaged files with the same reasons
**And** conversion runs on worker threads with progress per file and can be cancelled without leaving partial bundles in the local cache

**Given** a converted library
**When** the result is compared with the device importer's bundle for the same source
**Then** `library_id`, `instrument_key` and decoded audio are identical (test over the reference corpus subset)

### Story 9.15: Library transfer to the unit

As a keyboardist,
I want to send a converted library to the Velora over USB while I keep playing,
So that big libraries reach the instrument without pen drives and appear only when complete.

**Covers:** FR54 (partial: transfer over mediated ws.v1 on USB NCM, activation when complete, failed transfer installs nothing), UX-DR69 (partial: transfer progress and failure), UX-DR77 (library transfer, transfer failed), UX-DR91 (partial: live region for transfer)

**Runs on:** Dev board (Mac for protocol tests against a host librarian)

**Acceptance Criteria:**

**Given** a converted library and a paired connection
**When** the user clicks "Send to Velora"
**Then** Studio first asks `librarian` for free space via ws.v1 and refuses to start when it does not fit, stating required vs free space
**And** otherwise it streams the bundle in chunks with per-chunk hashes through the `sysd` bridge to `librarian`, which stages it on `/data`

**Given** a transfer in progress
**When** it runs
**Then** the Footer and Library management show "Transferring Viola Caipira · 4.2 of 18.4 GB", the unit keeps playing with zero xruns (librarian is low priority, off audio cores), and the transfer resumes from the last verified chunk after a brief reconnect within the same session

**Given** the transfer completes
**When** `librarian` verifies the bundle (manifest, block hashes, bit-exact decode round-trip)
**Then** it publishes it with the atomic rename and `current` flip, writes the catalog row, and the device shows the Notice Toast "Viola Caipira ready in Library"

**Given** the connection is lost and not restored, or verification fails
**When** the transfer ends
**Then** nothing is installed, the staging data is removed (immediately, or by the startup reconcile), and Studio shows "Transfer stopped: connection lost. Nothing was installed. Start again when connected."

### Story 9.16: Backup to a .velorabackup file

As a keyboardist,
I want to back up all my setlists and Live Sets to a file on my computer,
So that a lost or broken unit never costs me my show.

**Covers:** FR93, AR29 (partial: `.velorabackup` versioned), AR30 (partial: backup reads published state), UX-DR71 (partial: backup), UX-DR45 (last backup time), UX-DR91 (partial: live region for backup)

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** a paired connection
**When** the user clicks Backup (Title Bar or Device › Backups)
**Then** a native save dialog asks for the location, and Studio reads published state (setlists, Live Sets with parts, scenes and ASSIGN maps, the global default ASSIGN map, setlist tags, Live Set favorites) through ws.v1 and writes one `.velorabackup` file
**And** the file carries a format version per AD-12, the device model and firmware version, and the backup time

**Given** the backup succeeds
**When** the file is written
**Then** the Footer shows the new last backup time, a polite live region announces it, and the time is remembered per device

**Given** a Live Set that references libraries
**When** it is backed up
**Then** it stores `library_id` and `instrument_key` references only, never library audio or keys

**Given** the backup is written while the player has unsaved edits
**When** it completes
**Then** it contains the last written state, and Studio notes "Unsaved edits on the Velora are not in this backup"

### Story 9.17: Restore with Merge or Replace, and offline backup editing

As a keyboardist,
I want to restore a backup either merged into what is on the unit or replacing it, and to open backups without the unit,
So that I can recover or move my show the way I choose, and prepare it anywhere.

**Covers:** FR94, FR95 (partial: offline editing of backups), AR30 (partial: restore via control-thread commands), FR92 (restore ID collisions), UX-DR46 (Restore dialog), UX-DR71 (partial: restore), UX-DR77 (offline, restore)

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** no unit connected
**When** the user opens a `.velorabackup` file
**Then** Studio shows its setlists and Live Sets in the same Setlists view and Live Set view, editable offline (rename, reorder, part parameters), and saves changes back to the file; live edit, transfer, firmware and Write to Velora are disabled with the reason "Connect a Velora"

**Given** a connected unit and a backup
**When** the user chooses Restore
**Then** a Studio Dialog asks Merge ("Add and update from the backup; keep what the backup doesn't have") or Replace ("Velora content becomes the backup"), naming the counts that will be added, updated and removed

**Given** the user confirms
**When** the restore runs
**Then** Studio sends it as control-thread commands (never by writing files on the unit), Merge adds and updates matching IDs and leaves others untouched, Replace makes device setlists and Live Sets equal to the backup, and an imported Live Set whose ID collides with a different local Live Set gets a new UUIDv4
**And** after restore, a fresh backup of the device equals the expected result (Replace: equals the source backup; Merge: the union with backup values winning), verified by an automated test

**Given** a backup from an older firmware (N−1 format)
**When** it is restored
**Then** it is read through the N−1 reader; a backup from a newer, unknown major version is refused with "This backup needs a newer Velora Studio"

### Story 9.18: Export selected Live Sets and timbres (individually or as a bundle)

As a keyboardist,
I want to export one Live Set, a selection of Live Sets, selected timbres, or a bundle of Live Sets with the timbres they use,
So that I can share a song or move a whole repertoire to another Velora without a full backup.

**Covers:** FR122 (partial: export), AR29 (partial: `.velorasets` and `.veloratimbre` versioned), AR30 (partial: export reads published state and `librarian`), UX-DR71 (partial: Export…), UX-DR114 (partial: export), UX-DR46 (Export dialog), UX-DR45 (export progress), UX-DR91 (partial: live region for export)

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** a paired connection, or an opened `.velorabackup` offline
**When** the user chooses Export… (Backup & Restore, or right-click › Export… on a Live Set or timbre, which pre-selects it)
**Then** a Studio Dialog offers scope Live Sets, Timbres or Bundle and a Studio Selection List of the matching items with name, category and size, a header select-all and Space to toggle the focused row
**And** in Bundle scope the timbres used by the ticked Live Sets are listed beneath, checked and locked, with factory and partner timbres tagged "Reference only" at 0 B

**Given** a selection
**When** rows are ticked or unticked
**Then** the dialog shows a live total size estimate computed from the manifest ("About 2.4 GB · 3 Live Sets, 5 timbres (2 by reference)") and Export is disabled with nothing selected
**And** when user timbres push a bundle past 2 GB, or past the free space at the chosen location, a warn line names the size and its cause; not enough space at the location blocks Export

**Given** the user confirms and picks a location
**When** the export runs
**Then** Live Set data is read from published control-thread state through ws.v1, user-imported (SFZ/SF2) timbres are fetched through `librarian` commands as lossless formatlib bundle data (AD-11), and nothing is written on the unit
**And** Live Sets and bundles are written as one `.velorasets` file (manifest scope `sets` or `bundle`) and timbre selections as one `.veloratimbre` file, each carrying an AD-12 format version, the device model and firmware version, the export time and the item list with IDs and sizes

**Given** a factory or partner timbre in the selection
**When** it is exported
**Then** the file carries only its `library_id` and `instrument_key` (AD-10), never encrypted blocks, keys or plaintext audio (AD-14), verified by an automated test that scans the file for vendor bundle content
**And** a timbre-only export made only of factory/partner timbres is not offered

**Given** a running export
**When** it progresses or is cancelled
**Then** the Footer shows "EXPORTING · 2.1 of 6.8 GB" while the unit keeps playing, cancel leaves no partial file, and on success a polite live region announces "Exported 3 Live Sets and 5 timbres to Gig-Ana.velorasets"

### Story 9.19: Import partial exports with per-item merge decisions

As a keyboardist,
I want to import a `.velorasets` or `.veloratimbre` file and decide, item by item, what happens when something already exists,
So that I can bring songs and sounds from another Velora without losing or duplicating my own by accident.

**Covers:** FR122 (partial: import, per-item merge, missing library), FR92 (import ID collisions), AR30 (partial: import via control-thread and `librarian` commands), UX-DR114 (partial: import), UX-DR77 (partial: import states), UX-DR91 (partial: live region for import)

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** a connected unit and a `.velorasets` or `.veloratimbre` file
**When** the user chooses Import…
**Then** Studio reads the manifest through the N/N−1 reader (AD-12) and shows a Studio Selection List with every item ticked; a file from a newer, unknown major version is refused with "Couldn't import Gig-Ana.velorasets: file made by a newer Velora Studio. Update the app."
**And** each item whose ID already exists on the unit shows Skip / Replace / Keep both (default Keep both), with "Apply to all" in the header

**Given** the user confirms
**When** the import runs
**Then** user timbres reach the unit only as `librarian` import commands (staged, verified, published per AD-11) and Live Sets only as control-thread commands, never by writing files on the unit
**And** Skip leaves the existing item untouched, Replace overwrites it, and Keep both adds the incoming item with a new ID (UUIDv4 for Live Sets, a new `library_id` for user timbres per AD-10) and the suffix " (2)", rewriting the incoming Live Sets to reference the kept copy; an automated test checks each choice against the expected device state

**Given** an incoming Live Set that references a factory or partner timbre not installed on the unit
**When** it is imported
**Then** the Live Set is imported anyway, the result lists "2 Live Sets need libraries: Grand Piano Studio (factory)", and on the unit those parts are silent and show Missing library with its Action Sheet (never a substitute timbre)
**And** installing that library later resolves the parts with no edit

**Given** a running import
**When** it progresses, completes, or the connection drops
**Then** the Footer shows "IMPORTING TO VELORA 73 · 4 of 9 items" while the unit keeps playing; on success a polite live region announces "Imported 7 items · 2 skipped"; on disconnect every finished item is kept and Studio shows "connection lost · 4 of 9 imported. Reconnect and import again; finished items are kept."

### Story 9.20: Send firmware from Velora Studio

As a keyboardist,
I want to send a firmware update from my computer to the Velora,
So that I can update without preparing a USB stick.

**Covers:** FR100, UX-DR77 (firmware send), UX-DR70 (partial: firmware)

**Runs on:** Dev board (Mac for UI against a mocked sysd)

**Acceptance Criteria:**

**Given** Device › Firmware in Studio
**When** the user picks a `.raucb` file
**Then** Studio shows the bundle's version (from its manifest) and the device's current version, and streams the file to `sysd` with progress in the Device view and Footer

**Given** the file arrived
**When** `sysd` stages and verifies it (the Story 8.1 installer path)
**Then** the device announces "1.3 ready to install" and the install follows the device flow (Action Sheet with `WRITE · Install`, progress, result); Studio never starts the install itself
**And** Studio shows "Installing on Velora 73 — sound is off on the device until it restarts", expects the connection to drop, and reconnects after restart showing the new version

**Given** a damaged or wrongly signed file
**When** `sysd` rejects it
**Then** Studio shows the device's failure message and nothing is installed

### Story 9.21: Device view and diagnostics export

As a keyboardist or support contact,
I want to see storage, firmware and backups for my unit and export its audio-health counters,
So that I can manage the unit and send useful information if something goes wrong.

**Covers:** FR120 (Velora Studio export), AR44 (partial: export via desktop app), UX-DR70

**Runs on:** Mac (against host engine); dev board

**Acceptance Criteria:**

**Given** a paired connection
**When** the user opens DEVICE
**Then** the view shows model, tier, firmware version and image variant, storage (free and used, per installed library with size), firmware (send update, Story 9.20), and backups (last backup, Backup, Restore)

**Given** the user clicks Export diagnostics
**When** Studio requests the ws.v1 Diagnostics response (from Story 1.21)
**Then** it saves a JSON file with xrun, steal and stream-underrun counters, the block-time and latency histograms, engine uptime, firmware version and device ID, to a user-chosen location
**And** the request is served by the control thread and never writes to the device's disk

**Given** the export file
**When** it is written
**Then** it contains no Live Set content, library audio or keys

### Story 9.22: Velora Studio accessibility audit

As a keyboardist who uses a keyboard, a screen reader or large text,
I want every part of Velora Studio to work without a mouse, be announced correctly, and stay readable at 200%,
So that I can use the app fully whatever my needs.

**Covers:** NFR35, NFR31 (Studio), UX-DR87 (Studio part), UX-DR90, UX-DR91, UX-DR92

**Runs on:** Mac (VoiceOver), Windows (NVDA), Linux (Orca, best effort)

**Acceptance Criteria:**

**Given** every Studio surface built in this epic
**When** a keyboard-only walkthrough runs (pair, select Live Set, edit a part, Write to Velora, transfer, backup, restore, partial export and import, send firmware, export diagnostics)
**Then** each task completes with keyboard only, Tab order follows visual order, F6 cycles sidebar → Live Set → scenes → parts → editor → footer, and the focus ring is never hidden

**Given** VoiceOver on macOS and NVDA on Windows
**When** the same walkthrough runs
**Then** every control has a name and role, Studio Knobs read name, value and unit, and connection, unsaved, write, transfer, backup, export and import results are announced through live regions without stealing focus

**Given** OS text size or app zoom at 200%, and OS Increase contrast on
**When** each surface renders at 1280×800
**Then** nothing scrolls horizontally, the Part Editor is one column, and hairlines switch to `line-strong`
**And** an automated contrast check over both themes reports every text pair ≥4.5:1 and every meaningful boundary ≥3:1

**Given** issues found
**When** the audit ends
**Then** each is fixed in this story or logged with its WCAG reference, and the checklist is committed as `desktop/ACCESSIBILITY.md` for re-use at each release

### Story 9.23: Edit zone, keyboard and arpeggiator from Velora Studio

As a keyboardist,
I want to change a part's key zone, octave, transpose and arpeggiator from the Part Editor,
So that the controls Story 9.9 shows read-only become editable and play on the unit immediately.

**Covers:** FR97 (partial: Part Editor zone and arpeggiator), FR98 (partial: live edit), UX-DR41 (partial)

**Runs on:** Mac (against host engine); dev board

**Blocked by:** Story 9.9 (done) and Story 9.10

**Acceptance Criteria:**

**Given** the Part Editor's zone, keyboard and arpeggiator controls
**When** the user edits them
**Then** Studio sends additive ws.v1 commands `SetPartZone` and `SetPartArp`, carrying `base_revision` and judged like other edits (stale edits refused with `STALE_REVISION`), applied by the control thread through the same path the device UI uses

**Given** the published state
**When** an edit is accepted
**Then** the `PartState` fields reflect it, ◆ and `UNSAVED: SCENE n` appear as in Story 9.10, and the Part Editor controls reflect the unit's value after a device-side change

**Given** the controls
**When** they are edited from the keyboard
**Then** they are operable like the other Studio Knobs (arrows, Home/End, typed value, Esc) with a polite live region, and the 'read-only' hints of Story 9.9 are removed

## Epic 10: Settings, languages and themes

A keyboardist sets the unit up for the room and the player: the Keyboard & Pedals rows (velocity curve, sustain and expression calibration, aftertouch) and MIDI & Clock rows, Stage (dark) or Studio (light) theme, screen and LED brightness with the EXIT + VALUE quick path, Large text and Reduced motion, and the UI language among English (default), PT-BR and ES, all in one coherent Settings surface. The epic consolidates Settings rows that earlier epics added (Knobs · Global Assign, Footswitch and the stage guards from Epic 4, Storage & Library from Epic 5, Clock source from Epic 7, System & Update and Boot into from Epic 8, Computer from Epic 9) into the canonical seven-section structure. Development is Mac-first: every device surface here runs in the LVGL SDL simulator on the Mac and Velora Studio runs natively; screen backlight and panel LED brightness are accepted on the prototype.

### Story 10.1: Settings surface with the seven canonical sections

As a keyboardist,
I want one Settings surface organised in clear sections that I can operate with VALUE and WRITE or by touch,
So that I find any device option quickly without a manual.

**Covers:** FR115 (structure), UX-DR65, UX-DR34, UX-DR19 (Settings Hint Bar), AR39 (partial: sysd config with `schema_version` for all rows)

**Runs on:** Mac (LVGL SDL simulator with host `sysd` config service); dev board

**Acceptance Criteria:**

**Given** the Launcher's Settings tile
**When** Settings opens
**Then** it uses the two-column list layout (Top Bar 56 · content 488 · Hint Bar 56): sections on the left in canonical order Display & Language, Keyboard & Pedals, MIDI & Clock, Knobs · Global Assign, Computer (Velora Studio), Storage & Library, System & Update; rows of the highlighted section on the right
**And** sections and rows added by earlier epics (Storage & Library, System & Update, Boot into, Computer, and any Keyboard & Pedals, MIDI & Clock and Global Assign rows) appear in their canonical section and position with no duplicate entry points

**Given** a highlighted row
**When** the user turns VALUE, presses WRITE or touches
**Then** VALUE moves the highlight and changes the value of an inline Segmented Control (`line-strong` outline, accent-filled active segment), WRITE confirms or opens a sub-list, EXIT goes one level back per the Navigation rules, and touch selects directly
**And** the Hint Bar lists only the active controls with English control names and localized verbs

**Given** a changed value
**When** it is confirmed
**Then** `ui` sends it to `sysd`, which persists it in the versioned config (atomic write) and publishes the change to `engine` and `ui`, and the setting applies immediately without restart
**And** changing settings never interrupts sound

**Given** a row that does not apply to this unit (its availability flag published by `engine` or `sysd` is false)
**When** its section renders
**Then** the row is hidden rather than shown disabled, and a section with no applicable rows is hidden

### Story 10.2: Settings › Keyboard & Pedals and MIDI & Clock rows

As a keyboardist,
I want to set my velocity curve, calibrate my sustain and expression pedals, use aftertouch when my keybed has it, and set the MIDI channel, thru and clock source on the instrument,
So that the keyboard, pedals and MIDI behave the way I play without a computer or a command line.

**Covers:** FR70 (Settings › Keyboard & Pedals › Velocity curve row), FR72 (Aftertouch row shown only on keybeds with aftertouch), FR42 (partial: pedal calibration from Settings), FR115 (Keyboard & Pedals and MIDI & Clock sections), UX-DR65 (partial: Keyboard & Pedals and MIDI & Clock sections), UX-DR34, UX-DR19, AR39 (partial: rows bound to the `sysd` keys from Stories 3.6, 3.11, 3.17, 4.36 and 7.7)

**Runs on:** Mac (LVGL SDL simulator with host `sysd` config, host engine and the virtual panel); prototype board (real keybed and pedals)

**Acceptance Criteria:**

**Given** Settings › Keyboard & Pedals
**When** it renders
**Then** it shows, in this order, Velocity curve, Aftertouch (only when applicable), Footswitch function (the Story 4.36 row, moved here unchanged), Sustain calibration and Expression calibration; pedal polarity has no row because it is detected at boot (Story 3.8)
**And** there is no Release velocity row: release velocity is always measured and applied (FR71, Story 3.6)

**Given** the Velocity curve row
**When** the player picks Light, Normal, Heavy or Fixed in its Segmented Control
**Then** the value is written to the Story 3.6 `sysd` key, the next note uses the new curve without restart, and the Story 3.6 test that the curve is applied exactly once still passes

**Given** the Sustain calibration row
**When** the player presses WRITE on it
**Then** a guided flow asks the player to press the sustain pedal fully and release it, runs the Story 3.11 calibration routine, shows the measured travel as a Progress Bar and stores min/max in the `sysd` config; EXIT cancels and keeps the previous calibration
**And** a half-pedal render test after calibrating through the UI matches the result of the CLI calibration

**Given** the Expression calibration row
**When** the player runs it the same way (heel down, then toe down)
**Then** the measured min/max are stored in `sysd` config keys this story adds, and the expression controller then spans its full range between them

**Given** `aftertouch_available` published by the engine (Story 3.7)
**When** it is true
**Then** an Aftertouch row (Off / On, default On) is shown, stored in a `sysd` key this story adds, and Off stops aftertouch events reaching the parts
**And** when it is false the row is hidden per the Story 10.1 rule, verified in the simulator with a keybed profile with and without aftertouch

**Given** Settings › MIDI & Clock
**When** it renders
**Then** it shows Clock source (the Story 7.7 row, moved here unchanged), MIDI out channel (1–16), Keybed to MIDI OUT (On / Off) and MIDI THRU (Off / On), bound to the Story 3.17 `sysd` keys, each change applying without interrupting sound
**And** a loopback test on the prototype shows notes on the chosen channel and DIN IN echoed to DIN OUT only with MIDI THRU On

**Given** every row of both sections
**When** its value changes
**Then** it is persisted and published by `sysd` per Story 10.1, and every label is a locale-catalog key that passes the 1024×600 layout at +40% pseudo-localized length

### Story 10.3: Theme selection on the device

As a keyboardist,
I want to switch the screen between the dark Stage theme and the light Studio theme,
So that the screen reads well both on a dark stage and in daylight.

**Covers:** FR118 (device), FR115 (Theme row), UX-DR2 (Studio theme in use on every device surface), UX-DR3 (per-theme drawbar edge rule), UX-DR87 (device and Studio screenshot review), NFR31 (verified on every implemented surface)

**Runs on:** Mac (LVGL SDL simulator, Velora Studio); prototype display for the final visual check

**Acceptance Criteria:**

**Given** Settings › Display & Language
**When** the user selects Theme Stage or Studio in the Segmented Control
**Then** every surface redraws in the chosen theme within one frame transition, the guidance text "Studio theme reads best in daylight; Stage theme in dark venues" shows under the row, and the choice persists across power cycles
**And** there is no automatic switching (the unit has no light sensor)

**Given** the Drawbar Columns
**When** they render
**Then** black heads carry the 2 px black edge in Stage and white heads the 2 px white edge in Studio, with slots on `surface-light` in Studio

**Given** the Story 2.18 token generator with the Velora Studio CSS output from Story 9.5
**When** the existing CI contrast gate from Story 2.18 runs
**Then** it covers both themes of every implemented device surface and Velora Studio, and this story adds no second contrast check
**And** a screenshot set of every implemented device surface in both themes is captured from the SDL simulator for review

### Story 10.4: Screen and panel LED brightness

As a keyboardist,
I want to set screen brightness from very dim to very bright and pick how bright the panel LEDs are,
So that the screen never blinds me in a dark venue and stays readable in sunlight.

**Covers:** FR115 (Screen brightness, Panel LED brightness rows), UX-DR83 (partial: LED brightness 3 steps + Follow screen), NFR36 (software side: dimming range)

**Runs on:** Mac (UI in the SDL simulator with a simulated backlight); prototype (backlight and LED PWM measured)

**Acceptance Criteria:**

**Given** Settings › Display & Language
**When** the user changes Screen brightness
**Then** it ranges 5–100% in 5% steps, applies live through the DRM/backlight interface owned by `sysd` (the `ui` requests, `sysd` applies), and the value is shown with CLDR formatting for the current locale (`65 %` in ES)

**Given** Panel LED brightness Low / Mid / High / Follow screen
**When** a value is chosen
**Then** `ui` sends an LED brightness command to `engine`, which scales the PWM of all panel LEDs it drives (strong and weak levels keep their ratio so weak stays distinguishable from strong and off)
**And** Follow screen maps the screen brightness to LED brightness through a fixed curve

**Given** the prototype
**When** brightness is measured
**Then** the screen at 5% is ≤5 cd/m² and at 100% meets the panel's rated luminance (≥1000 cd/m² per NFR36), and LED on:off contrast is recorded at each step; measured values are logged in a report for the hardware partner

### Story 10.5: Brightness quick path: hold EXIT and turn VALUE

As a keyboardist on stage,
I want to change screen brightness from any surface by holding EXIT and turning VALUE,
So that I fix glare in seconds without leaving the song.

**Covers:** FR116, UX-DR83 (quick path), UX-DR31 (brightness overlay in the Notice Toast), UX-DR79 (EXIT hold rule)

**Runs on:** Mac (SDL simulator with keyboard-mapped EXIT/VALUE); prototype (panel)

**Acceptance Criteria:**

**Given** any surface, including Performance and open dialogs
**When** the user holds EXIT and turns VALUE
**Then** screen brightness changes by 5% per detent within 5–100%, and the level shows in a Notice Toast ("Brightness 65 %") that never covers the Live Set Hero or Scene strip
**And** releasing EXIT after turning VALUE does not perform the back action (it is cancelled)

**Given** EXIT is pressed and released without turning VALUE
**When** it is released
**Then** the normal one-level-back action happens on release, unchanged

**Given** the ASSIGN Overlay is open
**When** the quick path is used
**Then** the brightness Toast waits per the Toast rules, while the brightness itself still changes immediately
**And** the new value is persisted by `sysd` and reflected in the Settings row

### Story 10.6: Large text on Performance and Reduced motion

As a keyboardist who reads the screen from a distance or is bothered by motion,
I want a larger-text Performance view and an option to reduce motion,
So that I can read my show and use the unit comfortably.

**Covers:** FR115 (Large text on Performance, Reduced motion rows), UX-DR6, UX-DR89, NFR33, NFR34

**Runs on:** Mac (LVGL SDL simulator); prototype for the 1.5 m legibility check

**Acceptance Criteria:**

**Given** Settings › Display & Language › Large text on Performance On
**When** Performance renders
**Then** it uses the Large-text token variants (≈ +25%: 66 / 28 / 20 / 21 / 18 / 30 px), drops Scene Tile sub-labels and Part Strip categories, keeps timbre and Live Set names to one line with end ellipsis, and keeps the screen skeleton (bar heights) unchanged
**And** the LVGL fonts for these sizes are built with ascent measured from Á/Ç/Ñ/Ê so accented capitals do not clip

**Given** Large text in DRAWBARS mode, with pickup pending, missing library and clock lost states
**When** each state renders
**Then** nothing overlaps or clips, verified by screenshot tests in the SDL simulator

**Given** Reduced motion On
**When** any transition would play
**Then** cross-fades last ≤80 ms or none, the Notice Toast appears in place, the scene-recall LED dip becomes an instant change, and the boot animation shows its static frame (this row exposes the `reduced_motion` key created in Story 8.7, which the Boot Screen reads)
**And** with Reduced motion Off, transitions stay ≤150 ms and nothing animates in the Live Set Hero

### Story 10.7: PT-BR and ES locale catalogs and language selection

As a keyboardist who speaks Portuguese or Spanish,
I want to use the Velora and Velora Studio in my language,
So that every message on stage and at home is clear to me.

**Covers:** FR117, NFR30, AR45, UX-DR93, UX-DR94, UX-DR95 (CLDR formatting), UX-DR96, UX-DR35 (locale keyboard layout follows language)

**Runs on:** Mac (SDL simulator and Velora Studio)

**Acceptance Criteria:**

**Given** the shared locale-catalog format used by `ui` and `desktop` with EN as source
**When** PT-BR and ES catalogs are added
**Then** every EN key has a PT-BR and ES entry, a CI check fails on a missing key, an unused key, or a hard-coded user-facing string in `ui` or `desktop` (lint over string literals in UI code)
**And** the 17-category label table provides full and ≤10-character short labels in EN, PT-BR and ES, with the short-label length checked in CI

**Given** Settings › Display & Language › Language
**When** the user selects English, Português (Brasil) or Español
**Then** all device surfaces switch language live, user content (setlist, Live Set, scene and tag names) stays as typed, panel legends and Hint Bar control names stay English while verbs translate, and the On-screen Keyboard offers the matching locale layout by default
**And** numbers and units follow CLDR for the locale (`18.4 GB` EN, `18,4 GB` PT-BR, `65 %` ES)

**Given** Velora Studio
**When** it starts
**Then** it follows the OS language when PT-BR or ES is the OS language, falls back to EN otherwise, and its language override offers the same three languages

**Given** a future locale
**When** a system update adds a catalog file for it
**Then** the language appears in the Language row with no code change (catalogs are discovered from the catalog directory at start), proven by a test catalog added only as a data file

### Story 10.8: Pseudo-localization QA and truncation rules

As a keyboardist using any language,
I want every screen to fit longer translations without clipped or overlapping text,
So that the unit looks finished and reads correctly in Portuguese and Spanish.

**Covers:** UX-DR97, UX-DR95 (truncation rules), UX-DR5 (font ascent check), UX-DR98 (microcopy review)

**Runs on:** Mac (SDL simulator and Velora Studio; CI screenshot job)

**Acceptance Criteria:**

**Given** a pseudo-locale available in dev builds of `ui` and `desktop`
**When** it is selected
**Then** every string is expanded by +40%, wrapped in markers, with accented capitals substituted (e.g. "[Ŵŕîţé çĥåñĝéš?·····]"), and a test user-content set uses the clipping string `ÁÇÊÕÑ Ação`

**Given** the CI screenshot job
**When** it renders every implemented device surface and state in the SDL simulator, and every Velora Studio surface, in the pseudo-locale, both themes and Large text on/off
**Then** an automated check flags any text clipped by its container or overlapping another element, and the job fails on any flag
**And** the screenshots are attached to the CI run for human review

**Given** the truncation rules
**When** long content renders
**Then** the Top Bar setlist context ellipsizes in the middle, timbre names end-ellipsize at 2 lines on Part Strips (1 in Large text), the Live Set name is 1 line with end ellipsis, Part Strips use short category labels, and the Status Bar shows only the takeover line when one is active, each covered by a unit test

**Given** the EN, PT-BR and ES catalogs
**When** a microcopy review runs against the Voice and Tone rules
**Then** every string uses the fixed vocabulary (Setlist, Live Set, Scene, Part, Timbre, Library, Category, Write, ROTARY), names control, direction and target where relevant, and contains no third-party trademark; fixes are committed and the checklist is kept for future reviews

## Epic 11: Pre-launch: brand, certification and suppliers

The business is ready to launch the Ladiani Velora 73: the brand is cleared and filed, domains and handles are held, the price is validated against taxes and real market data, suppliers have committed in writing (keybed, SoM, panel, display, SSD, PSU), the reference material for organ acceptance exists, the final brand assets are delivered, and the direct-sales channel for the first ~100 units works. Apart from the closing Story 11.19, which integrates the delivered typeface, app icon and boot artwork into the device and Velora Studio builds (after Stories 11.15 and 11.16), none of these stories produce firmware; each ends in a verifiable business artifact (a report, a signed document, a filed application, a measured result) stored under `_bmad-output/business/` (confidential items stored outside the repository, with only a pointer and status in the repository). Stories are ordered so each can finish using only earlier ones. Epic 11 runs in parallel with the software epics from month 0 and gates the stories elsewhere that carry a **Blocked by:** line: Story 11.12 by month 5 (1.13, 5.4), Stories 11.6 and 11.7 by month 7 (3.2, 3.3, 3.4, 3.7, 3.8, 3.9, 6.16), Story 11.13 by month 8 (2.13), Story 11.8 by month 9 (3.8, 3.9), Story 11.9 by month 10 (3.13, 3.18, 3.20), Story 11.14 by month 11 (2.14) and Story 11.17 by month 12 (6.9). Deadlines are expressed against the GTM timeline (Milestone 1 month 6, first playable units month 12, v1 launch month 18).

### Story 11.1: Trademark clearance and INPI filing for Ladiani and Velora

As the founder,
I want a formal trademark clearance for "Ladiani" and "Velora" and INPI filings in Nice classes 15 and 9,
So that no branding money is spent on a name we cannot keep, and the brand is protected in Brazil before it appears in public.

**Covers:** EXT8 (trademark), NFR29, FR121 (compatibility wording)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the preliminary WIPO screening of October 2026 (no conflicting marks in classes 15 and 9)
**When** a Brazilian trademark attorney runs a formal clearance search on INPI, WIPO Global Brand Database and common-law use for "Ladiani", "Velora", "Ladiani Velora" and "Velora Studio" in classes 15 (musical instruments) and 9 (software, audio interfaces, downloadable content)
**Then** a written clearance opinion is stored stating, per mark and class, clear / risky / blocked with the conflicting marks cited
**And** if any mark is risky or blocked, the opinion names the fallback (different mark, figurative mark, or class narrowing) and the founder records a go/no-go decision before any further branding spend (domains, handles, designer)

**Given** a clear or accepted-risk opinion
**When** the attorney files at INPI
**Then** filings exist for "Ladiani" (word mark) and "Velora" (word mark) in classes 15 and 9, owned by the founder's company, with the INPI process numbers and filing receipts (GRU payment proof) stored
**And** the decision on whether to also file "Velora Studio" and the figurative mark (once a final logo exists) is recorded with its reason

**Given** INPI filings exist
**When** the export plan is reviewed
**Then** a dated Madrid Protocol plan is stored naming target territories (at minimum the ones considered for export after Brazil), estimated fees, and the trigger for filing (within the 6-month priority window from the INPI filing date, or a written decision to let it lapse)
**And** a calendar of INPI deadlines (publication, opposition period, office actions, grant fee) has an owner and reminder dates

**Given** the post-launch 6-operator FM engine and its SysEx import (FR121)
**When** the attorney reviews how compatibility may be stated
**Then** a written opinion gives the allowed compatibility wording for marketing and documentation (nominative use of third-party product names, with the required disclaimer), and the places where it may appear
**And** the device UI, Velora Studio and timbre names stay generic ("6-operator FM", "Import 6-OP SysEx") regardless of the opinion (NFR29)

### Story 11.2: Register domains and social handles

As the founder,
I want the Ladiani domains and social handles registered in the company's name,
So that nobody squats the brand online once the name becomes public.

**Covers:** EXT8 (domains and handles)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the go decision from Story 11.1
**When** the domains are registered
**Then** ladiani.com and ladiani.com.br (Registro.br, under the company CNPJ) are registered for at least 2 years with auto-renew on, and the decision on .net, .io, .music and .audio (register or skip, with cost) is recorded
**And** the registrar accounts use the company e-mail, 2FA and a recorded recovery path; registrant data is the company's, not a personal one

**Given** the handles `@ladianimusic` and `@ladiani.instruments` were free in October 2026
**When** they are claimed on Instagram and YouTube (and the decision on TikTok, Facebook and WhatsApp Business is recorded)
**Then** each claimed handle is listed with platform, URL, owner account and 2FA status
**And** if a handle was lost since screening, the replacement handle chosen is recorded and used consistently

**Given** the brand is not yet launched
**When** the domains and handles go live
**Then** they show only a holding page / placeholder profile with no product claims, no third-party trademarks (NFR29) and no personal data collection until an LGPD consent text and privacy notice exist

### Story 11.3: Run the Brazil keyboardist survey

As the founder,
I want the Brazil keyboardist survey run with at least 400 valid responses under LGPD consent,
So that price and feature priorities rest on what Brazilian keyboardists actually say, not on our assumptions.

**Covers:** EXT9 (run the survey), NFR47

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the survey design in `_bmad-output/research/pesquisa-tecladistas-brasil.md`
**When** the Google Form is built
**Then** it has the 9 sections and ~30 questions with the branching on questions 12 and 28, required questions 6, 7, 13, 14, 15, 17, 18 and 22–25, "select exactly 3" validation on question 18, numeric validation on 22–25, and automatic e-mail collection turned off
**And** the form never mentions "Ladiani" or "Velora" (only "um novo teclado de palco")

**Given** personal data (e-mail, WhatsApp) is collected only on question 28a/28b
**When** the form is published
**Then** question 29 (LGPD consent) is required whenever an e-mail is given, the consent text states the only purposes (contact about this survey and the launch), the controller's identity and how to withdraw consent
**And** the response sheet keeps contact data in a separate, access-restricted sheet from the anonymous answers, with a stored retention/deletion rule

**Given** the built form
**When** a pilot runs with 10 keyboardists
**Then** median completion time is recorded and is ≤10 minutes (otherwise Section 1 questions are cut, per the design) and pilot feedback and changes are logged
**And** pilot answers are excluded from the final dataset

**Given** the final form and the distribution plan (WhatsApp/Facebook groups, church and event musicians, teachers, shops, influencers, prize draw)
**When** collection closes
**Then** there are ≥400 valid responses after removing duplicates, test entries and inconsistent Van Westendorp answers (e.g. "too cheap" > "too expensive"), with the cleaning rules written down
**And** the distribution by region and usage context (church, band, events) is reported, with any under-represented group and the extra outreach done for it

**Given** the clean dataset
**When** the results report is written in `_bmad-output/research/`
**Then** it contains: brand/model ranking (Q6–8), NPS per brand (Q13), top pains and missing features in the musicians' words (Q14–16), feature importance and top-3 ranking (Q17–18), switching intent and barriers (Q20–21), Van Westendorp curves with acceptable range and optimal price point (Q22–25), and payment/instalment preference (Q26–27)
**And** the prize draw is run and its winner contacted only through the consented channel

### Story 11.4: Tax advisor validation of margin at R$ 17,990–19,990

As the founder,
I want a tax advisor to validate the landed cost, tax load and margin of the v1 at R$ 17,990–19,990,
So that the launch price is profitable under the real Brazilian tax structure.

**Covers:** EXT1, NFR48

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the BOM estimate (US$ 1,200–1,600), the imported vs locally sourced part list, the planned company tax regime and the direct-sales channel
**When** a tax advisor models the cost
**Then** a written report states per unit: import duties (II), IPI, ICMS (including the state of manufacture and interstate sale rules), PIS/COFINS, applicable ex-tarifário reductions on imported parts (SoM, keybed, display), freight, customs brokerage and assembly cost, giving a verified landed cost that replaces the unverified R$ 11,000 estimate
**And** the report states the assumptions (exchange rate, volume of ~100 units, tax regime) and how sensitive the result is to each

**Given** the verified landed cost
**When** margin is computed
**Then** the report gives the gross margin at R$ 17,990, R$ 18,990 and R$ 19,990 for direct sale, both paid in full (Pix/boleto) and in 10 interest-free instalments (card fees included), and for a future dealer sale (~30% dealer margin)
**And** the founder records whether the R$ 17,990–19,990 range holds, with the minimum viable price if it does not

**Given** the Pro tier at R$ 24,990+
**When** the same model is applied to the 32 GB SoM and Pro additions
**Then** the Pro margin at R$ 24,990 is stated as well

### Story 11.5: Price and feature-priority decision from survey and tax results

As the founder,
I want one decision document that sets the launch price and the feature priorities from the survey and the tax report,
So that the spec, the epics and the marketing all use the same, evidence-based numbers.

**Covers:** EXT9 (feed results into pricing and feature priority), EXT1 (decision), NFR48, NFR51

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the survey report (Story 11.3) and the tax report (Story 11.4)
**When** the decision document is written
**Then** it compares the Van Westendorp acceptable range and optimal price with R$ 17,990–19,990 and the minimum viable price, and states the chosen launch price (or range), the instalment offer and the reasoning
**And** if the chosen price is outside the current range, `go-to-market.md` and NFR48 are updated through the change process (bmad-correct-course), not edited silently

**Given** feature importance and top-3 results
**When** they are mapped to the epics
**Then** the document lists each surveyed feature with its importance and top-3 share, the epic that delivers it, and a keep / raise priority / lower priority recommendation
**And** any recommendation that changes release scope (for example raising "new engines by free update" ahead of launch, which is Epic 12) is raised as a proposed change, keeping NFR51 (date-driven release: samples, organ, effects) unless the founder decides otherwise

**Given** switching barriers (Q21)
**When** they are reviewed
**Then** each of the top 3 barriers (e.g. new brand, resale value, local service) has a named answer in the launch plan (warranty, local service network, demo units, trial events) with an owner

### Story 11.6: Fatar OEM resupply terms and 73SW contact and aftertouch confirmation

As the founder,
I want written Fatar OEM resupply terms and a confirmed contact count and aftertouch status for the 73SW stock,
So that production can continue past the first ~100 keybeds and the keybed scan and calibration are built for the real part.

**Covers:** EXT2, EXT10 (item 2), NFR42

**Runs on:** Business (no device); contact check on a sample keybed

**Acceptance Criteria:**

**Given** the partner's stock of ~100 Fatar 73SW keybeds
**When** Fatar (or its authorised distributor) is contacted for OEM supply
**Then** written terms are stored stating minimum order quantity, unit price at that quantity, lead time, payment terms, model continuity / last-time-buy notice policy, and whether the waterfall E-to-E 73SW variant is available
**And** if Fatar declines or the terms break the margin from Story 11.4, an alternative keybed (model and supplier) is named with the impact on FR73 (calibration table, no code change expected)

**Given** a sample 73SW from the stock
**When** the hardware partner inspects and measures it (datasheet plus a scope on the contact lines)
**Then** a short report states the contact count per key (2 or 3), the presence or absence of an aftertouch sensor strip, the connector pinout and the matrix layout, with photos
**And** the result is sent to the software team as input to the keybed MCU firmware (AR40) and the keybed calibration table (FR72, FR73)

### Story 11.7: SoM longevity commitment and board-level partner deliverables

As the founder,
I want the SoM vendor's written longevity commitment and the board-level decisions from the hardware partner,
So that the product can be built for 7–10 years and the software team builds against named parts.

**Covers:** EXT10 (items 1, 3; tier-ID strap, serial label and EEPROM, factory programming fixture, codec-loopback test points), NFR38, NFR41

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the requirement for an industrial RK3588 SoM (16 GB v1, 32 GB Pro, pin-compatible, no radio)
**When** the SoM is selected with the hardware partner
**Then** the vendor, part numbers (v1 and Pro) and a signed letter committing to 7–10 years of supply are stored
**And** the record confirms the SoM variant has no Wi-Fi/Bluetooth and the BSP supports mainline Linux 6.18 LTS for audio, USB, PCIe and display paths (HW-§2), or lists exceptions for the software team

**Given** the carrier board design
**When** the partner delivers the board-level decisions
**Then** a document names the MCU family and bootloader approach for both MCUs (due by month 7), the tier-ID strap (GPIO or EEPROM) and how it reads v1 vs Pro, the serial number label and EEPROM location, the factory programming fixture plan for secure-boot fusing, and the codec-loopback test points or fixture
**And** each item is marked delivered / pending with a date, and pending items past their due date are escalated

### Story 11.8: Panel, display, LED and legend specifications agreed with partners

As the founder,
I want the physical panel, display, LED and legend specifications agreed and measured with the hardware partners,
So that the panel is legible on bright and dark stages and matches the UX design.

**Covers:** EXT10 (items 4 and 8), EXT12, UX-DR4, UX-DR53 (fader caps and track), UX-DR113, NFR36, NFR43

**Runs on:** Business (no device); measurements on partner samples

**Acceptance Criteria:**

**Given** the panel layout A render package and render-reference tokens (DESIGN.md, UX-DR4)
**When** it is handed to the hardware partners
**Then** a signed-off panel specification records: legend method (engraving vs silkscreen) and ink, legend cap height ≥2.5 mm on a matte or bead-blasted band at ≥5:1 against the darkest aluminum, aluminum finish that hides wear, LED colours, and the MASTER control type (knob or fader) with the reason
**And** it records the tactile cues: WRITE concave cap or raised ring, MENU centre dot, VALUE encoder ≥30 mm knurled **with a push switch** reported as a `BUTTON` in the panel MCU frame, pointer lines on parameter encoders, dark non-motorized fader caps on a grey track with a distinct REV fader cap, nubs on part/scene buttons 1 and 5, toggle buttons taller than momentary buttons
**And** the dark-stage plan is chosen (tactile cues only, backlit navigation column and toggles, or a USB-A lamp port)

**Given** partner display samples
**When** the display is measured
**Then** a report gives the DSI panel and touch controller part numbers (touch controller with a mainline Linux driver), native 1024×600, measured luminance range (≥1000 cd/m² max, ≤5 cd/m² min) and cover-glass reflectance (≤2%, optically bonded AG/AR)

**Given** partner encoder and LED samples
**When** the LEDs are measured at 50 klx ambient
**Then** a report gives encoder and LED part numbers and the measured on:off contrast (≥10:1), the ~15% PWM weak level, and the three user brightness steps
**And** any measured value that misses its floor has a named corrective action before first playable units (month 12)

### Story 11.9: Power-fail timing, codec and thermal measurements from the partner

As the founder,
I want the partner's measured power-fail timing, codec performance and thermal results,
So that the power-loss safety and no-throttling claims rest on measurements of our board.

**Covers:** EXT10 (items 5, 6, 7), NFR11, NFR18

**Runs on:** Business (no device); measurements on the prototype board

**Acceptance Criteria:**

**Given** a prototype carrier board with the supercapacitor hold-up circuit
**When** input power is cut 20 times at full load (3 A76 cores busy, NVMe writing)
**Then** a report gives the measured time from input loss to `POWER_FAIL` assertion and from `POWER_FAIL` to rail drop, with min/max over the 20 cuts
**And** hold-up is ≥150 ms and `POWER_FAIL` is asserted ≥100 ms before rails drop in every cut, or the report names the redesign

**Given** the codec reference design on the board
**When** the partner measures it
**Then** a report gives output noise floor, THD+N and dynamic range on main, aux and headphone outputs, and the codec's analog in-to-out latency, with the measurement setup

**Given** the board in the closed production chassis at 40 °C ambient
**When** a 24 h full-load thermal test runs
**Then** the report shows SoC and NVMe temperatures over time and confirms no thermal throttling occurred (CPU frequency logged), or names the thermal change required

### Story 11.10: Inmetro power supply requirement and IEC 62368-1 test plan

As the founder,
I want to know exactly which Inmetro requirement applies to the power supply, a compliant off-the-shelf PSU selected, and an IEC 62368-1 safety/EMC test plan,
So that the product can be sold legally in Brazil and later certified for CE/FCC without redesign.

**Covers:** EXT3, NFR45, NFR46, NFR41

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the planned external (or internal) power supply topology
**When** a certification consultant or OCP (Inmetro-accredited body) is consulted
**Then** a written answer states which Inmetro ordinance applies to the PSU and whether the keyboard itself needs Inmetro certification, with the ordinance number and date
**And** it confirms that no Anatel homologation is needed because v1 has no radio (NFR41)

**Given** the applicable requirement
**When** the PSU is selected
**Then** the chosen off-the-shelf PSU's model, ratings (meeting the board's load plus hold-up charge), Inmetro registration number or certificate and IEC 62368-1 certificate are stored, plus one alternative PSU

**Given** the board and chassis design
**When** the test plan is written with an accredited lab
**Then** it lists IEC 62368-1 safety tests and the EMC emission/immunity standards to be run, the samples required, the lab, cost and schedule, scheduled so results arrive before the v1 launch (month 18)
**And** pre-compliance EMC scans on the prototype are scheduled before the first playable units (month 12)

### Story 11.11: SSD qualification for streaming latency and endurance

As the founder,
I want at least one M.2 NVMe SSD (preferably two) qualified on measured p99.99 read latency and endurance,
So that the zero-dropout promise holds on the SSD we actually ship and we are not locked to one model.

**Covers:** EXT11, NFR4, AR20 (head sizing input)

**Runs on:** Dev board (measurements); approval is a business decision

**Acceptance Criteria:**

**Given** at least three candidate M.2 2280 PCIe Gen3 x4 SSDs (1 TB) available in Brazil
**When** each is tested on the RK3588 dev board with the streaming load profile (io_uring + O_DIRECT, 256 voices of reads, SSD filled to ≥90%) for ≥12 h
**Then** a report gives per SSD the read-latency histogram with p99, p99.9, p99.99 and max, measured at drive temperature under the closed-chassis thermal condition
**And** the head size each SSD would require (p99.99 plus margin, AR20) is stated and checked against the v1 head budget (~4–6 GB)

**Given** the endurance requirement (imports and occasional writes over the product life)
**When** endurance is assessed
**Then** the report states each SSD's rated TBW and power-loss behaviour (flush on `POWER_FAIL`), and the expected writes per year of a heavy user

**Given** the results
**When** the approval decision is recorded
**Then** at least one SSD is approved with model, firmware version and supplier, a second approved or named as the next to test, and the minimum spec that any future SSD must meet to be accepted

### Story 11.12: License check of the vendored sfizz parser and dependencies

As the founder,
I want a written license check of the sfizz parser parts we vendor and every dependency they pull in,
So that the closed product ships no code whose license forbids it.

**Covers:** EXT5, NFR27, NFR28, AR27 (vendored sfizz parser)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** sfizz 1.2.3 (archived, BSD-2) and the exact files vendored into `formatlib`
**When** each vendored file and each transitive dependency (e.g. abseil, ghc filesystem, any bundled third-party code) is inventoried
**Then** a license table is stored listing file/dependency, version, license, copyright holder, and whether its obligations (attribution notice, no endorsement) are met
**And** no GPL-3.0/LGPL-3.0 or other copyleft code is included in the vendored set, or the offending part is removed before Story 1.13's parser is shipped

**Given** the license table
**When** attribution obligations are collected
**Then** the required BSD/MIT notices are added to the product's third-party notices file shipped in the image and shown in Velora Studio's About (content delivered in `THIRD_PARTY_NOTICES.md`; the image build and the Studio About screen must install and display it)
**And** sfizz used as the reference renderer in CI remains test-only and never linked into a shipped target (checked against the build license manifest)

### Story 11.13: Hammond M-solo drawbar CC/NRPN numbers for automated organ A/B

As the founder,
I want the Hammond M-solo's drawbar and organ-control MIDI CC/NRPN numbers transcribed and verified,
So that the automated organ A/B renders can set identical registrations on the reference and on the Velora.

**Covers:** EXT6, NFR20

**Runs on:** Business (no device); verification with the M-solo

**Acceptance Criteria:**

**Given** the M-solo manual's Drawbar Data List (pp. 64–65, an image only)
**When** it is transcribed
**Then** a machine-readable mapping file is committed under `tools/organ-ab/` listing, for each of the 9 upper drawbars, percussion (on, volume, decay, harmonic), vibrato/chorus, key click, leakage and rotary slow/fast/stop: the CC or NRPN number, value range and value-to-position mapping
**And** the source (manual edition, page) is cited in the file

**Given** an M-solo connected to a computer
**When** a script sends each mapped message across its range and records the audio
**Then** every mapping is confirmed by audible/spectral change on the expected control, mismatches are corrected, and the verification log is stored
**And** a full registration (e.g. `88 8000 000` with percussion on) can be set by script and rendered without manual touch

### Story 11.14: Tonewheel B-3 and rotary cabinet reference recordings and ABX panel

As the founder,
I want reference recordings of a real tonewheel B-3 through a real rotary speaker cabinet and a recruited panel of keyboardists,
So that the organ can be judged against the real instrument, objectively and in blind listening.

**Covers:** EXT7, NFR20, NFR21

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** access to a maintained B-3 (or other tonewheel console) with a rotary cabinet in a studio
**When** the recording session runs
**Then** recordings exist at 48 kHz/24-bit or higher of a written test script: single notes across the range on each drawbar alone, standard registrations, percussion variants, key click, vibrato/chorus modes, and rotary slow, fast and ramps, each with the registration and the MIDI or manual note list logged
**And** the cabinet is captured with the same mic setup throughout, documented with photos and positions, and the recordings and their usage rights are stored

**Given** the ABX test needs 3–5 professional keyboardists
**When** the panel is recruited
**Then** 3–5 named keyboardists with organ experience have agreed in writing to take part (date window around first playable units, compensation, consent to use anonymised results)
**And** contact data is kept under the same LGPD rules as Story 11.3

### Story 11.15: Final typeface licensed and delivered by a designer

As the founder,
I want a type designer to deliver the final typeface (custom or licensed) meeting the UX acceptance criteria and licensed for LVGL embedding and desktop,
So that the stand-in fonts on the device, Velora Studio and the panel can be replaced by our own face.

**Covers:** EXT13 (typeface), UX-DR110 (handoff part)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** a commissioned designer and a signed brief stating the acceptance criteria from DESIGN.md › Deferred
**When** the typeface is delivered
**Then** a checklist signed by the founder confirms: distinct I / l / 1 and 0 / O; x-height ≥0.5 em; open apertures; tabular numerals and ⅓ ⅔ ⅗ glyphs; weights ≥400 for device UI text (300 allowed only for the wordmark); full Latin-1 + Latin Extended-A with accented capitals (Á Ç Ê Õ Ñ) fitting a 1.2 line box
**And** the font files (OTF/TTF, and variable font if provided) are stored with version numbers

**Given** the delivered font
**When** its license is reviewed
**Then** the written license permits embedding as rasterised/subsetted glyphs in the LVGL firmware on every shipped unit (unlimited units), bundling in the Velora Studio desktop app on macOS/Windows/Linux, use on the printed/engraved panel and in marketing, without per-unit royalties
**And** the license is stored and the font is added to the third-party/asset notices if required

### Story 11.16: Final logo, symbol, app icon and avatar delivered by a designer

As the founder,
I want the final symbol, wordmark, model mark, panel lockup, boot artwork, desktop app icon and social avatar delivered by a designer,
So that the product, app and channels carry one finished brand.

**Covers:** EXT13 (logo, app icon), UX-DR111, UX-DR112 (delivery part)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the design direction in DESIGN.md (symbol S1 drawbar "L", wordmark LADIANI T3, model mark VELORA V1) and the trademark result of Story 11.1
**When** the designer delivers
**Then** vector files (SVG/PDF) exist for the symbol (with simplified versions that read at 32 px and 16 px), the LADIANI wordmark, the VELORA model mark, the panel lockup in the panel ink and legend colours, and boot-screen artwork with the drawbar pull-out animation frames at 1024×600
**And** the founder's sign-off confirms each works engraved or silkscreened on aluminum, on the dark boot screen, as a tiny icon and as a round avatar

**Given** the symbol
**When** the app icon and avatar are delivered
**Then** the desktop app icon set (macOS .icns sizes, Windows .ico sizes, Linux PNG sizes) and social avatars (round crop) for the handles of Story 11.2 are stored
**And** the copyright in all delivered artwork is assigned to the company in writing, and the decision to file the figurative mark at INPI (Story 11.1) is recorded

### Story 11.17: Factory library content plan and source delivery

As the founder,
I want the factory library content defined, produced by me with clear rights, and delivered as source to the content-pack tool,
So that every unit ships with the factory piano, organ presets and other categories needed for the v1 success signal.

**Covers:** EXT14, FR34 (source content only), NFR29

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the 17 categories and the v1 success signal (factory piano, organ presets)
**When** the content plan is written
**Then** it lists each planned instrument or preset with category, source (own recording, own sampling, own programming), size estimate on disk and target delivery month, totalling a size that fits the v1 NVMe with room for user imports
**And** it states a minimum shippable set (at least the factory piano and the organ presets) that must be ready by first playable units (month 12)

**Given** produced content
**When** each item is delivered
**Then** it arrives as SFZ + WAV (lossless, no resampling) with names and categories, plus a rights record stating it is the company's own work (recording dates, instrument owner permission, no third-party samples or trademarks in names)
**And** the content is handed to the content-pack tool owner (Epic 6) with the vendor-assigned library and program IDs reserved

### Story 11.18: Direct-sales channel for the first ~100 units

As the founder,
I want a working direct-sales channel in Brazil (storefront, payment, invoicing, shipping, warranty and service),
So that the first ~100 units reach musicians without a dealer and with the after-sales trust a new brand needs.

**Covers:** EXT15, NFR47, NFR52 (no retail for the first ~100 units)

**Runs on:** Business (no device)

**Acceptance Criteria:**

**Given** the price decision (Story 11.5) and the domain (Story 11.2)
**When** the storefront is set up
**Then** a storefront on ladiani.com.br takes orders and pre-orders with the chosen payment options (Pix, boleto, card in instalments as decided), issues NF-e for each sale under the company's tax registration, and publishes terms of sale, a privacy policy (LGPD) and the right of withdrawal (7 days, CDC art. 49)
**And** a test order is placed, paid, invoiced and refunded end to end before launch

**Given** a ~20 kg instrument shipped across Brazil
**When** logistics are set up
**Then** a carrier with insured door-to-door delivery is contracted, the shipping box passes a drop test with a packed unit, and shipping cost and lead time per region are published at checkout

**Given** a new brand's biggest barrier is trust (survey Q21)
**When** warranty and service are defined
**Then** written warranty terms (legal 90 days plus the chosen contractual warranty), an RMA process (contact, diagnosis, pickup, repair or swap, SoM-swap re-provisioning per AR33) and a spare-parts stock list (keybed, panel boards, PSU, display) for ~100 units are stored
**And** support contact (e-mail and WhatsApp Business) is live with a stated response time

**Given** the survey's waitlist e-mails (consented under Story 11.3)
**When** pre-orders open
**Then** only contacts who consented to launch communication are notified, with an unsubscribe link in every message

### Story 11.19: Brand assets integration: final typeface, app icon and boot artwork

As a keyboardist,
I want the finished Ladiani typeface, app icon and boot artwork on the unit and in Velora Studio,
So that the instrument and its app look like one finished product instead of a prototype with stand-ins.

**Covers:** UX-DR110 (integration), UX-DR111 (integration), UX-DR112 (integration), UX-DR5 (font ascent recheck), UX-DR97 (recheck), UX-DR89 (static boot frame)

**Runs on:** Mac (LVGL SDL simulator and Velora Studio builds) and dev board

**Acceptance Criteria:**

**Given** the licensed typeface delivered in Story 11.15 and the stand-in fonts used since Epic 2
**When** the fonts are swapped in the LVGL font build and in Velora Studio
**Then** the LVGL glyph subsets (Latin-1 + Latin Extended-A, tabular numerals, ⅓ ⅔ ⅗) are regenerated from the licensed files at every token size, weights ≥400 are used for device UI text (300 only for the wordmark), and Studio bundles the licensed font files with the license notice
**And** the type tokens (sizes and line-heights) are regenerated from the new face's metrics and the change is recorded in DESIGN.md

**Given** the new face
**When** the Story 10.8 pseudo-localization screenshot job runs over every device and Studio surface in both themes and Large text on/off, with the clipping string `ÁÇÊÕÑ Ação`
**Then** no text is clipped or overlapping, accented capitals fit the 1.2 line box, and the job fails on any flag
**And** on the dev board the font atlas stays within the UI memory budget and screen redraws keep their previous timing

**Given** the app icon set delivered in Story 11.16
**When** Velora Studio is built for macOS, Windows and Linux
**Then** each build carries the delivered icon set (.icns, .ico, PNG sizes) and no stand-in icon remains in the repository's build inputs

**Given** the boot artwork and animation frames delivered in Story 11.16
**When** the Boot Screen from Epic 8 is built
**Then** it plays the drawbar animation frames from the delivered symbol at 1024×600 within the existing boot timing, and with Reduced motion On it shows the delivered static frame
**And** the stand-in boot wordmark is removed and the boot-time budget of Story 8.10 is rechecked on the dev board

## Epic 12: FM and VA engines (post-launch)

A keyboardist with a shipped v1 unit receives a clean-room 6-operator FM engine (≥128 voices) and a clean-room virtual-analog engine (≥64 voices) by firmware update. Both run inside the shared voice pool behind the single engine interface (AD-7, AD-22), store their timbres in the existing versioned formats without migration (FR14, AD-12), appear in the Library by category (never by engine type), and are edited in the Part surface and the Velora Studio Part Editor. The FM engine is parameter-compatible with the classic 6-operator FM SysEx format, so players import their own SysEx banks and single voices from USB-A, Velora Studio or over MIDI (FR121); no third-party patches ship. Development is Mac-first: engine DSP is built and tested in `engine/core` on the developer's Mac and in the offline renderer; voice budgets are accepted only on the target hardware. Marketing, UI and panel names stay generic ("6-operator FM", "virtual analog"); no "DX7" or other third-party trademark anywhere (NFR29). GPL references (Dexed/MSFA) may be used only as unshipped black-box test oracles (NFR28).

### Story 12.1: Clean-room 6-operator FM voice in the engine core

As a keyboardist,
I want a 6-operator FM engine that plays classic FM sounds (electric pianos, bells, basses, brass) with velocity and envelopes,
So that the Velora covers the FM sounds every stage keyboard is expected to have.

**Covers:** FR12 (engine DSP), FR121 (partial: parameter compatibility), NFR28, NFR29, AR15 (FM engine implementation)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the clean-room rule (NFR28)
**When** the FM engine is started
**Then** a clean-room record in `engine/core/engines/fm/CLEAN-ROOM.md` lists the sources used (published FM synthesis literature, expired patents, generic textbooks) and states that no GPL FM source code (Dexed, MSFA or derivatives) was read, copied or given to the implementing agent as context
**And** the record is reviewed before merge and kept with the code

**Given** the single engine interface (AD-7)
**When** the FM engine is implemented in `engine/core/engines/fm/`
**Then** each voice has 6 sine operators with per-operator frequency ratio or fixed frequency, fine tune and detune, output level, velocity sensitivity, keyboard level scaling and rate scaling, a multi-stage rate/level envelope per operator, a pitch envelope, a set of operator routing algorithms with operator feedback, and an LFO with pitch and amplitude modulation depth
**And** the parameter set is a superset of the classic 6-operator FM SysEx voice format, value for value: 32 algorithms with feedback 0–7, 4-rate/4-level operator envelopes, keyboard level scaling (break point, left/right depth and curve) and rate scaling, operator output levels 0–99, coarse/fine/detune and fixed mode, velocity and amplitude-modulation sensitivity, the LFO (wave, speed, delay, pitch and amplitude depth, key sync, pitch-modulation sensitivity), the 4-rate/4-level pitch envelope, oscillator key sync and transpose
**And** a mapping table in `engine/core/engines/fm/SYSEX-MAP.md` lists every SysEx voice field and the Velora parameter it sets, with no unmapped field
**And** the engine consumes only MIDI 2.0 UMP (Note On/Off with velocity, pitch bend, mod wheel, sustain, aftertouch when present) and has no OS dependency

**Given** the RT-safety discipline (AD-3)
**When** the FM voice renders a 64-frame block
**Then** it allocates nothing, takes no locks and passes the RTSan render test in CI
**And** the engine declares a per-voice cost in cost units measured on the Mac (an estimate until it is measured on the target hardware) and exposes steal metadata (age, amplitude, held/sustained state)

**Given** the offline renderer (Story 1.9)
**When** a committed test suite of FM timbres (algorithm sweep, feedback sweep, envelope shapes, velocity layers, pitch bend) renders
**Then** renders are bit-identical across two runs and are stored as golden files for regression in CI
**And** a unit test checks aliasing on a high-feedback, high-ratio patch at the top key stays below a stated threshold (recorded with the test)

### Story 12.2: FM null test against an unshipped reference oracle

As the founder,
I want the FM engine null-tested against a reference implementation used only as a test oracle,
So that FM fidelity is proven automatically, as the acceptance tests require, without shipping GPL code.

**Covers:** NFR22, NFR28, NFR27, AR2 (null tests in CI)

**Runs on:** Mac + CI

**Acceptance Criteria:**

**Given** a reference FM implementation chosen as oracle (e.g. a GPL 6-operator implementation)
**When** it is added to the test job
**Then** it is fetched and built only in the CI null-test job, never vendored into a shipped directory, and the build license manifest of every shipped target shows no GPL oracle code
**And** the oracle is driven as a black box through its own input format by a test-only adapter in `tools/` that translates each test timbre's FM parameters into the oracle's parameters

**Given** a committed FM test suite (each algorithm, feedback levels, operator envelopes, key and velocity scaling, LFO off) with timbres expressed in the Velora FM parameter set
**When** the null-test harness compares the Velora offline render with the oracle render
**Then** the residual is below −50 dB relative to the reference RMS (decided tolerance, recorded with the tests with its reasoning), and any test where the oracle's own quantisation makes that tolerance unreachable is documented with an alternative spectral check
**And** the FM null tests run in CI on every change to the FM engine

**Given** a deliberately broken FM change (e.g. wrong feedback scaling)
**When** CI runs
**Then** the null-test job fails

### Story 12.3: Import 6-OP SysEx banks and single voices from files (USB-A and Velora Studio)

As a keyboardist with a collection of classic FM sounds,
I want to import my 6-operator FM SysEx banks and single voices from a USB-A drive or from Velora Studio,
So that the sounds I already own become timbres in my Library without retyping a single parameter.

**Covers:** FR121 (file import), FR56, FR58, NFR28, NFR29

**Runs on:** Mac (formatlib, librarian, offline renderer, LVGL simulator and Velora Studio); dev board for the USB-A path

**Acceptance Criteria:**

**Given** a `.syx` file found by the import preflight (Story 5.7) on a USB-A drive, or added in the Velora Studio library view (Story 9.14)
**When** formatlib parses it
**Then** it accepts a 32-voice bulk dump (4104 bytes) and a single-voice dump (163 bytes), validates the header, byte count and checksum, and decodes every voice into the Velora FM parameter set through the Story 12.1 mapping table
**And** a file with a bad checksum, a wrong byte count, an unknown header or out-of-range values is rejected as a whole with reason code `damaged` and the copy "This SysEx file is damaged or not a 6-OP voice dump. Nothing was imported.", and the rest of the batch continues

**Given** a valid 32-voice bank
**When** it is imported
**Then** it becomes one user library of 32 timbres, one per voice, stored as parametric timbre bundles in formatlib with a stable `library_id`/`instrument_key` (the same identity rules as sample libraries, FR23 semantics), so Live Sets keep resolving after Import again
**And** each timbre keeps its 10-character voice name, sanitized (non-printable bytes replaced, trailing spaces trimmed, empty names become "Voice n"), with duplicates in one bank kept and disambiguated by their slot number

**Given** the Import surface (Story 5.12) or the Velora Studio import view
**When** SysEx rows are shown
**Then** the source is labelled generically "Import 6-OP SysEx", the Category Picker (Story 5.11) offers the 17 categories with Synth Poly as the default, and apply-to-all works for the whole bank
**And** the imported timbres appear in Library search, category chips, Favorites and Recent by category, never by engine type (FR58)

**Given** the committed SysEx test corpus (self-made banks covering every algorithm, envelope extremes, keyboard scaling curves and LFO waves)
**When** each voice is imported and rendered by the offline renderer, and the same SysEx is loaded into the unshipped null-test oracle from Story 12.2
**Then** every render pair passes the null test within the −50 dB tolerance (or the documented spectral check), and a voice exported back to SysEx from the imported parameters is byte-identical to the source voice

**Given** a release build
**When** its content is audited
**Then** no third-party SysEx bank or patch is shipped, vendored or used as factory content; test banks in the repository are self-made and marked as such

### Story 12.4: Receive 6-OP SysEx over USB MIDI and DIN

As a keyboardist with a hardware FM synth or a librarian app,
I want to send a 6-OP SysEx dump to the Velora over USB MIDI or a DIN cable,
So that I can move sounds straight from my other gear or computer without a drive.

**Covers:** FR121 (MIDI receive), NFR16, NFR17

**Runs on:** Mac (engine host with a virtual MIDI port, librarian, LVGL simulator); dev board or prototype for USB MIDI and DIN

**Acceptance Criteria:**

**Given** SysEx arriving on USB MIDI (Story 1.20) or MIDI DIN in (Story 3.17)
**When** the input path receives it
**Then** the RT plane never parses or buffers the dump beyond handing its UMP packets to a lock-free queue for the control thread, which reassembles the message off the audio threads; a dump arriving during performance causes no xrun and changes no sound

**Given** a reassembled message with the 6-OP voice header
**When** it is validated with the Story 12.3 parser (checksum, byte count, header)
**Then** a valid single voice or 32-voice bulk dump raises a Notice Toast "6-OP SysEx received: n voices · Import?" that never blocks play, and nothing is imported until the player confirms; confirming opens the same Import rows, Category Picker (default Synth Poly) and naming rules as Story 12.3
**And** a malformed or truncated dump is discarded with the copy "SysEx received but damaged. Nothing was imported." and a counter in diagnostics

**Given** a 32-voice dump sent at full DIN speed, and the same dump split across several USB MIDI packets
**When** it is received while 256 voices play
**Then** it reassembles exactly, imports as 32 timbres equal to the file import of the same bank, and the engine counters show zero xruns
**And** SysEx that is not a 6-OP voice dump is ignored without a notice

### Story 12.5: FM voices in the shared pool with a 128-voice floor

As a keyboardist,
I want FM parts to share the voice pool with the sample and organ engines and always get at least 128 voices,
So that an FM layer never starves the piano and the piano never starves the FM layer.

**Covers:** FR12 (pool), FR6, FR7, NFR8, AR18

**Runs on:** Mac (arbiter logic); prototype unit or dev board (voice budget)

**Acceptance Criteria:**

**Given** a Live Set with at least one FM part
**When** it loads
**Then** the arbiter reserves the FM floor (≥128 voices at the FM cost per voice) after the FX budget, the organ/rotary reservation and the sample floor, and only when an FM part is present (zero reservation otherwise)
**And** FM voices are allocated and stolen only by the arbiter under the existing whole-Live-Set stealing rules (lowest held note and pedal-sustained notes protected, newer and quieter first, a stolen voice fading out keeps counting)

**Given** unit tests in `engine/core`
**When** scripted scenarios run (FM pad layered over a 256-voice piano, sustain pedal down, organ present)
**Then** victims match the expected order and FM never drops below its floor while the sample engine keeps its 256-voice floor

**Given** the target hardware (RK3588, 3 isolated A76 cores)
**When** the FM cost per voice is measured with the densest test timbre
**Then** the measured cost replaces the Mac estimate, and a render of 128 FM voices plus 256 sample voices plus organ and all effects of an 8-part Live Set completes within the block budget with zero xruns over 1 h
**And** if the budget cannot be met, the result is reported with the measured headroom before any floor is changed

### Story 12.6: Clean-room virtual-analog voice in the engine core

As a keyboardist,
I want a virtual-analog engine for leads, pads, basses and polysynth sounds,
So that the Velora covers the classic analog synth sounds expected on stage.

**Covers:** FR13 (engine DSP), NFR28, AR15 (VA engine implementation)

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the clean-room rule (NFR28)
**When** the VA engine is implemented in `engine/core/engines/va/`
**Then** a clean-room record lists the published sources used (e.g. band-limited oscillator and zero-delay-feedback filter literature) and states no GPL synth code was used
**And** each voice has 2 oscillators plus sub/noise (saw, pulse with PWM, triangle, sine), hard sync and detune, a mixer, a resonant low-pass filter with at least 12 and 24 dB/oct slopes plus high-pass and band-pass, filter and amp ADSR envelopes, velocity and key tracking, an LFO, unison/detune and glide, with mono/legato and poly modes

**Given** the RT-safety discipline
**When** the VA voice renders a 64-frame block
**Then** it allocates nothing, takes no locks, passes the RTSan render test, declares a per-voice cost and exposes steal metadata

**Given** no reference oracle is required for VA (acceptance-tests.md names none)
**When** objective DSP tests run in CI
**Then** oscillator aliasing at the top key and 48 kHz stays below a stated threshold, filter cutoff and resonance responses match their analytic targets within stated tolerances, self-oscillation is stable without NaN/denormal output, and envelope timings match their parameters within a stated tolerance
**And** a committed VA test suite renders bit-identically twice and is kept as golden files

### Story 12.7: VA voices in the shared pool with a 64-voice floor

As a keyboardist,
I want VA parts to share the voice pool and always get at least 64 voices,
So that a VA pad or lead plays reliably alongside piano, organ and FM.

**Covers:** FR13 (pool), FR6, FR7, NFR8, AR18

**Runs on:** Mac (arbiter logic); prototype unit or dev board (voice budget)

**Acceptance Criteria:**

**Given** a Live Set with at least one VA part
**When** it loads
**Then** the arbiter reserves the VA floor (≥64 voices at the VA cost per voice) in the engine-floor step, only when a VA part is present
**And** VA voices follow the same allocation and stealing rules as the other engines; mono/legato VA parts use one voice per part

**Given** unit tests
**When** a Live Set with sample, organ, FM and VA parts all at their floors is scripted
**Then** every engine keeps its floor, the organ is never stolen, and surplus is shared by the stealing rules

**Given** the target hardware
**When** the VA cost per voice is measured with the densest test timbre (unison on)
**Then** a render of 64 VA voices plus 128 FM voices plus 256 sample voices plus organ and all effects of an 8-part Live Set completes within the block budget with zero xruns over 1 h, or the measured headroom is reported before any floor change

### Story 12.8: FM and VA timbre parameters in the versioned formats

As a keyboardist,
I want FM and VA timbres and their edits saved in Live Sets, scenes and backups like any other part,
So that my FM and VA sounds survive Write, power cycles, updates, rollbacks and restores.

**Covers:** FR14, FR49, NFR44, AR12, AR29

**Runs on:** Mac

**Acceptance Criteria:**

**Given** the generated parameter registry in `proto/` (AD-2)
**When** the FM and VA parameter sets are added
**Then** every FM and VA parameter has a `param_id`, range, unit, default, structural/performance class and a locale-catalog key for its display name, and ASSIGN can map any performance-class FM/VA parameter; the FM entries describe the same parameter set held in the Story 12.3 parametric timbre bundles, so an imported SysEx timbre edited in a part saves like any other
**And** the additions are purely additive: no existing `param_id`, field number or meaning changes, and the `ws.v1` schema grows only by addition

**Given** a Live Set with FM and VA parts and scene edits of their parameters
**When** it is written, read back, exported to `.velorabackup` and restored
**Then** all FM/VA parameters round-trip exactly, scenes store FM/VA performance parameters (never the timbre assignment), and the format version is bumped where the schema changed

**Given** the compatibility corpus of every prior release's saved Live Sets (AR29)
**When** the new firmware's reader opens them
**Then** every one opens unchanged with no migration (FR14)
**And** a Live Set saved by the new firmware with FM/VA parts, opened by the N−1 reader (simulating rollback), opens without error, the FM/VA parts play silent (never substituted by another timbre), and their unknown fields are preserved when that Live Set is written back

### Story 12.9: Init and factory FM/VA timbres in the Library

As a keyboardist,
I want FM and VA timbres to appear in the Library under their categories, with a starting set of factory sounds,
So that I find and audition them exactly like sample and organ timbres, without knowing which engine makes them.

**Covers:** FR14 (UI from day one), FR56, FR58, FR59, FR34 (content path), FR46

**Runs on:** Mac (catalog and Library logic); verified on a unit

**Acceptance Criteria:**

**Given** the engines are present in the firmware
**When** the Library is browsed
**Then** an "Init FM" and an "Init VA" timbre are always available (built into the engine, not library content) under Synth Poly, so FM and VA are usable without any content pack (decided: Init timbres are built into the engine)
**And** every FM/VA timbre shows its category (short label on Part Strips, full label elsewhere), never the engine type (FR58)

**Given** a founder-authored factory FM/VA pack (decided: at least 50 timbres covering electric pianos, bells, basses, brass, leads, pads and polys, each with one of the 17 categories, shipped as a signed library)
**When** it is packed with the content-pack tool as a signed vendor library
**Then** it installs separately from the firmware bundle through Velora Studio or a USB-A stick (FR46: library updates never travel inside a firmware bundle), and its timbres appear in Library search, category chips, Favorites and Recent
**And** key-play audition and WRITE assignment to a part work as for any timbre (FR59), and the timbre names contain no third-party trademarks

**Given** the catalog
**When** a factory parametric (FM/VA) timbre is indexed
**Then** it uses the parametric timbre bundle from Story 12.3 (VA timbres with the VA parameter set) with a stable `library_id`/`instrument_key`, so Live Sets that use it keep resolving after a re-install of the pack (FR23 semantics)

### Story 12.10: Design story — FM and VA Sound sections and not-available state

As the founder,
I want the FM and VA Sound sections of the Part surface and the Velora Studio Part Editor drawn before they are built,
So that dozens of synth parameters fit the device's list-left/fields-right pattern and the Studio editor without breaking the design system.

**Covers:** UX-DR60, UX-DR42, UX-DR99 (FM/VA extension), UX-DR97, UX-DR98

**Runs on:** Design (no device)

**Acceptance Criteria:**

**Given** the Part surface spine (Timbre, Sound, Envelope & EQ, Zone & keyboard, Level & output, Effects, Arpeggiator) and the existing Part surface mockup
**When** the FM and VA Sound sections are drawn
**Then** mockups exist in Stage and Studio themes showing parameter grouping (FM: algorithm with a routing diagram, operators 1–6 with ratio/level/envelope, feedback, pitch envelope, LFO; VA: oscillators, mixer, filter, envelopes, LFO, voice mode/unison/glide), how a group is chosen with VALUE and touch, and how BRIGHTNESS (fixed encoder) maps for each engine
**And** they pass pseudo-localization at +40% with `ÁÇÊÕÑ Ação`, use only existing components and tokens, and keep touch targets ≥56 px

**Given** a Live Set opened on firmware without the engine (rollback, Story 12.8)
**When** its FM/VA parts are shown
**Then** the mockup defines the part's not-available state (wording following Voice and Tone, e.g. "Needs update n", silent, never substituted) on Part Strips and the Part surface

**Given** the Velora Studio Part Editor's Sound section
**When** the FM/VA variants are drawn
**Then** they reflow 4 → 2 → 1 columns at ≥150% / 200% text size and are fully keyboard operable

### Story 12.11: Part surface Sound section for FM timbres

As a keyboardist,
I want to edit an FM part's sound on the device's Part surface,
So that I can tweak an FM electric piano or bell on stage without a computer.

**Covers:** FR112 (FM), FR14, UX-DR60, NFR16, NFR30

**Runs on:** Mac (LVGL UI on host); verified on a unit

**Acceptance Criteria:**

**Given** a part with an FM timbre
**When** its Part surface opens (Part Strip card body or VALUE push on the knob-target part)
**Then** the Sound section shows the FM groups as drawn in Story 12.10, VALUE edits the highlighted field and touch selects, and the fixed encoders keep their PART-mode functions (BRIGHTNESS mapped as designed)
**And** edits play immediately, mark the Live Set ◆ unsaved, and persist only through Write with the normal scopes

**Given** the UI process
**When** an FM parameter is edited rapidly while notes are held
**Then** sound never cuts or clicks beyond the parameter's own change, and a UI crash during editing does not interrupt sound (NFR16)

**Given** the locale catalog
**When** the language is PT-BR or ES
**Then** every FM label and value is a catalog key with no hard-coded strings, and no third-party trademark appears

### Story 12.12: Part surface Sound section for VA timbres

As a keyboardist,
I want to edit a VA part's sound on the device's Part surface,
So that I can shape a pad or lead's filter and envelopes during rehearsal or on stage.

**Covers:** FR112 (VA), FR14, UX-DR60, NFR16, NFR30

**Runs on:** Mac (LVGL UI on host); verified on a unit

**Acceptance Criteria:**

**Given** a part with a VA timbre
**When** its Part surface opens
**Then** the Sound section shows the VA groups as drawn in Story 12.10, with VALUE/touch editing and BRIGHTNESS mapped to filter cutoff (or as designed)
**And** edits play immediately, mark ◆, and persist only through Write

**Given** a mono/legato VA part
**When** voice mode, unison or glide is changed while a note is held
**Then** the change applies without a stuck or doubled note

**Given** PT-BR or ES
**When** the section is shown
**Then** all labels come from the locale catalog and fit per the +40% budget

### Story 12.13: Velora Studio Part Editor Sound sections for FM and VA

As a keyboardist,
I want to see and edit all FM and VA parameters at once in Velora Studio,
So that deep synth programming is comfortable on the computer and plays live on the unit.

**Covers:** FR97 (FM/VA), FR98, FR99, FR14, UX-DR42, UX-DR43, NFR35

**Runs on:** Mac (Velora Studio) + a connected unit

**Acceptance Criteria:**

**Given** a connected unit with FM/VA firmware and a part with an FM or VA timbre selected
**When** the Part Editor shows the Sound section
**Then** all FM or VA parameters appear as drawn in Story 12.10 using Studio Knobs (drag, type, scroll, keyboard; ARIA slider with `aria-valuetext`), edits play live on the unit (`STUDIO`) and mark ◆, and "Write to Velora" persists them
**And** the FM algorithm routing diagram updates when the algorithm changes and has a text alternative for screen readers

**Given** a unit on firmware without FM/VA (version mismatch)
**When** Velora Studio connects
**Then** the FM/VA editors are hidden, FM/VA parts show the not-available state, and the update banner offers the firmware update (FR99)

**Given** OS text size at 200%
**When** the Part Editor shows an FM part
**Then** the section reflows to one column with no horizontal scroll

### Story 12.14: FM and VA firmware update reaches shipped v1 units

As a keyboardist who already owns a Velora 73,
I want to install the firmware update that adds FM and VA and keep everything I had,
So that my instrument gains two engines for free without losing a single Live Set.

**Covers:** FR12, FR13, FR45, FR49, FR66, NFR1, NFR4, NFR8, NFR12, NFR51

**Runs on:** Production v1 unit (release acceptance)

**Acceptance Criteria:**

**Given** a production v1 unit on the launch firmware with user setlists, Live Sets, scenes, imported libraries and the factory library
**When** the signed RAUC bundle with FM and VA is installed from Velora Studio and from a USB-A stick (two units)
**Then** the install follows the FR47 flow, the unit restarts on the new slot, and every pre-existing Live Set opens and plays identically (automated render comparison of a test Live Set before and after)
**And** the FM/VA factory timbre library installs separately (Story 12.9) and Init FM / Init VA are available even without it

**Given** the updated unit
**When** an 8-part Live Set with sample, organ, FM and VA parts and all effects runs
**Then** key-to-audio latency stays ≤5 ms under full load (NFR1) and the 24 h soak shows zero xruns and underruns (NFR4) with the FM ≥128 and VA ≥64 floors reached (NFR8)

**Given** the same update
**When** 100 random power cuts are made during installation
**Then** the unit boots every time on the previous or the new firmware (NFR12), and after a rollback a Live Set saved with FM/VA parts opens with those parts in the not-available state and its data preserved
**And** the release notes in EN, PT-BR and ES describe the engines with generic names only ("6-operator FM", "virtual analog")

## Epic 13: SFZ coverage for the reference corpus

The reference corpus imports and passes the sfizz null test by widening the SFZ opcode coverage in measured steps, after an explicit scope decision. Findings and the opcode inventory: Story 5.4 spec and its gap analysis. Corpus vocabulary: `tools/corpus/README.md`.

### Story 13.1: Decision: SFZ scope for the reference corpus

As the founder,
I want a written decision on which SFZ opcodes beyond the v1 subset the product supports,
So that FR18 (a deliberate subset) and FR20 (every corpus library imports) stop contradicting each other.

**Covers:** FR18, FR20, FR22

**Runs on:** Mac (documentation and decision)

**Acceptance Criteria:**

**Given** the Story 5.4 gap analysis (every rejected opcode per library with region counts, what the sample engine can express, effort and risk)
**When** the founder reviews it
**Then** each opcode tier (importer rules, note-on scalars, trigger and release modes, CC at voice start, choke groups, pitch bend, per-voice filter, region EQ, LFO/EG modulation) is marked in scope, deferred or out of scope with a reason
**And** FR18 and the epic list are amended to name the final subset, and the stories below are re-ordered or cancelled accordingly

**Given** an out-of-scope tier
**When** a library needs it
**Then** the import report skips that file with `unsupported_features` and the offending opcodes (FR22), and Story 5.4's manifest lists the file with the same reason
**And** the manifest lists only files skipped for tiers marked out of scope, and the Epic 5 corpus requirement is restated as 'imports and plays with mapping intact for every file in a supported tier'

### Story 13.2: Importer value rules and inert opcodes

As a keyboardist,
I want libraries whose SFZ uses out-of-range note values, `<curve>` headers or non-sounding opcodes to import like they do in sfizz,
So that a harmless value does not reject a whole library.

**Covers:** FR18, FR22

**Runs on:** Mac

**Blocked by:** Story 13.1

**Acceptance Criteria:**

**Given** an SFZ with `lokey=-1` or `lokey=c-2`
**When** it is imported
**Then** the value follows sfizz: an unparseable or out-of-range note falls back to the opcode default, and `hikey=-1` marks a region that never triggers from a note
**And** a unit test per case compares the region table with sfizz's behaviour, and the report logs the ignored value

**Given** a `<curve>` header
**When** it is parsed
**Then** the curve is stored and stays inert until an opcode that uses it is supported
**And** a file using a curve through an unsupported opcode is still skipped with that opcode named

**Given** the allow-list of inert opcodes
**When** `ampeg_dynamic` is listed
**Then** it is inert only while no `ampeg_*_oncc` is supported (`ampeg_vel2*` does not need it: the trigger velocity is constant, verified against sfizz in the Story 13.4 `ampeg_vel2` fixture)
**And** Story 13.5 revisits it when `*_oncc` lands

### Story 13.3: Sample end, first/legato triggers and release_key

As a keyboardist,
I want samples with an `end` point and `trigger=first`, `legato` and `release_key` regions to play like sfizz,
So that a library such as Manthey Klaviano imports and plays with its mapping intact.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Story 13.1

**Acceptance Criteria:**

**Given** a region with `end`
**When** it plays
**Then** the last played frame is `min(end, sample length) - 1` (exclusive end), `end=0` disables the region, and the importer and bundle store the field
**And** a test covers a truncated sample (end beyond the file), end equal to the length and end=0

**Given** a region with `trigger=release_key`
**When** the key is released while the sustain pedal is down
**Then** it fires at the physical key-up, while `trigger=release` keeps waiting for the pedal
**And** the engine tells the two apart in its release path and a render test with the pedal covers both

**Given** `trigger=first` and `trigger=legato`
**When** notes overlap
**Then** first fires only for the first note of a phrase and legato only for overlapping notes, using held-key state on the control path with no allocation in the render thread
**And** the bundle `format_version` minor is bumped, the reader accepts N and N-1, and existing demo bundles still render bit-identically

**Given** Manthey Klaviano from the corpus
**When** the corpus job runs
**Then** it imports, its MIDI suite renders and the sfizz null test passes within its recorded tolerance
**And** the manifest keeps no exclusion for it

### Story 13.4: Velocity and amplitude model

As a keyboardist,
I want `amp_veltrack`, `amp_velcurve_N`, `amplitude`, `group_volume`, `master_volume` and the velocity-dependent envelope opcodes to shape the sound as in the library,
So that touch response matches what the sample author built.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Story 13.1

**Acceptance Criteria:**

**Given** regions using these opcodes
**When** they are imported
**Then** each region carries a velocity-to-gain mapping evaluated at note-on, replacing the fixed velocity-squared gain only where the opcodes ask for it
**And** a region without the opcodes renders bit-identically to before

**Given** `ampeg_vel2attack`, `vel2decay`, `vel2sustain`, `vel2release` and `amp_keytrack`/`keycenter`
**When** a note starts
**Then** the scalar is applied at note-on, deterministically and without allocation or locks in the render thread
**And** a null test against sfizz passes for a fixture using each opcode

**Given** the bundle format
**When** the fields are added
**Then** `format_version` minor is bumped, N and N-1 are readable and every existing demo bundle opens and renders bit-identically

### Story 13.5: CC at voice start and sfizz pseudo-CCs

As a keyboardist,
I want `locc`/`hicc` gates, `*_oncc` modulation and `set_ccN` defaults to follow the controller value when a note starts,
So that libraries that react to pedals, wheels and velocity-as-CC play as designed.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Stories 13.2 and 13.4

**Acceptance Criteria:**

**Given** regions with `locc`/`hicc` gates
**When** a note starts
**Then** the region plays only when the per-part CC value is inside the range, and `set_ccN`/`set_hdccN` seed the initial values
**And** a test covers boundaries and a CC change between notes

**Given** `*_oncc` and `*_curvecc` on envelope, offset, amplitude, pan and tune
**When** a note starts
**Then** the CC value is sampled at voice start through the stored `<curve>`, and sfizz's pseudo-CCs (131 velocity, 135/136 random, 137 alternate, 140/141 key delta) are resolved from the note, never from a real controller
**And** `ampeg_dynamic` is no longer inert and the importer rejects `ampeg_*_oncc` with `ampeg_dynamic` unless the continuous form is implemented

**Given** a CC that moves during a held note
**When** the null test runs
**Then** the divergence from sfizz's continuous per-block modulation is documented as a known limit and the corpus keeps such probes out of the null picks with a manifest reason
**And** kamoepiano301, Scarypiano, Osiris and Steel Drum import

> **Status note (Story 13.5 run): this last AC is NOT met by 13.5 alone.** None of the four imports yet. Blocked by Story 13.6 (`group`, `note_polyphony`: Osiris), 13.7 (`bend_up`/`bend_down`: Scarypiano), 13.8 (`cutoff_oncc131`, `cutoff_curvecc131`: kamoepiano301) and by opcodes no Epic 13 story names: velocity crossfades `xfin_lovel`/`xfout_lovel` (Steel Drum), `ampeg_release_shape` and `sustain_cc` (kamoepiano301, Osiris), `width_oncc`/`width_curvecc` (Steel Drum), plus `..` sample paths in Osiris' included map files. The CC-at-voice-start opcodes themselves no longer block any of them.

### Story 13.6: Choke groups and polyphony limits

As a keyboardist,
I want `group`, `off_by`, `off_mode`, `off_time`, `polyphony` and `note_polyphony` to cut notes as the library intends,
So that hi-hats, noise releases and mono instruments behave correctly.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Story 13.1

**Acceptance Criteria:**

**Given** a region with `off_by` matching another region's `group`
**When** the second region starts
**Then** the first group's voices are released with the fast or normal release named by `off_mode` and `off_time`, in the shared voice pool and under the existing voice-stealing rules
**And** a test covers the cut, the fast release time and the interaction with the sustain pedal

**Given** `polyphony` and `note_polyphony`
**When** the limit is reached
**Then** the oldest voice of that group or note is released, deterministically, with no allocation in the render thread
**And** parallel voice render (3 partitions) is bit-identical to the 1-thread parallel render and equal to the serial render within the repository's 1e-5 contract (the partition sums differ from the serial accumulation only by float rounding), with the same voice table after every block

**Given** the null test against sfizz
**When** a fixture uses groups
**Then** it passes, or the divergence of sfizz's stealing and `off_mode` timing is documented with evidence

> **Status note (Story 13.6 run):** `group`, `off_by`, `off_mode`, `off_time`, `polyphony`, `note_polyphony` (and `note_selfmask`) land with bundle format minor 5; sfizz null fixtures `choke_groups`, `choke_sustain` and `polyphony_limits` pass at -62.5, -62.6 and -64.4 dB, so no divergence of sfizz's stealing or `off_mode` timing needed documenting beyond the two limits in the spec (no synthetic note off for a choked voice; region-set `polyphony` rejected). Parallel render: as the AC above states, 1 and 3 threads are bit-identical and the voice table equals the serial one after every block; the audio matches the serial render within the 1e-5 contract. Corpus: `group`/`off_by`/`note_polyphony` no longer block Osiris, but `sustain_cc`, `ampeg_release_shape`, `..` map paths and the 13.5 `ampeg_dynamic` + `ampeg_*_oncc` rejection still do; no library that did not import before imports now (VSCO2 and Manthey Klaviano still do).

### Story 13.7: Pitch bend and CC-triggered voices

As a keyboardist,
I want `bend_up`/`bend_down`, `bend_step` and regions triggered by a controller (`on_locc`/`on_hicc`, `hikey=-1`) to work,
So that the pedal-action noises of Salamander and the pitch-bend range of other libraries play as designed.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Stories 13.2 and 13.5

**Acceptance Criteria:**

**Given** a pitch-bend message
**When** a note sounds
**Then** its pitch follows `bend_up`/`bend_down` in cents with smoothing and no allocation in the render thread, and a part without the opcodes keeps its current bend behaviour
**And** a test covers both extremes and a bend during release

**Given** a region with `on_locc`/`on_hicc` and `hikey=-1`
**When** the CC crosses into its range
**Then** the region starts as a voice from the controller and never from a note, and Salamander's pedal-action region imports
**And** Salamander from the corpus imports and passes the null test within its recorded tolerance

> **Status note (Story 13.7 run): the last AC ("Salamander imports and passes the null test") is NOT met.** Salamander still fails import: `rt_decay`, `amp_veltrack_oncc99`/`amp_veltrack_curvecc99`, and its samples are FLAC (the importer reads WAV only). Scarypiano's bend opcodes import now, but every sample path is `..\Samples\...` (outside the SFZ directory). The pedal-action regions themselves import and are null-tested against sfizz (`cc_trigger`).

### Story 13.8: Per-voice filter

As a keyboardist,
I want `cutoff_oncc`/`cutoff_cc`, `fil_keytrack`, `fil_keycenter`, `fil_veltrack`, `resonance_cc` and a second filter to shape the tone per note,
So that libraries that open the filter with touch (kamoepiano301, Headroom Piano) sound as designed.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Story 13.5

**Acceptance Criteria:**

**Given** a region with these opcodes
**When** a note starts
**Then** the filter coefficients are computed per voice, in the render thread without allocation or locks, within the existing per-voice CPU budget
**And** the NFR8 voice floors still hold in the rt-traffic and parallel-render tests

**Given** the null test against sfizz
**When** a fixture uses each opcode
**Then** it passes within a recorded tolerance, and the filter's stability at extreme cutoff and resonance is tested including NaN and subnormal inputs

**Given** kamoepiano301 and Headroom Piano
**When** the corpus job runs
**Then** they import and pass, or the manifest records the exact remaining opcode and reason

> **Status note (Story 13.8 run): the last AC ("kamoepiano301 and Headroom Piano import and pass, or the manifest records the exact remaining opcode and reason") is recorded, not met.** The filter opcodes of this story (`fil_keytrack`, `fil_keycenter`, `fil_veltrack`, `cutoff*_oncc`/`_cc`/`_curvecc`, `resonance*_oncc`/`_cc`, the second filter) import and are null-tested against sfizz. kamoepiano301 still fails import on `sustain_cc`, `ampeg_release_shape` and `ampeg_dynamic=1` together with `ampeg_releasecc64` (Story 13.5 rule); Headroom Piano on `amp_veltrack_oncc73`, `width_oncc76`/`79` (+ `_curvecc`) and `eq1_*`/`eq2_*` (Story 13.9). With those three opcodes removed from a scratch copy for both renderers, kamoepiano301 imports (90 regions) and 5 of its 6 corpus suites null-test at -94 to -104 dB; `sustained_loops` fails at -54.9 dB with and without the filter opcodes.

### Story 13.9: Region EQ and LFO/EG modulation (conditional)

As a keyboardist,
I want region EQ bands, LFOs and envelope generators (`eqN_*`, `lfoNN_*`, `egNN_*`, `pitcheg`, `fileg`, `amplfo_*`) if the scope decision includes them,
So that Karoryfer and Sonatina libraries import with their character intact.

**Covers:** FR18, FR20

**Runs on:** Mac

**Blocked by:** Story 13.1 (scope) and Stories 13.5 and 13.8

**Acceptance Criteria:**

**Given** Story 13.1 marks the tier in scope
**When** the story starts
**Then** a design note fixes the modulation matrix, LFO phase, smoothing and sum rules against sfizz before any code, and states the CPU cost per voice on the target
**And** if the tier is out of scope this story is cancelled and the affected files stay in the manifest with that reason

**Given** regions with these opcodes
**When** they play
**Then** modulation runs on the render thread with no allocation or locks, deterministically, and 1 versus 3 worker partitions stay bit-identical

**Given** the corpus job
**When** Sonatina and the Karoryfer libraries run
**Then** they import and pass the null test within recorded tolerances, or the remaining differences are documented per opcode

### Story 13.10: Random-layer parity with the null oracle

As the founder,
I want `lorand`/`hirand` instruments to be null-testable,
So that the corpus proves the random-layer libraries too, not only the deterministic ones.

**Covers:** FR20, AR2 (partial: null tests)

**Runs on:** Mac

**Blocked by:** Story 5.4 (findings)

**Acceptance Criteria:**

**Given** the sfizz oracle's fixed-seed random draw (per note-on, note-off and CC)
**When** the options are assessed
**Then** a decision records one of: a test-only mode that aligns the engine's draw order with the oracle, a seeded comparison with the oracle's draw sequence replayed, or a statistical check of layer distribution, with the reason
**And** the production random generator is not changed for a test

**Given** the chosen method
**When** FluteStac and TrumpetStac from VSCO run
**Then** they are null-tested or checked, and are removed from the 'left out of null picks' list in the corpus report

### Story 13.11: SF2 libraries in the reference corpus

As the founder,
I want SF2 packs to run through the corpus with a FluidSynth oracle,
So that a Pianobook or other pack delivered as SoundFont can count toward the corpus.

**Covers:** FR19, FR20, AR2 (partial: null tests)

**Runs on:** Mac

**Blocked by:** Story 5.3 and Story 5.4

**Acceptance Criteria:**

**Given** a corpus entry of kind SF2
**When** the corpus job runs
**Then** it imports with the SF2 reader (Story 5.3), renders the per-library MIDI suite and is null-tested against the existing FluidSynth oracle within a recorded tolerance, with the same hash pinning and hardening as SFZ entries

**Given** the report
**When** it is written
**Then** it lists SF2 and SFZ entries together and fails the job on any import or null-test failure

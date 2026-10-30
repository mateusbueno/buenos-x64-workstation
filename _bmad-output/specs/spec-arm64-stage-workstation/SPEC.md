---
id: SPEC-arm64-stage-workstation
companions:
  - performance-targets.md
  - engines.md
  - hardware-platform.md
  - mechanical.md
  - library-format.md
  - acceptance-tests.md
  - go-to-market.md
  - rejected-approaches.md
  - ip-and-references.md
  - glossary.md
  - ../../planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md
  - ../../planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md
  - ../../planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md
  - ../../planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md
sources:
  - ../../forge/kronos-64bit-os/forged-idea.md
---

> **Canonical contract.** This SPEC and the files in `companions:` are the complete, preservation-validated contract for what to build, test, and validate. Source documents listed in frontmatter are for traceability — consult them only if you need narrative rationale or prose color this contract intentionally omits.

# Ladiani Velora 73 — ARM64 Stage Workstation

## Why

This spec has two drivers: a vision and a pain. The founder's Korg Kronos caps sample memory at about 2.2 GB, and the factory autoload alone takes about 2 GB, even in Virtual Memory mode. Korg's successor, the Nautilus, ships with the same cap of about 2 GB. The cap cannot be lifted, because the Kronos synth engine is a closed i386 kernel module (see `rejected-approaches.md`). So the founder will build an own commercial ARM64 stage keyboard, launching in Brazil first. Its differentiator is a sample engine that streams libraries from NVMe at sizes far beyond RAM. It is sold at the price of a Nord Electro 6D or a Yamaha YC73, and it adds a USB audio interface, which Nord does not offer. An investor is interested and brings partners for the physical build. The first units may be hand-built, but quality must be good enough to scale.

On top of that differentiator sits an opportunity for the keys players the product is built for. Recordings fix an electric piano's response into velocity layers, and they cannot hold how a real piano's undamped strings answer each other. Two owned models close those gaps: a modeled electric piano and modeled resonance on the sample pianos.

## Capabilities

- **CAP-1**
  - **intent:** A player can load and play sample instruments from an on-device library much larger than RAM, with no loss of audio quality.
  - **success:** With a library of up to 1 TB indexed, the unit plays 256 simultaneous stereo sample voices streamed from NVMe. A 24 h stress run with the SSD near full produces zero dropouts. Decoded output is bit-identical to the source audio.
- **CAP-2**
  - **intent:** A player hears a note fast enough that the instrument feels immediate.
  - **success:** Measured latency from key press to audio output is ≤5 ms under full polyphony load.
- **CAP-3**
  - **intent:** The system shares voices across engines dynamically. Each engine keeps a guaranteed minimum, and voice stealing never drops bass notes or notes held by sustain first.
  - **success:** Under a saturation test, each engine reaches its floor in `engines.md`. In a stealing test, the lowest held note and the pedal-sustained notes survive while newer, quieter voices are stolen.
- **CAP-4**
  - **intent:** A player can play a tonewheel organ engine that is never voice-limited.
  - **success:** The organ engine plays all keys with all drawbars at full polyphony and is never stolen. It passes the organ protocol in `acceptance-tests.md`.
- **CAP-5**
  - **intent:** A player can play a 6-operator FM engine.
  - **success:** The engine sustains ≥128 voices, passes the FM null test in `acceptance-tests.md`, and reaches shipped v1 units by firmware update.
- **CAP-6**
  - **intent:** A player can play a virtual-analog synth engine.
  - **success:** The engine sustains ≥64 voices and reaches shipped v1 units by firmware update.
- **CAP-7**
  - **intent:** A player can combine up to 8 parts as layers and splits in a v1 Live Set.
  - **success:** A Live Set with 8 parts across engines plays with correct zones and layering. The Pro tier raises the limit to 16 parts.
- **CAP-8**
  - **intent:** A user can import their own SFZ+WAV libraries (priority) and SF2 libraries and play them, unencrypted.
  - **success:** Every library in the reference corpus in `acceptance-tests.md` imports and plays with its key, velocity, round-robin, and loop mapping intact, within the v1 opcode subset. Every imported instrument carries one of the 17 categories in `library-format.md`, auto-suggested from filename or metadata; an instrument without a category cannot import.
- **CAP-9**
  - **intent:** Factory libraries, and future partner libraries, ship in a proprietary lossless format that resists extraction.
  - **success:** An encrypted library cannot be decoded with off-the-shelf tools or by copying it to another unit. A tampered file is rejected at load. A custom OS cannot boot to read the keys. The design is in `library-format.md`.
- **CAP-10**
  - **intent:** A user can connect the keyboard to a computer over USB-C as an audio and MIDI interface, with no driver to install.
  - **success:** Without extra drivers, macOS, Windows, and Linux enumerate the unit as a class-compliant audio and MIDI device, and a DAW records its outputs.
- **CAP-11**
  - **intent:** A player can connect the unit to stage gear with the v1 I/O set in `hardware-platform.md`.
  - **success:** Every listed port works on a production unit.
- **CAP-12**
  - **intent:** The vendor can add engines and features to shipped units without a hardware change.
  - **success:** A firmware update that adds a new engine installs on a v1 unit, and the engine runs inside the shared voice pool.
- **CAP-13**
  - **intent:** A player can shape the sound with built-in effects: rotary speaker, reverb, delay, chorus/phaser, EP amp/drive, EQ, compressor.
  - **success:** All effects run on an 8-part Live Set with full polyphony and do not break the latency or dropout targets.
- **CAP-14**
  - **intent:** A user can move libraries of hundreds of GB onto the unit, either from an external drive or from a desktop companion app, without opening the unit.
  - **success:** A 300 GB SFZ library on an external SSD imports through USB-A and becomes playable. The desktop app transfers and activates a library through a mediated USB protocol. Neither path exposes the internal NVMe as mass storage.
- **CAP-15**
  - **intent:** A player can find any timbre in a library of hundreds of GB quickly, on stage, by search, the 17 categories, favorites, or recent.
  - **success:** With a library of 1,000+ instruments, a player reaches any favorite within 2 interactions from the Library, and any instrument within 3 interactions through search or categories on the touchscreen. Every timbre name in the UI shows its category, not its engine type.
- **CAP-16**
  - **intent:** A player can play soon after power-on.
  - **success:** The user's Boot into target (last sound used, start of setlist, or default Live Set, chosen in Settings) is playable ≤10 s after power-on with a 1 TB library installed.
- **CAP-17**
  - **intent:** A power loss never bricks the unit or corrupts its library.
  - **success:** Power cut at random points during a firmware update or a library import, repeated 100 times: the unit boots every time, into the previous or the new firmware, and holds no partially imported library.
- **CAP-18**
  - **intent:** A player gets full expression from the keybed: attack and release velocity, aftertouch when the keybed has it, and organ onset driven by the timing of each key's contacts.
  - **success:** Velocity curves cover the full MIDI range evenly. Release velocity changes release samples. In the organ engine, the onset follows the timing between the key's contacts, as the Hammond M-solo's Virtual Multi-Contact does.
- **CAP-19**
  - **intent:** A player can run an arpeggiator on any part, synced to the internal tempo or to external MIDI clock.
  - **success:** With an arpeggiator active on 2 parts, notes land sample-accurately on the tempo grid. Following external MIDI clock from DIN or USB, the timing jitter stays ≤1 ms against the incoming clock.
- **CAP-20**
  - **intent:** Changing scene, Live Set, or setlist never cuts held, sustained, or releasing notes.
  - **success:** An automated test switches scene, Live Set, and setlist during held, pedal-sustained, and releasing notes, and finds no audible gap or truncation. The next Live Set of the loaded setlist is preloaded, so an in-setlist switch is immediate. A jump to a non-preloaded Live Set keeps the current sound playing until the target is ready.
- **CAP-21**
  - **intent:** A player organises songs as Setlist → Live Set (one per song) → 8 Parts plus up to 8 Scenes. The Live Set holds the instrument of each part; a scene stores parameters only (including arpeggiator on/off and all arpeggiator settings, with the Live Set holding the initial values) and never changes an instrument.
  - **success:** Recalling any scene changes no part's instrument. The Setlist Browser finds a Live Set by name across all setlists and filters by user-created setlist tags. A player jumps off-script to any Live Set without cutting sound (CAP-20), and LIVE SET ◀ ▶ returns to the loaded setlist.
- **CAP-22**
  - **intent:** A user can back up and restore setlists and Live Sets with Velora Studio, and use Studio without the unit connected. Backup and restore can also be partial: individual Live Sets or a selection, individual timbres or a selection, or a Live Set bundle (a selection of Live Sets plus the timbres they use) to move a repertoire to another Velora.
  - **success:** Studio writes a full `.velorabackup` file to a location the user chooses. Restore asks Merge (add and update, keep what the backup lacks) or Replace (unit content becomes the backup), and both produce that result. With no unit connected, Studio opens and edits backups and the computer-side library. Partial exports write a versioned `.velorasets` file (a Live Set selection or a Live Set bundle) or a `.veloratimbre` file (a timbre selection), each with a manifest that declares its scope. Studio shows a size estimate before any export and warns when a bundle with large user timbres is big. User-imported timbres (SFZ/SF2) are exported with their audio, losslessly. Factory and partner timbres are never exported: the file carries only their `library_id` and `instrument_key`, and on restore a part whose library is not installed shows "Missing library". Importing a partial file asks per item, on a collision: skip, replace, or keep both (the copy gets a new ID), and each choice produces that result.
- **CAP-23**
  - **intent:** A player can map the 8 encoders to parameters of their choice per Live Set (ASSIGN mode).
  - **success:** In ASSIGN, each encoder controls its mapped parameter in the current Live Set, and turning one shows the K1–K8 names for 3 s. A new Live Set starts from the global default map, which the user can edit. The map survives Write and a power cycle.
- **CAP-24**
  - **intent:** A player can play modeled tine and reed electric pianos whose tone responds continuously to touch, with no velocity layers.
  - **success:** The engine sustains ≥64 voices inside the shared pool and reaches shipped units by firmware update. Its timbres appear under E.Piano, never by engine type. A velocity sweep shows no layer steps, and the timbres pass the modeled EP protocol in `acceptance-tests.md`.
- **CAP-25**
  - **intent:** A player can voice a modeled electric piano to taste: hammer, pickup, and damper character.
  - **success:** The voicing parameters listed in `engines.md` are editable on the Part surface and in the Velora Studio Part Editor. They change the sound audibly, and they survive Write, a power cycle, and a backup and restore.
- **CAP-26**
  - **intent:** A player hears a sample piano's strings answer each other: undamped strings ring with the pedal down, silently held keys respond to played notes, and half-pedal moves continuously between the two.
  - **success:** On a factory piano with resonance data, a resonance amount parameter runs from off to full. The instrument passes the piano resonance protocol in `acceptance-tests.md`. With resonance off, output is bit-identical to the same instrument without resonance data.
- **CAP-27**
  - **intent:** The vendor can derive a piano's resonance data from that library's own recordings, with no note-by-note hand tuning.
  - **success:** An offline tool produces the resonance data for the Factory Grand and the Studio Upright from their source recordings, and the result ships inside the signed library bundle. Rerunning it on the same recordings produces identical data.

## Constraints

- The engine runs as a 64-bit userspace process on mainline Linux with PREEMPT_RT. RTAI and out-of-tree realtime patches are not allowed.
- RK3588 is the single SoC for the v1 (16 GB) and Pro (32 GB) tiers, and production uses an industrial SoM with a written 7–10 year longevity commitment. Milestone 1 first validates realtime latency and the display path on mainline. The fallback (CM5 or Intel N-series) is allowed only if that validation fails.
- v1 and Pro use one software base. The sequencer is a module, not a fork.
- The UI runs in a process isolated from the audio engine, so a UI crash never interrupts sound.
- English is the default and source UI language. PT-BR and ES are selectable, and more languages arrive by system update. User content (setlist, Live Set, scene, tag names) is never translated.
- Physical panel legends are English only. Rotary-speaker controls read ROTARY, and "Leslie" is never a product label.
- The v1 panel control set is fixed (detail in `hardware-platform.md`): 9 dual-function faders (drawbars / mixer), 8 endless encoders with LED rings, a VALUE encoder with a push switch, 3 toggle buttons with one LED per state (DRAWBARS/MIXER, PARTS/SCENES, PART/ALL/ASSIGN), a MENU button, ROTARY stop and slow/fast buttons, a MASTER level potentiometer in the analog output path, and a footswitch that advances the scene by default. v1 faders are non-motorized and use pickup. The soft power button sits on the rear panel.
- The touchscreen is 7-inch at a native 1024×600, landscape.
- Engines are clean-room or licensed. GPL code (e.g. Dexed/MSFA, setBfree) must not ship. Permissively licensed code such as sfizz (BSD-2) and libFLAC (BSD) is allowed. Marketing uses no third-party trademarks (no "DX7", no "Hammond").
- The architecture spine (`ARCHITECTURE-SPINE.md`, AD-1 to AD-31) and `HARDWARE-INTERFACES.md` are binding companions.
- The audio thread never does disk I/O, makes blocking syscalls, or takes page faults. Sample heads stay resident in locked RAM.
- Sample data is never lossy-compressed.
- Main audio output goes over I2S/TDM to a codec that is the clock master, with its own crystal in the 48 kHz family. USB is never in the main output path.
- Production firmware exposes no serial console and no mass storage over USB-C. The NVMe is sealed inside the unit.
- v1 has no radio (no Bluetooth, no Wi-Fi), which avoids Anatel homologation.
- The keybed is a Fatar 73SW (semi-weighted) from the partner's stock of ~100 units, and keybed scanning plus transport must fit inside the 5 ms latency budget.
- The enclosure follows `mechanical.md`: 1164.2 mm wide, a flat brushed-aluminum top, a painted steel bottom, 25 mm plastic end cheeks, and the pitch and mod wheels in an 80 mm block left of the keybed. Note names follow C4 = MIDI 60, so the keys span E1–E7.
- Power comes from an external off-the-shelf Inmetro-compliant supply through a locking 5.5 × 2.5 mm DC plug. The supercapacitor hold-up stays inside the unit.
- The modeled electric piano and piano resonance ship by firmware after v1. Neither may move the v1 date or become a v1 acceptance item.
- Resonance off leaves the sample path bit-identical, so CAP-1 and the sample null test stay valid.
- A preliminary patent search runs before either model's design closes, and neither model ships until an attorney's freedom-to-operate opinion clears it. Clean-room rules cover copyright, not patents.
- Third-party reference recordings never ship in the product. Development uses the attribution-licensed recordings in `ip-and-references.md`, and non-commercial material stays out without the author's written permission.
- Models derive from published literature and the founder's own measurements, never from disassembling or reverse engineering a competing product.
- Modeled electric pianos carry generic names (Tine, Reed). Marketing and UI never use "Rhodes" or "Wurlitzer".
- Release is date-driven. v1 requires the sample engine, the organ, and the effects, while FM and VA may follow by firmware. Voice pool, patch format, UI, and the engine API support all four engines from day one.
- v1 retail price sits between R$ 17,990 and R$ 19,990, so the BOM and the tax structure must leave margin at that price.
- The founder supplies the factory library content, so no sample-production partner is assumed for v1.
- The founder and AI build the software, and the investor's partners cover only the physical build. Scope must fit one developer.

## Non-goals

- Running, porting, or staying compatible with the Korg Kronos OS, OA.ko, or Kronos patches and sounds.
- A sequencer in v1, which is reserved for the Pro tier.
- DRM that claims to be unbreakable.
- Encrypting user-imported SFZ or SF2 content.
- More than 8 simultaneous parts in v1.
- Bluetooth or Wi-Fi in v1.
- A user-replaceable or user-accessible NVMe.
- An x86_64 product, which exists only as a fallback platform.
- A partner library store in v1. The format supports it later.
- Retail distribution for the first ~100 units, which are sold direct.
- SFZ scripting and SFZ effect buses.
- Motorized faders in v1, which are reserved for the Pro tier.
- A fully modeled acoustic piano. Acoustic pianos stay sample-based (see `rejected-approaches.md`).
- Removing the sampled electric pianos from the factory plan.
- Resonance on user-imported SFZ or SF2 instruments in its first release.
- Resonance that crosses parts of a Live Set.
- Modeling una corda, lid position, or microphone placement.

## Success signal

- **Milestone 1 (month 6, investor demo):** on an RK3588 dev board, the sample engine plays 256 stereo voices streamed from a library of up to 1 TB, at ≤5 ms latency, with zero dropouts over 24 h.
- **v1 (month 18):** a production unit boots in ≤10 s, plays the factory piano and the organ with its rotary speaker live on stage, and records into a laptop over USB-C. The first ~100 units sell direct at R$ 17,990–19,990.
- **Modeled keys (firmware after v1):** a shipped unit updates and plays a modeled tine and reed electric piano and a Factory Grand with resonance in one 8-part Live Set, with no dropouts, and both pass their blind panel protocols.

## Assumptions

- The engine runs internally at 48 kHz.
- The stereo line input is for future sampling and play-along. There is no sampling feature in v1.
- The 73SW keybed has a waterfall profile and spans E to E.
- The 73SW shares the key geometry of Fatar's 61 TP/9 drawing (23.6 mm white key pitch, 10.4 mm keybed frame per side). `mechanical.md` uses those values until the 73SW drawing arrives.
- The keybed's contact count (2 or 3) and whether it has aftertouch are still unknown, so the scan MCU handles both cases.
- The low-volume BOM is about US$ 1,200–1,600 and the landed cost about R$ 11,000. These are forge estimates, not verified.
- The modeled electric piano's floor of ≥64 voices matches VA. Board measurement confirms or changes it.
- The founder can reach a well-maintained tine and reed instrument for measurement, as the rights record implies.

## Open Questions

- Do the tax costs (II, IPI, ICMS, PIS/COFINS, ex-tarifário) on imported parts leave margin at the v1 price? This needs a tax advisor.
- What minimum order and lead time does Fatar require for OEM resupply beyond the initial ~100 keybeds?
- Does the 73SW stock have 2 or 3 contacts per key, and does it have aftertouch?
- Which Inmetro requirement applies to the chosen external power supply, and what voltage and current does it deliver?
- What are the 73SW's mechanical drawing, waterfall face height and fixing holes? They close the keybed values marked "to confirm" in `mechanical.md`.
- What display glass and cut-out size does the chosen 7-inch panel need? It fixes the panel zone depth, and with it the total depth (provisionally 304.4 mm).
- Does MIPI DSI work on mainline RK3588 for the 7-inch panel, or is an internal HDMI panel needed?
- sfizz was archived upstream (final 1.2.3, BSD-2). The architecture vendors its SFZ parser (AD-15). Do the licenses of the vendored parts and their dependencies allow closed-product use?
- Which MIDI CC numbers does the Hammond M-solo use for drawbars? The Drawbar Data List on pp. 64–65 of the manual is an image.
- Can the founder record a real tonewheel B-3 through a real rotary speaker cabinet for organ reference? This is recommended, not blocking.
- Which real tine and reed instruments supply the final CAP-24 reference recordings?
- Which attorney runs the patent search and opinion for CAP-24 and CAP-26? `ip-and-references.md` holds the preliminary search.
- Is the Brazil launch of CAP-26 clear, as INPI suggests, and what would export need against the Yamaha, Kawai, Casio, and Modartt patents in `ip-and-references.md`?

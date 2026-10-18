# Cross-document consistency review — Ladiani Velora 73 UX

Reviewed 2026-10-18. Scope: DESIGN.md, EXPERIENCE.md, .memlog.md (where entries conflict, the later one wins), mockups/*.html, the upstream SPEC.md (plus its companions hardware-platform.md, glossary.md, performance-targets.md, engines.md), ARCHITECTURE-SPINE.md and HARDWARE-INTERFACES.md. Nothing was edited.

**Verdict: PASS WITH FIXES.** The two spines agree with each other. They also agree with every decision in the last ~40 memlog entries: the triage block, MENU launcher, 500 ms long press, footswitch, per-Live-Set ASSIGN with a global default, 0.5 s Write hold, pickup ghost in warn, 4.5:1 contrast floor, Studio muted #606a62 and warn #7a6620, and 56 px touch targets. No critical issues. The remaining issues are gaps that Upstream Feedback does not yet capture, a few contradictions inside DESIGN.md, and mockups that still show superseded values.

| Severity | Count |
|---|---|
| Critical | 0 |
| High | 3 |
| Medium | 12 |
| Low | 17 |
| **Total** | **32** |

## Checks that passed

- **Launcher list.** The memlog has two lists. Line 70 includes "Part". Line 89 is later, replaces "Part" with "Setlist", and supersedes line 70. Both spines use the later list: Setlist Browser, Setlist, Library, Effects, Scenes, Import, Settings (DESIGN L425, L467; EXPERIENCE L42, L177). panel-v2.html matches.
- **Component names.** DESIGN frontmatter `components`, DESIGN Components and EXPERIENCE Component Patterns each list the same 35 components with the same names.
- **Token references.** Every `{group.token}` reference in both spines resolves to a frontmatter token. None is unresolved.
- **Counts.** 8 parts, up to 8 scenes, 17 categories, 9 faders (1–8 + REV) and 8 encoders are consistent across the spines and mockups. The 17 category labels in EN, PT-BR and ES match key-screen-drawbars-categories.html.
- **Toggles.** DRAWBARS/MIXER, PARTS/SCENES and PART/ALL/ASSIGN (3-state cycle) are consistent in both spines.
- **Themes.** Stage and Studio only, with no Auto, in EXPERIENCE L27 and DESIGN's Segmented Control. No mockup shows an "Auto" theme.
- **Languages.** English is the default; PT-BR and ES are selectable. EXPERIENCE (Foundation, Localization) and key-screens-device.html §5 agree.
- **Contrast figures.** DESIGN L404–406 was recalculated with WCAG 2.x luminance and every listed value is correct. For example, Studio muted/surface is 4.53, warn/surface 4.52 and accent/surface 4.56. The old values failed (#677269/surface 4.04, #8a7424/surface 3.68). The one exception is the panel legend; see M10.

---

## High

**H1 — Boot restores the "last sound used", which conflicts with the spec's "default program" and the architecture's write and preload rules. Upstream Feedback does not capture this.**
- **Where:** EXPERIENCE L40 (Boot: "restores the last sound used"), L142 ("≤10 s to playable (spec CAP-16)"), Flow 1 step 1; memlog L21 and L78/L97 ("Boot into: last sound used, start of setlist, default Live Set"). Upstream: SPEC CAP-16 and performance-targets.md ("default program playable ≤10 s"; "Favorites' heads load first"); ARCHITECTURE AD-24 ("Heads for favorites load first"); AD-18 ("During a performance the disk only reads… Writes happen only on save, import and update"; on power-fail, `sysd` freezes writes).
- **Conflict:**
  - To restore the last sound, the current Live Set and scene must be saved somewhere. That needs a write during the performance or at power-off, and AD-18 forbids both.
  - Boot preloading must put the restored Live Set's heads first, ahead of favorites.
  - CAP-16 is written against a "default program".
- **Fix:** Add an Upstream Feedback item:
  - Boot restores the target chosen in "Boot into" (default: last sound used).
  - CAP-16 measures time to playable for that target.
  - AD-24 loads the boot target's heads first.
  - AD-18 allows a tiny journaled "last-sound pointer" write, or records the pointer whenever the Live Set or scene changes.

**H2 — The 56 px touch-target minimum contradicts the 42 px Top Bar and 48 px Status Bar, and both bars carry touch shortcuts.**
- **Where:** DESIGN frontmatter `spacing.device-topbar: 42px` and `spacing.device-status: 48px` versus `spacing.touch-target-min: 56px`; DESIGN L421 ("for every tappable element… smaller visuals get a padded hit area"); EXPERIENCE L154 (tap the setlist context in the Top Bar opens the Setlist Browser) and L161 (tap `REV` in the Status Bar opens Effects); memlog L86/L105 (56 px minimum).
- **Conflict:** A full-height target in either bar is under 56 px. A padded hit area would spill into the Live Set Hero or the part area, which are tap targets themselves.
- **Fix (latest memlog: 56 px minimum):** One of:
  - raise `device-topbar` and `device-status` to at least 56 px and rebalance `device-hero` and the part area; or
  - state that each bar target extends to 56 px only into a non-interactive zone, and say which zone.

**H3 — The arpeggiator (spec CAP-19) has no UX. The Studio mockup shows "ARP Off" anyway.**
- **Where:** SPEC CAP-19 (arpeggiator on any part, internal or external clock); ARCHITECTURE AD-21; key-screen-studio.html Part Editor "ARP Off". DESIGN and EXPERIENCE never mention the arpeggiator. The Settings row "MIDI & Clock" names clock only.
- **Conflict:** A v1 spec capability has no surface, control or state in the spines, while a mockup implies one.
- **Fix:**
  - Add arpeggiator controls to the Part surface (device) and the Part Editor (Studio). Clock source belongs in Settings › MIDI & Clock.
  - Or list the gap in Open Questions or Upstream Feedback.

## Medium

**M1 — The spines claim CAP-15's "favorite in one touch" but do not deliver it.**
- **Where:** EXPERIENCE Flow 3 step 2 ("★ Favorites would reach a favorite in one touch (spec CAP-15)"); SPEC CAP-15.
- **Problem:** From the Library, tapping the ★ Favorites chip and then the tile takes two touches. From Performance it takes three or more (open Library first). The old color-themes mock had a favorites row on Performance; the current Performance layout has none.
- **Fix:** Either define a one-touch favorites path (for example, a favorites strip, or a Library that opens on Favorites when the timbre name is tapped) or soften the claim and add it to Upstream Feedback.

**M2 — No device-side pairing UX.**
- **Where:** ARCHITECTURE AD-13 ("a pairing code shown on the device screen is required"); EXPERIENCE L138 and DESIGN L471 ("CONNECTED VIA USB · PAIRED"); the Settings row "Computer (Velora Studio)" is a name only.
- **Problem:** No state, surface or copy shows a pairing code on the device or an entry field in Studio.
- **Fix:** Add a "Pairing" state (device shows the code; Studio prompts for it) under State Patterns, or open a question for it.

**M3 — The 1024×600 display resolution is assumed but not specified upstream.**
- **Where:** EXPERIENCE L24 and L264; DESIGN L62 and all device px tokens. HARDWARE-INTERFACES §8 and hardware-platform.md say only "7-inch… MIPI DSI", with an HDMI fallback.
- **Fix:** Add an Upstream Feedback item: panel resolution 1024×600, needed in HARDWARE-INTERFACES §8.

**M4 — The Setlist / Live Set / Scene data model has no owner in the architecture.**
- **Where:** ARCHITECTURE AD-9 table ("Programs and performances — engine control thread"; Catalog holds "tags for libraries and programs"). Setlists, scenes and Live Set ASSIGN maps are absent.
- **Problem:** Upstream Feedback items 3, 6, 8 and 11 touch the model but never ask for a dataset owner for setlists and scenes.
- **Fix:** Extend item 11, or add an item, so AD-9 names who writes setlists, Live Sets (including ASSIGN and scenes) and setlist tags.

**M5 — The "Boot into" options are missing from the spines.**
- **Where:** memlog L78/L97 (last sound used, start of setlist, default Live Set). EXPERIENCE L51 lists "Boot into" with no values; the Boot row (L40) and Restoring state (L142) describe only the last-sound case.
- **Fix:** List the three options and note the default, last sound used (key-screens-device.html §5 shows that default).

**M6 — The `● Ready` indicator uses different tokens in different places.**
- **Where:** DESIGN frontmatter `live-set-hero.ready-indicator: '{colors.accent-dark}'` and Components L448 ("● Ready in accent"). DESIGN Colors L396 says `ok` is used for "streaming ok, ready". EXPERIENCE L127 says `● Ready` in `{colors.ok-dark}`.
- **Fix:** Use `{colors.ok-dark}` / `{colors.ok-light}` in the live-set-hero token and in the L448 prose. The hex is the same, but readiness is semantic.

**M7 — "Studio" names both the light theme and the desktop app.**
- **Where:**
  - DESIGN L406: "Studio —" means the theme.
  - DESIGN L415: "**Studio** uses a denser desktop ramp" means the app.
  - Tokens `studio-*` (`rounded.studio-sm`, `spacing.studio-gutter`, the `studio-live-set-name` typography) mean the app.
  - EXPERIENCE L27: "Studio (light)" theme versus "Velora Studio".
- **Fix:** Always write "Velora Studio" for the app. In prose, call the theme "Studio theme". Optionally rename the app tokens (`desktop-*`).

**M8 — WRITE has two meanings: a 0.5 s hold to save, and a short press to confirm.**
- **Where:** EXPERIENCE L103 and L183 (Write Dialog only after a 0.5 s hold; guard configurable), L291 ("A brief press of WRITE does nothing"); Hint Bars use a short WRITE to load, assign, start import and open a Launcher tile (L99, L107; key-screens-device.html).
- **Problem:** Not defined:
  - whether a short WRITE confirms everywhere except on Performance;
  - whether "WRITE · Yes" inside the dialog needs a hold;
  - what happens on Performance when the guard is off.
- **Fix:** Add a rule to Interaction Primitives. The memlog decision stays: hold 0.5 s to open the Write Dialog, configurable.

**M9 — Unsaved edits across several Live Sets are undefined.**
- **Where:** EXPERIENCE L105 and L134 (edits kept in memory across Live Set switches until Write or power-off); the Write Dialog scope covers only the current Live Set (L103).
- **Problem:** Not defined:
  - how the player sees or writes pending edits on a Live Set they are not on;
  - whether the ● shows in Setlist or Setlist Browser rows.
- **Fix:** Add a state row (for example, ● on List Rows and Result Tiles of Live Sets with pending edits) or an Open Question.

**M10 — The panel legend contrast claim is only true on part of the aluminum.**
- **Where:** DESIGN L406 ("legend/aluminum 4.7… Every text pair is ≥ 4.5:1"); tokens `panel-legend #4a524d`, `alu-low #b1b5b8`; DESIGN L401 (aluminum gradient alu-high → alu-low).
- **Measured:** legend/alu-mid 4.74, legend/alu-low 3.90 (below 4.5).
- **Fix:** Say the figure is measured against `alu-mid`, or darken `panel-legend` until it clears 4.5:1 on `alu-low`. These are render references only, so this goes to the hardware partner either way.

**M11 — key-screen-performance.html still uses the superseded Studio-theme hex values, and the spines do not flag it.**
- **Where:** key-screen-performance.html header comment and `.light` block (`--muted:#677269; --warn:#8a7424`). DESIGN L365 and L452 flag only the engine-type sub-label and the text-colored ghost.
- **Correct value:** muted-light `#606a62`, warn-light `#7a6620` (memlog L85/L104; DESIGN L30 and L33).
- **Fix:** Add the old light hex values to DESIGN's list of superseded items in the mock, or regenerate the mock.

**M12 — color-themes-1.html (palette evidence, variation 4) carries the superseded values.**
- **Where:** color-themes-1.html `.v4.light` (`--muted:#677269; --warn:#8a7424`; `#8a7424` is also the warn in variations 1–3). DESIGN L383 cites it as palette evidence without a supersession note. memlog L19 still records the old values.
- **Fix:** Note in DESIGN L383 that the Studio muted and warn values in this mock are superseded by `#606a62` / `#7a6620`.

## Low

**L1 — The memlog never records the final Studio muted and warn hex values.**
memlog L19 lists `#677269` / `#8a7424`. L85/L104 say only "darken… to comply". The values in DESIGN L30, L33 and L404 (`#606a62`, `#7a6620`) have no memlog entry.
- **Fix:** Append a memlog entry with the final hex values.

**L2 — color-themes-1.html uses several superseded conventions.**
- It shows "Leslie: lento", where memlog L54 and DESIGN L377 require ROTARY.
- Its UI text is PT-BR ("Programa 014", "Vozes", "Biblioteca ausente na parte 6"), where EN is the default and "Program" is a banned synonym (EXPERIENCE L77).
- Part sub-labels show engine types (Sample / Tonewheel / VA), where memlog L56 requires category.
- **Fix:** Add a supersession note, or keep the mock as rejected-palette evidence only.

**L3 — No mockup shows the ⏻ power icon on Part Strips.**
The icon is decided in memlog L70/L89 and DESIGN L450, but key-screen-performance.html lacks it, and DESIGN does not flag that.
- **Fix:** Add it to the list of mock deviations.

**L4 — key-screen-studio.html has no Unsaved Indicator in the Live Set header.**
The footer shows "UNSAVED: SCENE 2", but the header "LIVE SET 03 Perfect" has no ●. DESIGN L465 requires ● after the name in Velora Studio's header.

**L5 — key-screen-drawbars-categories.html contradicts itself.**
Its intro text says registration "88 8000 000"; the Organ Card shows "88 8600 000". The spines use `88 8600 000` (DESIGN L414).

**L6 — The Settings mock and EXPERIENCE disagree on the Display & Language rows.**
- key-screens-device.html §5 has no Reduced motion row, which memlog L79/L98 and EXPERIENCE L51/L258 require.
- The mock places "Accidental-press guard · Hold Write 0.5 s" under Display & Language, but EXPERIENCE L51 does not list the guard in the Settings structure.
- **Fix:** Add the guard to EXPERIENCE L51 under a named Settings section.

**L7 — "songs" is used where the vocabulary says "Live Set".**
DESIGN L458 ("searches setlists and songs") and key-screens-device.html ("12 songs", "SONGS · DRAG OR HOLD ▲▼", "16 SONGS") versus EXPERIENCE's fixed vocabulary (L77) and the Setlist Browser definition ("setlists and Live Sets", L44 and L97).
- **Fix:** Use "Live Sets" in UI copy.

**L8 — Setlist Browser chips and tag editing are under-specified.**
memlog L68 fixes the chips as Favorites / Recent / Setlists / All Live Sets plus user tags, with tags editable on the device. DESIGN L459 writes "★ Favorites, Recent, …". EXPERIENCE L98 mentions only tags, and no device surface hosts tag editing.
- **Fix:** List the four fixed chips and say where tags are edited on the device (for example, the Setlist surface).

**L9 — The Part surface can only be reached by touch.**
This follows from the later launcher list (memlog L89), which dropped "Part". EXPERIENCE L46 says Part is reached only by tapping a Part Strip.
- **Fix:** If that is intended, note in Interaction Primitives that Part needs touch; otherwise provide a panel path.

**L10 — "MENU again closes" the Launcher, but the destination is not stated.**
EXPERIENCE L42 and L107 do not say whether MENU returns to the previous surface or to Performance; EXIT goes to Performance.
- **Fix:** Say which.

**L11 — Notice Toast and ASSIGN Overlay can both cover the part area.**
DESIGN L425 places both there. Their stacking and precedence are undefined.

**L12 — Token hygiene in DESIGN.md.**
- Unused tokens: `typography.organ-name` (the Organ Card lists an organ name but has no `name` key), `spacing.1`, `spacing.4`.
- `part-strip` has no key for the warn "Missing library" label or the ⏻ icon.
- `live-set-hero` has no key for the muted "Loading…" text, although the prose (L448–450) specifies them.

**L13 — The memlog's "Device IA (10 surfaces)" (L34) predates the current list.**
The spines list 12 rows; Setlist Browser and Launcher were added later (memlog L68, L70/L89). Informational only; no fix needed.

**L14 — Upstream documents still say "Leslie".**
SPEC Success signal ("organ with Leslie") and engines.md ("Leslie-style") versus ROTARY (memlog L54) and SPEC's own trademark constraint. Upstream Feedback item 11 does not mention it.
- **Fix:** Add it to item 11.

**L15 — Some Upstream Feedback items name less than they should.**
- Item 5 cites AD-20 but not the architecture Deferred row "Locales beyond PT-BR — before export".
- Item 10 targets HARDWARE-INTERFACES but not AD-16's "knobs, 9 drawbars" wording.
- Item 11 names only the spec glossary. The architecture also uses "programs/performances" (AD-1, AD-9, AD-10) and CAP-16 says "default program". The glossary's "Companion app" should become "Velora Studio".

**L16 — Upstream hardware topics with no UX.**
- HARDWARE-INTERFACES pedals (continuous sustain with half-pedal, expression) are only implied by "Keyboard & Pedals".
- The Pro tier's 16 parts (CAP-7, AD-27) are not considered in the 8-strip layout.
- A USB-A stick can carry a RAUC update (AD-26), but the USB notice only offers "Import?".
- **Fix:** Add these to Open Questions.

**L17 — Upstream Feedback item 3 ("scenes store parameters only") does not say how a scene recall maps onto AD-2.**
AD-2 applies initial performance values only when `program_generation` changes. A scene recall must therefore be either a batch of performance events or a new generation.
- **Fix:** State which in item 3.

---

## Is each upstream conflict in Upstream Feedback?

| Upstream conflict | In Upstream Feedback? |
|---|---|
| Seamless switching (no spec CAP) | Yes, item 1 |
| Next-Live-Set preload (AD-24) | Yes, item 2 |
| Scenes never change instruments | Yes, item 3 (refine per L17) |
| Backup & restore, Studio offline | Yes, item 4 |
| EN default vs AD-20 PT-BR source locale | Yes, item 5 (add the Deferred row, L15) |
| ASSIGN per Live Set with a global default | Yes, item 6 |
| 17 categories, mandatory on import | Yes, item 7 |
| Setlist tags | Yes, item 8 |
| Motorized faders reserved for Pro | Yes, item 9 |
| Encoders vs "absolute pots or encoders"; dual-function sliders vs "9 sliders as drawbars"; MENU; footswitch; "one knob per function" | Yes, item 10 (add AD-16 wording, L15) |
| Program/performance terminology | Partly, item 11 (spec only; see L14, L15) |
| Boot target vs CAP-16 "default program", AD-24 favorites first, AD-18 writes | **No** (H1) |
| Arpeggiator CAP-19 has no UX | **No** (H3) |
| CAP-15 one-touch favorite | **No** (M1) |
| AD-13 pairing code on the device screen | **No** (M2) |
| 1024×600 resolution not in HARDWARE-INTERFACES | **No** (M3) |
| AD-9 owner for setlists, scenes and Live Sets | **No** (M4) |
| "Leslie" in spec and engines.md | **No** (L14) |
| Pedals, Pro 16 parts, USB-A update stick | **No** (L16) |

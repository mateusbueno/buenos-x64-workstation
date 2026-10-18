---
title: Accessibility & stage-ergonomics review — Ladiani Velora 73 UX spines
reviewed: DESIGN.md, EXPERIENCE.md (draft, 2026-10-18); mockups/ used as evidence only
date: 2026-10-18
verdict: Not ready — 1 critical, 10 high, 13 medium, 6 low
---

# Accessibility & stage-ergonomics review

## Verdict

**Not ready to hand off.** The token palette is well built. Every *plain* text/background pair in DESIGN.md meets the stated 4.5:1 floor, and the published ratios are accurate (re-computed below). The failures are in **composited and derived states**, which the spine does not measure. They include: text at 42% opacity, a ghost drawn over a fill, muted text on an accent fill, black-on-black drawbar heads, panel legends on the dark end of the aluminium gradient, and LED intensity levels. The spine also treats "1.5 m legibility" as covered by one large token. In fact only the Live Set name can be read at that distance.

The single critical issue is that **pickup feedback**, a core stage task, has no luminance contrast where it matters most and its written instruction is too small to read from the playing position. The high findings break the spine's own floors (4.5:1, 3:1, 56 px, never colour-only), or leave a stage or desktop requirement unspecified.

Method: WCAG 2.x relative luminance computed from the frontmatter hex. Opacity is composited over the stated background. CVD simulation uses Machado et al. 2009 at severity 1.0, with ΔE*76 in CIELAB. Visual angle uses the 7" 1024×600 panel: 169.5 ppi, 0.150 mm/px, cap height ≈ 0.68 em for a Gill-Sans-like face. Reviewer's legibility heuristic: **≥ 10 arcmin cap height for anything that must be read at a glance from 1.5 m** (≈ 43 px), and **≥ 12 arcmin at 0.7 m playing distance for text read mid-song** (≈ 24 px). For reference, ISO 9241-303 asks for 16 arcmin minimum for sustained reading.

---

## 1. Computed contrast

### 1.1 Plain pairs (all pass the text floor; spine figures confirmed)

| Pair | Stage | Studio |
|---|---|---|
| text / bg · surface · raised | 16.20 · 14.74 · 12.81 | 16.37 · 14.59 · 18.10 |
| muted / bg · surface · raised | 6.13 · 5.58 · 4.85 | 5.09 · **4.53** · 5.62 |
| accent / bg · surface · raised | 9.34 · 8.50 · 7.39 | 5.12 · **4.56** · 5.65 |
| on-accent / accent | 9.34 | 5.12 |
| warn / bg · surface · raised | 9.90 · 9.02 · 7.83 | 5.07 · **4.52** · 5.60 |
| drawbar ink / head (brown · white · black) | 5.18 · 12.94 · 12.94 | (undefined) |

Bold = passes with < 0.1 margin (see M4).

### 1.2 Composited / derived pairs (not measured in the spine)

| Pair (where) | Stage | Studio | Floor | Result |
|---|---|---|---|---|
| text @ 42% on surface (Part Strip **off**, timbre name) | 3.58 | 2.61 | 4.5 | **FAIL** |
| muted @ 42% on surface (Part Strip off, category) | 2.01 | 1.73 | 4.5 | **FAIL** |
| muted on accent (Scene Tile sub-label on the *current* tile, as the spine literally says "sub-label in muted") | 1.52 | 1.01 | 4.5 | **FAIL** |
| on-accent @ 75% on accent (mockup's current-tile sub-label) | 5.79 | 3.67 | 4.5 | Studio **FAIL** |
| warn vs accent (Pickup Ghost over Volume Bar level fill) | 1.06 | 1.01 | 3.0 | **FAIL** |
| warn vs line (ghost over empty track) | 6.67 | 3.69 | 3.0 | pass |
| accent vs line (level fill vs track) | 6.30 | 3.73 | 3.0 | pass |
| line vs bg · surface · raised (borders: search field, empty slot, volume track, chip outline) | 1.48 · 1.35 · 1.17 | 1.37 · 1.22 · 1.52 | 3.0 | **FAIL** where the border is the only cue |
| raised vs surface (selection "lift") | 1.15 | 1.24 | — | not a usable cue by itself |
| drawbar black head vs slot (surface-dark) | 1.08 | — | 3.0 | **FAIL** |
| drawbar-black-edge vs slot | 1.67 | — | 3.0 | **FAIL** |
| drawbar brown head vs slot | 2.85 | 5.02 | 3.0 | Stage **FAIL** |
| drawbar white head vs surface-light / raised-light | — | 1.02 / 1.27 | 3.0 | **FAIL** (light theme undefined) |
| panel-legend vs alu-high · alu-mid · alu-low | 5.91 · 4.74 · **3.90** | | 4.5 | **FAIL** on alu-low (the spine reports only 4.7) |
| panel-ink vs alu-high · alu-low | 9.63 · 6.36 | | 4.5 | pass |
| led-on vs led-off (render) | 1.82 | | — | weak; see H8 |
| led-dim vs led-off (render) | 1.93, and **dim is darker than off** | | — | semantic inversion; see M9 |
| button-edge vs button-cap | 2.22 | | 3.0 | fail (cap outline on panel) |
| fader-track vs alu-mid | 2.31 | | 3.0 | fail (low, physical) |

### 1.3 Colour-vision deficiency (ΔE*76 after simulation)

| Pair | Normal | Protan | Deutan | Tritan | Note |
|---|---|---|---|---|---|
| Stage accent vs warn | 36.6 | 34.6 | 36.1 | 28.8 | OK, but by chroma only: equal luminance (9.3 vs 9.9 on bg) |
| Studio accent vs warn | 36.6 | 31.9 | 32.6 | 32.9 | OK by chroma; luminance 1.01:1 |
| Stage accent vs muted | 15.6 | 14.4 | 13.3 | 14.6 | marginal; luminance 1.52:1 |
| **Studio accent vs muted** | 13.9 | 7.4 | **4.7** | 11.5 | **near-identical for deuteranopes, luminance 1.01:1** |
| Stage warn vs muted | 41.8 | 41.6 | 42.3 | 27.1 | OK |
| Drawbar brown vs black | 35.8 | 31.1 | 35.3 | 36.9 | OK on its own (but the black head vanishes into the slot) |

Conclusion: Sálvia accent vs warn yellow stays separable for protan/deutan viewers, because warn keeps chroma while accent collapses to grey-beige. However, **accent vs muted is not separable in the Studio theme under deuteranopia**. Every place where "accent text" alone marks a state is therefore colour-only in practice (H9). Accent vs warn also differs only in chroma, which veiling glare erodes first (M5).

### 1.4 Visual angle of the device type ramp

| Token | px | Cap mm | @1.5 m | @0.7 m |
|---|---|---|---|---|
| live-set-name | 53 | 5.40 | 12.4′ ✓ | 26.5′ |
| modal-title | 27 | 2.75 | 6.3′ | 13.5′ |
| organ-registration | 25 | 2.55 | 5.8′ | 12.5′ |
| next-title | 22 | 2.24 | 5.1′ | 11.0′ |
| list-item / drawbar-value | 19 | 1.94 | 4.4′ | 9.5′ |
| part-name | 17 | 1.73 | 4.0′ | 8.5′ |
| scene name (mock 1.6cqw) | 16 | 1.63 | 3.7′ | 8.0′ |
| bar-label (Top Bar mode mirror) | 15 | 1.53 | 3.5′ | 7.5′ |
| status (pickup instruction, warnings) | 14 | 1.43 | 3.3′ | 7.0′ |
| meta-caps (category, K1–K8, footage) | 12 | 1.22 | 2.8′ | 6.0′ |

Only the Live Set name clears 10′ at 1.5 m. Nothing below next-title clears 12′ even at 0.7 m.

---

## 2. Findings

### Critical

**C1 — Pickup feedback fails at stage distance and in the common case.**
*Location:* DESIGN.md Components › Pickup Ghost, Volume Bar, Status Bar; frontmatter `pickup-ghost`, `typography.status`; EXPERIENCE.md State Patterns › Pickup; Flow 1 step 4.
*Problem:* After a scene recall the fader usually sits *below* the stored value (Flow 1: "Fader 3 sits low"). The ghost is then drawn **inside the accent level fill**. Warn vs accent is **1.06:1 (Stage) / 1.01:1 (Studio)**, so the ghost is separable by hue alone. Hue is the first thing glare removes, and in Studio theme it is weak for deuteranopes. The redundant text cue is the 14 px uppercase status line. That is 3.3′ at 1.5 m and 7′ at 0.7 m, so it cannot be read mid-song. The same applies to the drawbar ghost on a lit rod. The spine's "never colour-only: pickup is dashed ghost + written instruction" holds on paper but fails in use.
*Fix:*
1. Draw the ghost as a 3 px warn dash with a **2 px keyline in the theme bg on both sides** (`#0b0d0c` / `#f2f4f2`). The keyline-to-fill contrast is 9.34:1 (Stage) and 5.12:1 (Studio), whatever the hue. Add a small ▲/▼ chevron at the strip edge showing which way to move.
2. While a pickup is pending, the Status Bar warn line **replaces the other status items** and is set at **≥ 24 px** (12.2′ at 0.7 m, fits the 48 px bar), prefixed with a non-colour glyph (⚠ or ▲/▼): `▲ FADER 3 → 65%`.
3. Optional: outline the affected Part Strip / Drawbar Column with a 2 px dashed warn border, so the cue is spatial as well as chromatic.
4. Add "ghost over fill" and "ghost over glare" rows to the measured-contrast table.

### High

**H1 — Off parts drop below the text floor.**
*Location:* DESIGN.md frontmatter `part-strip.off-opacity: 0.42`; Components › Part Strip.
*Problem:* At 42% opacity the timbre name is 3.58:1 (Stage) / 2.61:1 (Studio) and the category is 2.01 / 1.73. Off parts are not disabled controls: the player has to read which part to bring in next.
*Fix:* Drop the opacity treatment. For off parts, render name and category in **muted at full opacity** (5.58:1 Stage, 4.53:1 Studio; or ≥ 5.0 with the M4 tokens). Hollow the LED dot (outline ring in `line-strong`, see M1). Add a meta-caps `OFF` tag in place of the volume value. State then reads by text, dot shape and tone.

**H2 — Current Scene Tile sub-label is unspecified on the accent fill (and fails as written).**
*Location:* DESIGN.md Components › Scene Tile ("sub-label in muted"); mockups `.scenes .cur small { opacity:.75 }`.
*Problem:* Muted on accent is 1.52:1 / 1.01:1. The mockup's on-accent at 75% gives 5.79 (Stage) but **3.67 (Studio)**.
*Fix:* State the rule explicitly: on the current tile, number, name and sub-label all use **on-accent at 100%** (9.34:1 Stage, 5.12:1 Studio). Never apply opacity to text on accent fills.

**H3 — Performance bars are below the 56 px touch floor.**
*Location:* DESIGN.md frontmatter `device-topbar: 42px`, `device-status: 48px`, `device-hint: 54px`; EXPERIENCE.md Interaction Primitives (tap setlist context, tap `REV`).
*Problem:* The tappable setlist context (42 px = 6.3 mm) and `REV` (48 px = 7.2 mm) cannot meet the spine's own 56 px rule. Their hit areas cannot be padded vertically without overlapping the Hero or the Part Strips. Hint Bar keycaps are 54 px.
*Fix:* Set Top Bar **56 px**, Hero **118 px** (from 132; the 53 px name still fits at line-height 1.2), Status Bar **56 px**, part area **298 px** (from 306), Hint Bar **56 px**. Re-check: 56 + 118 + 72 + 298 + 56 = 600. Hit areas must not overlap an adjacent target. Alternatively, make Top/Status Bar items non-tappable and move the shortcuts to the Launcher.

**H4 — No guard against accidental touch on Performance.**
*Location:* EXPERIENCE.md Component Patterns › Part Strip, Scene Tile; Performance touch shortcuts; Accessibility Floor › Accidental-press guard.
*Problem:* The player's hand passes the screen constantly. A brush on ⏻ **mutes a part instantly**. A brush on a Scene Tile changes the sound. A brush on the card body or timbre name navigates away from Performance mid-song. The timbre name is a target nested inside the card body (≈ 115 × 306 px strip with three targets, 17 px name ≈ 40 px tall < 56 px). Tiles are only 8 px (1.2 mm) apart.
*Fix:*
- Add **Settings › Display › Touch on Performance: Full / Safe / Off**, defaulting to **Safe**. In Safe mode, ⏻ and Scene Tiles act on **release after ≥ 150 ms contact**, with no movement over 10 px. Navigation targets (card body, timbre name) require a 500 ms long press, and long-press-to-select-knob-target moves to a double-tap or to the panel only.
- Remove the nested timbre-name target. Library stays reachable from the Part surface.
- Raise inter-target spacing on Performance to **≥ 12 px (1.8 mm)**.
- Show a brief visual "armed" state on press-down.

**H5 — No brightness control is specified, and there is no quick access.**
*Location:* EXPERIENCE.md Settings row (Display & Language lists theme, reduced motion, large text, Boot into, but not brightness); Accessibility Floor says the player chooses "screen brightness in Settings"; Foundation (no light sensor).
*Problem:* With no light sensor, the player has to fix glare (outdoor or very bright stage) and blinding (dark stage, dark theme at full backlight) by hand, and quickly. The spine also sets no hardware floor for luminance or anti-glare.
*Fix:*
- Add **Screen brightness (5–100%)** and **Panel LED brightness (3 steps + Auto-follow screen)** to Settings › Display.
- Add a **quick path**: hold EXIT + turn VALUE adjusts screen brightness from any surface, with an overlay that never covers the Hero.
- Add to the hardware hand-off (Deferred): **≥ 1000 cd/m²** panel, optically bonded, anti-glare/anti-reflection cover glass (≤ 2% reflectance), backlight dimmable to ≤ 5 cd/m².
- Add guidance copy in Settings: "Studio theme reads best in daylight; Stage theme in dark venues."

**H6 — Panel legends fail contrast on the dark end of the aluminium.**
*Location:* DESIGN.md frontmatter `panel-legend: #4a524d`; Colors ("Panel — legend/aluminum 4.7").
*Problem:* 5.91 on alu-high, 4.74 on alu-mid, **3.90 on alu-low**. Brushed aluminium also produces specular highlights under stage lights, which collapse contrast further. Legends are unlit, so they are unreadable on dark stages.
*Fix:*
- Set `panel-legend: #3b413d`, giving **7.66 / 6.14 / 5.06** against alu-high/mid/low (`#414844` is the minimum, at 4.55 on alu-low). Or use `panel-ink #2b322e` (9.63 / 6.36).
- Specify a matte or bead-blasted legend band behind each legend row.
- Specify a minimum legend cap height of **≥ 2.5 mm** (readable at 0.7 m) in the hardware hand-off.
- See M10 for dark-stage illumination.

**H7 — Drawbar black heads vanish into the slot.**
*Location:* DESIGN.md frontmatter `drawbar-black`, `drawbar-black-edge`, `drawbar-column.slot-background`.
*Problem:* The black head is 1.08:1 against the surface-dark slot and its edge is 1.67:1. The head position, which is the graphic cue for 2⅔′, 1⅗′ and 1⅓′, is invisible. The numeric value mitigates but does not replace it. Brown heads are 2.85:1.
*Fix:*
- Set `drawbar-black-edge: #667069` as a 2 px border: **3.45:1** vs slot, 3.00 vs raised, 3.20 vs the black head.
- Set `drawbar-brown: #7f5e47`, giving **3.04:1** vs slot; its ink `#f1e9df` stays 4.86:1.
- Light theme (Open Question): give white heads a 2 px `#7d8680` edge (3.03:1 vs surface-light, 3.75 vs raised-light).

**H8 — Panel state relies on LED intensity alone, and is not readable in sunlight.**
*Location:* EXPERIENCE.md Panel toggles (SCENES: "existing scenes weak LED, current scene strong LED"); DESIGN.md Part/Scene Button, `led-on/led-dim/led-off`.
*Problem:* Three intensity levels (off / weak / strong) on the same colour. In daylight, weak looks off, and strong vs weak is a brightness judgement. The sage LED colour (rendered #9fbaa8) suggests a low-saturation emitter, which washes out first. The rendered on vs off is only 1.82:1. The spine's "never colour-only" claim does not cover intensity-only cues.
*Fix:*
- Make **current = solid strong**, **existing = dim**, **empty = off**, and **current additionally pulses once (200 ms dip) on recall**.
- Specify in the hardware hand-off: diffused LED ≥ 3 mm, luminous intensity sized for **≥ 10:1 on:off ratio at 50 klx ambient** (e.g. ≥ 150 mcd at full drive), dim level fixed at ~15% PWM, and user LED brightness (H5).
- The screen remains the authoritative mirror: the Scene Tile row already encodes current by fill and position.

**H9 — "Now playing" and other states use accent text alone.**
*Location:* DESIGN.md Components › List Row ("now playing row in accent text"), Studio Part Strip / Organ Card / Category Chip ("category in accent", "✓ suggested in accent").
*Problem:* Accent vs muted is 1.52:1 (Stage) / **1.01:1 (Studio)** in luminance, and ΔE 4.7 for deuteranopes in Studio. "Now playing" is therefore a colour-only state.
*Fix:*
- Prefix the now-playing row with a **▶ glyph** and a meta-caps `NOW` tag.
- Keep "✓ suggested" (it already has the ✓).
- Treat category-in-accent as decorative: it is fine because the text itself carries the meaning.
- Add the rule to Do's and Don'ts: "accent text must never be the only marker of a state."

**H10 — Velora Studio accessibility is unspecified, and its type ramp is too small.**
*Location:* EXPERIENCE.md Velora Studio surfaces, Component Patterns (Studio Knob, List Row drag), Responsive & Platform; DESIGN.md `studio-body 12px`, `studio-section-label 10px`, mock 10–11 px metas.
*Problem:* There is no keyboard-navigation model, no screen-reader labelling, no focus indicator, no OS text-scaling behaviour, and no non-drag alternative for reordering or for the knobs. The Unsaved ● is glyph-only. The fixed four-column Part Editor at a 1280×800 minimum will break at 200% text.
*Fix:* add a Studio accessibility section:
- **Keyboard.** Every control reachable by Tab in visual order. F6 cycles regions (sidebar, Live Set, scenes, parts, editor, footer). Arrow keys move within the scene and part grids.
- **Knobs.** Studio Knob = ARIA `slider` with name, unit and `aria-valuetext`. ↑/↓ step by 1, Shift ±10, PgUp/PgDn, Home/End. Value entry with Enter.
- **Reorder.** Reorder via keyboard (Alt+↑/↓) and a "Move to…" menu (WCAG 2.5.7).
- **Focus.** Ring: 2 px `accent` + 2 px offset. Accent-dark is 8.50 / 7.39 on surface/raised. Accent-light is 4.56 / 5.65; use `#466853` (≥ 5.0) per M4.
- **Screen readers.** Live regions for connection state, "Unsaved changes", and write/backup results. Unsaved ● gets `aria-label="Unsaved changes"` and a tooltip.
- **Type scale.** All sizes in rem. Min body **13 px**, section labels **11 px** at weight 600. Honour OS text size and ⌘/Ctrl +/− to 200%. At ≥ 150% the Part Editor reflows to 2 columns, then 1.
- **Contrast preference.** Respect the OS "increase contrast" setting by swapping in the `line-strong` tokens (M1).

**H11 — Localized strings overflow fixed layouts, even in English.**
*Location:* DESIGN.md Part Strip (category meta-caps 12 px, tracked 0.12em, in a ≈ 95 px content width); Top Bar; Status Bar; EXPERIENCE.md Categories table.
*Problem:* At ≈ 8.9 px per tracked uppercase character, a Part Strip fits about 10 characters. English already overflows ("MALLET & BELLS", 14 characters). PT-BR/ES are worse: "EFEITOS & AMBIÊNCIAS" (20 ≈ 178 px), "BATERIA & PERCUSSÃO" (19), "PERCUSIÓN MELÓDICA" (18). The Top Bar (≈ 910 / 980 px in EN with a short setlist name) and the Status Bar (≈ 900 / 980 px) have no truncation rules.
*Fix:*
- Add a **short-label column (≤ 10 characters)** to the Categories table for device strips. Examples: EN MALLETS, CHOIR, FX/AMB; PT PERC.MEL., EFEITOS, BATERIA, VIOLÃO/GT; ES PERC.MEL., EFECTOS, BATERÍA. Use full labels in Library and Import.
- Define truncation per component: user names ellipsize in the middle for setlist context and at the end for timbres, with 2 lines max on Part Strips.
- The Status Bar shows only the warn line when one is active (C1).
- Require pseudo-localization (+40%, with accented capitals) in mockup reviews.

### Medium

**M1 — Meaningful boundaries use the decorative line token (1.17–1.52:1).**
*Location:* search field, empty scene/part dashed outline, volume track, inactive chip and segment outlines, Hint Bar keycaps.
*Fix:* add tokens `line-strong-dark: #5a6860` (3.03 on surface, 3.33 on bg) and `line-strong-light: #728a77` (3.01 on surface, 3.38 on bg), used wherever the border identifies a control or an empty slot. Keep `line` for hairline dividers.

**M2 — Selection relies on an unspecified accent border.**
*Location:* raised vs surface is 1.15 / 1.24, so the "lift" is invisible.
*Fix:* specify the selected border as **3 px accent on device, 2 px in Studio** (accent vs surface 8.50 / 4.56).

**M3 — Large text on Performance is undefined.**
*Location:* DESIGN.md Typography, Open Questions.
*Fix (proposed sizes):*

| Role | Normal | Large |
|---|---|---|
| live-set-name | 53 | 64 (15′ at 1.5 m) |
| next-title | 22 | 30 |
| scene name | 16 | 22 |
| part-name | 17 | 22 |
| status/warn | 14 | 24 |
| meta-caps | 12 | 16 |

Large mode drops scene sub-labels and Part Strip categories, and limits names to one line.

**M4 — Studio-theme muted, accent and warn pass by less than 0.1 margin on surface (4.53 / 4.56 / 4.52).**
*Location:* these colours are used at 10–14 px in a thin humanist face, where anti-aliasing lowers effective contrast.
*Fix:* `muted-light #5a635c` (5.02 / 5.63), `accent-light #466853` (5.03 / 5.64; on-accent 5.6+), `warn-light #725f1e` (5.02 / 5.64). These are HSL-lightness only, so hue is unchanged.

**M5 — Unsaved ● and Ready ● share a glyph and differ by hue only, at equal luminance.**
*Location:* Live Set Hero. Warn is 9.90 vs bg and accent 9.34. Glare desaturates both.
*Fix:* give Unsaved a distinct shape, e.g. **✱** or a small filled diamond **◆**, set at next-title size directly after the name. Keep ● for Ready. Studio: add `aria-label` (H10).

**M6 — Top Bar mode mirror is too small to confirm mode at a glance.**
*Location:* 15 px tracked caps (3.5′ at 1.5 m).
*Fix:* set the active mode label at ≥ 20 px in on-accent on a small accent pill, so the mode is read by shape and fill rather than colour. Inactive labels stay muted.

**M7 — Physical tactile differentiation is not specified.**
*Location:* DESIGN.md MENU Button ("same cap and edge"), Encoder, Fader, Physical panel layout.
*Fix (hardware hand-off):*
- **Nav buttons.** EXIT · MENU · WRITE are adjacent with identical caps. Give WRITE a concave cap or a raised ring, and MENU a centre dot.
- **VALUE encoder.** Larger diameter (≥ 30 mm vs ~20 mm) and knurled, unlike the 8 parameter encoders, which also carry a pointer line.
- **REV fader.** Different cap shape or colour from faders 1–8, with a centre detent on the 5th drawbar position.
- **Part/scene buttons.** Tactile nubs on buttons 1 and 5.
- **Toggle buttons.** Slightly taller than momentary buttons.

**M8 — Hardware buttons with instant stage effect have no guard.**
*Location:* LIVE SET ◀ ▶ (song change), part buttons in PARTS mode (short press mutes immediately), ROTARY STOP.
*Fix:* add Settings › Keyboard & Pedals › **Stage guard**. When on, LIVE SET ◀ ▶ needs a 300 ms hold or a double-press, and part-off needs a 300 ms hold (part-on stays instant). Defaults: LIVE SET guarded, parts unguarded. Document that WRITE hold "configurable" has a minimum of 300 ms and cannot be set to 0.

**M9 — LED token semantics are inverted and overloaded.**
*Location:* `led-dim #4b5a50` is darker than `led-off #7d8581`. `led-dim` is used both for the "weak" state (scene exists) and for the unlit encoder ring.
*Fix:*
- Rename to `led-lens-off` (unlit lens colour) and `led-weak` (a lit, low-drive colour that must render *lighter* than the lens, e.g. `#7f9a88`).
- Specify the encoder ring background as **unlit** (lens), not weak. A permanently glowing ring is distracting on dark stages and makes "weak" ambiguous.

**M10 — No dark-stage legibility plan for panel legends.**
*Location:* unlit grey legends on aluminium.
*Fix:* state the strategy in the hardware hand-off. Either (a) LED-adjacent legends only for state, plus a screen mirror (current), with an optional **legend backlight** for the nav column and toggles; or (b) recommend a gooseneck lamp port (USB-A powered). At minimum, the nav column (EXIT/MENU/WRITE, LIVE SET, SCENE) should be locatable in darkness via the tactile cues in M7.

**M11 — Motion beyond boot is not specified.**
*Location:* Toast entry/exit, overlays, the LED "blinks twice" (no organ), the Loading countdown, scene recall feedback.
*Fix:*
- Add a motion table: UI transitions ≤ 150 ms ease-out; nothing animates in the Hero.
- Reduced motion means cross-fades ≤ 80 ms or none, the Toast appears in place, and the H8 recall pulse is replaced by an instant state change.
- All blinking stays **≤ 3 Hz** (WCAG 2.3.1); "blinks twice" = 2 × 250 ms on / 250 ms off.

**M12 — Typeface legibility criteria are missing from the Deferred brief.**
*Location:* Gill Sans stand-in: identical I/l, a 1 that is close to l, and a small x-height. Labels such as `K1`, `PART 1`, `1′`, `1⅓′` depend on these glyphs.
*Fix:* add acceptance criteria for the final face:
- distinct I / l / 1 and 0 / O
- x-height ≥ 0.5 em
- open apertures
- tabular and fraction glyphs (⅓ ⅔ ⅗)
- a weight ≥ 400 for all device UI text (300 only for the wordmark)
- full Latin-1 + Latin Extended-A coverage, with accented capitals designed to fit the line box

**M13 — Accented capitals and line-height.**
*Location:* `live-set-name lineHeight 1.05`; uppercase tracked labels with diacritics (ÓRGÃO, PERCUSSÃO, ÉTNICOS); user content such as "Ação de Graças".
*Problem:* LVGL labels clip accented capitals when the line box is tight or the font conversion range omits them.
*Fix:* use line-height ≥ 1.2 for all device tokens. Build LVGL fonts with ascent measured from Á/Ç/Ñ/Ê. Add a clipping test string to QA: `ÁÇÊÕÑ Ação`.

### Low

**L1 — Number and unit formatting per locale.**
*Location:* `REV 35%`, `65%`, tempo, sizes such as `18.4 GB`.
*Fix:* follow CLDR formatting: ES `65 %`, PT-BR decimal comma `18,4 GB`.

**L2 — Hint Bar mixing.**
*Location:* control names stay English (they match the panel), verbs localize.
*Fix:* state the rule explicitly and budget verbs at +40%, e.g. "VALUE desplazar".

**L3 — Notice Toast auto-dismiss "after a few seconds".**
*Fix:* fix it at **≥ 8 s**, pause it while touched, and keep MENU › Import as the persistent path (already stated).

**L4 — Long-press threshold fixed at 500 ms.**
*Fix:* allow 400 / 500 / 800 ms in Settings for players with motor impairments, or for players who trigger it accidentally while playing.

**L5 — ASSIGN Overlay K-labels at meta-caps 12 px over 3 s.**
*Fix:* set parameter names at tile-label 16 px minimum, and highlight the turned knob's slot with the 3 px accent border (M2).

**L6 — Write Dialog backdrop.**
*Location:* dimmed 35%; background muted text drops to 1.95:1, which is intended.
*Fix:* add a note that nothing on the backdrop is interactive or needs reading. Dialog text pairs pass (text/raised 12.81, muted/raised 4.85).

---

## 3. What already works

- All plain text pairs meet 4.5:1 in both themes, and the published figures are correct. The Studio muted/warn darkening was done properly, in HSL lightness only.
- Accent vs warn stays distinguishable under protanopia and deuteranopia (ΔE ≥ 31).
- Mode state uses LED + legend + Top Bar text. The current scene uses fill + position. Warnings carry text. The 0.5 s WRITE hold and the absence of on-stage dialogs are good stage ergonomics.
- The 56 px floor (8.4 mm) is a sound choice. The Launcher, Scene Tiles (≈ 115 × 72) and Drawbar Columns comfortably exceed it.
- The reduced-motion boot frame and Studio following the OS reduced-motion setting are both specified.

## 4. Suggested additions to the spines (summary)

1. **DESIGN.md Colors.** Add a "Derived states" contrast table covering opacity, ghost-over-fill, text-on-accent, drawbar heads, panel gradient ends and LEDs. Add the `line-strong-*` tokens, the revised panel-legend, drawbar edge/brown and Studio muted/accent/warn values.
2. **DESIGN.md Typography.** Add the Large-text table, the stage-legibility tiers (glance at 1.5 m ≥ 10′; mid-song at 0.7 m ≥ 12′) and the final-face criteria.
3. **DESIGN.md Layout.** Rebalance bar heights to 56 px, and set Performance inter-target spacing to ≥ 12 px.
4. **EXPERIENCE.md Accessibility Floor.** Add sections for Touch-on-Performance mode, Stage guard, brightness and LED brightness with quick access, the motion table, and Velora Studio accessibility (keyboard, ARIA, focus, text scaling, reflow).
5. **EXPERIENCE.md Localization.** Add short category labels, truncation rules, pseudo-localization, CLDR formatting and the accented-capital test.
6. **DESIGN.md Deferred (hardware hand-off).** Add display luminance and AG/AR targets, LED intensity and dim ratio, legend cap height and matte band, and tactile cues.

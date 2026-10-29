---
name: Ladiani Velora 73
description: Visual identity of the Ladiani brand and its first model, the Velora 73 ARM64 stage keyboard — physical panel, on-device 7" LVGL UI and the Velora Studio desktop app. Elegant, luxurious, discreet.
status: final
created: 2026-10-18
updated: 2026-10-18
sources:
  - ../../../specs/spec-arm64-stage-workstation/SPEC.md
  - ../../../specs/spec-arm64-stage-workstation/engines.md
  - ../../architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md
  - ../../architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md
  - .memlog.md
colors:
  # Palette "Sálvia" (muted green-grey). Stage theme = *-dark, Studio theme = *-light.
  # Velora Studio (desktop) uses the same two sets.
  bg-dark: '#0b0d0c'
  surface-dark: '#151917'
  raised-dark: '#202622'
  line-dark: '#2b322e'
  line-strong-dark: '#5a6860'
  text-dark: '#e7ebe8'
  muted-dark: '#88938c'
  accent-dark: '#9fbaa8'
  on-accent-dark: '#0b0d0c'
  warn-dark: '#cdb86c'
  ok-dark: '#9fbaa8'
  bg-light: '#f2f4f2'
  surface-light: '#e3e8e4'
  raised-light: '#ffffff'
  line-light: '#cbd4cd'
  line-strong-light: '#728a77'
  text-light: '#131714'
  muted-light: '#5a635c'
  accent-light: '#466853'
  on-accent-light: '#f2f4f2'
  warn-light: '#725f1e'
  ok-light: '#466853'
  # Theme-invariant (no -dark/-light twin): drawbar, panel, LED and logo tokens below.
  # Drawbar heads (traditional colors, desaturated to sit inside Sálvia)
  drawbar-brown: '#7f5e47'
  drawbar-brown-ink: '#f1e9df'
  drawbar-white: '#e7e4dc'
  drawbar-white-ink: '#1d201e'
  drawbar-white-edge: '#7d8680'
  drawbar-black: '#1d201e'
  drawbar-black-ink: '#e7e4dc'
  drawbar-black-edge: '#667069'
  # Physical panel (render references for the hardware partner)
  alu-high: '#dadddf'
  alu-mid: '#c3c7ca'
  alu-low: '#b1b5b8'
  panel-ink: '#2b322e'
  panel-legend: '#3b413d'
  button-cap: '#e9ecea'
  button-edge: '#9aa19c'
  button-edge-lit: '#4b6f58'
  fader-track: '#7b827e'
  led-on: '#9fbaa8'
  led-weak: '#7f9a88'
  led-lens-off: '#7d8581'
  # Logo (monochrome master artwork)
  logo-ink: '#111111'
  logo-paper: '#f4f2ee'
typography:
  # Typeface: Lato (SIL OFL 1.1), adopted by the founder 2026-10-29 for device, Studio and panel
  # (files in ui/assets/fonts/lato/). "Gill Sans" stays only as a local fallback in the stack.
  # The logo is drawn geometry (logo/), not set in this face.
  # Device sizes are px at the native 1024x600 panel (mockups use container units; 1cqw = 10.24px).
  # Every device token uses lineHeight >= 1.2 so accented capitals (Á Ç Ê Õ Ñ) never clip.
  live-set-name:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 53px
    fontWeight: '400'
    lineHeight: '1.2'
  live-set-name-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 66px
    fontWeight: '400'
    lineHeight: '1.2'
  next-title:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 22px
    fontWeight: '400'
  next-title-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 28px
    fontWeight: '400'
  modal-title:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 27px
    fontWeight: '400'
  organ-registration:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 25px
    letterSpacing: 0.2em
    note: 'tabular numerals'
  organ-name:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 23px
  search-input:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 20px
  list-item:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 19px
  drawbar-value:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 19px
    note: 'tabular numerals'
  part-name:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 17px
    lineHeight: '1.2'
  part-name-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 21px
    lineHeight: '1.2'
  tile-label:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 16px
  tile-label-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 20px
  bar-label:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 15px
    letterSpacing: 0.25em
    note: 'uppercase'
  mode-mirror:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 20px
    fontWeight: '700'
    lineHeight: '1.2'
    letterSpacing: 0.06em
    note: 'uppercase; Top Bar mode mirror (DRAWBARS/MIXER, PARTS/SCENES, KNOBS PART n / ALL / ASSIGN)'
  status:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 14px
    letterSpacing: 0.06em
    note: 'uppercase'
  status-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 18px
    letterSpacing: 0.06em
    note: 'uppercase'
  status-takeover:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 24px
    letterSpacing: 0.06em
    note: 'uppercase; pending-pickup line that takes over the Status Bar (12 arcmin at 0.7 m)'
  status-takeover-large:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 30px
    letterSpacing: 0.06em
    note: 'uppercase'
  chip:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 14px
  meta-caps:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 12px
    letterSpacing: 0.12em
    note: 'uppercase; part number, category short label, footage, sub-labels, OFF / NOW tags'
  # Velora Studio (desktop): rem units, honour OS text size and zoom to 200%.
  desktop-live-set-name:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 1.75rem
    fontWeight: '400'
  desktop-part-name:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 0.875rem
  desktop-body:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 0.8125rem
    note: '13 px at default text size; minimum body size'
  desktop-section-label:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 0.6875rem
    fontWeight: '600'
    letterSpacing: 0.18em
    note: 'uppercase; 11 px at default text size; minimum label size'
  wordmark-brand:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontWeight: '300'
    letterSpacing: 0.2em
    note: 'LADIANI, uppercase, light, wide tracking (direction T3). Stand-in for custom-drawn wordmark'
  wordmark-model:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    letterSpacing: 0.42em
    note: 'VELORA (V1), uppercase spaced, same family, always subordinate to LADIANI'
  boot-wordmark:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    fontSize: 25px
    letterSpacing: 0.5em
  panel-legend:
    fontFamily: "'Lato', 'Gill Sans', Calibri, sans-serif"
    letterSpacing: 0.17em
    note: 'uppercase English legends; cap height >= 2.5 mm on a matte legend band; final mm with the hardware partner'
rounded:
  sm: 5px
  desktop-sm: 6px
  md: 8px
  lg: 12px
  full: 9999px
spacing:
  '1': 6px
  '2': 8px
  '3': 10px
  '4': 12px
  '5': 16px
  screen-gutter: 22px
  desktop-gutter: 18px
  desktop-sidebar: 220px
  device-topbar: 56px
  device-hero: 118px
  device-scene-strip: 72px
  device-part-area: 298px
  device-status: 56px
  device-hint: 56px
  touch-target-min: 56px
  selection-device: 3px
  selection-desktop: 2px
  focus-ring: 2px
  mode-underline: 2px
components:
  top-bar:
    background: '{colors.surface-dark}'
    foreground: '{colors.muted-dark}'
    active-mode: '{colors.accent-dark}'
    active-underline: '{spacing.mode-underline} {colors.accent-dark} under the active mode; no fill'
    type: '{typography.bar-label}'
    mode-mirror: '{typography.mode-mirror}'
    height: '{spacing.device-topbar}'
    clock-lost: '{colors.warn-dark}'
  live-set-hero:
    name: '{typography.live-set-name}'
    name-large: '{typography.live-set-name-large}'
    label: '{typography.bar-label}'
    next-title: '{typography.next-title}'
    next-title-large: '{typography.next-title-large}'
    ready-indicator: '{colors.ok-dark}'
    loading: '{colors.muted-dark}'
    divider: '{colors.line-dark}'
    height: '{spacing.device-hero}'
  scene-tile:
    background: '{colors.surface-dark}'
    border: '{colors.line-dark}'
    radius: '{rounded.md}'
    gap: '{spacing.4}'
    label: '{typography.tile-label}'
    label-large: '{typography.tile-label-large}'
    current-background: '{colors.accent-dark}'
    current-foreground: '{colors.on-accent-dark}'
    current-sublabel: '{colors.on-accent-dark} at 100% (never opacity on text over accent)'
    empty: 'transparent fill, dashed {colors.line-strong-dark} border, {colors.muted-dark} number'
    height: '{spacing.device-scene-strip}'
  part-strip:
    background: '{colors.surface-dark}'
    radius: '{rounded.md}'
    gap: '{spacing.4}'
    height: '{spacing.device-part-area}'
    selected-border: '{spacing.selection-device} {colors.accent-dark}'
    selected-background: '{colors.raised-dark}'
    on-led: '{colors.accent-dark}'
    off-led: 'hollow ring, 2px {colors.line-strong-dark}'
    off-foreground: '{colors.muted-dark} at full opacity (name and category)'
    off-tag: 'OFF in {typography.meta-caps}, in place of the volume value'
    power-icon: '{colors.text-dark}; hit area {spacing.touch-target-min}'
    missing-library: '{colors.warn-dark}'
    pickup-outline: '2px dashed {colors.warn-dark} while a pickup is pending'
    name: '{typography.part-name}'
    name-large: '{typography.part-name-large}'
    category: '{typography.meta-caps}'
  volume-bar:
    track: '{colors.line-strong-dark}'
    level: '{colors.accent-dark}'
    radius: '{rounded.sm}'
  pickup-ghost:
    stroke: '3px dashed'
    color: '{colors.warn-dark}'
    keyline: '2px {colors.bg-dark} on both sides of the dash'
    chevron: '▲ / ▼ in {colors.warn-dark} at the strip edge, pointing the way to move'
  status-bar:
    background: '{colors.surface-dark}'
    foreground: '{colors.muted-dark}'
    ok: '{colors.ok-dark}'
    warn: '{colors.warn-dark}'
    type: '{typography.status}'
    type-large: '{typography.status-large}'
    takeover-type: '{typography.status-takeover}'
    takeover-type-large: '{typography.status-takeover-large}'
    height: '{spacing.device-status}'
  hint-bar:
    background: '{colors.surface-dark}'
    foreground: '{colors.muted-dark}'
    keycap-border: '{colors.line-strong-dark}'
    keycap-radius: '{rounded.sm}'
    height: '{spacing.device-hint}'
  organ-card:
    background: '{colors.raised-dark}'
    border: '{colors.accent-dark}'
    radius: '{rounded.md}'
    name: '{typography.organ-name}'
    registration: '{typography.organ-registration}'
  drawbar-column:
    slot-background: '{colors.surface-dark}'
    slot-border: '{colors.line-dark}'
    rod: '{colors.muted-dark}'
    head-brown: '{colors.drawbar-brown}'
    head-brown-ink: '{colors.drawbar-brown-ink}'
    head-white: '{colors.drawbar-white}'
    head-white-ink: '{colors.drawbar-white-ink}'
    head-white-edge: '2px {colors.drawbar-white-edge} (Studio theme)'
    head-black: '{colors.drawbar-black}'
    head-black-ink: '{colors.drawbar-black-ink}'
    head-black-edge: '2px {colors.drawbar-black-edge} (Stage theme)'
    value: '{typography.drawbar-value}'
  list-row:
    background: '{colors.surface-dark}'
    radius: '{rounded.md}'
    selected-background: '{colors.raised-dark}'
    selected-border: '{spacing.selection-device} {colors.accent-dark}'
    current-marker: '▶ glyph + NOW tag in {typography.meta-caps}, {colors.accent-dark}'
    pending-edits: '◆ in {colors.warn-dark} after the name'
    type: '{typography.list-item}'
  search-field:
    background: '{colors.surface-dark}'
    border: '{colors.line-strong-dark}'
    caret: '{colors.accent-dark}'
    radius: '{rounded.md}'
    type: '{typography.search-input}'
  filter-chip:
    background: '{colors.surface-dark}'
    border: '{colors.line-strong-dark}'
    on-background: '{colors.accent-dark}'
    on-foreground: '{colors.on-accent-dark}'
    radius: '{rounded.full}'
    type: '{typography.chip}'
  result-tile:
    background: '{colors.surface-dark}'
    selected-background: '{colors.raised-dark}'
    selected-border: '{spacing.selection-device} {colors.accent-dark}'
    favorite-star: '{colors.accent-dark}'
    pending-edits: '◆ in {colors.warn-dark} after the name'
    radius: '{rounded.md}'
  import-row:
    background: '{colors.surface-dark}'
    radius: '{rounded.md}'
    error: '{colors.warn-dark}'
  category-chip:
    border: '{colors.line-strong-dark}'
    suggested-foreground: '{colors.accent-dark}'
    needs-category: '{colors.warn-dark}'
    radius: '{rounded.sm}'
  progress-bar:
    track: '{colors.line-dark}'
    fill: '{colors.accent-dark}'
    radius: '{rounded.full}'
  write-dialog:
    background: '{colors.raised-dark}'
    border: '{colors.accent-dark}'
    radius: '{rounded.lg}'
    title: '{typography.modal-title}'
    confirm-background: '{colors.accent-dark}'
    confirm-foreground: '{colors.on-accent-dark}'
    disabled-option: '{colors.muted-dark} with reason line'
    backdrop: 'underlying screen dimmed to 35% brightness; nothing on it is interactive'
  unsaved-indicator:
    glyph: '◆'
    color: '{colors.warn-dark}'
    type: '{typography.next-title}'
  notice-toast:
    background: '{colors.raised-dark}'
    border: '{colors.line-strong-dark}'
    foreground: '{colors.text-dark}'
    action: '{colors.accent-dark}'
    radius: '{rounded.full}'
    type: '{typography.status}'
  launcher-grid:
    tile-background: '{colors.surface-dark}'
    tile-selected-background: '{colors.raised-dark}'
    tile-selected-border: '{spacing.selection-device} {colors.accent-dark}'
    radius: '{rounded.md}'
    type: '{typography.tile-label}'
    min-target: '{spacing.touch-target-min}'
  assign-overlay:
    background: '{colors.raised-dark}'
    border: '{colors.accent-dark}'
    label: '{typography.meta-caps}'
    name: '{typography.tile-label}'
    radius: '{rounded.md}'
  segmented-control:
    border: '{colors.line-strong-dark}'
    on-background: '{colors.accent-dark}'
    on-foreground: '{colors.on-accent-dark}'
    radius: '{rounded.sm}'
  on-screen-keyboard:
    background: '{colors.surface-dark}'
    key: '{colors.raised-dark}'
    key-label: '{typography.list-item}'
    key-border: '{colors.line-strong-dark}'
    radius: '{rounded.sm}'
    min-key: '{spacing.touch-target-min}'
  category-picker:
    background: '{colors.raised-dark}'
    tile: '{colors.surface-dark}'
    tile-selected: '{spacing.selection-device} {colors.accent-dark}'
    label: '{typography.tile-label}'
    radius: '{rounded.md}'
  action-sheet:
    background: '{colors.raised-dark}'
    border: '{colors.line-strong-dark}'
    radius: '{rounded.lg}'
    title: '{typography.modal-title}'
    action: '{typography.list-item}'
    min-target: '{spacing.touch-target-min}'
  boot-screen:
    background: '{colors.bg-dark}'
    symbol: '{colors.text-dark}'
    wordmark: '{typography.boot-wordmark}'
    progress: '{colors.accent-dark}'
  studio-title-bar:
    background: '{colors.surface-dark}'
    connection: '{colors.ok-dark}'
  studio-sidebar:
    background: '{colors.surface-dark}'
    width: '{spacing.desktop-sidebar}'
    active-background: '{colors.raised-dark}'
    active-edge: '{colors.accent-dark}'
  studio-part-strip:
    background: '{colors.surface-dark}'
    radius: '{rounded.md}'
    selected-border: '{spacing.selection-desktop} {colors.accent-dark}'
    category: '{colors.accent-dark}'
    name: '{typography.desktop-part-name}'
  part-editor:
    section-divider: '{colors.line-dark}'
    section-label: '{typography.desktop-section-label}'
    body: '{typography.desktop-body}'
  studio-knob:
    body: '{colors.raised-dark}'
    arc: '{colors.accent-dark}'
    focus-ring: '{spacing.focus-ring} {colors.accent-dark}, 2px offset'
  primary-button:
    background: '{colors.accent-dark}'
    foreground: '{colors.on-accent-dark}'
    radius: '{rounded.desktop-sm}'
  studio-footer:
    background: '{colors.surface-dark}'
    foreground: '{colors.muted-dark}'
    warn: '{colors.warn-dark}'
    unsaved-glyph: '◆ in {colors.warn-dark}'
    type: '{typography.desktop-body}'
  studio-dialog:
    background: '{colors.raised-dark}'
    border: '{colors.line-strong-dark}'
    radius: '{rounded.lg}'
    title: '{typography.desktop-live-set-name}'
    body: '{typography.desktop-body}'
  studio-selection-list:
    row-background: '{colors.surface-dark}'
    row-divider: '{colors.line-dark}'
    radius: '{rounded.md}'
    checkbox: '{colors.line-strong-dark} outline, {colors.accent-dark} fill when checked'
    focus-ring: '{spacing.focus-ring} {colors.accent-dark}, 2px offset'
    meta: '{colors.muted-dark}'
    reference-tag: '{colors.muted-dark}'
    size-warning: '{colors.warn-dark}'
    body: '{typography.desktop-body}'
  studio-banner:
    background: '{colors.raised-dark}'
    edge: '4px {colors.warn-dark} left edge'
    foreground: '{colors.text-dark}'
    type: '{typography.desktop-body}'
  panel-toggle-button:
    cap: '{colors.button-cap}'
    edge: '{colors.button-edge}'
    led-active: '{colors.led-on}'
    led-inactive: '{colors.led-lens-off}'
  menu-button:
    cap: '{colors.button-cap}'
    edge: '{colors.button-edge}'
    legend: '{colors.panel-legend}'
    legend-type: '{typography.panel-legend}'
  navigation-button:
    cap: '{colors.button-cap}'
    edge: '{colors.button-edge}'
    legend: '{colors.panel-legend}'
    legend-type: '{typography.panel-legend}'
  part-scene-button:
    cap: '{colors.button-cap}'
    led-strong: '{colors.led-on}'
    led-weak: '{colors.led-weak}'
    led-off: '{colors.led-lens-off}'
    lit-edge: '{colors.button-edge-lit}'
  encoder:
    knob: '{colors.panel-ink}'
    ring-lit: '{colors.led-on}'
    ring-unlit: '{colors.led-lens-off}'
  fader:
    cap: '{colors.panel-ink}'
    track: '{colors.fader-track}'
---

## Brand & Style

> Mockups in `mockups/` are visual evidence; this spine and EXPERIENCE.md win on any conflict. Known deviations in the mocks: the Performance mock shows engine type under part names (category wins), a text-colored ghost (warn ghost with keylines wins), off parts at 42% opacity, no ⏻ icon and the old bar heights; `color-themes-1.html` is palette evidence only (its PT-BR copy, "Programa" and engine-type sub-labels are superseded); the mocks predate the ◆ Unsaved Indicator (they show ● or nothing) and the 20 px underlined Top Bar mode mirror; the Studio mock lacks the Unsaved Indicator after the Live Set name; `key-screen-drawbars-categories.html` intro says `88 8000 000` (the card's `88 8600 000` wins); the Settings mock lacks Reduced motion and the new Display rows; `key-screens-device.html` says "songs" where the UI says "Live Sets".

Ladiani is "the Apple of keyboards": a late entrant that arrives with a better product and says so quietly. The posture is **elegance, luxury, discretion** — Nord's artisanal care, Apple's restraint and build quality, Ferrari's exclusivity. Nothing shouts. The instrument should look as composed under stage light as it does in a studio, and every surface (aluminum, screen, desktop app) speaks the same calm, precise voice.

**Logo system.** The symbol (direction S1) is an "L" built from organ drawbars: one tall bar and four short bars, rounded ends. The brand wordmark LADIANI (direction T3) is a humanist-technical sans — light weight, wide tracking, "British and industrial, like a precision instrument." The model name VELORA (V1) is set uppercase and spaced in the same family. On the panel the lockup reads symbol + LADIANI, a hairline, then VELORA 73 below and smaller. At small sizes the symbol simplifies (1 tall + 2 short at 32 px, 1 tall + 1 short at 16 px). It must work in every context: engraved or silkscreened single-color on metal, on the dark 7" boot screen, as a tiny desktop icon and as a round social avatar.

- **Portfolio rule.** Velora is the first of a portfolio. Every model name is set uppercase, spaced, in the brand typeface, and is always subordinate to the LADIANI lockup.
- **Custom drawing.** System fonts are stand-ins. The final symbol, wordmark and model mark are to be custom-drawn by a type/logo designer from these directions.
- **Boot animation.** The drawbars pull out one by one (each ~0.7 s, ease-out, staggered ~0.1–0.25 s), as an organist setting a registration, then VELORA fades in. Under reduced motion the frame is static.

→ Logo directions and boot concept: [mockups/logo-drawbar-refine.html](mockups/logo-drawbar-refine.html) (chosen: S1, T3, V1, boot animation; other options shown are rejected).

**Physical product.** A brushed aluminum top, sturdy yet light, that must **not show wear marks** (the Motif XS problem is the anti-reference). End cheeks in tough plastic, as on Studiologic and Roland. Knobs and fader caps are machined anodized aluminum. Dimensions are in the spec's `mechanical.md`: a flat top, 25 mm end cheeks, and the wheels in an 80 mm block left of the keys. All panel legends in **English**. The rotary-speaker controls are labelled **ROTARY**, never "Leslie" (trademark); marketing likewise avoids third-party trademarks per the spec.

## Colors

Palette **Sálvia** — a muted green-grey, organic and calm, "with a touch of wood and nature"; the least common register among keyboards. The **Stage** theme uses the `-dark` tokens, the **Studio theme** the `-light` tokens; Velora Studio (desktop) uses the same two sets. Drawbar, panel, LED and logo tokens are theme-invariant.

→ Palette evidence: [mockups/color-themes-1.html](mockups/color-themes-1.html), variation 4 "Sálvia" (variations 1–3 and 5 rejected).

| Role | Stage (dark) | Studio theme (light) | Use |
|---|---|---|---|
| bg | `#0b0d0c` | `#f2f4f2` | Screen canvas; pickup-ghost keylines |
| surface | `#151917` | `#e3e8e4` | Bars, tiles, rows, part strips |
| raised | `#202622` | `#ffffff` | Selected part/tile, dialogs, organ card |
| line | `#2b322e` | `#cbd4cd` | Hairline dividers, progress track |
| line-strong | `#5a6860` | `#728a77` | Borders that identify a control or empty slot: search field, chips, segments, keycaps, empty slots, volume track, off-part LED ring (≥ 3:1) |
| text | `#e7ebe8` | `#131714` | Primary text |
| muted | `#88938c` | `#5a635c` | Labels, meta, hints, off-part names |
| accent | `#9fbaa8` | `#466853` | Current scene, selection edge, active mode, levels, primary action |
| on-accent | `#0b0d0c` | `#f2f4f2` | All text on accent fills, at 100% |
| warn | `#cdb86c` | `#725f1e` | Semantic only: pickup ghost and takeover line, needs-category, missing library, clock lost, unsaved-changes ◆ |
| ok | `#9fbaa8` | `#466853` | Semantic only: streaming ok, ready, connected (● is the ok glyph; `● READY` never shares its shape with the warn ◆ Unsaved Indicator) |

- **Accent is neutral by intent.** No orange (Studiologic/Roland) and no red (Nord) brand references. Accent fills appear on exactly one thing per area — the current scene, the active chip, the confirm button.
- **Warn and ok are semantic, never brand.** They never decorate. `ok` shares the accent hue; `warn` is a soft yellow. Accent and warn differ by chroma, not luminance, so a warn state always carries a glyph or text as well.
- **Drawbar heads** keep the traditional organ colors, desaturated to sit inside Sálvia: brown `{colors.drawbar-brown}` for 16' and 5⅓', white `{colors.drawbar-white}` for 8', 4', 2', 1', black `{colors.drawbar-black}` for 2⅔', 1⅗', 1⅓'. Head numerals use `{colors.drawbar-brown-ink}`, `{colors.drawbar-white-ink}`, `{colors.drawbar-black-ink}`. Stage theme: black heads get a 2 px `{colors.drawbar-black-edge}` border. Studio theme: slots sit on `{colors.surface-light}` and white heads get a 2 px `{colors.drawbar-white-edge}` border.
- **Panel colors** (aluminum gradient `{colors.alu-high}` → `{colors.alu-mid}` → `{colors.alu-low}`, ink `{colors.panel-ink}`, legends `{colors.panel-legend}`, LED states `{colors.led-on}` / `{colors.led-weak}` / `{colors.led-lens-off}`) are render references for the hardware partner, not production finish specs. `led-weak` is a lit low-drive state and always renders lighter than the unlit lens.
- **Logo master** is monochrome `{colors.logo-ink}` on `{colors.logo-paper}`; on screens it takes `{colors.text-dark}` on `{colors.bg-dark}`.

**Contrast floor.** WCAG AA **4.5:1 for all text** and **3:1 for meaningful non-text boundaries** (control outlines, drawbar heads, ghost), in both themes, in Velora Studio and on the panel's darkest aluminum stop. Opacity is never applied to text; dimmed states use `muted` at full opacity.

**Measured contrast** (WCAG 2.x relative luminance; opacity composited over the stated background).

| Pair | Stage | Studio theme |
|---|---|---|
| text / bg · surface · raised | 16.2 · 14.7 · 12.8 | 16.4 · 14.6 · 18.1 |
| muted / bg · surface · raised (incl. off-part name and category) | 6.1 · 5.6 · 4.8 | 5.63 · 5.02 · 6.22 |
| accent / bg · surface · raised | 9.3 · 8.5 · 7.4 | 5.64 · 5.03 · 6.24 |
| warn / bg · surface · raised | 9.9 · 9.0 · 7.8 | 5.64 · 5.02 · 6.23 |
| on-accent / accent (current Scene Tile number, name and sub-label) | 9.3 | 5.64 |
| line-strong / surface · bg | 3.03 · 3.33 | 3.01 · 3.38 |
| pickup-ghost keyline (bg) / accent level fill | 9.3 | 5.64 |
| drawbar brown / slot · ink on brown | 3.04 · 4.86 | 4.71 · 4.86 |
| drawbar black edge / slot · black head | 3.45 · 3.20 | (black head on slot 13.3) |
| drawbar white edge / slot · raised | (white head on slot 14.0) | 3.03 · 3.75 |
| panel legend / alu-high · alu-mid · alu-low | 7.66 · 6.14 · 5.06 | — |
| panel ink / alu-high · alu-low | 9.6 · 6.4 | — |

Studio-theme muted, accent and warn use the darker set (`#5a635c` / `#466853` / `#725f1e`) so every text pair clears 5:1, leaving margin if the final face renders thin.

## Typography

One humanist-technical sans family across panel, screen and desktop (Lato 400/700, SIL OFL; adopted 2026-10-29 — see Deferred). Roles:

- **Live Set name** `{typography.live-set-name}` is the largest element on the device and the only one read at a glance from 1.5 m.
- **Uppercase tracked labels** (`{typography.bar-label}`, `{typography.meta-caps}`, `{typography.status}`) carry system state and structure. The Top Bar mode mirror uses `{typography.mode-mirror}` (20 px bold) so mode reads at a glance. Content names (Live Sets, timbres, scenes) stay in sentence/title case.
- **Numerals** for drawbar values and registrations (`{typography.drawbar-value}`, `{typography.organ-registration}`) are tabular; the registration is written as organists say it, e.g. `88 8600 000`.
- **Velora Studio** uses a denser desktop ramp in rem (`{typography.desktop-live-set-name}`, `{typography.desktop-part-name}`, `{typography.desktop-body}`, `{typography.desktop-section-label}`); minimum body 13 px, minimum label 11 px at weight 600.
- **Wordmarks**: `{typography.wordmark-brand}` light and wide; `{typography.wordmark-model}` spaced wider and smaller.

**Stage legibility tiers** (cap height ≈ 0.68 em, 0.150 mm/px):

| Tier | Target | Tokens that meet it |
|---|---|---|
| Glance at 1.5 m | ≥ 10 arcmin (≈ 43 px) | `live-set-name`, `live-set-name-large` |
| Mid-song at 0.7 m | ≥ 12 arcmin (≈ 24 px) | `status-takeover` (pending pickup), `next-title-large`, `status-takeover-large` |

**Large text on Performance** (Settings option, ≈ +25%):

| Role | Normal | Large |
|---|---|---|
| Live Set name | `{typography.live-set-name}` 53 | `{typography.live-set-name-large}` 66 |
| Next title | `{typography.next-title}` 22 | `{typography.next-title-large}` 28 |
| Scene name | `{typography.tile-label}` 16 | `{typography.tile-label-large}` 20 |
| Part name | `{typography.part-name}` 17 | `{typography.part-name-large}` 21 |
| Status | `{typography.status}` 14 | `{typography.status-large}` 18 |
| Pickup takeover | `{typography.status-takeover}` 24 | `{typography.status-takeover-large}` 30 |

Large mode drops Scene Tile sub-labels and Part Strip categories and keeps every name to one line, so the 1024×600 skeleton does not change.

## Layout & Spacing

**Device (7", 1024×600, landscape).** Gutter `{spacing.screen-gutter}`; gaps `{spacing.2}` between list items, `{spacing.3}` inner padding, `{spacing.5}` between columns, `{spacing.1}` inside compound labels (glyph + text). **Minimum touch target** `{spacing.touch-target-min}` (≈ 8.4 mm) for every tappable element; smaller visuals get a padded hit area that never overlaps an adjacent target. On Performance, targets are at least `{spacing.4}` (≈ 1.8 mm) apart. Two screen skeletons:

- **Performance (600 px):** Top Bar `{spacing.device-topbar}` · Live Set Hero `{spacing.device-hero}` · Scene strip `{spacing.device-scene-strip}` (8 equal tiles) · Part area `{spacing.device-part-area}` (8 equal Part Strips, or Organ Card + 9 Drawbar Columns in DRAWBARS mode) · Status Bar `{spacing.device-status}`. Every tappable bar is ≥ 56 px tall.
- **All other surfaces (600 px):** Top Bar `{spacing.device-topbar}` · content (488 px) · Hint Bar `{spacing.device-hint}`. Content uses a two-column list layout (Setlist, Settings, Part, Effects, Scenes: ~1 : 1.6–2), search + chips + 3-column result grid (Library, Setlist Browser), full-width rows (Import) or the Launcher Grid (tiles in two rows, 4 + 3; tile list in EXPERIENCE.md Information Architecture).
- **Overlays.** Notice Toast sits above the Status/Hint Bar and never covers the Live Set Hero or Scene strip. ASSIGN Overlay covers the part area and takes precedence; a Toast waits until it closes. The On-screen Keyboard covers the lower content area, never the Top Bar or the field being edited. Action Sheet and Write Dialog float centred over the dimmed surface.

→ [mockups/key-screen-performance.html](mockups/key-screen-performance.html), [mockups/key-screen-drawbars-categories.html](mockups/key-screen-drawbars-categories.html), [mockups/key-screens-device.html](mockups/key-screens-device.html) (§1 Setlist, §1b Setlist Browser, §2 Library, §3 Import, §4 Write, §5 Settings, §6 Boot).

**Velora Studio (desktop).** Title bar · body (sidebar `{spacing.desktop-sidebar}` + main) · Studio Footer. Main stacks Live Set header, 8 Scene Tiles, 8 Studio Part Strips and a four-column Part Editor (Sound · Amp envelope + EQ · Effects inserts + sends · Zone & keyboard + Arpeggiator + Assign), gutter `{spacing.desktop-gutter}`. At ≥ 150% text size or zoom the Part Editor reflows to 2 columns, then 1; nothing scrolls horizontally at 200%. → [mockups/key-screen-studio.html](mockups/key-screen-studio.html).

**Physical panel (layout A).** Left: lockup, MASTER, ROTARY STOP and S/F; pitch and mod wheels on the left below. Mixer zone: 9 faders (1–8 + REV) with the DRAWBARS/MIXER toggle, 8 part/scene buttons with the PARTS/SCENES toggle. Center: 7" display. Navigation column: LIVE SET ◀ ▶, SCENE ◀ ▶, VALUE encoder, then a row of three buttons EXIT · MENU · WRITE (MENU in the middle). Right: 8 encoders in two rows (DELAY, BRIGHTNESS, ATTACK, RELEASE / LOW, MID, MID FREQ, HIGH) with the PART/ALL/ASSIGN toggle. The VALUE encoder has a push switch. MASTER is an analog level potentiometer with a Ø20 mm knob. The headphone jack sits on the front face under the wheels, and the power button sits on the rear panel. **Pro tier (16 parts):** two banks of 8 (A/B) toggled by a bank button; part buttons and faders act on the active bank; details deferred to Pro. → [mockups/panel-v2.html](mockups/panel-v2.html) (the mock shows two LEDs on the knob toggle; the approved version is 3-state PART/ALL/ASSIGN).

## Elevation & Depth

Depth is tonal, not shadowed: bg → surface → raised. The one-tone lift (raised vs surface ≈ 1.2:1) is never the only selection cue: selection always adds an accent border, `{spacing.selection-device}` on the device and `{spacing.selection-desktop}` in Velora Studio. The only shadows on the device are the Write Dialog and Action Sheet, which float over the underlying screen dimmed to 35% brightness. In Velora Studio, the OS draws window shadows; inside the window only the Studio Dialog casts one.

## Shapes

Soft, instrument-like corners: `{rounded.sm}` for small fills (volume bars, keycaps in the Hint Bar, category chips, drawbar heads, segmented control, on-screen keys), `{rounded.md}` for tiles, rows, part strips, search field and cards, `{rounded.lg}` for the Write Dialog, Action Sheet and Studio Dialog, `{rounded.full}` for filter chips, the Notice Toast pill, progress bars, LEDs and the logo's bar ends. Velora Studio inputs and buttons use `{rounded.desktop-sm}`. Empty slots (scenes, parts) are drawn with a dashed `line-strong` outline rather than a fill.

## Components

Visual specs; behavior lives in EXPERIENCE.md Component Patterns. Tokens in frontmatter `components`; Stage theme values shown, the Studio theme swaps each `-dark` token for its `-light` twin (theme-invariant tokens listed in Colors).

| Component | Anatomy & appearance |
|---|---|
| **Top Bar** | Left: context (e.g. `SETLIST · WEDDING ANA & PEDRO · 3 / 12`, or `NO SETLIST`). Right: the mode mirror in `{typography.mode-mirror}` (20 px bold) — DRAWBARS/MIXER, PARTS/SCENES, `KNOBS PART n / ALL / ASSIGN` — with the active state in accent text and a `{spacing.mode-underline}` accent underline, no fill (keeps one accent fill per area), then tempo with its clock source (`♩ 72`, `♩ 72 · DIN`, `♩ 72 · USB`); clock lost reads `⚠ CLOCK LOST` in warn. |
| **Live Set Hero** | Small label `LIVE SET 03`, the Live Set name in `{typography.live-set-name}` followed by the Unsaved Indicator when edits are pending; right-aligned `NEXT`, next title and `● READY` in ok (or `Loading <name>… Ns` in muted while a jump target loads; `End of setlist` on the last song). Hairline divider below. |
| **Scene Tile** | Number + name, sub-label (e.g. "piano + pad") in muted. Current scene is the only accent-filled tile; its number, name and sub-label are all on-accent at 100%. Empty slots dashed (`line-strong`) with a muted number. Also used in Velora Studio's Live Set header. |
| **Part Strip** | Part number + LED dot, ⏻ power icon (hit area ≥ `{spacing.touch-target-min}`), timbre name (up to 2 lines), **category short label** (not engine type) in meta caps, Volume Bar. Selected: accent border + raised. **Off:** name and category in muted at full opacity, hollow `line-strong` LED ring, `OFF` tag in place of the volume value. Empty: "—" / "empty". Missing library: "Missing library" in warn in place of the category. Pending pickup: 2 px dashed warn outline. |
| **Volume Bar** | Vertical track in `line-strong` with accent level fill showing the part's stored volume. |
| **Pickup Ghost** | 3 px dashed **warn** line with a 2 px bg keyline on both sides, across the Volume Bar or drawbar slot at the physical control's position, plus a ▲/▼ chevron at the strip edge showing which way to move. The keyline keeps it visible over the accent fill whatever the hue. Same treatment on every screen. |
| **Status Bar** | `VOICES 142 / 256`, `● STREAMING OK` (ok), `ROTARY SLOW`, `REV 35%`, `STUDIO` while Velora Studio is live-editing. **Pickup takeover:** while a pickup is pending, a single warn line in `{typography.status-takeover}` replaces every other item, prefixed with the direction glyph (`▲ FADER 3 → 65%`). Other warnings (missing library, clock lost) use the same takeover with `⚠`. |
| **Hint Bar** | Keycap-outlined (`line-strong`) physical control names (`VALUE`, `WRITE`, `EXIT`, `KEYS`) + verb; right-aligned reassurance or warning. |
| **Organ Card** | `PART 4 · DRAWBARS`, organ name in `{typography.organ-name}`, category `ORGAN` in accent, percussion/vibrato/rotary meta, numeric registration. Raised with accent border. |
| **Drawbar Column** | Footage label (16' … 1'), slot with rod and colored head showing 0–8, numeric value below. Nine across. Head edges per Colors (black edge in Stage theme, white edge in Studio theme). |
| **List Row** | Name left, count/meta right; selected = raised + accent border; the now-playing row carries a ▶ glyph and `NOW` tag (accent); a Live Set with pending edits shows a warn ◆ after its name; dragged row dashed-accent outline. |
| **Search Field** | ⌕ glyph, query, accent caret, `line-strong` border, muted scope hint ("searches setlists and Live Sets"). |
| **Filter Chip** | Pill with `line-strong` outline; active chip accent-filled. Fixed chips first, then categories or user tags. |
| **Result Tile** | Name, meta caps line (`STRINGS · FACTORY`, `LIVE SET · IN SUNDAY SERVICE`), ★ in accent for favorites, warn ◆ for pending edits; 3-column grid. |
| **Import Row** | File name + size/format, Category Chip, row status (`ready` / `needs category` / `skipped: …` / `already imported`) — problem states in warn with text. |
| **Category Chip** | `E.Piano ✓ suggested` in accent; `Choose category ▾` with warn border and text. |
| **Progress Bar** | Thin pill track in line, accent fill; used for import, firmware update, transfer and boot. |
| **Write Dialog** | Title "Write changes?", scope options (Scene · Whole Live Set · Save as new…; an unavailable scope in muted with a one-line reason), name field, muted "Changed: …" line, `EXIT · Cancel` and accent-filled `WRITE · Yes`. |
| **Unsaved Indicator** | A `◆` in warn immediately after the Live Set name in the Live Set Hero (and after the name in Velora Studio's Live Set header); no text, no dialog. |
| **Notice Toast** | Non-modal pill on raised with `line-strong` border: message in text (`USB drive found`), separator `·`, action in accent (`Import?`). Also hosts the brightness overlay (label + Progress Bar). Message and action copy is sentence case (not the uppercase `status` token). Never covers the Live Set Hero or Scene strip. → [notice-toast.html](mockups/notice-toast.html) |
| **Launcher Grid** | 7 tiles (list in EXPERIENCE.md IA), tile label in `{typography.tile-label}`, each ≥ `{spacing.touch-target-min}`; highlighted tile raised + accent border. |
| **ASSIGN Overlay** | Card over the part area listing K1–K8 (meta caps) with the assigned parameter name under each in `{typography.tile-label}`; the turned knob's slot raised with accent border → [assign-overlay.html](mockups/assign-overlay.html). |
| **Segmented Control** | Inline options (e.g. Stage / Studio, Full / Safe / Off, On / Off) with `line-strong` outline; active segment accent-filled. Also the on/off control in Settings. |
| **On-screen Keyboard** | Locale layout with accented letters reachable, keys ≥ `{spacing.touch-target-min}` on raised with `line-strong` edge, Done key accent-filled. |
| **Category Picker** | Raised panel with the 17 categories as a grid of full localized labels; highlighted tile accent-bordered; "Apply to all" row in Import batches. → [category-picker.html](mockups/category-picker.html) |
| **Action Sheet** | Raised card, `{rounded.lg}`, title in `{typography.modal-title}`, 2–4 actions as rows ≥ 56 px with their control (`WRITE · Replace timbre`, `EXIT · Cancel`); used for missing-library choices, pairing code, firmware install and `★ Add to Favorites` (long press on a Result Tile) → [action-sheet.html](mockups/action-sheet.html). |
| **Boot Screen** | Centered symbol and VELORA on bg, thin Progress Bar, muted status (`RESTORING · PERFECT · SCENE 2`, `UPDATING TO 1.3`). |
| **Studio Title Bar** | App name, connection status in ok (`● VELORA 73 · CONNECTED VIA USB · PAIRED`), buttons Import, Backup, Primary Button "Write to Velora". |
| **Studio Sidebar** | Sections SETLISTS (nested Live Sets), LIBRARY (by category), DEVICE (storage, firmware, backups); active item raised with accent left edge. |
| **Studio Part Strip** | `PART n` + LED, name, category in accent, level meter, `VOL n` and key range; organ parts show the registration; empty parts "+ Add timbre". |
| **Part Editor** | Four sections divided by hairlines, each titled in `{typography.desktop-section-label}`; envelope drawn as an accent polyline; insert/send chains as rows with a dashed "+ Add insert". |
| **Studio Knob** | Raised disc with accent value arc, label in caps, value below; keyboard focus shows a 2 px accent ring at 2 px offset. |
| **Primary Button** | Accent fill, on-accent text; one per window. |
| **Studio Footer** | Status line in muted: unsaved scope (`UNSAVED: SCENE 2`, ◆ in warn), `● LIVE EDIT ON`, write/transfer progress, last backup time → [mockups/studio-footer-dialog-banner.html](mockups/studio-footer-dialog-banner.html). |
| **Studio Dialog** | Modal on raised, `{rounded.lg}`, title in `{typography.desktop-live-set-name}`; used for pairing-code entry, Write to Velora scope, Restore (Merge / Replace), Export and Import → [mockups/studio-footer-dialog-banner.html](mockups/studio-footer-dialog-banner.html) and [mockups/studio-backup-restore.html](mockups/studio-backup-restore.html). |
| **Studio Selection List** | Rows on surface divided by line hairlines inside a Studio Dialog, `{rounded.md}`; each row a checkbox (line-strong outline, accent fill when checked), name, category and size in muted; "Reference only" tag in muted for factory/partner timbres; collision rows end in a Segmented Control Skip / Replace / Keep both; total size and the large-bundle warning sit under the list, the warning in warn → [mockups/studio-backup-restore.html](mockups/studio-backup-restore.html). |
| **Studio Banner** | Non-modal strip at the top of the main area, raised with a 4 px warn left edge, message + one action (Reconnect, Update, Reload) → [mockups/studio-footer-dialog-banner.html](mockups/studio-footer-dialog-banner.html). |
| **MENU Button** | Same cap and edge as the Navigation Buttons, no LED; English legend `MENU` below the cap in `{typography.panel-legend}`, centered between EXIT and WRITE. |
| **Navigation Button** | EXIT, WRITE, LIVE SET ◀ ▶, SCENE ◀ ▶: button-cap with button-edge, no LED, legend in `{typography.panel-legend}`; tactile differences per Deferred › Hardware asks. |
| **Panel Toggle Button** | Single button with one legend above and one below, an LED beside each legend; only the active mode's LED is lit. The knob toggle has three legends/LEDs (PART / ALL / ASSIGN). |
| **Part/Scene Button** | Cap with LED strip above; strong LED, weak LED (lit, low drive) or off (unlit lens); meaning per mode in EXPERIENCE.md. |
| **Encoder** | Endless encoder with an LED ring showing the value arc; the rest of the ring is unlit lens, never weak-lit. The VALUE encoder also has a push switch (behaviour in EXPERIENCE.md Interaction Primitives). |
| **Fader** | Dark cap on a grey track; non-motorized in v1. |

## Do's and Don'ts

| Do | Don't |
|---|---|
| One accent fill per area (current scene, active chip, confirm) | Use accent, warn or ok decoratively |
| Keep warn/ok strictly semantic, always with a glyph or text | Introduce orange or red brand accents |
| Show category (short label) under timbre names | Show engine type (Sample / VA / Tonewheel) as the sub-label |
| Dim off states with `muted` at full opacity + shape (hollow dot, OFF tag) | Apply opacity to text, especially over accent fills |
| Mark current/now with ▶ or a `NOW` tag as well as accent | Use accent text as the only marker of a state |
| Pickup ghost in warn with bg keylines and a ▲/▼ chevron on every screen | Different ghost colors per screen, or a ghost that relies on hue over a fill |
| `line-strong` for any border that identifies a control or empty slot | `line` hairlines as the only cue for a control |
| Text ≥ 4.5:1, boundaries ≥ 3:1; touch targets ≥ 56 px, ≥ 12 px apart on Performance | Light-theme text below AA; tiny or crowded tap targets |
| Brushed aluminum finish that hides wear; legends on a matte band | Finishes that show scratches; legends on bare specular aluminum |
| English panel legends; ROTARY | "Leslie", "Hammond", "DX7" or any third-party trademark |
| Single toggle buttons with an LED per state | Button pairs for one mode |
| Model names uppercase spaced, subordinate to LADIANI | A model mark that competes with the brand lockup |
| Final logo and typeface drawn by a designer | Ship the system-font stand-ins |
| Static boot frame under reduced motion | Motion that cannot be turned off |

## Open Questions

None open; earlier questions are resolved in .memlog.md (Studio-theme darker set, ◆ Unsaved Indicator, 20 px underlined mode mirror).

- **Design backlog.** The undrawn surfaces — Part, Effects, Scenes, Launcher Grid, On-screen Keyboard, Category Picker, Action Sheet and the Velora Studio footer/dialog/banner (Notice Toast and ASSIGN Overlay likewise have token specs but no mockup) — become design stories in the epics, each drawn before its surface is implemented.

## Deferred

Not open UX questions; owned by other parties and tracked here for handoff.

- **Final typeface**: Lato 400/700 adopted 2026-10-29 (OFL, no royalties; measured against the criteria below: x-height 0.506 em, tabular figures by default, ⅓ ⅔ ⅗, full Latin-1 + Extended-A, accented capitals span 1.10 em). Still open: visual check of I/l/1 and open apertures at 16–20 px, and legal confirmation of OFL Reserved Font Name "Lato" for subsetted LVGL glyphs. Final **logo drawing** stays with the **logo designer**. Original acceptance criteria: distinct I / l / 1 and 0 / O; x-height ≥ 0.5 em; open apertures; tabular numerals and fraction glyphs (⅓ ⅔ ⅗); weight ≥ 400 for all device UI text (300 only for the wordmark); full Latin-1 + Latin Extended-A with accented capitals that fit a 1.2 line box.
- **Physical panel specs** — engraving vs silkscreen, LED colors, aluminum finish — **hardware partners**, within the Hardware asks below. The MASTER control type was decided on 2026-10-23: an analog potentiometer with a Ø20 mm knob.
- **Desktop app icon and social avatar** renderings of the symbol — **logo designer**.

**Hardware asks** (for the hardware partners):

1. **Display:** 7" 1024×600 panel, ≥ 1000 cd/m², optically bonded cover glass with anti-glare + anti-reflection (≤ 2% reflectance), backlight dimmable to ≤ 5 cd/m² (UI range 5–100%).
2. **LEDs:** diffused, ≥ 3 mm; luminous intensity giving ≥ 10:1 on:off at 50 klx ambient (e.g. ≥ 150 mcd at full drive); weak level fixed at ~15% PWM; user brightness (3 steps) driven by firmware.
3. **Legends:** color `{colors.panel-legend}` (≥ 5:1 on the darkest aluminum), cap height ≥ 2.5 mm, printed on a matte or bead-blasted band. Dark-stage plan: tactile cues below at minimum; optional backlight for the navigation column and toggles, or a USB-A powered lamp port.
4. **Tactile cues:** WRITE with a concave cap or raised ring and MENU with a centre dot (EXIT · MENU · WRITE are adjacent); VALUE encoder larger (Ø35 mm, decided 2026-10-23; floor ≥ 30 mm), knurled and with a push switch (push-click), parameter encoders with a pointer line; REV fader cap distinct from faders 1–8; nubs on part/scene buttons 1 and 5; toggle buttons slightly taller than momentary buttons.

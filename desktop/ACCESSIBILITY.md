# Velora Studio Accessibility Audit & Release Checklist (Story 9.22)

This document records the WCAG 2.1 Level AA accessibility audit for **Velora Studio** (Tauri desktop app across macOS, Windows, and Linux), along with the mandatory accessibility checklist to be validated at every software release.

---

## 1. Executive Summary & Standards Compliance

- **Standard:** Web Content Accessibility Guidelines (WCAG) 2.1 Level AA & EN 301 549.
- **Coverage:**
  - NFR35 (Accessibility: keyboard navigation, screen reader support, contrast $\ge 4.5:1$).
  - NFR31 (Desktop app native feel and accessibility).
  - UX-DR87, UX-DR90, UX-DR91, UX-DR92 (Studio navigation, live regions, focus indicators, low vision).
- **Supported Screen Readers:**
  - macOS: VoiceOver
  - Windows: NVDA
  - Linux: Orca (best-effort)

---

## 2. Accessibility Architecture

### 2.1 Keyboard Navigation & Focus Order (WCAG 2.1.1, 2.1.2, 2.4.3)
- **Tab Flow:** Strictly mirrors visual and logical reading order from top-left to bottom-right.
- **Focus Rings (WCAG 2.4.7):**
  - High-visibility 2 px solid accent ring (`var(--accent)`) with 2 px offset (`outline-offset: 2px`).
  - Active on `:focus-visible` across every button, slider, text input, grid tile, and list row.
  - Never hidden or styled with `outline: none` without replacement.
- **Landmark Cycling (F6 / Shift+F6):**
  - Sequentially cycles between canonical landmarks:
    `Sidebar` (`#sidebar-region`) $\to$
    `Live Set Header` (`#live-set-header`) $\to$
    `Scenes Grid` (`#live-scenes`) $\to$
    `Parts Grid` (`#live-part-grid`) $\to$
    `Part Editor` (`#real-part-editor`) $\to$
    `Footer / Status Bar` (`#footer-region`).
  - Automatically adapts when dedicated subviews are open (e.g. Setlists or Device View).
- **Roving Tabindex:**
  - Used in Scene grids (1–8) and Part grids (1–16).
  - Arrow keys move focus across tiles; <kbd>Enter</kbd> selects; <kbd>Tab</kbd> exits the grid without stepping through all 16 items.
- **Modal Dialog Focus Traps:**
  - Pairing Dialog, Write to Velora Scope Dialog, Restore Modal, Export Dialog, and Import Dialog trap focus while open.
  - <kbd>Esc</kbd> key or Cancel button safely closes modal and restores focus to triggering element.

### 2.2 Screen Reader Semantics & ARIA (WCAG 4.1.2, 4.1.3)
- **Studio Knobs:**
  - Rendered with `role="slider"`.
  - Expose `aria-label` (parameter name), `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, and `aria-valuetext`.
  - `aria-valuetext` speaks the formatted value including units (e.g. `"440 Hz"`, `"120 ms"`, `"-3.5 dB"`, `"Off"`, `"Saw"`).
  - Arrow keys (<kbd>Up</kbd>/<kbd>Right</kbd> to increment, <kbd>Down</kbd>/<kbd>Left</kbd> to decrement) adjust parameters in real time.
- **Live Regions (WCAG 4.1.3):**
  - Global `#live-region` configured with `role="status"` and `aria-live="polite"`.
  - Background task completions (firmware update, backup creation, restore finish, partial export/import, diagnostics save) announce results without stealing focus.
  - Connection changes (unit connected, paired, disconnected, version mismatch) announce state immediately.

### 2.3 Low Vision, 200% Zoom & Reflow (WCAG 1.4.4, 1.4.10)
- **Zoom & Reflow at 1280×800:**
  - With app zoom or OS text scaling at 200% (effective viewport width 640 px), the layout reflows without any horizontal scrolling.
  - Part Editor (`.pe-sections`) uses `@container (max-width: 45em)` and media queries to collapse from multi-column to a single-column stacked layout.
  - `overflow-x: hidden` enforced on root containers prevents accidental horizontal panning.

### 2.4 High Contrast & Color Ratios (WCAG 1.4.3, 1.4.11)
- **Automated Color Contrast Ratios:**
  - **Stage (Dark Theme):**
    - Primary text (`#e7ebe8`) on background (`#0b0d0c`): **16.20:1** (target $\ge 4.5:1$).
    - Muted text (`#88938c`) on background (`#0b0d0c`): **6.13:1** (target $\ge 4.5:1$).
    - Text on accent button (`#0b0d0c` on `#9fbaa8`): **9.34:1** (target $\ge 4.5:1$).
    - Warning text (`#cdb86c`) on background (`#0b0d0c`): **9.90:1** (target $\ge 4.5:1$).
    - UI borders (`--line-strong`: `#5a6860`) on background: **3.33:1** (target $\ge 3.0:1$).
  - **Studio (Light Theme):**
    - Primary text (`#131714`) on background (`#f2f4f2`): **16.37:1** (target $\ge 4.5:1$).
    - Muted text (`#5a635c`) on background (`#f2f4f2`): **5.63:1** (target $\ge 4.5:1$).
    - Text on accent button (`#f2f4f2` on `#466853`): **5.64:1** (target $\ge 4.5:1$).
    - Warning text (`#725f1e`) on background (`#f2f4f2`): **5.64:1** (target $\ge 4.5:1$).
    - UI borders (`--line-strong`: `#728a77`) on background: **3.38:1** (target $\ge 3.0:1$).
- **High Contrast Mode Support:**
  - When OS Increase Contrast is enabled (`prefers-contrast: more`) or `data-contrast="more"` is set, subtle hairline borders (`--line`) automatically switch to `--line-strong` across all surfaces, ensuring every bounding container achieves $\ge 3:1$ contrast.

---

## 3. Re-usable Release Accessibility Checklist

Every production or release candidate build of Velora Studio must pass this checklist:

### 1. Keyboard Navigation
- [ ] Connect / Pair dialog can be completed, cancelled, and navigated using only <kbd>Tab</kbd>, <kbd>Enter</kbd>, and <kbd>Esc</kbd>.
- [ ] Live Set view: Arrow keys navigate scene tiles and part strips (roving tabindex).
- [ ] Part Editor: Studio Knobs increment/decrement with <kbd>Arrow</kbd> keys; numeric text fields accept typing and commit on <kbd>Enter</kbd>.
- [ ] Write to Velora Dialog: Arrow keys select write scope; <kbd>Enter</kbd> submits when focused on Write; <kbd>Esc</kbd> cancels.
- [ ] Backup & Restore: File pickers and restore confirmation actions operate without a mouse.
- [ ] Export & Import Dialogs: <kbd>Space</kbd> toggles checkbox selections; Arrow keys move between rows; <kbd>Esc</kbd> cancels.
- [ ] Device View: <kbd>Tab</kbd> navigates through all cards; close button closes view and returns focus to previous landmark.
- [ ] <kbd>F6</kbd> cycles landmarks forward; <kbd>Shift+F6</kbd> cycles backwards.
- [ ] Focus indicator (2 px accent ring with 2 px offset) is plainly visible on every focused element.

### 2. Screen Reader Verification (VoiceOver / NVDA)
- [ ] All icon buttons have non-empty `aria-label` or `title`.
- [ ] Studio Knobs announce parameter name, numeric value, and physical unit (e.g., `"Cutoff 1200 Hz slider"`).
- [ ] Modals announce their title upon opening (`role="dialog"` and `aria-modal="true"`).
- [ ] Background operations (Backup, Restore, Export, Import, Diagnostics) announce completion or error via `#live-region` without jumping keyboard focus.
- [ ] Disconnect and reconnect events announce status politely.

### 3. Visual & Zoom Inspection
- [ ] App at 1280×800 rendered at 200% zoom has **zero horizontal scrollbar**.
- [ ] Part Editor collapses to a single clean column at 200% zoom.
- [ ] When OS "Increase Contrast" is turned on, all card borders and dividers switch to `--line-strong`.
- [ ] No text is clipped or overlapping in Portuguese (`pt-BR`), Spanish (`es`), or English (`en`).

### 4. Automated Tests
- [ ] `node --test desktop/tests/accessibility_contrast.test.js` passes (verifies $\ge 4.5:1$ text and $\ge 3:1$ boundaries).
- [ ] `node --test desktop/tests/accessibility_walkthrough.test.js` passes (verifies F6 cycling, knob accessibility, and modal traps).

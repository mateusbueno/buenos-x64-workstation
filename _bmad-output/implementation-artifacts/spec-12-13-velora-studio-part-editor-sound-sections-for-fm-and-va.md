---
title: 'Story 12.13: Velora Studio Part Editor Sound sections for FM and VA'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: 'f856764'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/desktop/src/index.html'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src-tauri/src/backup_restore.rs'
  - '{project-root}/locale/en.json'
  - '{project-root}/locale/es.json'
  - '{project-root}/locale/pt-BR.json'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to view and edit all FM and VA sound parameters in Velora Studio comfortably on a computer, audition changes live on the unit (`STUDIO` mode, `◆` edit mark), persist them on Write, see dynamic routing diagrams with accessibility screen reader descriptions, reflow smoothly up to 200% text size with zero horizontal scroll, and have version mismatches (firmware without FM/VA) gracefully handled with a firmware update offer.

**Approach:**
1. **Full FM & VA Sound Sections in Velora Studio**:
   - Implemented responsive Part Editor layout per Story 12.10 UX mockups (`studio-part-editor-fm-va.html`).
   - FM Sound Section:
     - Algorithm card with interactive selection (1–32), dynamic SVG routing diagram with carrier boxes (solid), modulator boxes (dashed), `◐` indicators for modulators influenced by Brightness (KNOB 2), feedback loop, and accessible screen reader text alternative (`role="img"`, `aria-label`).
     - Feedback slider (0–99).
     - 6 Operator cards (OP 1–6) displaying dynamic Carrier/Modulator roles, ratio inputs (step 0.01), level sliders (0–99), polyline envelope previews, and "Edit envelope" buttons.
     - Pitch envelope card (Rates 1–4, Levels 1–4) with polyline envelope preview.
     - LFO card (Wave select, Speed, Delay, Pitch depth, Amp depth, Key sync).
     - Dynamic Brightness readout note (`→ modulator levels OP 2 · 4 · 6`).
   - VA Sound Section:
     - Osc 1 & Osc 2 cards (Wave, Octave, Detune, Pulse width).
     - Mixer card (Osc 1, Osc 2, Sub, Noise sliders).
     - Filter card (Type select LP 24/LP 12/BP/HP, Cutoff, Resonance, Drive, Key track, Env amount).
     - Amp Envelope & Filter Envelope cards (Attack, Decay, Sustain, Release) with polyline SVG previews.
     - LFO card (Wave, Rate, To pitch, To filter, To amp, Key sync).
     - Voice card (Poly/Mono/Legato, Unison voices, Unison detune, Glide time, Glide mode).
     - Brightness readout note (`→ filter cutoff`).
2. **Accessible Controls & Keyboard Operability**:
   - Jump links navigation (`<nav class="jump" aria-label="Sound groups">`) enabling rapid keyboard jump to sound groups via Tab and Enter, with Esc to return.
   - Studio sliders with ARIA roles (`role="slider"`, `aria-valuemin`, `aria-valuemax`, `aria-valuenow`, `aria-valuetext`).
   - Keyboard interaction support (Arrow keys for +/-1, PageUp/PageDown for +/-10, Home/End for bounds).
   - 2px accent focus ring with 2px offset (`outline: 2px solid var(--accent); outline-offset: 2px;`).
3. **Live Edit Auditioning & Persistence**:
   - Edits immediately mark `◆` on part strip, show header `◆ EDITED` badge, and update footer to `STUDIO · ◆ EDITED`.
   - Real-time parameter update dispatched to connected unit via Tauri command `update_part_param`.
   - "Write to Velora" persists changes to device state (`write_to_device`), clears `◆`, and announces confirmation.
4. **Firmware Version Mismatch Handling (FR99)**:
   - When connected to a unit with firmware lacking FM/VA (< 1.4, `has_fm_va: false`), FM/VA editors are hidden.
   - Part strips display warning `! Needs update 1.4`.
   - Not-available banner displayed with explanation and actions: "Change timbre" and "Update firmware".
   - "Update firmware" triggers firmware update flow (Tauri `set_firmware_version`), unlocking FM/VA editors immediately.
5. **Responsive 200% Zoom Reflow**:
   - Container queries on `.ed` reflow cards: 4 columns at normal width, 2 columns at medium/150% zoom, and 1 column at 200% text size with zero horizontal scroll (`overflow-x: hidden`).
6. **Localization & Design System Compliance**:
   - All strings localized across English, Spanish, and Brazilian Portuguese.
   - 100% compliant with design tokens (`tokens.css`, no color literals).
   - WCAG AA contrast compliance verified.

## Boundaries & Constraints

**Always:**
- Use design tokens (`var(--...)`) for all styling; no color literals outside theme files.
- Reflow cleanly at 200% zoom with no horizontal scroll.
- Provide accessible text alternatives (`aria-label`) on algorithm diagrams.
- Mark live edits with `◆` and persist only through Write.
- Provide silent graceful degradation and firmware update banner on version mismatch.

**Never:**
- Hardcode user-facing strings or colors.
- Break existing backup/restore or offline editing features.

</intent-contract>

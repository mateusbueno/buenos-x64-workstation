---
title: 'Story 2.15: Design story — Effects surface mockup'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '464c22a'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/mockups/part-surface-fm-va.html'
  - '{project-root}/mockups/part-surface.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need an intuitive, legible, and responsive layout for the device's Effects surface (inserts, sends, master, outputs) drawn and verified before it is built in LVGL, adhering to DESIGN.md design tokens and EXPERIENCE.md navigation principles.

**Approach:**
1. Author `mockups/effects-surface.html` rendered at 1024×600 in Stage and Studio themes.
2. Structure around the canonical Surface Skeleton:
   - Top Bar: 56 px
   - Content: 488 px
   - Hint Bar: 56 px
3. Four sections:
   - Part Inserts: Part selector (P1–P8), ordered chain rows with effect name, bypass toggle (Segmented Control with text + shape), add, remove, and reorder controls (hold ▲▼).
   - Sends: Master Reverb (type, time, level), Delay (time, tempo sync, feedback, level), and per-part send levels.
   - Master: Master EQ (Low, Mid, Mid freq, High), Compressor (Threshold, Ratio, Attack, Release, Makeup), and Line-in level.
   - Outputs: Main/Aux assignment per part (P1–P8) as Segmented Controls.
4. Active Hint Bar listing section-specific physical controls (`VALUE`, `WRITE`, `EXIT`) with localized verbs.
5. Contrast (≥4.5:1 text, ≥3:1 boundaries), touch targets (≥56 px), never-colour-only state, pseudo-localization (+40% with `ÁÇÊÕÑ Ação`), and fixed ROTARY microcopy.
6. Link mockup in `EXPERIENCE.md`.

</intent-contract>

---
title: 'Story 2.16: Design story — organ section of the Part surface'
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
  - '{project-root}/mockups/key-screen-drawbars-categories.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists playing tonewheel organ presets need to adjust drawbars, percussion, key click, vibrato/chorus scanner, leakage, and rotary speaker directly from the Part surface. A clear, responsive 1024×600 HTML mockup must illustrate this list-left / fields-right layout prior to LVGL implementation, conforming strictly to DESIGN.md tokens and EXPERIENCE.md interaction rules.

**Approach:**
1. Author `mockups/part-surface-organ.html` drawn at 1024×600 in Stage and Studio themes.
2. Layout:
   - Header: `PART n · timbre · ORGAN` with ⏻ and ★, and `KNOBS PART n` mode mirror.
   - List-left (224/260 px): Part sections (Timbre, Sound [active], Envelope & EQ, Zone & keyboard, Level & output, Effects, Arpeggiator).
   - Fields-right (764/790 px): Organ Sound section containing:
     - 9 Drawbar Columns: 16' (brown), 5⅓' (brown), 8' (white), 4' (white), 2⅔' (black), 2' (white), 1⅗' (black), 1⅓' (black), 1' (white) with theme-invariant head colours, per-theme edge rule (black edge in Stage, white edge in Studio), tabular numerals, and registration readout `88 8600 000`.
     - Percussion controls: On/Off, 2nd/3rd, Fast/Slow, Normal/Soft as Segmented Controls and value fields.
     - Key click level: value field (0..8 or Off/Soft/Normal/Hard).
     - Vibrato / Chorus scanner: Segmented Control / selector (Off, V1, V2, V3, C1, C2, C3).
     - Leakage level: value field.
     - Rotary: SLOW / FAST / STOP as Segmented Control and status field (ROTARY wording only).
3. Hint Bar listing active controls (`VALUE`, `WRITE`, `EXIT`) with localized verbs.
4. Review criteria: Stage & Studio themes, contrast (≥4.5:1 text, ≥3:1 boundaries), touch targets (≥56 px), never-colour-only checks, pseudo-localization (+40% with `ÁÇÊÕÑ Ação`), and ROTARY wording only.
5. Update `EXPERIENCE.md` to link `part-surface-organ.html`.

</intent-contract>

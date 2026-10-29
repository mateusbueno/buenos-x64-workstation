---
title: 'Story 9.22: Velora Studio accessibility audit'
type: 'feature'
created: '2026-10-29'
status: 'done'
baseline_revision: '9bc1f28e21a2d483b8b15d0335e3b6cb59f6e6cb'
review_loop_iteration: 1
followup_review_recommended: false
context:
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/desktop/src/tokens.css'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/part_editor_dom.js'
  - '{project-root}/desktop/src/device_view_dom.js'
  - '{project-root}/desktop/src/export_dialog_dom.js'
  - '{project-root}/desktop/src/import_dialog_dom.js'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists who navigate with a keyboard, use screen readers (VoiceOver, NVDA, Orca), or need 200% zoom / high contrast must be able to use every feature in Velora Studio without barrier, loss of content, or unannounced states.

**Approach:**
1. Harden keyboard navigation across all Studio workflows: ensure Tab order strictly matches visual order, F6 cycles landmark regions (`sidebar → Live Set → scenes → parts → editor → footer`), and 2px focus ring is never hidden.
2. Verify screen reader announcements for all Studio controls: Studio Knobs expose name, value, and unit via `role="slider"` and `aria-valuetext`; operation outcomes (connection, write, backup, restore, export, import, diagnostics) announce via polite live regions without stealing focus.
3. Support 200% zoom and high contrast at 1280×800: no horizontal scrollbar, Part Editor reflows to single column, hairlines switch from `--line` to `--line-strong` under `prefers-contrast: more` / `data-contrast="more"`.
4. Automated contrast check over both Stage (dark) and Studio (light) themes verifying all text pairs $\ge 4.5:1$ and boundaries $\ge 3:1$.
5. Author and commit `desktop/ACCESSIBILITY.md` with WCAG 2.1 AA audit findings and reusable release checklist.

## Boundaries & Constraints

**Always:**
- Keyboard walkthrough must work without mouse for every workflow.
- Tab order follows DOM and visual layout order.
- Focus visible ring (2px solid var(--accent), 2px offset) must never be hidden.
- F6 cycles landmark regions dynamically based on active view.
- Studio Knobs must read name, value, and unit.
- Screen reader announcements must not steal focus.
- 200% text/zoom at 1280×800 must have zero horizontal scroll.
- High contrast switches hairlines to `--line-strong`.
- All text pairs $\ge 4.5:1$ and UI boundaries $\ge 3:1$.

**Never:**
- Never hide focus outlines without a visible `:focus-visible` replacement.
- Never trap keyboard focus without an escape key / cancel path.
- Never truncate text into unreadable or overlapping overflow at 200% zoom.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| F6 Landmark Cycling | User presses F6 / Shift+F6 | Cycles through visible regions: sidebar → Live Set → scenes → parts → editor → footer | Safe fallback if sub-regions are hidden |
| 200% Zoom / Small Viewport | Window at 1280×800 at 200% zoom (effective width 640px) | Content wraps/reflows, Part Editor is 1 column, zero horizontal scroll | `overflow-x: hidden` prevents horizontal panning |
| High Contrast Mode | OS `prefers-contrast: more` or `data-contrast="more"` | `--line` maps to `--line-strong`, borders achieve $\ge 3:1$ contrast | Graceful CSS token override |
| Screen Reader Live Updates | Background task completes (export/import/backup) | Polite aria-live announces result without shifting keyboard focus | Live region `#live-region` receives text update |
| Automated Contrast Verification | Test suite runs color contrast checker | Tests pass for all dark and light theme token pairs | Fails if any pair < 4.5:1 (text) or < 3:1 (boundary) |

</intent-contract>

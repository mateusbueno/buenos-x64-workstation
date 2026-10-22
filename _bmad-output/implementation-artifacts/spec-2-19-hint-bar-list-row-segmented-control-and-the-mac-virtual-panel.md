---
title: 'Story 2.19: Hint Bar, List Row, Segmented Control and the Mac virtual panel'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'f7e67a87e5b562f0a1789c8a98b1bce11a7f0eef'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Non-Performance surfaces need a consistent structural layout (Top Bar 56, content 488, Hint Bar 56) and standardized interactive widgets (Hint Bar, List Row, Segmented Control) that support both touch and panel controls (VALUE turn, WRITE confirm) before hardware exists. The Mac simulator requires a virtual panel mapping keyboard inputs to real `PanelEvent` messages.

**Approach:**
1. Implement `PanelEvent` structure and `VirtualPanel` on Mac:
   - Maps keyboard keys (Escape/X for EXIT, Tab/M for MENU, Enter/W for WRITE, Space/V for VALUE_PUSH, Up/Down or [/] for VALUE_ENCODER turn, PageUp/PageDown or ,/. for LIVE SET and SCENE navigation) to standard `PanelEvent` messages.
   - SdlAdapter forwards key events into the `VirtualPanel`.
2. Implement Non-Performance surface skeleton (`SurfaceSkeleton`):
   - 1024x600 fixed layout: Top Bar (56px) · Content Area (488px) · Hint Bar (56px).
3. Implement `HintBar` widget:
   - Shows keycap-outlined English physical control names (`VALUE`, `WRITE`, `EXIT`) with localized verbs, plus right-aligned reassurance/warning text.
4. Implement `ListRow` widget:
   - Touch target >= 56px height.
   - Normal and selected states (raised with 3px accent border).
   - Responds to touch selection, VALUE turn, and WRITE confirmation.
5. Implement `SegmentedControl` widget:
   - Each segment touch target >= 56px.
   - Visual selection highlight with design tokens.
   - VALUE turn navigates segments; WRITE confirms; touch selects.
6. Verify 100% test pass on headless and SDL environments.

## Boundaries & Constraints

**Always:**
- Use tokens from `DESIGN.md` via `Theme` styles and token accessors (no color literals).
- All touch targets in these widgets must be >= 56px (`spacing.touch_target_min`).
- Panel control names in the Hint Bar remain English (`VALUE`, `WRITE`, `EXIT`, `KEYS`); verbs localize via catalog (`Locale::tr`).
- Zero locks or heap allocations on audio thread (UI operates decoupled on main thread).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Hint Bar display | Surface declares `[{"VALUE", "scroll"}, {"WRITE", "select"}]` | Hint Bar draws keycaps with `line_strong` outline, English name, and localized verb | Empty hints renders blank hint bar cleanly |
| List Row touch | Touch on row container | Row raises, gets 3px accent border, calls selection callback | Ignored if already selected |
| List Row VALUE turn | PanelEvent `VALUE_ENCODER` delta=+1/-1 | Moves focus/selection up or down list | Clamps at top/bottom of list |
| Segmented Control turn | PanelEvent `VALUE_ENCODER` delta=+1/-1 | Cycles active segment index | Wraps or clamps according to mode |
| Virtual Panel key | Press 'W' in SDL window | Emits `PanelEvent` with id `MCU_BTN_WRITE` and `pressed=true` | Unknown keys ignored |

</intent-contract>

## Tasks & Acceptance

- [x] Define `PanelEvent` and implement `VirtualPanel` keyboard mapping
- [x] Connect `VirtualPanel` to `SdlAdapter`
- [x] Implement `HintBar` widget with keycaps and localized verbs
- [x] Implement `ListRow` widget (height >= 56px, selection style, VALUE/WRITE/touch)
- [x] Implement `SegmentedControl` widget (touch target >= 56px, segment navigation)
- [x] Implement `SurfaceSkeleton` (Top Bar 56 · content 488 · Hint Bar 56)
- [x] Create unit tests in `ui/tests/widget_test.cpp` covering widgets, skeleton, and virtual panel
- [x] Verify all CI linters and test suites pass 100%

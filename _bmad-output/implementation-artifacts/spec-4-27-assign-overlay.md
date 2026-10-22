---
title: 'Story 4.27: ASSIGN Overlay'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '3af0b01'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/assign-overlay.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists turning encoders in ASSIGN mode need immediate visual clarity on which parameters K1–K8 control and which knob they just adjusted, without leaving the Performance screen.

**Approach:**
1. In `PerformanceView`:
   - Create `assign_overlay_` over `part_area_` (pos (22, 8), size 980x280).
     - Raised container with `line_strong` border, `r.md` corners.
     - Header: `ASSIGN · KNOBS (THIS LIVE SET)`.
     - 8 Knob Slots (K1..K8) in a 4x2 grid (233x115 each):
       - Label `K1`..`K8` in `meta_caps` (accent on turned knob, muted on others).
       - Parameter name (up to 2 lines, word break / ellipsis; unassigned slots show "Not assigned" / "—").
       - Current parameter value in tabular numerals.
       - Turned knob slot raised with 3 px accent border.
   - Behavior & Auto-dismiss:
     - In ASSIGN mode, encoder turn shows the overlay, highlights the turned knob slot, and starts/restarts a 3000 ms timer.
     - When 3 seconds pass without encoder activity, the overlay automatically closes.
     - Informational only: keys, panel, scenes, and Live Sets continue working normally; Hero and Scene strip remain visible.
2. Unit tests in `ui/tests/assign_overlay_test.cpp`:
   - Encoders in PART/ALL mode do not show ASSIGN overlay.
   - Encoder turn in ASSIGN mode shows ASSIGN overlay over part area.
   - Turned knob slot is highlighted with accent border and accent label.
   - New turn on another knob switches highlight and restarts 3s timer.
   - 3s elapsed without turn hides the overlay.
   - Live Set / Scene stepping works while overlay is visible.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` and `assign-overlay.html`.
- Zero audio allocations or locks on RT plane.
- Localized strings from catalog only (`locale/*.json`).

</intent-contract>

## Tasks & Acceptance

- [x] Add localization keys for assign overlay to `locale/*.json`
- [x] Implement `assign_overlay_` widget hierarchy (4x2 grid, header, K1–K8 cards) in `PerformanceView`
- [x] Connect encoder turn events in ASSIGN mode to show overlay and highlight active knob
- [x] Implement 3 s auto-dismiss timer with restart on new turns
- [x] Add unit test suite in `ui/tests/assign_overlay_test.cpp`
- [x] Verify build, CI linters, and 100% test pass

---
title: 'Story 4.24: Pickup Ghost and the Status Bar takeover'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'd3ef7c99684cb322a36b5ec632fbdbd480bbce4c'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** When a scene or Live Set is recalled, the physical faders may not match the newly recalled stored part volumes. Moving a fader without immediate visual feedback can cause abrupt jumps or confusion; the player needs to know exactly which fader is pending pickup, what its physical position is relative to stored, which direction to move it, and when pickup is achieved.

**Approach:**
1. Extend `PartStripData` in `PerformanceView`:
   - `bool pickup_pending{false}`
   - `float physical_fader_pos{0.0f}` (0.0 .. 1.0)
   - `enum class PickupDirection { Up, Down, None }`
2. Render Pickup Ghost on the Part Strip:
   - When `pickup_pending` is true:
     - Part Strip card receives a 2 px dashed `p.warn` outline.
     - A ghost line (3 px warn line with 2 px bg keylines) is displayed across the Volume Bar at `physical_fader_pos`.
     - A direction chevron (`▲` if physical < stored, `▼` if physical > stored) is displayed beside the Volume Bar.
     - Status Bar shows the 24 px warn takeover line: e.g. `▲ FADER 3 → 65%` naming the most recently moved pending fader.
   - When fader crosses the stored value:
     - `pickup_pending` clears for that part.
     - Ghost line, direction chevron, card outline, and takeover line disappear.
     - Volume Bar begins following the physical fader smoothly.
3. Unit tests in `ui/tests/pickup_ghost_test.cpp`:
   - Verification of pickup pending state, chevron direction (`▲` vs `▼`).
   - Status Bar takeover text `▲ FADER n → xx%`.
   - Priority to most recently moved pending fader when multiple are pending.
   - Crossing the stored value triggers takeover completion and visual restoration.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` tokens (warn color `Theme::current().warn`, keylines in `Theme::current().bg`).
- Takeover text formatted with localized tokens and $\ge 24$ px size.
- Zero audio allocations or locks.

</intent-contract>

## Tasks & Acceptance

- [x] Add pickup pending state and physical fader position to `PartStripData`
- [x] Implement Pickup Ghost visual elements (3px warn line, 2px keyline, direction chevron, 2px dashed warn card outline)
- [x] Implement Status Bar takeover formatting (`▲ FADER n → xx%`) prioritizing the most recently moved fader
- [x] Implement takeover completion when fader crosses stored value
- [x] Add unit test suite in `ui/tests/pickup_ghost_test.cpp`
- [x] Verify all CI linters and 100% test pass

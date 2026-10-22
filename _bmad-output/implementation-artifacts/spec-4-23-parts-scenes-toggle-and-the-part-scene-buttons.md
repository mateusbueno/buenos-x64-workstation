---
title: 'Story 4.23: PARTS/SCENES toggle and the part/scene buttons'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'c650553aa3ec93be81f9ba3db5aafe5a306ae019'
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

**Problem:** The 8 physical buttons below the faders must seamlessly serve a dual purpose controlled by the single PARTS/SCENES toggle button: in PARTS mode, brief presses toggle parts on/off while long presses select the knob-target part (reflected by an accent outline on the Part Strip and Top Bar mirror); in SCENES mode, they recall scenes 1–8 with LED states (current = solid strong, existing = weak, empty = off) and a 200 ms LED dip on recall.

**Approach:**
1. Extend `PerformanceView`:
   - Support outlined Part Strip styling for knob-target selection (`selected_part_idx_` in 0..7).
   - In PARTS mode:
     - Short press on part button (1..8) toggles that part on/off.
     - Long press (> 500 ms) selects that part as knob target without toggling on/off, updating Top Bar `KNOBS PART n` and applying an accent selection border (`selection_border` = 3 px, `p.accent`) to that Part Strip card.
   - In SCENES mode:
     - Press on scene button (1..8) recalls the scene if non-empty; empty slot does nothing.
     - LED states tracking: current (strong), valid (weak), empty (off), with 200 ms dip on recall.
   - PARTS/SCENES toggle button (`MCU_BTN_TOGGLE_PARTS_SCENES`):
     - Toggles between PARTS and SCENES mode immediately.
     - Top Bar mode mirror underlines/highlights `PARTS` or `SCENES` accordingly.
2. Connect to `VirtualPanel` and `NavigationController`:
   - Panel events `MCU_BTN_PART_1` through `MCU_BTN_PART_8` (and `MCU_BTN_TOGGLE_PARTS_SCENES`).
3. Unit test in `ui/tests/parts_scenes_toggle_test.cpp`:
   - Mode toggling with `MCU_BTN_TOGGLE_PARTS_SCENES`.
   - Button press in PARTS mode toggling part on/off.
   - Long press in PARTS mode selecting knob target without toggling.
   - Button press in SCENES mode recalling scene.
   - Empty scene button press ignoring.
   - LED state calculation and 200 ms recall dip.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` tokens.
- Zero audio allocations or locks.
- Real-time LED feedback logic decoupled from audio engine.

</intent-contract>

## Tasks & Acceptance

- [x] Extend `PerformanceView` with knob-target selection outline and selection state
- [x] Implement PARTS mode: short press toggles part on/off, long press (>500ms) selects knob target
- [x] Implement SCENES mode: button press recalls scene, empty scene ignored
- [x] Implement PARTS/SCENES toggle button (`MCU_BTN_TOGGLE_PARTS_SCENES`)
- [x] Add LED states representation (strong, weak, off, 200ms dip)
- [x] Add unit test in `ui/tests/parts_scenes_toggle_test.cpp`
- [x] Verify all linters and 100% test pass

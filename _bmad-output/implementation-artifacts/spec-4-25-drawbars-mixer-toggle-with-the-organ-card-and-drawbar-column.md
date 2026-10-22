---
title: 'Story 4.25: DRAWBARS/MIXER toggle with the Organ Card and Drawbar Columns'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '2bf1e3d36006c9a334e2c84218ebf1659178cf53'
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

**Problem:** Keyboardists playing tonewheel organ need direct drawbar control via physical faders and immediate visual feedback on the 7" display. Pressing DRAWBARS toggles the faders and display between MIXER (8 Part Strips) and DRAWBARS (Organ Card + 9 Drawbar Columns); if no organ exists, the system safely refuses with feedback ("No organ in this Live Set").

**Approach:**
1. Extend `PerformanceView`:
   - Support DRAWBARS mode in `part_area_`:
     - Toggle between `mixer_container_` (8 Part Strips) and `drawbars_container_`.
     - Organ Card:
       - Header: `PART n · DRAWBARS` (meta).
       - Timbre name: e.g. "Gospel B3".
       - Category: `ORGAN` in accent.
       - Registration: e.g. `88 8600 000` in tabular numerals.
       - Organ meta: Percussion, Vibrato/Chorus, Rotary speed (`SLOW`/`FAST`/`STOP`).
       - Styling: raised card with accent border.
     - 9 Drawbar Columns:
       - Footages: 16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1'.
       - Head colors per `Theme::invariant()`:
         - Brown: 16', 5⅓' (`drawbar_brown`)
         - White: 8', 4', 2', 1' (`drawbar_white`)
         - Black: 2⅔', 1⅗', 1⅓' (`drawbar_black`)
       - Numeric values: 0..8 (0 = all the way pushed in, 8 = fully pulled out).
   - Drawbar fader mapping:
     - Faders 1–8 map to drawbars 1–8; Fader 9 (Master/REV fader in mixer) maps to drawbar 9 (1').
     - Pickup ghost support on drawbars.
   - Refusal without organ:
     - If no organ part is in the current Live Set, pressing DRAWBARS keeps mode in MIXER, triggers 2 blinks, and displays "No organ in this Live Set".
   - Rotary speed changes:
     - `MCU_BTN_ROTARY_STOP` and `MCU_BTN_ROTARY_FAST_SLOW` update organ rotary state and reflect in Organ Card and Status Bar.
2. Unit tests in `ui/tests/drawbars_view_test.cpp`:
   - Attempting DRAWBARS in Live Set without organ -> refused with "No organ in this Live Set".
   - Attempting DRAWBARS in Live Set with organ -> enters DRAWBARS mode, Organ Card + 9 Drawbar Columns visible.
   - Drawbar value updates and registration string rendering (`88 8600 000`).
   - Fader movement updates drawbar positions.
   - ROTARY STOP and S/F button events toggle rotary state across Organ Card and Status Bar.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` invariant colors for drawbar heads.
- Zero audio allocations or locks.
- Localized strings in `locale/*.json`.

</intent-contract>

## Tasks & Acceptance

- [x] Add localization strings for organ, drawbars, and refusal notice to `locale/*.json`
- [x] Implement `DrawbarsContainer` in `PerformanceView` with Organ Card and 9 Drawbar Columns
- [x] Implement Drawbar column coloring (brown, white, black) using `Theme::invariant()`
- [x] Implement DRAWBARS/MIXER toggle validation: refuse and notice when no organ is present
- [x] Implement registration calculation and tabular numeral display (`88 8600 000`)
- [x] Connect panel Rotary buttons (`MCU_BTN_ROTARY_STOP`, `MCU_BTN_ROTARY_FAST_SLOW`)
- [x] Add unit test suite in `ui/tests/drawbars_view_test.cpp`
- [x] Verify all CI linters and 100% test pass

---
title: 'Story 4.26: PART/ALL/ASSIGN toggle and encoder LED rings'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: 'c819996'
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

**Problem:** Keyboardists shaping timbres live need unambiguous clarity on what the 8 encoders control. The knob toggle must cycle cleanly through PART, ALL, and ASSIGN modes, updating panel LEDs (one lit per state), the Top Bar mirror (`KNOBS PART n`, `KNOBS ALL`, `KNOBS ASSIGN`), the Part Strip outline on Performance, and driving the 8 encoder LED rings with exact value arcs across all surfaces.

**Approach:**
1. In `PerformanceView`:
   - Support `mode_knobs_`: 0 (PART), 1 (ALL), 2 (ASSIGN).
   - Method `cycle_knob_mode()`:
     - Cycles 0 → 1 → 2 → 0.
     - When entering ALL mode, captures base values for relative offsets.
     - Updates Top Bar mode label (`KNOBS PART n`, `KNOBS ALL`, `KNOBS ASSIGN`).
     - Updates Part Strips: only outlines `selected_knob_part_idx_` when in PART mode (`mode_knobs_ == 0`).
   - Query `knob_mode_led(uint8_t led_id) const`:
     - Returns `LedLevel::Strong` for the active mode's LED (`MCU_LED_TOGGLE_LED_PART`, `MCU_LED_TOGGLE_LED_ALL`, `MCU_LED_TOGGLE_LED_ASSIGN`), `LedLevel::Off` for the other two.
   - 8 Encoders & Rings:
     - Method `handle_encoder_turn(size_t encoder_idx, int16_t delta)`.
     - In PART mode: adjusts selected part's parameter, clamped to [0.0, 1.0].
     - In ALL mode: adjusts relative offset for all parts, offset clamped to [-0.5, +0.5].
     - In ASSIGN mode: adjusts assigned parameter for encoder, clamped to [0.0, 1.0].
     - Getter `encoder_ring_value(size_t encoder_idx) const` -> float [0.0, 1.0].
     - Getter `encoder_ring_fill(size_t encoder_idx) const` -> uint8_t [0, 255].
     - Helper `encoder_ring_led_set(size_t encoder_idx) const` -> `mcu_led_set` frame struct (brightness 255, arg = fill, lit segments only).
2. In `NavigationController`:
   - Global routing for `MCU_BTN_TOGGLE_PART_ALL_ASSIGN` and `MCU_CTRL_ENCODER_1`..`MCU_CTRL_ENCODER_8` across all surfaces (Performance, Launcher, Effects).
3. Unit tests in `ui/tests/encoder_rings_test.cpp`:
   - Verify cycling PART → ALL → ASSIGN → PART with Top Bar copy and LED states.
   - Verify Part Strip selection outline only in PART mode.
   - Verify encoder turns adjust rings: absolute in PART and ASSIGN, relative offset in ALL.
   - Verify encoder rings and toggle work while on Launcher and Effects surfaces via `NavigationController`.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` and `EXPERIENCE.md`.
- Zero audio allocations or locks on RT plane.
- Localized strings from catalog only (`locale/*.json`).
- Lit segments only for encoder rings, never weak-lit (`brightness = 255`).

</intent-contract>

## Tasks & Acceptance

- [x] Add `cycle_knob_mode()` and `knob_mode_led()` to `PerformanceView`
- [x] Connect `MCU_BTN_TOGGLE_PART_ALL_ASSIGN` (34u) in `PerformanceView` and `NavigationController`
- [x] Implement Part Strip outline gating (only outlined in PART mode)
- [x] Implement 8 encoder parameter storage and LED ring value/fill calculation
- [x] Support ALL mode relative offset calculation across all parts
- [x] Route encoders across all surfaces in `NavigationController`
- [x] Add unit test suite in `ui/tests/encoder_rings_test.cpp`
- [x] Verify build, CI linters, and 100% test pass

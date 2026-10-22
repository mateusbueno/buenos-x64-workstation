---
title: 'Story 4.21: Performance surface — Part Strips, Volume Bars and Status Bar'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'bd3650974b882eb727284b397fa08d4885871f3b'
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

**Problem:** Keyboardists on stage need to monitor 8 part states (timbre, category, on/off, volume level), system health (voices, streaming, rotary, reverb send), and critical alerts (missing libraries) directly from the performance surface.

**Approach:**
1. Extend `PerformanceView` (`ui/include/velora/ui/screens/performance_view.hpp` and `ui/src/screens/performance_view.cpp`):
   - In `part_area_` (1024x298), create 8 equal `PartStrip` cards (115x280 px).
   - Each `PartStrip`:
     - Header: Part number (1..8) + LED dot (accent filled when on, hollow when off).
     - ⏻ Power toggle button with $\ge 56$ px touch target (`touch_target_min`).
     - Timbre name: 2 lines with end ellipsis.
     - Category label: short localized label ($\le 10$ chars), or "— empty" for empty parts, or "Missing library" in warn for missing libraries, or "OFF" tag when part is disabled.
     - Volume Bar: vertical bar displaying stored (sounding) volume as an accent-filled level indicator.
     - Whole card body tappable to open Part surface (or trigger callback).
   - In `status_bar_` (1024x56):
     - Normal mode:
       - `VOICES n / max`
       - `● STREAMING OK`
       - `ROTARY SLOW / FAST / STOP` (visible if organ or rotary insert present)
       - `REV n%` (clickable button, opens Effects surface)
     - Warning takeover mode:
       - 24 px warn takeover line prefixed with `⚠` (e.g. `⚠ Part 3: Missing library`) that replaces all normal status items until cleared.
2. Connect ⏻ to dispatch part enable toggle.
3. Connect `REV` button tap to callback / Effects navigation.
4. Add localization keys to `en.json`, `es.json`, `pt-BR.json`.
5. Unit tests in `ui/tests/performance_view_test.cpp` verifying:
   - 8 Part Strips creation and geometry.
   - Part on/off state rendering (LED dot, hollow ring, OFF tag, muted styling at full opacity).
   - Empty part ("— empty") and missing library ("Missing library" in warn).
   - Volume Bar level updates.
   - ⏻ power button activation and event dispatch.
   - Status Bar normal layout (voices, streaming, rotary, REV).
   - REV tap action.
   - Warning takeover mode with `⚠` and restoration when resolved.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` tokens via `Theme` (zero color literals, contrast $\ge 4.5:1$ text and $\ge 3.0:1$ borders).
- Touch target $\ge 56$ px for ⏻ and REV button.
- Zero audio allocations or locks on real-time plane.
- Localized strings via `tr(...)` catalog.

</intent-contract>

## Tasks & Acceptance

- [x] Add localization strings for Part Strips and Status Bar to `locale/en.json`, `locale/es.json`, `locale/pt-BR.json`
- [x] Implement `PartStripData` struct and Part Strip UI creation in `PerformanceView`
- [x] Implement Volume Bar vertical level indicator in each Part Strip
- [x] Implement ⏻ toggle handling and callback
- [x] Implement Status Bar widgets: voices, streaming, rotary, REV button
- [x] Implement Status Bar warning takeover for missing library alerts
- [x] Add unit test suite in `ui/tests/performance_view_test.cpp`
- [x] Verify all CI linters and 100% test pass

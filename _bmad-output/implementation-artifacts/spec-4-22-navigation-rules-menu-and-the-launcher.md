---
title: 'Story 4.22: Navigation rules, MENU and the Launcher'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'fa8ae7ad3c59124a919248232ea27be6396e3fc9'
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

**Problem:** Keyboardists need a consistent, predictable navigation model across the entire instrument: pressing MENU opens the Launcher grid of 7 tiles; VALUE moves the highlight; WRITE or tap opens the selected surface; EXIT steps back one level (and does nothing on Performance); and stepping LIVE SET or SCENE works seamlessly from any surface.

**Approach:**
1. Implement `LauncherView` (`ui/include/velora/ui/screens/launcher_view.hpp` and `ui/src/screens/launcher_view.cpp`):
   - Built on `SurfaceSkeleton`: Top Bar (56px) · Launcher Grid Content (488px) · Hint Bar (56px).
   - 7 canonical tiles in 2 rows (4 + 3):
     - Row 1: Setlist Browser, Setlist, Library, Effects.
     - Row 2: Scenes, Import, Settings.
   - Tiles $\ge 56$ px height and width (`touch_target_min`).
   - Highlighted tile is raised (`p.raised`) with accent border (`p.accent`, width 3px `spacing.selection_border`).
   - Disabled tiles (Library, Import until Epic 5) are marked disabled with "Not available" status text in muted and ignore selection.
   - Effects opens the Epic 2 Effects surface.
   - Hint Bar: names `VALUE · Move`, `WRITE · Open`, `EXIT · Back`.
2. Navigation controller / manager (`NavigationController` or integrated in `ShellView`/`UiApp`):
   - Manages active screen stack: Performance (root/home), Launcher, Effects, etc.
   - `MENU` press toggles Launcher on/off, preserving previous surface.
   - `EXIT` press:
     - On Performance: does nothing.
     - On Launcher: returns to previous surface.
     - On surface opened from Launcher: returns to the surface Launcher was opened from.
   - `VALUE` turn on Launcher moves highlight (0..6, skipping disabled if applicable or navigating circular).
   - `WRITE` press on Launcher activates highlighted tile.
   - `LIVE SET ◀ ▶` and `SCENE ◀ ▶` work globally from any surface.
3. Unit test in `ui/tests/navigation_test.cpp`:
   - Launcher 7-tile grid creation, geometry, styling, disabled states.
   - VALUE navigation and highlight movement.
   - WRITE and touch activation.
   - MENU toggle behavior.
   - EXIT navigation hierarchy (back one level, no-op on Performance).
   - Global LIVE SET and SCENE stepping on secondary surfaces.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` tokens (no color literals, contrast $\ge 4.5:1$ text, $\ge 3.0:1$ borders).
- All touch targets $\ge 56$ px.
- All strings localized through `locale/*.json`.
- Zero audio allocations or locks.

</intent-contract>

## Tasks & Acceptance

- [x] Add localization keys for Launcher tiles and hints in `locale/*.json`
- [x] Implement `LauncherView` screen with 7 tiles (2 rows 4+3), highlight cursor, and disabled tiles
- [x] Implement `NavigationController` managing screen stack, MENU toggle, EXIT back rules, and global stepping
- [x] Connect `EffectsView` and `PerformanceView` to `NavigationController`
- [x] Add unit test suite in `ui/tests/navigation_test.cpp`
- [x] Verify all CI linters pass and 100% test pass

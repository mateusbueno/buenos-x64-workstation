---
title: 'Story 4.35: Settings surface with Knobs · Global Assign'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '6aaab86'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/key-screens-device.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need a dedicated Settings surface to configure instrument parameters, starting with editing the global default ASSIGN map (so every new Live Set starts with their preferred knob layout) and editing the current Live Set's ASSIGN map directly on the instrument without a computer.

**Approach:**
1. Implement `SettingsView` in `ui/include/velora/ui/screens/settings_view.hpp` and `ui/src/screens/settings_view.cpp`:
   - Screen skeleton: Top Bar (56px) · Content (488px) · Hint Bar (56px).
   - Top Bar: Context title `SETTINGS` (left) and model / firmware info `Velora 73 · firmware 1.2.0` (right).
   - Two-column content layout (~1 : 2 ratio):
     - Left column (sidebar list of 7 canonical sections from EXPERIENCE.md):
       1. Display & Language
       2. Keyboard & Pedals
       3. MIDI & Clock
       4. Knobs · Global Assign (ENABLED)
       5. Computer (Velora Studio)
       6. Storage & Library
       7. System & Update
       Sections 1..3 and 5..7 are visually muted/disabled with sub-label `settings.section.coming_soon` or disabled style.
     - Right column (detail area): Knobs · Global Assign editor:
       - Scope Segmented Control (UX-DR34):
         `[ Global Default | This Live Set ]`
       - Reassurance message:
         - Global Default: `settings.knobs.reassurance_global` ("Seeds each new Live Set · edits take effect immediately")
         - This Live Set: `settings.knobs.reassurance_live_set` ("Edits this Live Set's ASSIGN map · Live Set is marked ◆")
       - Grid of 8 knob cards K1..K8 (4x2 grid):
         - Shows knob index badge `K1`..`K8`, parameter name, or `assign.slot.not_assigned`.
         - Highlighted slot raised with 3px accent border.
         - Tapping a slot opens the Parameter Picker modal.
       - Parameter Picker:
         - Modal dialog or action sheet over dimmed backdrop:
         - `Clear slot` option.
         - List of assignable parameters queried from `velora::core::params::kParams` / parameter registry.
         - Selecting an item applies `set_assign_slot(target, slot, param_id)`.
         - When target is `ASSIGN_TARGET_CURRENT_LIVE_SET`, marks the Live Set dirty (`◆`).
   - Hint Bar:
     - `VALUE`: `select slot`
     - `WRITE`: `assign`
     - `EXIT`: `back`
     - Reassurance: `Global default seeds each new Live Set`
2. Wire into `NavigationController`:
   - Add `SettingsView` to `NavigationController`.
   - Wire `LauncherTileId::Settings` to `SurfaceId::Settings`.
   - Handle `VALUE` turn/push and `EXIT` navigation.
3. Localization & Styling:
   - Add keys in `locale/en.json`, `locale/es.json`, `locale/pt-BR.json`.
   - Full compliance with UI linters (no color literals outside theme, no raw strings).
4. Tests:
   - Unit tests in `ui/tests/settings_view_test.cpp`.
   - Register in `ui/CMakeLists.txt`.

</intent-contract>

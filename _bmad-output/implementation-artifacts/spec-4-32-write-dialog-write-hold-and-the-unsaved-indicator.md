---
title: 'Story 4.32: Write Dialog, Write hold and the Unsaved Indicator'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: 'db0289d'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/proto/ws_v1.proto'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** When musicians make changes during rehearsals or soundchecks (tweaking EQ, volume, timbre, scene names), they must be able to hold WRITE to open a clear Write Dialog allowing them to save with exact scope (Scene, Whole Live Set, Save as new…), while preventing accidental overwrites, warning when timbre changes cannot be saved to Scene-only scope, handling name collisions and storage-full gracefully, and displaying the clean `◆` unsaved indicator wherever dirty Live Sets are listed.

**Approach:**
1. **Write Hold Detection (UX-DR81, FR89):**
   - Physical `MCU_BTN_WRITE` pressed and held $\ge 500\text{ ms}$ (Write hold, 0.5 s) on editing surfaces (`Performance`, `Effects`, `Scenes`, etc.):
     - If pending edits exist (`has_unsaved_edits_` / `state.edits().current_dirty()`): opens `WriteDialog` over a 35%-dimmed surface.
     - If NO pending edits exist: no dialog opens; Hint Bar temporarily shows "Nothing to write".
   - Brief press of `WRITE` (< 500 ms) retains its standard context action (e.g. recall scene on Scenes, confirm in lists/dialogs, no-op on Performance).

2. **Write Dialog Component (`WriteDialog` in `ui/include/velora/ui/components/write_dialog.hpp` & `ui/src/components/write_dialog.cpp`):**
   - Modal card centered over 35%-dimmed overlay (`lv_layer_top` or skeleton overlay):
     - Card width 560 px, radius `r.lg`, border `p.line_strong`, background `p.surface` with tonal lift.
   - Title: `Write changes?` (`write_dialog.title`).
   - Scope selection options:
     1. `Scene` (`write_dialog.scope.scene`)
     2. `Whole Live Set` (`write_dialog.scope.whole_live_set`)
     3. `Save as new…` (`write_dialog.scope.save_as_new`)
   - Scope unavailable handling:
     - When edits include timbre/engine modifications (`has_timbre_edit == true` or `!scene_write_available`), `Scene` scope is rendered muted and disabled with explanation:
       `Timbre changes are saved with the Live Set` (`write_dialog.scene_disabled_timbre`).
   - "Changed: …" line:
     - Muted summary of changes from `state.edits().change_summary_text()`, e.g., `Changed: Part 2 volume, Scene 1 name`.
   - Name field for `Save as new…`:
     - Input field pre-filled with suggested free name (e.g. `"<CurrentName> 2"`).
     - Tapping opens `OnScreenKeyboard`.
   - Error / Status banners:
     - Name already in use: `"A Live Set named 'x' is already in this setlist"` (`write_dialog.error.name_in_use`).
     - Storage full: `"Not enough space to write. Free space in Settings › Storage & Library."` (`write_dialog.error.storage_full`).
   - Action buttons (min touch target 56 px):
     - Secondary `EXIT · Cancel` (`p.surface`, border `p.line_strong`).
     - Primary `WRITE · Yes` (`p.accent` background, `p.on_accent` text).
   - Physical controls inside `WriteDialog`:
     - `VALUE` encoder: cycles scope selection (or focus between scopes and action buttons).
     - `VALUE_PUSH`: activates focused item / toggles scope.
     - `WRITE`: confirms and sends `WriteLiveSet` with chosen scope and name.
     - `EXIT`: cancels without saving and closes dialog.

3. **Unsaved Indicator (`◆`):**
   - Displayed in warning color (`p.warn`) following the Live Set name:
     - `PerformanceView` Live Set Hero: `<Name> ◆` when dirty.
     - `SetlistView` / `SetlistBrowserView` / Result Tiles / List Rows: `<Name> ◆`.
   - Cleared on successful write.

4. **Integration & Verification:**
   - Wire `WriteDialog` into `NavigationController` so any editing surface triggering WRITE hold opens it.
   - Add unit/component tests in `ui/tests/write_dialog_test.cpp`.
   - Lint checks: zero hardcoded strings and zero color literals.

</intent-contract>

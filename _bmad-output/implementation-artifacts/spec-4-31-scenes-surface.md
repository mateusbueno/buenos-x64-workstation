---
title: 'Story 4.31: Scenes surface'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '7d8cd11'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/scenes-surface.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to inspect, recall, create, rename, duplicate, reorder and clear scenes directly on the instrument without a computer, with clear visual state of active/empty slots and guards preventing accidental destruction of the last scene.

**Approach:**
1. Implement `ScenesView` in `ui/include/velora/ui/screens/scenes_view.hpp` and `ui/src/screens/scenes_view.cpp`:
   - Screen skeleton: Top Bar (56px) · Content (488px) · Hint Bar (56px).
   - Top Bar: Context title `SCENES · <LiveSetName>` and used slots counter (`N / 8 SLOTS USED`).
   - 4x2 Grid of 8 Scene Slots:
     - Currently sounding scene marked `▶ CURRENT` (or `▶`) and filled with `p.accent` background, on-accent text.
     - Populated slots show slot number (01..08), title, and timbre summary / sub-label.
     - Empty slots have dashed border (`p.line_strong`) with muted "— Empty —" and "Slot available".
     - Highlighted/selected slot raised with 3 px accent border.
   - Reassurance line:
     - "Edits become permanent with Write (Scene) · Live Set is marked ◆"
   - Action Bar (height 56 px touch targets):
     - `+ New Scene`: copies the current scene into the next available empty slot.
     - `Rename`: opens `OnScreenKeyboard` to edit the selected scene's title.
     - `Duplicate`: copies the selected slot into the next empty slot.
     - `Move ◀ ▶`: shifts the scene left/right in order.
     - `Clear`: empties the slot (refused if it is the only remaining populated scene).
     - Any modifying action flags the Live Set as unsaved (`◆`).
   - Hint Bar:
     - `VALUE`: `select slot`
     - `WRITE`: `recall scene`
     - `EXIT`: `back to Performance`
     - Reassurance: `Hold WRITE to save scene`
2. Physical controls:
   - `VALUE` encoder: moves slot selection (1..8).
   - `VALUE` push / `WRITE` press: recalls selected scene (`RecallScene(idx)`).
   - `SCENE ◀ ▶` buttons: moves/steps scene or shifts order.
   - `EXIT`: returns to Performance or previous surface.
3. Wire into `NavigationController` and `PerformanceView`:
   - Launcher tile 4 ("Scenes") navigates to `ScenesView`.
   - Long press on any Scene Tile on Performance opens `ScenesView` focusing that slot (UX-DR14).
   - Update `sprint-status.yaml`.

</intent-contract>

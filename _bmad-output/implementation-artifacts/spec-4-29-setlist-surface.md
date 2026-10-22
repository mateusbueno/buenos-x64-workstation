---
title: 'Story 4.29: Setlist surface'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: 'e2ca0b4'
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

**Problem:** Keyboardists need to choose, create, rename and reorder setlists and their Live Sets, create a new Live Set and edit setlist tags directly on the instrument without a computer, while maintaining uninterrupted audio playback and receiving clear feedback.

**Approach:**
1. Implement `SetlistView` in `ui/include/velora/ui/screens/setlist_view.hpp` and `ui/src/screens/setlist_view.cpp` using `SurfaceSkeleton`:
   - Top Bar: Context label `SETLISTS` and subtitle showing current loaded setlist and song count.
   - Content area:
     - Two-column list layout (~1:2 ratio):
       - Left column (width ~320 px): "YOUR SETLISTS" tag, list of setlists (name and song count), "+ New setlist" row.
       - Right column (width ~660 px): "SONGS · DRAG OR HOLD ▲▼ TO REORDER" tag, list of Live Sets in the selected setlist (with ▶ `NOW` for currently playing, and `◆` in warn for Live Sets with unsaved edits), "+ Add Live Set" row.
     - Empty state (when no setlists exist): displays "No setlists yet." with "+ New setlist" highlighted with accent border.
   - Hint Bar:
     - Physical control keycaps `VALUE` (scroll), `WRITE` (load setlist / jump to song), `EXIT` (back to Performance).
     - Right reassurance: "Loading a setlist keeps the current sound playing".
2. On-screen Keyboard integration:
   - Creating a new setlist or Live Set opens the `OnScreenKeyboard`.
   - On confirmation, dispatches the control-thread command (`CreateSetlist` or `CreateLiveSet` + `AddLiveSetToSetlist`), creating a new Live Set with 8 empty parts and 1 scene, making it the current Live Set.
3. Panel controls:
   - `VALUE` turn: scrolls items in focused column.
   - `VALUE` push: toggles focus between left (setlists) and right (songs) columns.
   - `WRITE` press: loads the selected setlist (keeps sound playing) or jumps to the selected Live Set.
   - `EXIT`: returns to Performance or previous surface.
   - Hold ▲ / ▼: reorders Live Sets within the setlist (`ReorderLiveSetsInSetlist`).
4. Wire into `NavigationController` and update `LauncherView`.

</intent-contract>

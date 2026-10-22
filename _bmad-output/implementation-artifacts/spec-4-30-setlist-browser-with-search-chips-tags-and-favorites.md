---
title: 'Story 4.30: Setlist Browser with search chips, tags and favorites'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: 'e6049e5'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/key-screens-device.html'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/action-sheet.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists performing live need to quickly find and jump to any Live Set even outside the current setlist (off-script), filter by favorites, recent, setlists, all Live Sets and custom tags, without losing the currently loaded setlist or interrupting sound.

**Approach:**
1. Implement the generic `ActionSheet` component in `ui/include/velora/ui/components/action_sheet.hpp` and `ui/src/components/action_sheet.cpp`:
   - Floating card, centered over 35% dimmed background, width 540 px, `Theme::radius().lg` (12 px).
   - Eyebrow in muted caps, title in modal-title.
   - Action rows with min-height 56 px, highlighted row with 3 px accent border and `▶` marker.
   - Physical control keycaps (e.g. `WRITE · ★ Add to Favorites`, `EXIT · Cancel`).
   - `VALUE` highlights rows, `WRITE` confirms highlighted row, `EXIT` cancels.
2. Implement `SetlistBrowserView` in `ui/include/velora/ui/screens/setlist_browser_view.hpp` and `ui/src/screens/setlist_browser_view.cpp`:
   - Screen skeleton: Top Bar (56px) · Content (488px) · Hint Bar (56px).
   - Search Field:
     - ⌕ glyph, query string, accent caret, `searches setlists and Live Sets` hint.
     - Tapping opens the `OnScreenKeyboard`.
   - Filter Chips:
     - `★ Favorites`, `Recent`, `Setlists`, `All Live Sets`, followed by user tags (`Wedding`, `Church`, `Pop`, `Jazz`).
     - Selected chip accent-filled (`p.accent`, on-accent text).
   - 3-column Result Grid of `ResultTile`s:
     - Title, meta line (`LIVE SET · IN SUNDAY SERVICE` or `SETLIST · 16 SONGS`), `★` for favorite, `◆` for pending edits.
     - Selected tile raised + 3 px accent border.
     - Long press (or action sheet trigger) opens `ActionSheet` to add/remove from favorites.
     - `WRITE` on Live Set jumps to it seamlessly (keeps loaded setlist); `WRITE` on setlist loads it.
   - Empty states (UX-DR73):
     - Query with no matches: "No matches for 'x'. Try another word or a tag."
     - Empty Favorites: "No favorites yet. Long-press a Live Set to add one."
     - Empty Recent: "Nothing played yet."
   - Hint Bar:
     - `VALUE` scroll, `WRITE` load setlist · or jump to Live Set, `EXIT` back.
     - Reassurance: "Jumping to a song outside the setlist keeps the setlist; ◀ ▶ return to it".
3. Navigation:
   - Wire `SetlistBrowserView` into `NavigationController` (accessible via Launcher tile 0 and Top Bar setlist context).
   - Update `sprint-status.yaml`.

</intent-contract>

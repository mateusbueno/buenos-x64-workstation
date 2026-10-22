---
title: 'Story 4.20: Performance surface — Top Bar, Live Set Hero and Scene strip'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '50ebf338d172e81fc69c73b067ee0d1fb9633e7f'
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

**Problem:** Keyboardists on stage need to read their setlist position, active panel toggle modes, tempo, current Live Set name, next song readiness, and 8 scenes at a glance from 1.5 m under stage lighting, reflecting published engine state without animation or local guessing.

**Approach:**
1. Implement `PerformanceView` (`ui/include/velora/ui/screens/performance_view.hpp` and `ui/src/screens/performance_view.cpp`):
   - Fixed 1024x600 layout: Top Bar (56px) · Live Set Hero (118px) · Scene strip (72px) · Part area (298px) · Status Bar (56px).
2. Top Bar (56px):
   - Context: `SETLIST · <name> · n / m` (with middle ellipsis), or `NO SETLIST · <Live Set>` when no setlist is loaded.
   - Mode mirror: three toggle states in 20px bold (`MIXER`/`DRAWBARS`, `PARTS`/`SCENES`, `KNOBS PART n`/`ALL`/`ASSIGN`), with active mode in accent text and 2px accent underline.
   - Tempo: `♩ <bpm>` (e.g. `♩ 72`).
3. Live Set Hero (118px):
   - Shows `LIVE SET nn` in meta uppercase.
   - Live Set name in 53px token on one line with end ellipsis, followed by `◆` when edits are pending.
   - NEXT section on the right: `NEXT` label, next title, and status: `● READY` (ok), `Loading <name>… Ns` (muted), or `End of setlist`.
   - Nothing in Hero animates.
4. Scene strip (72px):
   - 8 equal Scene Tiles.
   - Current scene: accent-filled tile (`p.accent` bg, `p.on_accent` text, 100% opacity).
   - Valid scenes: `p.surface` tile, number + name + muted sub-label.
   - Empty scenes: transparent, dashed `p.line_strong` border, muted number.
   - Tapping a tile sends `ws::v1::RecallScene`.
5. Connect `PerformanceView` to `WsClient` and `VirtualPanel` (`SCENE_PREV`/`SCENE_NEXT`, `LIVE_SET_PREV`/`LIVE_SET_NEXT`).
6. Test in `ui/tests/performance_view_test.cpp`.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` tokens via `Theme` (no color literals).
- All touch targets in these widgets must be >= 56px (`touch_target_min`).
- Touch and panel equivalents: tapping scene recalls it, same as SCENE ◀ ▶.
- Zero audio allocations or locks; UI decoupled via domain socket.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Active setlist | State has `setlist_name="Sunday Service"`, index 2 of 10 | Top Bar reads `SETLIST · Sunday Service · 3 / 10` | Middle ellipsis if name overflows |
| No setlist | State has `setlist_name=""` | Top Bar reads `NO SETLIST · <Live Set Name>` | Defaults cleanly |
| Next ready | State loading status has next Live Set ready | Hero NEXT shows `● READY` in ok color | Muted `Loading…` while loading |
| Scene tap | Tap Scene Tile 3 | Dispatches `ws::v1::RecallScene(index=2)`; server Ack updates current scene | Clamped to 0..scene_count-1 |
| Unsaved edits | State has `edits.dirty=true` | Hero displays `◆` after Live Set name | Hidden when clean |

</intent-contract>

## Tasks & Acceptance

- [x] Implement `PerformanceView` layout skeleton (Top Bar 56, Hero 118, Scene strip 72, Part area 298, Status 56)
- [x] Implement Top Bar with setlist context, mode mirrors, and tempo
- [x] Implement Live Set Hero with 53px name token, pending edits `◆`, and NEXT status
- [x] Implement Scene strip with 8 tiles (current accent-filled, empty dashed)
- [x] Connect `PerformanceView` to `WsClient` state updates and `VirtualPanel` events
- [x] Add unit tests in `ui/tests/performance_view_test.cpp`
- [x] Verify 100% test pass and all CI linters green

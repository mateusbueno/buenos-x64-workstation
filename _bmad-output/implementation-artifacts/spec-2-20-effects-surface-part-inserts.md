---
title: 'Story 2.20: Effects surface — part inserts'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '46632f01f0fa2ef70d6a2f4da41e6c31bf0db2a2'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to add, remove, reorder, bypass, and edit parameters of each part's insert effects directly on the device screen (7" 1024x600) without a computer. The screen must reflect engine-published state, send `ws.v1` structural commands (`SetInsertChain`), and handle engine refusals (e.g., FX budget exceeded) gracefully in the Hint Bar.

**Approach:**
1. Implement `EffectsView` (`ui/include/velora/ui/screens/effects_view.hpp` and `ui/src/screens/effects_view.cpp`) built atop `SurfaceSkeleton`.
2. Part Inserts section:
   - Part selector: `SegmentedControl` choosing Part 1–8.
   - Chain view: Ordered `ListRow` widgets for active insert slots (up to 5), showing effect name (Rotary, Chorus/Phaser, EP Amp, EQ, Compressor), bypass toggle (`SegmentedControl` / button), reorder controls (▲/▼), and remove button.
   - "Add effect" entry offering the 5 insert types when chain size < 5.
3. Command & State binding:
   - Modifications send `ws::v1::SetInsertChain` via `WsClient`.
   - UI updates when `State` is published from engine (never guessing locally).
   - Refused commands (error from server) display human-readable reason in the Hint Bar (`set_right_text`).
4. Parameter view:
   - Tapping an insert row opens its parameter editor.
   - Lists the effect's parameters (1..5) with values formatted per locale.
   - VALUE encoder adjusts selected parameter.
5. Navigation & EXIT:
   - EXIT goes one level back (Parameter editor -> Chain view -> Shell/Launcher) without discarding edits in memory.
6. Verify against headless tests and real `engine-host` rig.

## Boundaries & Constraints

**Always:**
- Use tokens from `DESIGN.md` (no color literals).
- All touch targets in these widgets must be >= 56px (`touch_target_min`).
- Touch and panel equivalents: VALUE turn moves/changes, WRITE confirms, touch selects.
- Zero audio allocations or locks; UI decoupled via domain socket.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Select part | User taps "Part 2" or turns VALUE | Part 2 insert chain rendered from `PartState.inserts` | Empty chain shows only "+ Add effect" |
| Add insert | Select "Rotary" on empty part | `SetInsertChain` sent with rotary slot; server Acks; state publishes; slot appears | If refused (budget exceeded), error shown in Hint Bar |
| Bypass toggle | Toggle bypass on slot 0 | `SetInsertChain` sent with `bypass=true`; chain updates from state | Error shown in Hint Bar on refusal |
| Reorder slots | Move slot 1 up to slot 0 | `SetInsertChain` sent with reordered slots array | Slots revert if server rejects |
| Remove slot | Tap remove on slot 0 | `SetInsertChain` sent without slot 0 | Reverts if server rejects |
| Parameter edit | VALUE encoder on parameter | Value updates and commands sent | Clamped to registry min..max |
| EXIT press | EXIT key or button pressed | Navigates back one level; in-memory edits preserved | Stays at root if already top-level |

</intent-contract>

## Tasks & Acceptance

- [x] Add `ErrorCallback` to `WsClient` for engine command refusal handling
- [x] Implement `EffectsView` with `SurfaceSkeleton` for Part Inserts (Part 1-8 selector, chain rows, add effect, bypass, remove, reorder)
- [x] Implement parameter editor view inside `EffectsView`
- [x] Connect `EffectsView` to `WsClient` commands and `VirtualPanel` events
- [x] Create unit and integration tests in `ui/tests/effects_view_test.cpp`
- [x] Verify 100% test pass and all CI linters green

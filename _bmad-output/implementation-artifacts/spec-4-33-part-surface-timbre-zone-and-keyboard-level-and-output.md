---
title: 'Story 4.33: Part surface — timbre, zone and keyboard, level and output'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: 'da4268d'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/part-surface.html'
  - '{project-root}/proto/ws_v1.proto'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists building live splits and layers need to inspect and edit a single part's timbre, keyboard split range (low key, high key), octave shift, transpose, keyboard enable toggle, volume, and stereo output routing directly on the hardware screen without a computer, hearing changes immediately and keeping pending edits flagged with `◆`.

**Approach:**
1. **Screen Layout (`PartView`):**
   - Screen skeleton: Top Bar (56px) · Content (488px) · Hint Bar (56px).
   - Top Bar: Header `PART n · <TimbreName> · <Category>` with ⏻ (power toggle) and ★ (hidden until Epic 5 library favorite catalog).
   - Content: Two-column layout (List-left / Fields-right per Story 4.18 mockup):
     - Left column (260px): Navigation sections (Timbre, Zone & Keyboard, Level & Output).
     - Right column (732px): Active section fields:
       - **Timbre:** Current timbre card + "Change timbre" button ($\ge 56$ px touch target).
       - When "Change timbre" is activated: opens modal list titled "CHOOSE TIMBRE FOR PART n" with installed timbres (Grand Piano, Rhodes Mark I, B3 Tonewheel, Analog Brass, Warm Strings, etc.). Selecting with VALUE + WRITE assigns the chosen timbre and marks Live Set `◆`; EXIT cancels without change.
       - **Zone & Keyboard:**
         - Low key (e.g. `E1 (28)`), High key (e.g. `G7 (103)`): editable via VALUE or key press.
         - Octave shift (-3 to +3), Transpose (-12 to +12 st).
         - Keyboard on/off switch.
       - **Level & Output:**
         - Volume slider (0% to 100%, 0.0 dB).
         - Output selector: `Main L/R` (balanced) or `Aux 1/2`.
   - Hint Bar:
     - `VALUE`: `edit highlighted field`
     - `WRITE`: `change timbre / confirm`
     - `EXIT`: `back to Performance`
     - Reassurance: `Hold WRITE to save edits · Live Set is marked ◆`
2. **Opening PartView (UX-DR82, UX-DR86):**
   - On `PerformanceView`:
     - Touching a Part Strip's card body opens `PartView` for that part.
     - Pushing `VALUE` on `PerformanceView` opens `PartView` for the currently outlined knob-target part.
   - EXIT on `PartView` returns to Performance (or previous surface), preserving edits.
3. **Audio Continuity & Unsaved State:**
   - Edits are communicated to engine/control thread via Protobuf commands (`SetPartTimbre`, `SetPartVolume`, etc.) or local state without RT audio allocations or locks.
   - Live Set is marked dirty (`◆`).
4. **Verification:**
   - Comprehensive test suite in `ui/tests/part_view_test.cpp`.
   - Verify all navigation, field changes, timbre picker, and exit rules.
   - Lint clean: zero hard-coded strings, zero color literals.

</intent-contract>

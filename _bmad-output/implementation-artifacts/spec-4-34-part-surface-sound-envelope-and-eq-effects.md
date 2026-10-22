---
title: 'Story 4.34: Part surface — sound, envelope and EQ, effects'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '7855a77'
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

**Problem:** Keyboardists shaping a part's tone need full direct access to its engine sound parameters (drawbars and tonewheel percussion/click/vibrato/leakage/rotary for organ parts, brightness/cutoff/resonance for sample parts), envelope stages (Attack, Decay, Sustain, Release), 3-band parametric EQ (Low, Mid, Mid Freq, High), and effects (direct link to part insert chain, reverb send, delay send) directly on the instrument without RT glitching.

**Approach:**
1. **Extend `PartView` Sections (`part_view.hpp` & `part_view.cpp`):**
   - Add sections to `PartSection`:
     - `SoundEngine`
     - `EnvelopeEQ`
     - `EffectsSends`
   - **Sound Engine Section:**
     - Sample parts: Brightness (0-100%), Cutoff (20-20000 Hz), Resonance (0-100%), Velocity Sensitivity (0-100%).
     - Organ parts:
       - 9 drawbars (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1') with numeric registration display (e.g. `88 8000 000`).
       - Percussion: On/Off, Soft/Normal, Fast/Slow, 2nd/3rd harmonic.
       - Key Click: 0..8.
       - Vibrato/Chorus: V1, C1, V2, C2, V3, C3, Off.
       - Leakage: 0..8.
       - Rotary: Stop, Slow, Fast.
   - **Envelope & EQ Section:**
     - Envelope: Attack (ms), Decay (ms), Sustain (%), Release (ms).
     - 3-band EQ: Low Gain (-12..+12 dB), Mid Gain (-12..+12 dB), Mid Frequency (200..8000 Hz), High Gain (-12..+12 dB).
     - Synchronized with physical encoders 1..8 in PART mode.
   - **Effects Sends Section:**
     - Insert Chain link button: opens `EffectsView` scoped to the current part's inserts (`EffectsViewMode::ChainList`).
     - Reverb Send level: 0%..100%.
     - Delay Send level: 0%..100%.
   - **Arpeggiator Guard:**
     - Section is suppressed/hidden until Epic 7 arpeggiator engine integration.
2. **Physical Controls & Navigation:**
   - VALUE encoder adjusts the currently highlighted parameter.
   - Encoders 1..8 in PART mode adjust the corresponding sound/envelope/EQ parameters.
   - EXIT returns to the previous view (or chain return).
   - Any modification marks the Live Set as unsaved (`◆`).
3. **Verification:**
   - Expand `part_view_test.cpp` to verify sample parameters, organ drawbars/percussion/rotary, envelope/EQ edits, insert chain navigation, and send levels.
   - Lint checks pass: zero raw colors, zero hard-coded strings.

</intent-contract>

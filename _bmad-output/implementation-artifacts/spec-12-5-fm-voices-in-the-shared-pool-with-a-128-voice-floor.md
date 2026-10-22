---
title: 'Story 12.5: FM voices in the shared pool with a 128-voice floor'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '5623ede'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/voice_arbiter.hpp'
  - '{project-root}/engine/core/src/voice_arbiter.cpp'
  - '{project-root}/engine/core/include/velora/core/fm_engine.hpp'
  - '{project-root}/engine/core/src/fm_engine.cpp'
  - '{project-root}/engine/core/include/velora/core/fm_voice.hpp'
  - '{project-root}/engine/host/include/velora/host/live_set_loader.hpp'
  - '{project-root}/engine/host/src/live_set_loader.cpp'
  - '{project-root}/engine/core/tests/voice_arbiter_test.cpp'
  - '{project-root}/engine/core/tests/fm_engine_test.cpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists layering FM voices with sample piano and organ need FM parts to share the single real-time voice pool and be guaranteed a 128-voice floor. An FM layer must never starve the piano, and a dense piano part must never starve the FM layer below its 128-voice floor.

**Acceptance Criteria (from Epics - Story 12.5):**
1. **Live Set FM Floor Reservation (FR12, FR6, FR7, NFR8, AR18):**
   - Given a Live Set with at least one FM part, when it loads, the arbiter reserves the FM floor ($\ge 128$ voices at the FM cost per voice, 4 cost units per voice) after the FX budget, the organ/rotary reservation, and the sample floor.
   - The FM floor is reserved *only* when an FM part is present in the Live Set (`fm_parts > 0`). When no FM part is present, zero voices are reserved for FM (`waive_fm_floor = true`).
2. **Whole-Live-Set Allocation and Stealing Rules:**
   - FM voices are allocated and stolen strictly by the arbiter under the existing whole-Live-Set stealing rules:
     - Lowest held note across all engines is protected (priority 3).
     - Pedal-sustained notes are protected above held notes (priority 2).
     - Ordinary held notes steal newer and quieter first (priority 1).
     - Releasing voices steal first (priority 0).
     - Organ voices are steal-protected and never stolen.
     - An engine at or below its floor is never stolen from by another engine.
     - A stolen FM voice fading out over `kStealRampFrames` (4 ms / 192 frames) keeps its slot and continues counting against budget until silent.
3. **Engine Core Unit Verification:**
   - Given scripted scenarios in `engine/core` unit tests: FM pad layered over a 256-voice piano, sustain pedal down, and organ present.
   - Victims match the expected priority order, and FM never drops below its 128-voice floor while the sample engine keeps its 256-voice floor.

</intent-contract>

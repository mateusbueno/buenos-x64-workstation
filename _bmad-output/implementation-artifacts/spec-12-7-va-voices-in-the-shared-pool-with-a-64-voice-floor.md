---
title: 'Story 12.7: VA voices in the shared pool with a 64-voice floor'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'f35af86'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/voice_arbiter.hpp'
  - '{project-root}/engine/core/src/voice_arbiter.cpp'
  - '{project-root}/engine/core/include/velora/core/va_engine.hpp'
  - '{project-root}/engine/core/src/va_engine.cpp'
  - '{project-root}/engine/core/include/velora/core/va_voice.hpp'
  - '{project-root}/engine/host/include/velora/host/live_set_loader.hpp'
  - '{project-root}/engine/host/src/live_set_loader.cpp'
  - '{project-root}/engine/core/tests/voice_arbiter_test.cpp'
  - '{project-root}/engine/core/tests/va_engine_test.cpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists layering VA voices with sample, organ, and FM parts need VA parts to share the single real-time voice pool and always get at least 64 voices, so that a VA pad or lead plays reliably alongside piano, organ, and FM. Mono/legato VA parts must use exactly one voice per part.

**Acceptance Criteria (from Epics - Story 12.7):**
1. **Live Set VA Floor Reservation (FR13, FR6, FR7, NFR8, AR18):**
   - Given a Live Set with at least one VA part, when it loads, the arbiter reserves the VA floor ($\ge 64$ voices at the VA cost per voice, 4 cost units per voice) in the engine-floor step, only when a VA part is present (`va_parts > 0`). When no VA part is present, zero voices are reserved for VA (`waive_va_floor = true`).
2. **Whole-Live-Set Allocation and Mono/Legato Voice Behavior:**
   - VA voices follow the same allocation and stealing rules as the other engines under the VoiceArbiter.
   - Mono/legato VA parts use exactly one voice per part.
3. **Multi-Engine Floor Preservation Verification:**
   - Given unit tests scripting a Live Set with sample (256-voice floor), organ (steal-protected 73-key reservation), FM (128-voice floor), and VA (64-voice floor) parts all at their floors:
   - Every engine keeps its floor.
   - The organ is never stolen.
   - Surplus budget is shared by the stealing rules.

</intent-contract>

---
title: 'Story 14.5: Modeled electric piano voices in the shared pool with a 64-voice floor'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: 'aef35bf'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/voice_arbiter.hpp'
  - '{project-root}/engine/core/src/voice_arbiter.cpp'
  - '{project-root}/engine/core/include/velora/core/ep_engine.hpp'
  - '{project-root}/engine/core/src/ep_engine.cpp'
  - '{project-root}/engine/host/include/velora/host/tier_profile.hpp'
  - '{project-root}/engine/host/include/velora/host/live_set_loader.hpp'
  - '{project-root}/engine/host/src/live_set_loader.cpp'
  - '{project-root}/engine/host/src/tier_profile.cpp'
  - '{project-root}/engine/core/tests/voice_arbiter_test.cpp'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Integrate modeled electric piano (`kModeledEP` = 5) into the whole-system VoiceArbiter pool with a guaranteed 64-voice floor (FR123, FR6, FR7, AR51).
   When a Live Set containing an EP part loads:
   - VoiceArbiter reserves the EP floor (>= 64 voices on v1 tier profile, 128 on Pro) at the declared cost (4 cost units per voice).
   - The reservation occurs strictly after FX and organ/rotary reservations, alongside existing engine floors (Sample, FM, VA).
   - When no EP part is in the Live Set, the floor is waived (`waive_ep_floor = true`, floor = 0).
   - Voice allocation and cross-engine voice stealing follow whole-Live-Set rules: releasing voices first, then held/pedal-sustained, respecting floor protections (an engine at or below its floor is never stolen from to feed another engine).
   - `EpEngine` voice capacity is configured to at least `floor + kFadeHeadroomVoices` (128 voices on v1).
2. Quality & Constraints:
   - RT-safety AD-3: Zero memory allocations, zero locks, zero blocking calls on the audio path.
   - Deterministic steal victim ordering and steal fade ramp (4 ms / 192 frames).
   - Unit tests covering mixed layer scenarios (EP + 256-voice sample piano + organ + pedal sustain), verifying floor preservation and stealing victim hierarchy.
</intent-contract>

## Implementation Plan
1. `engine/core/include/velora/core/voice_arbiter.hpp` & `voice_arbiter.cpp`:
   - Add `kEpFloorVoices = 64`.
   - Add `waive_ep_floor` to `LiveSetConfig`.
   - Add `kEpFloorTooLow` to `ReserveStatus`.
   - Add `set_min_ep_floor_for_tests()` and `min_ep_floor_`.
   - Update `reserve()` to validate EP floor when bound and check `kEpFloorTooLow`.
2. `engine/core/include/velora/core/ep_engine.hpp`:
   - Update `kVoiceCapacity = 128` (64 floor + 64 fade headroom).
3. `engine/host`:
   - Update `tier_profile.hpp` / `tier_profile.cpp`: add `ep_floor_voices` (64 v1 / 128 pro) and `ep_voice_capacity` (128 v1 / 256 pro).
   - Update `live_set_loader.hpp` / `live_set_loader.cpp`: track `ep_parts`, set `ep_floor_voices`, waive when 0.
4. Unit Tests:
   - In `engine/core/tests/voice_arbiter_test.cpp`:
     - Test EP reservation, floor verification, `kEpFloorTooLow`, `waive_ep_floor`.
     - Multi-engine layered scenario: EP + Sample + Organ + pedal sustain.
     - Verify steal victim order and floor protection for EP.

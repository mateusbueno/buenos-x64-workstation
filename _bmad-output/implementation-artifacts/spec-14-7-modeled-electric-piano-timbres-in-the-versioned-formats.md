---
title: 'Story 14.7: Modeled electric piano timbres in the versioned formats'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '3cfb7d1'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/params.json'
  - '{project-root}/proto/live_set.proto'
  - '{project-root}/engine/core/include/velora/core/ep_parameters.hpp'
  - '{project-root}/engine/core/include/velora/core/ep_engine.hpp'
  - '{project-root}/engine/core/src/ep_engine.cpp'
  - '{project-root}/engine/host/tests/live_set_loader_test.cpp'
  - '{project-root}/engine/core/tests/ep_engine_test.cpp'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Persist modeled electric piano voicing parameters in the versioned formats (`EngineData` in `live_set.proto` and parameter registry `proto/params.json`):
   - Parameters: `ep_model_type` (1300), `ep_hammer_hardness` (1301), `ep_pickup_position` (1302), `ep_pickup_level` (1303), `ep_damper_noise` (1304), `ep_attack_noise` (1305), `ep_decay_rate` (1306), `ep_tone_bar_coupling` (1307).
   - Versioned format compliance: Protobuf versioned fields with zero schema migrations, N and N-1 compatibility, and unknown fields preserved.
   - Persistence round-trip: Saved timbre/LiveSet round-tripping through protobuf serialization/deserialization restores every voicing parameter identically and renders bit-identical audio output.
   - Unbound engine fallback: If loaded on a system/firmware where `kModeledEP` (5) is not bound, the part reports `PartState::kEngineNotAvailable`, remains silent, and retains all its parameters untouched.
2. Quality & Constraints:
   - Zero heap allocations or blocking operations during audio rendering.
   - Bit-identical offline rendering before and after parameter round-trip.
</intent-contract>

## Implementation Plan
1. Add EP part tests to `engine/host/tests/live_set_loader_test.cpp`:
   - `test_ep_part_floor_reservation()`: EP part present reserves 64 voices * 4 = 256 cost units; EP part absent waives reservation (0 cost).
   - `test_ep_part_unbound_engine_not_available()`: when EP engine is not bound in renderer, part transitions to `kEngineNotAvailable`, stays silent, and keeps data.
2. Add EP parameter serialization roundtrip & bit-identical verification to `engine/core/tests/ep_engine_test.cpp`:
   - `test_ep_voicing_parameters_persistence_and_bit_identical_render()`

---
title: 'Story 14.11: Init and factory modeled electric piano timbres'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: 'ffd0d93'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/formatlib/include/velora/format/ep_format.hpp'
  - '{project-root}/formatlib/src/ep_format.cpp'
  - '{project-root}/formatlib/include/velora/format/factory_ep.hpp'
  - '{project-root}/formatlib/src/factory_ep.cpp'
  - '{project-root}/formatlib/include/velora/format/parametric_bundle.hpp'
  - '{project-root}/formatlib/src/parametric_bundle.cpp'
  - '{project-root}/engine/host/include/velora/host/library_resolver.hpp'
  - '{project-root}/engine/host/src/library_resolver.cpp'
  - '{project-root}/librarian/tests/story_14_11_test.cpp'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Provide ready-made factory and Init electric piano timbres in the Velora Library:
   - Factory Electric Piano Sound Pack (`create_factory_ep_bundle()`):
     - At least 6 tine timbres (Stage 73 Pure, Stage 73 Bright, Suitcase 73 Warm, Suitcase 88 Bell, Studio Vintage Tine, Dyno Mod Tine) + 1 Init Tine.
     - At least 4 reed timbres (200A Classic Reed, 200A Bark Reed, 140B Early Reed, Vintage Reed Bell) + 1 Init Reed.
     - Total 12 factory timbres under Category `E.Piano`.
   - Built-in resolution (`kBuiltinLibraryId`):
     - `kInitTineInstrumentKey` ("init-tine", "Init Tine") and `kInitReedInstrumentKey` ("init-reed", "Init Reed") resolve directly without filesystem access.
   - Strictly generic naming (NFR29):
     - Zero references to "Rhodes", "Wurlitzer", "Wurli", "Clavinet", "Fender", etc.
   - Format serialization & librarian catalog indexing:
     - Parametric bundle container serialization (`VLPB`) extended to pack and unpack `EpVoiceParameters` (64 bytes).
     - SQLite library and instruments tables catalog and query all factory timbres under `E.Piano`.
   - Coexistence:
     - Existing sampled electric piano libraries remain installed and unchanged.
   - 8-Part Live Set recall & effects rendering:
     - 8-part Live Sets recalling factory EP timbres preserve voicing parameters, zones, send levels, and render cleanly through insert effects (EP Amp Drive and Stereo Chorus Phaser).
2. Verification:
   - Automated acceptance test suite `story_14_11_test` in `librarian/tests/story_14_11_test.cpp` passes 100%.
</intent-contract>

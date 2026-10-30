---
title: 'Story 14.9: Part surface Sound section for modeled electric pianos'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '6eb713e'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/ui/include/velora/ui/screens/part_view.hpp'
  - '{project-root}/ui/src/screens/part_view.cpp'
  - '{project-root}/ui/tests/part_view_test.cpp'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Implement the on-device Part surface Sound section for Modeled Electric Piano timbres in LVGL:
   - Grouping: 4 groups (`Model`, `Hammer`, `Pickup`, `Damper`) navigated via Segmented Control and VALUE encoder.
   - Voicing parameters: Model type (Tine/Reed), Decay rate, Tone bar coupling, Hammer hardness, Attack noise, Pickup position, Pickup level, Damper noise.
   - Live real-time modulation: Knob 2 (Brightness) smoothly offsets hammer stiffness without zipper noise; ASSIGN K1..K8 directly map each voicing parameter.
   - Hierarchical EXIT navigation: edit field -> field focus -> group strip -> Part section list -> Performance.
   - Unbound / Not-Available state (`kEngineNotAvailable`): displays clean warning card, prevents editing, part remains silent, parameters preserved.
   - Process isolation: UI runs in a separate process; UI restart never interrupts audio thread or drops sound.
2. Verification:
   - Automated unit test suite in `ui/tests/part_view_test.cpp` (`test_part_view_modeled_ep_sound_section`) passes 100%.
</intent-contract>

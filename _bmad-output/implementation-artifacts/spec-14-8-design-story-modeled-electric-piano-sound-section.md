---
title: 'Story 14.8: Design story: modeled electric piano Sound section'
type: 'design'
created: '2026-10-30'
status: 'done'
baseline_revision: '694b4d6'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/part-surface-modeled-ep.html'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/studio-part-editor-modeled-ep.html'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Specify the Modeled Electric Piano Sound section for both the on-device Part surface (LVGL) and the desktop Velora Studio Part Editor, adhering to Velora design language:
   - Fields, ranges, units, and defaults:
     * `ep_model_type` (Tine [0], Reed [1]; default Tine)
     * `ep_hammer_hardness` (0.00..1.00, ratio, default 0.50)
     * `ep_pickup_position` (0.00..1.00, ratio, default 0.50)
     * `ep_pickup_level` (0.00..1.00, ratio, default 0.70)
     * `ep_damper_noise` (0.00..1.00, ratio, default 0.30)
     * `ep_attack_noise` (0.00..1.00, ratio, default 0.30)
     * `ep_decay_rate` (0.00..1.00, ratio, default 0.50)
     * `ep_tone_bar_coupling` (0.00..1.00, ratio, default 0.50)
   - Encoder mapping (ASSIGN K1..K8 and fixed panel encoders).
   - Display rules: Always show Category (`E.PIANO`) and Timbre Name; never show raw engine type name (NFR29).
   - Unbound / Not-Available state (`kEngineNotAvailable`): Part surface and Studio show plain informational notice; controls disabled, data preserved, part silent.
   - Multilingual specifications in English, PT-BR, and ES.
   - Responsive HTML mockups for both on-device LVGL 1024×600 (Stage & Studio themes) and Velora Studio Part Editor (100%, 150%, 200% reflow).
</intent-contract>

---
title: 'Story 14.10: Velora Studio Part Editor section for modeled electric pianos'
type: 'feature'
created: '2026-10-30'
status: 'done'
baseline_revision: '1e26c09'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/desktop/src/live_set_view.js'
  - '{project-root}/desktop/src/part_editor.js'
  - '{project-root}/desktop/tests/part_editor.test.js'
  - '{project-root}/desktop/tests/part_editor_dom.test.js'
  - '{project-root}/desktop/tests/support/fake_unit.js'
warnings: []
deferred: []
---

<intent-contract>
1. Goal:
   Implement and integrate the Modeled Electric Piano (EP) voicing section in Velora Studio Part Editor:
   - Registry-driven architecture: EP parameters (IDs 1300..1307) automatically surface into the 4 sound groups (`ep.model`, `ep.hammer`, `ep.pickup`, `ep.damper`).
   - Live editing over WebSocket: Editing EP voicing sliders coalesces per-knob requests and transmits `SetPerformanceParam` with optimistic UI update and verification via live read-back.
   - Offline editing & backup persistence: When disconnected, backups with EP timbres are editable offline; changes persist and apply on the next connection via Write to Velora.
   - ASSIGN K1..K8 integration: All 8 EP parameters are registered as assignable to performance macro knobs.
   - Inert parameter tagging: Amp attack / release parameters are marked inert for EP engine parts since EP voices use dedicated physical decay and coupling parameters.
   - Engine not available state: If the unit reports `kEngineNotAvailable`, Studio renders controls in read-only state with explanation (`studio.part_editor.ro_engine_unavailable`) while preserving all parameter values.
2. Verification:
   - Automated test suite in `desktop/tests/part_editor.test.js` verifying model construction, assignable options, live editing via FakeUnit, and unavailable engine read-only state.
   - DOM integration test suite in `desktop/tests/part_editor_dom.test.js` verifying accessible slider rendering, ARIA attributes, and keyboard navigation.
   - Full suite execution via `ctest --test-dir build -R desktop_live_set_view_node_tests` passes 100%.
</intent-contract>

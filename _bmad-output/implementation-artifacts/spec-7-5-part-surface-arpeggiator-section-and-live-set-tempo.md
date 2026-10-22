---
title: 'Story 7.5: Part surface Arpeggiator section and Live Set tempo'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '030a40b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/part-surface.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to configure arpeggiator parameters (on/off, mode, rate, octaves, gate, swing, latch) per part and adjust the Live Set tempo directly from the Part surface without needing a computer.

**Approach:**
1. Extend `PartView` (`ui/include/velora/ui/screens/part_view.hpp`, `ui/src/screens/part_view.cpp`):
   - Add `PartSection::Arpeggiator` to the navigation sidebar (now 7 sections).
   - In Arpeggiator section, list:
     - Arp: On / Off (Segmented Control)
     - Mode: Up / Down / Up-Down / Random / As played (Segmented Control / selector)
     - Rate: 1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T, 1/32T (Segmented Control / selector)
     - Octaves: 1, 2, 3, 4 (Segmented Control)
     - Gate %: 5% to 100% (default 50%, with CLDR percent formatting)
     - Swing %: 50% to 75% (default 50%, with CLDR percent formatting)
     - Latch: On / Off (Segmented Control)
     - Tempo: 20.0 to 300.0 BPM (default 120.0 BPM; read-only when following external clock)
   - Value editing: VALUE encoder changes highlighted field; touch selects.
   - When Tempo changes:
     - Updates Live Set tempo for all parts.
     - Calls `on_tempo_edit(bpm)` and marks the Live Set dirty (`◆`).
   - When Arpeggiator settings change:
     - Updates part arpeggiator state.
     - Calls `on_arp_edit(part_idx, settings)` and marks the Live Set dirty (`◆`).
     - Note: Arpeggiator edits are scene-scoped, so Scene write scope remains available!
2. Localization:
   - Add catalog keys to `locale/en.json`, `locale/es.json`, `locale/pt-BR.json`.
3. Unit tests in `ui/tests/part_view_test.cpp`:
   - Test Arpeggiator section navigation.
   - Test changing Arp On/Off, Mode, Rate, Octaves, Gate, Swing, Latch, and Tempo.
   - Test that Write Dialog keeps Scene scope available for Arp edits.

</intent-contract>

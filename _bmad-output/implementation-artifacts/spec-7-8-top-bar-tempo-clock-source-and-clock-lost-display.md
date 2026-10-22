---
title: 'Story 7.8: Top bar tempo, clock source and clock-lost display'
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
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists playing live or in rehearsal need unambiguous visual feedback in the Top Bar and Status Bar about the current tempo, whether the instrument is synced to internal or external clock (DIN/USB), and an immediate clear warning when external clock is lost, without screen flicker or relying on color alone.

**Approach:**
1. Top Bar tempo display in `PerformanceView`:
   - Internal clock source: shows `♩ 72` with the Live Set tempo.
   - External clock source (DIN / USB): shows followed tempo rounded to whole BPM with source name (`♩ 72 · DIN`, `♩ 72 · USB`).
   - Throttling: tempo updates are throttled to no faster than twice per second (500 ms) to avoid visual flicker.
2. Clock Lost state handling:
   - When external clock ticks cease without a Stop message (`clock_lost` active):
     - Top Bar displays `⚠ CLOCK LOST` (or `⚠ CLOCK PERDIDO`) in warning style (`p.warn`) with glyph and text.
     - Status Bar shows warning takeover: `⚠ CLOCK LOST · ARP AT ♩ 72` (using `tr("performance.status.clock_lost_takeover")`).
     - State never relies on color alone (both glyph `⚠` and text provided).
     - When clock resumes, `clock_lost` clears and normal tempo/source display is restored.
3. Read-only Tempo on Part surface:
   - When external clock following is active, the Tempo field in Part surface Arpeggiator section is read-only and displays `♩ 72 · DIN`.
4. Localization:
   - Translated in EN, PT-BR and ES (`shell.top_bar.clock_lost`, `performance.status.clock_lost_takeover`).
5. Unit tests:
   - Covered in `ui/tests/performance_view_test.cpp` and `ui/tests/part_view_test.cpp`.

</intent-contract>

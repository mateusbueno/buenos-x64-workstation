---
title: 'Story 9.4: Design story — Velora Studio footer, dialog, banner and Backup & Restore'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '3eaab7b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/studio-footer-dialog-banner.html'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/studio-backup-restore.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Implementation of Velora Studio requires visual clarity, component anatomy, and state specifications for elements not shown on the main window mockup (`key-screen-studio.html`): the Studio Footer, modal Studio Dialogs, non-modal Studio Banners, the Backup & Restore screen, Export Dialog, and Import collision resolution.

**Approach:**
1. Created `mockups/studio-footer-dialog-banner.html` rendering in Stage and Studio themes:
   - 5 Footer states: idle with last backup, `● LIVE EDIT ON`, `UNSAVED: SCENE 2` with warn ◆, `WRITING TO VELORA…`, and transfer progress.
   - 3 Dialog variants: Pairing code entry with wrong-code message, Write to Velora scope (Scene / Whole Live Set / Save as new…) with disabled-reason line, and Restore Merge / Replace with explicit outcome statements.
   - 4 Banner variants: Disconnect · Reconnect, Version mismatch · Update firmware/app, Write failed · Retry, and Stale edit · Reload.
2. Created `mockups/studio-backup-restore.html` rendering in Stage and Studio themes:
   - Backup & Restore surface with backup location selection, last backup timestamp, and restore file pick.
   - Export Dialog with Live Sets / Timbres / Bundle scopes, Studio Selection List with individual sizes, locked Reference-only rows, total size estimate, and large-bundle warning.
   - Import Dialog with per-item collision Segmented Control (Skip / Replace / Keep both) defaulting to Keep both.
3. Verified against WCAG AA contrast (≥4.5:1 text, ≥3:1 boundaries), 2px focus rings at 2px offset, 200% text reflow without horizontal scroll, and +40% pseudo-localization with `ÁÇÊÕÑ Ação`.
4. Updated `DESIGN.md`, `EXPERIENCE.md`, and `.memlog.md` with links and decisions.

</intent-contract>

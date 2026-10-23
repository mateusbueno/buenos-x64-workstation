---
title: 'Story 9.16: Backup to a .velorabackup file'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: 'cc12fed'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/backup.proto'
  - '{project-root}/desktop/src-tauri/src/backup_restore.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/desktop/src-tauri/build.rs'
  - '{project-root}/desktop/src/index.html'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/locale/en.json'
  - '{project-root}/locale/es.json'
  - '{project-root}/locale/pt-BR.json'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need a full backup of all setlists and Live Sets written to a versioned `.velorabackup` file on their computer so that a broken or lost Velora unit never costs them their show (FR93, AR29, AR30, UX-DR71, UX-DR45, UX-DR91).

**Approach:**
1. **`.velorabackup` Schema & Versioning (`proto/backup.proto`):**
   - Defined `velora.backup.v1.VeloraBackup` format containing `format_version` (AD-12, N=1), `device_model` ("Velora 73"), `firmware_version` ("1.2.0"), `backup_time` (ISO-8601 UTC), `notes`, `setlists` (`SetlistLibrary`), `assign_default` (`AssignDefault`), `favorites` list, and `live_sets` (`LiveSet` list).
   - Live Set timbres store only `library_id` and `instrument_key` references, never audio or sample binary data.
2. **Unsaved Edits Handling:**
   - If a backup is triggered while player has unsaved edits on the unit, the backup writes the last saved state and sets the note: `"Unsaved edits on the Velora are not in this backup"`.
3. **Save Dialog & Device Memory:**
   - When Backup is clicked (Title Bar or Device › Backups), asks for file location.
   - Saves `.velorabackup` file.
   - Updates Footer with new last backup timestamp, announces completion in a polite live region (`aria-live="polite"`), and remembers backup timestamp per device in localStorage (`velora_last_backup_<device_id>`).
4. **Automated Verification:**
   - Unit tests verify roundtrip serialization, reference-only timbre preservation, unsaved edits notes, and CTest integration.

</intent-contract>

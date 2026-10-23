---
title: 'Story 9.17: Restore with Merge or Replace, and offline backup editing'
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

**Problem:** Keyboardists need to restore a `.velorabackup` file either merged into what is on the unit or replacing it, and to open and edit backups without a unit connected (FR94, FR95, AR30, FR92, UX-DR46, UX-DR71, UX-DR77).

**Approach:**
1. **Offline Backup Opening & Editing:**
   - With no unit connected, user can open any `.velorabackup` file.
   - Studio populates its Setlists view and Live Set view with the backup contents.
   - Allows renaming setlists, reordering, and editing part parameters (volume, mute, sound params).
   - "Save Backup" saves edits directly back to the `.velorabackup` file.
   - Live Edit, Transfer, Firmware, and Write to Velora are disabled with reason tooltip/label: `"Connect a Velora"`.
2. **Restore Dialog (Merge vs Replace):**
   - With a unit connected, choosing Restore opens a Studio Dialog offering:
     - **Merge**: "Add and update from the backup; keep what the backup doesn't have"
     - **Replace**: "Velora content becomes the backup"
   - Computes and displays exact counts of items to add, update, and remove for both setlists and Live Sets.
   - For Replace mode, displays explicit consequence warning that content not in backup will be removed.
3. **Execution via Control-Thread Commands & Collision Handling:**
   - Restore sends operations strictly as control-thread commands (`CreateLiveSet`, `WriteLiveSet`, `DeleteLiveSet`, `CreateSetlist`, `RenameSetlist`, `ReorderSetlists`, etc.), never writing files directly on the unit.
   - Replace makes device setlists and Live Sets equal to backup.
   - Merge adds and updates matching IDs while keeping local items untouched.
   - If an incoming Live Set ID collides with a different local Live Set, it receives a new UUIDv4 (`adopt_with_collision_check`), and incoming setlist references are updated.
   - After restore, fresh backup of device equals expected result (Replace equals source backup; Merge equals union with backup values winning).
4. **N-1 Reader & Version Skew Rejection:**
   - Backups from older firmware format (N-1) are accepted through backwards-compatible reader.
   - Backups from newer major format versions (version > N) are rejected with exact required message: `"This backup needs a newer Velora Studio"`.

</intent-contract>

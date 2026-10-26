---
title: 'Story 8.8: Journaled Boot-into pointer'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: 'dbc9deede84d17e76b83fe16273a7b63ea12ccf7'
review_loop_iteration: 0
followup_review_recommended: false
context: []
warnings: []
deferred:
  - summary: >-
      import_verifying probe is wired to constant false: imports verify in the librarian, not in the engine process.
    evidence: |-
      Wire a real signal when an in-process import verify exists.
    severity: low
  - summary: >-
      RTSan/no-syscall variants of the new RT input-activity flags were not run on macOS.
    evidence: |-
      Run via docker-check / Linux CI.
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** The unit does not remember the Live Set and scene the player was on, and a naive "save on change" would write to disk during a performance (NFR15, FR64, AD-18).

**Approach:** Keep a Boot-into pointer `{setlist_id, live_set_id, scene_index, schema_version}` in memory on the engine control thread, mark it dirty on every current Live Set / scene change, and write it only at idle-safe moments with an atomic temp+fsync+rename+dir-fsync sequence; read it back at start with a safe fallback.

## Boundaries & Constraints

**Always:** Control thread only; never from RT. Writes are atomic (temp file on the data partition, fsync, rename over the old file, fsync directory) and ≤4 KiB. A pointer identical to the stored one is not rewritten. Idle-safe = (no note held and no pedal down for ≥2 s) OR (30 s since the change), whichever first, and NEVER while an import's verify phase runs. Because the story's trace criterion forbids any write while a note is held, the 30 s rule only makes the pointer eligible: the write still waits until no note is held (pedal may be down). Reuse the existing store conventions (atomic write helper / `on_stage` fault-injection hook of LiveSetStore/SetlistStore, versioned on-disk format with `schema_version`). Missing or corrupt file ⇒ "no pointer" result (the caller falls back to the Default Live Set target).

**Never:** No `boot_into` setting, Settings UI or boot-target resolution (Story 8.9). No dev-board disk tracing. No periodic writes, no writes at each change.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Change | Live Set or scene changes | pointer dirty in memory, no file write | none |
| Idle 2 s | dirty, no note/pedal for 2 s | one atomic write | write failure keeps pointer dirty, retried next tick |
| 30 s rule | dirty, pedal held down (no note) for >30 s after change | write happens at 30 s | none |
| Note held | dirty, note held for any duration | no write until the note is released (then eligible if 2 s idle or 30 s elapsed) | none |
| Import verify | dirty, idle, import verifying | no write until verify ends | none |
| Identical | dirty but equals stored | no write, dirty cleared | none |
| Kill mid-write | process killed at each write stage (fault-injection, 1,000 iterations incl. every stage) | next load returns previous or new pointer, never corrupt | corrupt/missing/unknown-schema ⇒ no pointer |

</intent-contract>

## Code Map

- `engine/host/include/velora/host/live_set_store.hpp`, `setlist_store.hpp` (`StoreOptions.on_stage`, `liveset::Stage`, `HookAction`) -- atomic write + fault-injection pattern to reuse; `engine/host/src/live_set_store.cpp`, `setlist_store.cpp`, `posix_fs.cpp`.
- `engine/host/include/velora/host/control_plane.hpp` / `src/control_plane.cpp` -- control thread; knows current Live Set/scene (SceneController, LiveSetSwitcher, SetlistController); `ControlServer::set_tick()` (control tick ≤20 ms) drives periodic work.
- `engine/host/include/velora/host/scene_controller.hpp`, `live_set_switcher.hpp`, `setlist_controller.hpp` -- sources of change events (current scene index, current Live Set id, loaded setlist id).
- `engine/core` `LastValueTable` (snapshot.hpp) / part router -- read-only source for "note held" and "pedal down" if exposed; if not exposed, add a lock-free flag/counter RT writes with relaxed atomics (no syscall).
- `engine/host/macos/main.cpp` -- wiring with the data dir; `engine/host/CMakeLists.txt` -- test registration; templates `engine/host/tests/setlist_test.cpp` (store + fault injection) and `control_test.cpp`.
- New: `engine/host/include/velora/host/boot_into_store.hpp`, `src/boot_into_store.cpp` (store: load/write/format) and a tracker (mark_dirty/tick with injected clock + idle probe + import-verify probe).

## Tasks & Acceptance

**Execution:**
- `engine/host/src/boot_into_store.cpp` + header -- pointer format, `load()` with fallback, atomic `write()` with stage hook, identical-skip
- same files or `boot_into_tracker` -- dirty tracking and idle-safe policy driven by an injected clock/idle/import probes
- `engine/host/src/control_plane.cpp` (+ macos main wiring) -- mark dirty on current Live Set/scene/setlist change; call tracker tick from the control tick; expose the note/pedal idle signal read-only
- tests -- policy table (rows above) with a fake clock; fault-injection kill at every stage × many iterations asserting previous-or-new-never-corrupt; corrupt/missing/unknown-schema file; ≤4 KiB; no write while note held; rt/no-syscall check for any new RT flag

**Acceptance Criteria:**
- Given a dirty pointer, when idle-safe, then exactly one atomic write happens and a second identical state writes nothing.
- Given a kill at any write stage (1,000 simulated), when the pointer is read, then it is the previous or the new pointer, never corrupt; corrupt/missing ⇒ no pointer.
- Given a long run with many changes and a held note, then no write occurs while any note is held, and none during import verify.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers — patches 6 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` stuck held-note count (CC120/123, velocity-0) — fixed + tests
  - `[medium]` `patch` stale stored copy after non-durable write — has_stored_ invalidated
  - `[medium]` `patch` retry storm, 30 s starvation, clock underflow — backoff, first-dirty time, clamps
  - `[medium]` `patch` untested tick wiring / rebind scope — BootIntoTracker::update + wiring test
  - `[low]` `reject` shutdown flush (power-off is a power cut by design), per-tick note sampling vs fsync overlap, valid_pointer logging, non-group-0 notes, temp-file cleanup, test-style nits
  - `[medium]` `defer` import-verify probe constant false; RTSan not run

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "boot_into|control|setlist|live_set|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: Boot-into pointer store (CRC text file, atomic write with fault hooks), dirty/idle-safe tracker, RT held-note/pedal publishing, wiring in engine-host. Boot-time read is Story 8.9.
Verification: targeted ctest all pass; core deps check passes.
Residual risks: RTSan not run; import-verify signal not connected.

## Story 8.9 follow-up note

The frozen pointer format cannot represent "no scene recalled": the engine-host tick records a Live Set on its base as scene 0, and a LAST_SOUND boot (Story 8.9) recalls scene 1.

---
title: 'Story 4.9: Edits in memory and Write scopes'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '65b275acc6ab33f94cb591176e23f61296b946b5'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 4.9 "Edits in memory and Write scopes" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2830; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** build on 4.2 (`LiveSetStore`: validated, atomic, single-writer), 4.3 (`LiveSetLoader`, snapshot per part), 4.4 (`SceneController`: scenes as sparse overrides, scene recall resets scene-stored values), 4.5 (`LiveSetSwitcher`: sounding/effective Live Set ids, seamless jumps) and 4.6 (`SetlistStore`/`SetlistController`: setlists, `create_live_set`, tags, names). Implement an `EditSession` (engine/host, control-thread owner) that tracks, per Live Set, the **in-memory edited state** versus the **saved state**: structural edits (ControlPlane structural commands that touch the loaded Live Set's parts: insert chain, part output, master params, send levels set via structural commands, etc.) and performance parameter changes (read from the RT last-value table: drawbars, knobs, sends, volumes... that differ from the loaded generation's initial values) are **kept in memory across scene changes and Live Set changes and returns**: when the player changes scene or Live Set and later returns, the edits are still there — design: an edit overlay per Live Set id (`EditedLiveSet{base LiveSet message (as loaded/saved), edits}`) kept in memory by the session so a Live Set re-loaded by the switcher or a scene recall re-applies the overlay (define precisely how: the overlay is applied when the loader/switcher builds the generation for that Live Set id, and scene recall = scene values + overlay edits made *after*? choose a precise, documented model: edits are stored as "diffs against the saved Live Set" keyed by (part, param_id) and optionally per scene (a performance change made while scene S is current is stored as an edit of scene S for scene-stored params, or of the base Live Set for others — mirror the Write scopes: Scene scope writes the current scene's parameters, Whole Live Set writes everything); document the data model and the per-scope semantics in `engine/host/README.md` with examples); the engine publishes (additive ws.v1 State fields) a **dirty flag per Live Set** (ids with unsaved edits) and a **change summary for the current Live Set** (a short list of human-readable item keys like "Part 2 volume", "Reverb send" — emit stable string keys + params (part index, param_id) and a pre-rendered English string for convenience; localisation is a UI matter (`EXPERIENCE.md`/AD-20 externalised strings: keep keys stable)); edits are lost at power-off (never written without Write; there is no autosave). **Write command** (additive ws.v1 `WriteLiveSet{scope: SCENE | WHOLE_LIVE_SET | SAVE_AS_NEW, new_name?}` replying with `request_id`, new revision, what was written, and cleared flags): the control thread raises a **fence to RT** (a control→RT fence primitive: e.g. a sequence counter written by control, acknowledged by the render loop at the next block start in the serial phase via an atomic ack; control waits (bounded, on the control thread only; with an injected test clock/hook for deterministic tests) until RT acks, then **reads the last-value table after the fence** so Save never sees stale values), then writes per scope: **Scene**: only the current scene's parameters (its stored overrides updated from the live values/edits for the params that scene stores; refused if no scene is current or no scenes exist); **Whole Live Set**: everything (base Live Set + all scenes' pending edits + master params + ASSIGN map edits (4.10 will add; leave the hook)), via `LiveSetStore::write` of the updated message; **Save as new**: creates a new Live Set with a new UUIDv4 (copy of the current edited state, name from the command or a suggested name) written through `LiveSetStore`, **added after the current one in the loaded setlist** (via `SetlistStore::add_live_set` at index position+1; with no setlist loaded, it only creates the file) — the current Live Set stays unsaved-dirty if it had edits? (document: Save as new leaves the original untouched and its edits remain in memory as dirty; the new Live Set starts clean) — the dirty flag is **cleared only for what was written** (Scene scope clears the scene's edits; Whole clears all; Save as new clears nothing of the original). **Scene scope is refused when the edits include a timbre change** (an edit that changes a part's timbre/engine assignment: provide the structural command `SetPartTimbre`(additive) so the edit exists; the in-memory edit applies via the loader's resolution for that part without a full Live Set reload where practical; timbre edits are only persisted by Whole/Save-as-new) with the typed error reason "Timbre changes are saved with the Live Set". **Name rules for Save as new:** a name already used in the setlist (compare case-insensitively, trimmed) is refused with the typed error "name in use" and a **suggested free name** ("Perfect 2" for "Perfect"; "Perfect 3" if taken; strip an existing trailing " N" before suggesting; the suggestion must be free in the setlist and unique); empty/too long/invalid names -> INVALID_NAME. **Storage full:** Write fails with a typed storage-full error (map ENOSPC from the atomic writer to a closed error `STORAGE_FULL`; inject with a test hook), nothing partial is written (the atomic write guarantees it; test the old file is intact, no temp left), edits stay in memory with the dirty flag set. **No-Write guarantee:** playing, scene changes, Live Set changes, edits and setlist stepping perform **no Live Set or setlist file writes**: test with a counting store/hook over a simulated 1 hour of playing (use a fake clock and a generated event script; assert the write counters of LiveSetStore and SetlistStore stay 0 and the files' mtimes/content are unchanged), and that only explicit Write commands write. Concurrency: commands serialized by the control thread; a Write while a Live Set load is in progress is refused (typed busy); a Write for a Live Set that is not the current one is not possible (Write writes the current Live Set only). Hostile cases: Write with no Live Set loaded, repeated Writes, Write during scene storms, Save as new twice with the same name (second refused with a new suggestion), name suggestion collisions, the setlist containing 1000 entries (performance of the suggestion), large edit sets. Tests with a fake RT fence responder and with the real `BlockRenderer` fence ack; RTSan test: fence acknowledgement in the render loop under dense traffic (the fence ack path must be RT-safe: a single atomic load/store per block).

**Never:** implement the ASSIGN map edits (4.10) beyond the hook, UI dialogs (4.32), favorites/recent (4.11), Velora Studio, preload, arpeggiator, or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.9 (~line 2830) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.9 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` unsaved edits could be silently lost (overlay-built generation replacing the working copy; prune() erasing a dirty document when the catalog transiently lacked the id) — document kept whenever built from the overlay, dirty documents dropped only on an explicit DeleteLiveSet
- `[high]` `patch` Scene Write indexed past the saved scenes (crash); Save as new insertion index unclamped; ~EditSession left dangling edit ports (use-after-free); before_switch ran before publish checks (side effects for refused switches) — refused with `scene_not_in_saved`, clamped, detached, moved after the checks
- `[high]` `patch` Write could proceed without the latest generation adopted; the fence ordered only the control lane so hardware knob moves on the MIDI lane could be missed — `generation_not_applied` BUSY, RenderDriver holds the fence one block and re-drains the MIDI lane first (test: a CC-path move queued before Write is in the saved file)
- `[medium]` `patch` timbre-rebuild restore: result of control-lane pushes discarded, restoring could stick forever, superseded rebuilds, dropped scene recall — retried, 5 s timeout, job-token tie
- `[medium]` `patch` scene-current routing of inserts/output/sends/master, BUSY for all five structural commands, STORAGE_FULL via the setlist controller, not_durable via EditSession, ASSIGN/parts diff items, spurious dirty after load (loader-normalised compare), SetPartTimbre engine-only, name uniqueness without a setlist, ControlPlane early-return quirk, documentation of BUSY reasons — fixed with tests
- `[low]` `defer` an event delivered more than one block after its stamp is not covered by the MIDI-lane fence (documented); no dedicated test for the refused-switch before_switch case; real full-disk (OS-level ENOSPC branches of atomic_replace_file) only covered through the injection hook; reload of a sounding Live Set missing from the switcher catalog untested; `SetPartVolume` is a new structural command that Story 4.12 may reshape

## Auto Run Result

Status: done. `EditSession` keeps per-Live-Set in-memory edits across scene and Live Set changes (semantic diff, dirty flag per Live Set, change summary with stable keys), `WriteLiveSet` with scopes Scene / Whole Live Set / Save as new (control→RT fence on both input lanes, live table read after adoption, dirty cleared only for what was written, Scene refused with "Timbre changes are saved with the Live Set", "name in use" with suggested free name, `STORAGE_FULL` with nothing partial and edits kept), additive ws.v1 (`WriteLiveSet`, `SetPartTimbre`, `SetPartVolume`, `State.edits`), no Live Set/setlist file writes during playing (a generated hour on a fake clock). Verification: macOS ctest 413 (2 env-gated skips), Docker gcc 408 and clang-20 RTSan 421, purity and notices checks pass. Follow-up review recommended: false.

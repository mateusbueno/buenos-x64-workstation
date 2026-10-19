---
title: 'Story 4.6: Setlists, setlist tags and Live Set stepping'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: 'a4304df5ef29f0431f5b8a8e62a1b99f4a791147'
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

**Problem and approach:** Story 4.6 "Setlists, setlist tags and Live Set stepping" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2735; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** build on Story 4.2 (`LiveSetStore`, UUIDv4 minting, atomic persistence with the single-writer + control-thread rule) and 4.5 (`LiveSetSwitcher`, ws.v1 commands, State). Add versioned protobuf forms in `proto/` (`setlist.proto`, package `velora.liveset.v1` or sibling): `Setlist{id, name, ordered live_set_ids, tag_ids, format_version}`, `SetlistTag{id, name}`, and a persisted `SetlistIndex`/library file layout (e.g. `<data>/setlists/<id>.setlist`, `<data>/setlists/tags.tags` or a single `setlists.db`-style protobuf file: choose and document; atomic write via the same temp+fsync+rename+dir-fsync discipline as the Live Set store, same single-writer lock/ownership rules, N/N−1 version policy, unknown fields preserved, size caps, validation: names 1..64 bytes UTF-8 no control characters, unique ids, no duplicate Live Set ids inside one setlist (document: or allow? the story says ordered Live Set ids — a Live Set may appear in several setlists but once per setlist), setlist references to missing Live Sets are preserved but flagged (state shows missing), tag ids must exist), plus a compatibility corpus directory seeded with files saved by this release (opened by a ctest on every build, like Story 4.2). A `SetlistStore` (engine/host, control-thread owner) as the **sole writer** implementing commands: create/rename/reorder/delete setlists (reorder = the order of setlists in the list; define a stored `position` or an ordered index), add/remove/reorder Live Sets in a setlist, **create a new Live Set** (id UUIDv4 minted by the store, 8 empty parts, one scene, ASSIGN map copied from the **global default ASSIGN map** — define a global default as a persisted small settings file `assign_default.pb` or registry value with a built-in factory default of 8 `param_id`s (choose sensible ids from the registry, e.g. the knob functions) and a setter command used by Story 4.10 later; here only the read side + the copy), create/rename/delete tags (deleting a tag removes it from all setlists), assign/remove tags on a setlist; deleting a setlist never deletes Live Sets; deleting a Live Set (command) removes it from all setlists (provide via the store's delete API used by commands); all commands validated with typed errors; each command applies atomically (all-or-nothing, file written before state publish) and publishes the new state revision. **Loaded setlist & stepping:** state holds `loaded_setlist_id`, `last_setlist_position`, `current_live_set_id` (from 4.5). `load_setlist(id)`: the current generation keeps playing (no Live Set load triggered); the new setlist becomes the loaded one and **its first Live Set is not loaded until the player steps or chooses**; stepping: `next_live_set`/`previous_live_set` — with a setlist loaded, step relative to the last setlist position (an off-script jump to a Live Set outside the setlist does not change the last setlist position; stepping returns into the setlist relative to it; a jump to a Live Set that IS in the loaded setlist updates the position to its index); with no setlist loaded stepping walks **All Live Sets in name order** (case-insensitive, then id for ties; document the collation); at the end of a setlist "next" does nothing and the state reports `end_of_setlist` (a flag in the state and a typed result); symmetric `start_of_setlist` for previous at the beginning (document); stepping issues `LiveSetSwitcher::jump_to` (seamless switch from 4.5) and returns the typed switch outcome; if the next Live Set file is missing/invalid: skip? (define: report `LIVE_SET_LOAD_FAILED`/missing and do not move the position). ws.v1 (additive): commands for all of the above (`CreateSetlist`, `RenameSetlist`, `ReorderSetlists`, `DeleteSetlist`, `AddLiveSetToSetlist`, `RemoveLiveSetFromSetlist`, `ReorderLiveSetsInSetlist`, `CreateLiveSet`, `CreateTag`, `RenameTag`, `DeleteTag`, `SetSetlistTags`, `LoadSetlist`, `NextLiveSet`, `PreviousLiveSet`, and read: `GetSetlists` returning setlists, tags, loaded setlist, positions, flags) each with `request_id` replies and closed error codes (additive enum values: `NO_SUCH_SETLIST`, `NO_SUCH_TAG`, `DUPLICATE_LIVE_SET_IN_SETLIST`, `INVALID_NAME`, `END_OF_SETLIST`, ...); engine-host wiring (`--data-dir DIR` pointing at the persisted data root containing live_sets/, setlists/; keep `--live-set-dir`) and the `--no-audio` smoke over the socket. Tests: store round trip and golden bytes, validation, every command with persistence checked by re-opening the store, crash-injection atomicity (fork + `_exit` at write stages as in Story 4.2), single-writer enforcement, name collation order, stepping scenarios (setlist loaded; off-script jump then next/previous returning into the setlist; no setlist stepping through All Live Sets; end of setlist; start of setlist; empty setlist; setlist with a missing Live Set), tag deletion cascade, Live Set deletion cascade, new-Live-Set creation content (8 empty parts, 1 scene, ASSIGN copied from the default — and a change of the default does not alter existing Live Sets), the corpus test, ws.v1 over a socket including concurrent clients serialized by the control thread; hostile input fuzz of files (never crashes).

**Never:** implement the Setlist Browser UI/search/favorites (4.30/4.11), Live Set preload (4.7), ASSIGN editing UI (4.10), arpeggiator, UI, or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.6 (~line 2735) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.6 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` a fresh `--data-dir` could never be bootstrapped (engine-host exited with "no Live Set to start with") — starts without a Live Set, CreateLiveSet → JumpToLiveSet → restart covered by the smoke test
- `[medium]` `patch` delete_live_set could leave the library rewritten while the reply said "nothing changed"; catalog entries not named `<id>.liveset` were unlinked at the wrong path — consistent ordering/partial-failure reporting and catalog-path deletion, injected-failure tests
- `[medium]` `patch` LIMIT_REACHED / STORAGE_FAILED never observed over ws.v1; committed-not-durable hidden from clients; position not re-anchored when adding before the current song; step results silently mapped; position moved to targets that never became current — tests and explicit mapping, `not_durable` surfaced
- `[low]` `patch` engine-host smoke now asserts a State push to a subscriber (fails if the control tick wiring is removed)
- `[low]` `defer` End of setlist is an error reply (`END_OF_SETLIST`), unlike the scene precedent; bad setlists.library stops engine-host (exit 1) instead of overwriting; ASCII-only case-insensitive name collation; registry has no brightness/attack/release ids, so the factory ASSIGN default uses existing ids; revisions bump without a snapshot generation

## Auto Run Result

Status: done. Versioned protobuf setlist library (`proto/setlist.proto`, `SetlistStore`: single atomic file, N/N−1, unknown fields preserved, corpus, crash-injection tests), `SetlistController` with 18 additive ws.v1 commands (create/rename/reorder/delete setlists, add/remove/reorder Live Sets, create/delete Live Set with 8 empty parts, one scene and the default ASSIGN map copied, tags with cascade deletes, load/unload setlist, next/previous stepping through the seamless switcher, end/start-of-setlist states, off-script return, All-Live-Sets name order), `engine-host --data-dir`. Verification: macOS ctest and Docker gcc/clang-20 RTSan pass, purity and notices checks pass. Follow-up review recommended: false.

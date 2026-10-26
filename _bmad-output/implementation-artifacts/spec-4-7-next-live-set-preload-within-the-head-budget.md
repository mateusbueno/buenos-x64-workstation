---
title: 'Story 4.7: Next Live Set preload within the head budget (Mac logic)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '87a61bba22fc9c4ac945ea2b841ce17a03d40c33'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md', '{project-root}/engine/host/include/velora/host/live_set_switcher.hpp']
warnings: []
deferred:
  - summary: >-
      Board acceptance not done: command-to-swap under 10 ms on the dev board and head budget against the tier profile (Story 4.1 blocked). Structural 'no load at command time' is asserted on the Mac only.
    evidence: |-
      Counters of provider load/estimate calls do not move across the jump on a READY preload.
    severity: medium
  - summary: >-
      Known limits documented in live_set_switcher.hpp: draining generations are not counted in the preload budget; FAILED/SKIPPED_BUDGET are terminal until the pair or sounding set changes (no retry/backoff); a cancelled preload can delay a user load until the next cancel check; FileSig is size+mtime of the Live Set file only; engine-host has no library-change event (identity re-check at jump and at cadence instead). Untested: refused non-busy commit leaving the preload READY; duplicate ids in a setlist (store cannot hold them).
    evidence: |-
      See header comments and the review triage log.
    severity: low
---

<intent-contract>

## Intent

**Problem:** Stepping to the next Live Set of a loaded setlist always waits for the Story 4.5 background load. The next Live Set should already be resident so "next Live Set" swaps at the next block with no load wait (FR80, AR21, AR20 partial, NFR3 partial). The tier head budget must be shared by the current and the preloaded generation, counting heads used by both once.

**Approach:** In the host switcher/loader (engine/host), when a Live Set becomes current and a setlist is loaded, the control thread preloads the NEXT Live Set of that setlist: it stages the target generation (instrument heads resident, reservation, snapshot) WITHOUT publishing it, within the remaining head budget (budget minus the heads already held by the current generation, with bundles shared by both counted once), publishes a `ready` flag/state for the preloaded set, and when "next Live Set" is issued while the preload is ready, commits the already-staged generation at the next block with no load wait and then starts the preload of the following Live Set. If current + next do not fit the budget even after sharing, the preload is skipped, the state reports not ready, and stepping falls back to the Story 4.5 background load path unchanged.

## Boundaries & Constraints

**Always:** Reuse the Story 4.5 machinery (`LiveSetLoader::stage()/commit_seamless()`, `LiveSetSwitcher`, leases/`BundleLeases`, head sizing from bundle indexes, cancellation, progress). A preloaded generation holds its heads and leases until committed or discarded; discard releases them (and any voice-pool reservation held by the staged generation must not be taken until commit — state how the arbiter reservation is handled: either re-prepared at commit with the 4.5 refusal path, or reserved at preload if the existing API supports it without blocking the current set; document the choice). Invalidate/discard the preload when: the current Live Set changes through any path other than "next" (jump, previous, scene recall does not matter), the setlist is changed/unloaded/reordered so the next entry changes, the next Live Set file is edited/written, or a library change makes its heads stale. A jump to a non-next Live Set cancels the preload. Heads shared between current and next are counted once for the budget (dedupe by instrument bundle identity). Publish preload state in the ws.v1 State (additive, new fields/message; schema grows by addition only): fields at least `next_live_set_id`, `preload_state` (NONE, LOADING, READY, SKIPPED_BUDGET, FAILED), and the head bytes held; the existing loading status for user-initiated loads stays as is. Control thread only (no RT changes except via the existing commit path); injectable clock/hold ticks like 4.5 for deterministic tests. Existing 4.5/4.6 tests must keep passing.

**Never:** No timing claims (the <10 ms command-to-swap on the dev board is a board item: only assert "no load work happens at command time" structurally, e.g. commit does no instrument loading, and measure in blocks/ticks with the injected clock). No tier-profile reading (Story 4.1, blocked): the budget stays the existing `head_budget_bytes` option. No UI changes. No weakening of seamless-switch guarantees (nothing cut, voices finish on the old generation).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Preload starts | setlist loaded, current set becomes active | next set staged within remaining budget; state LOADING then READY | failure → FAILED, falls back |
| Next with READY | "next Live Set" | committed at the next block, no instrument load at command time; preload of the following set starts | none |
| Shared heads | current and next share an instrument bundle | counted once in the budget check and held once | none |
| Over budget | current + next (deduped) > budget | preload SKIPPED_BUDGET, state not ready, next falls back to 4.5 background load | no error to the player |
| Last in setlist | current is the last | no preload (NONE) | none |
| Jump elsewhere | jump to a set that is not next | preload discarded, heads/leases released, new preload for the new current's next | none |
| Setlist changes | next entry changes or setlist unloaded | preload discarded and recomputed | none |
| Set edited | next set's file written | preload invalidated and restaged | none |
| Next while LOADING | "next" before READY | falls back to the 4.5 load for that target (or waits on the in-flight stage; choose the one that keeps cancel semantics simple, document) | none |
| Cancel | cancel_load while preloading | preload dropped; current untouched | none |
| Failure | next file invalid / bundle missing | FAILED, no effect on sound, "next" falls back to 4.5 and reports its normal typed error | typed error as 4.5 |

## Code Map

- `engine/host/include/velora/host/live_set_switcher.hpp`, `src/live_set_switcher.cpp` -- jump_to/tick/commit, head budget doc (line ~42 says counting current+next is 4.7), SwitcherOptions (hold_ticks, clock, head_budget_bytes), loading status.
- `engine/host/include/velora/host/live_set_loader.hpp`, `src/live_set_loader.cpp` -- stage()/commit_seamless(), StageControl (head_budget_bytes, cancel, progress), kHeadBudgetExceeded, head sizing from bundle indexes, BundleLeases.
- `engine/host/include/velora/host/setlist_controller.hpp`, `src/setlist_controller.cpp` -- next/previous Live Set, loaded setlist position (Story 4.6); where "next" is issued.
- `engine/host/src/control_plane.cpp`, `control_plane.hpp`, `proto/ws_v1.proto` (State, LoadingStatus, SetlistStatus) -- publication and tick.
- `engine/host/tests/live_set_switcher_test.cpp`, `setlist_controller_test.cpp`, `rtsan_switch_test.cpp`, fixtures -- templates; `engine/host/CMakeLists.txt`.
- `_bmad-output/implementation-artifacts/spec-4-5-*.md`, `spec-4-6-*.md`, `epic-4-context.md` -- design notes and continuity.

## Tasks & Acceptance

**Execution:**
- switcher/loader -- preload orchestration: staged-not-published generation, budget accounting with shared-head dedupe, invalidation rules, commit-on-next with no load, chaining the following preload
- ws.v1 State -- additive preload fields; control plane tick publishes changes
- setlist controller wiring -- "next" uses the preload when READY, else 4.5 path; jump/setlist change/edit invalidation
- tests -- every matrix row with injected clock/hold ticks and fake head provider; assert no load calls at commit time; budget math with shared heads; fallback paths; lease release on discard (no leaked leases); seamless render check that nothing is cut when switching via preload (reuse the 4.8 render-suite pattern if cheap)

**Acceptance Criteria:**
- Given a loaded setlist and a current Live Set, when the current becomes active, then the next Live Set's heads are loaded within the remaining head budget and `ready` is published when all are resident.
- Given the next Live Set is ready, when "next Live Set" is issued, then the swap happens at the next block with no load work and the following preload starts.
- Given current + next exceed the budget after counting shared heads once, then the preload is skipped, not-ready is reported, and stepping uses the Story 4.5 background load.
- The <10 ms board measurement is recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass
- verdicts: 4 layers, ~35 findings — patches 7 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` overlay (unsaved edit) preload never re-checked, library replaced under READY preload, budget verdict stale after generation change, preload consumed on refused commit — fixed + tests
  - `[medium]` `patch` untested EditSession write hooks / overlay path / bundle identity / setlist controller pair cases; weak or sleep-based tests; dead status fields and status_seq; duplicate-id base selection — fixed
  - `[medium]` `defer` board timing; documented limits
  - `[low]` `reject` cancel_load on READY preload asymmetry, token mismatch, catalog name refresh

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "switch|setlist|live_set|control|rt_traffic|rtsan" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: LiveSetSwitcher preload (staged-not-published next generation within the remaining head budget, shared heads counted once, READY/LOADING/SKIPPED_BUDGET/FAILED state in ws.v1 State.preload), loader reuse of resident heads and head-identity checks, setlist controller naming the next entry, edit-session invalidation.
Verification: targeted and full ctest pass except 4 pre-existing failures; ui_shell_test segfaulted once under parallel load and passes alone (3/3).

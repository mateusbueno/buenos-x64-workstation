---
title: 'Story 8.9: Boot into setting and restoring the target (Mac logic and simulator)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'cc4fdad7103b42029679a34af4612c2b7b46467e'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-8-8-journaled-boot-into-pointer.md', '{project-root}/sysd/README.md']
warnings: []
deferred:
  - summary: >-
      Open product decision: a fresh data root persists an empty "Live Set" (vs an in-memory one). Not verified: boot time (the bounded 1.5 s sysd wait adds to boot when sysd is absent: account in Story 8.10); a late-starting sysd boots LAST_SOUND (needs After=velora-sysd ordering); a Live Set on its base is stored as scene 0 and LAST_SOUND recalls scene 1 (frozen pointer format); main.cpp fallback paths after 3 failed loads are untested; the library_revision re-request and UiApp->NavigationController queue are untested; leaving Settings with the picker open; Boot Screen (8.7) and board timing (8.10) not covered.
    evidence: |-
      engine/host/README.md, sysd/README.md.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** After power-on the unit must start where the show needs: the last sound used, the start of the setlist, or a default Live Set (FR62, FR65, UX-DR78, AR39 partial). Story 8.8 writes the Boot-into pointer but nothing reads it, `boot_into` exists in the sysd config but nothing applies it, and the Settings surface has no Boot into row.

**Approach:** (1) Engine boot target resolution (engine-host, control thread, at start): read the sysd `boot_into` config and resolve the target: LAST_SOUND loads the Live Set and scene from the Story 8.8 pointer (`BootIntoStore::load`) with its setlist loaded; START_OF_SETLIST loads the first Live Set of the last loaded setlist (from the pointer's setlist_id) at scene 1; DEFAULT_LIVE_SET loads `default_live_set_id` at scene 1 with no setlist loaded (`NO SETLIST`). Fallbacks: a deleted/missing/corrupt target falls back to the first Live Set of All Live Sets, or an empty new Live Set when none exist; a missing library shows Missing library on the affected parts, silent, never substituted (use the existing missing-library behaviour of the loader/State); boot always ends on Performance and never stalls waiting for a missing target or for sysd (bounded wait for the first sysd config, document the bound, then use the last-known/default value). (2) The first observation after boot must not overwrite the stored pointer with the default position before it is read (Story 8.8 risk note): the tracker starts after the boot target is resolved. (3) Settings › Display & Language gets a Boot into row: Segmented Control "Last sound used / Start of setlist / Default Live Set"; choosing Default Live Set opens a List Row picker of All Live Sets; the choice is saved through the sysd `SysdConfigSender` seam (as in Story 7.7) immediately (patch with only boot_into fields). (4) Tighten sysd validation of `default_live_set_id` to the Live Set id format (UUID as used by `live_set_store`), keeping legacy values already stored loadable (normalise like the pedal-span rule: invalid id with DEFAULT_LIVE_SET mode falls back to LAST_SOUND with a logged note).

## Boundaries & Constraints

**Always:** Reuse `BootIntoStore`/`BootIntoTracker` (Story 8.8), `LiveSetSwitcher`/`SetlistController`/`LiveSetLoader`, the sysd config client callback pattern (clock_source/velocity/pedals), `SysdConfigSender` and the SettingsView patterns of Story 7.7, the ui List Row picker component(s) already used for Live Set lists. Resolution is deterministic and testable without audio (host test with fake data dirs and a fake switcher or the real one with synthetic Live Sets). Never leave the unit without a sounding Live Set when any Live Set exists. All fallback reasons are logged (and published in State if a field exists; else log only). Existing 8.8, 4.5/4.6/4.7, 7.7 and settings tests stay green.

**Never:** No Boot Screen work (Story 8.7), no timing claims (≤10 s boot is 8.10 on the board), no change to the pointer file format, no new sysd fields.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Last sound | boot_into=LAST_SOUND, valid pointer | loads pointer's Live Set + scene with its setlist loaded | none |
| Last sound, no pointer | missing/corrupt pointer | first Live Set of All Live Sets (reason logged) | none |
| Start of setlist | boot_into=START_OF_SETLIST, last setlist in pointer | first Live Set of that setlist, scene 1, setlist loaded | missing setlist/empty → fallback to first Live Set |
| Default Live Set | boot_into=DEFAULT + id | that Live Set at scene 1, NO SETLIST | deleted id → first Live Set of All Live Sets |
| No Live Sets at all | empty library | empty new Live Set, Performance | none |
| Target deleted | pointer or default id gone | fallback as above, boot ends on Performance | none |
| Library missing | target uses a missing library | Live Set loads; affected parts show Missing library, silent, no substitution | none |
| sysd absent | no config | bounded wait, then LAST_SOUND default | no stall |
| Pointer not overwritten | boot position observed | stored pointer unchanged until a real user change | none |
| Settings row | choose each option | patch sent to sysd; Default Live Set opens picker; value shown from sysd | failure → notice + revert |
| Invalid default id | stored id not a UUID with DEFAULT mode | loads as LAST_SOUND, note logged; patches with invalid ids refused | typed error |

## Code Map

- `engine/host/macos/main.cpp` (startup: initial Live Set load, --data-dir, sysd callback wiring, boot_tracker from Story 8.8), `engine/host/include/velora/host/boot_into_store.hpp` + src (BootIntoStore::load, BootIntoTracker::update), `live_set_switcher.hpp`, `setlist_controller.hpp`, `live_set_loader.hpp`, `live_set_store.hpp` (list of Live Sets, UUID rule), `factory_live_set.hpp`.
- `sysd/src/config_store.cpp` (valid_live_set_id, normalized), `proto/ws_v1.proto` (BootIntoMode, BootIntoConfig, BootIntoPatch), `tools/sysdctl`.
- `ui/src/screens/settings_view.cpp` (Display & Language section), `ui/include/velora/ui/sysd_config_sender.hpp` + src (Story 7.7 sender), `system_config.hpp`, ui list-row picker usage (setlist/live set pickers), `locale/*.json`.
- Tests: `engine/host/tests/boot_into_test.cpp`, `live_set_switcher_test.cpp` (rig), `ui/tests/epic10_settings_test.cpp`, `sysd/tests/*`.

## Tasks & Acceptance

**Execution:**
- host: BootTargetResolver (pure logic over a store/catalog view) + engine-host startup integration with bounded sysd wait; tracker starts after resolution
- sysd: UUID validation for default_live_set_id with legacy normalisation
- ui: Boot into row, Segmented Control, Live Set picker, sender patches, notice + revert, locale strings (en, pt-BR, es)
- tests: resolver matrix (all rows), engine-host smoke with real sysd + real Live Sets in a temp data dir (each mode; pointer not overwritten), ui test with fake sender, sysd validation tests
- docs

**Acceptance Criteria:**
- Given `boot_into` in the sysd config (LAST_SOUND default), then it is published to engine and ui at start and on change.
- Given the Settings surface, then the Boot into row shows the three options, Default Live Set opens a picker of All Live Sets, and the choice is saved immediately through sysd.
- Given each option at boot, then the engine resolves the target as specified (last pointer with setlist, first of last setlist at scene 1, default at scene 1 with NO SETLIST).
- Given a deleted target or missing library, then boot ends on Performance with the specified fallbacks and never stalls.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` picker showed only the sounding Live Set (AC gap) — additive GetSetlists.include_all_live_sets / SetlistsReply.all_live_sets, ui reply callback and request on open and on library_revision change
  - `[medium]` `patch` unbounded boot retry and exit 1 when every load fails — capped at 3, then empty Live Set, then dev config; vacuous or skipped smoke checks (scene restore, missing library, off-script pointer); stale README/proto comments; es wording; unknown clock value hid a valid boot_into
  - `[low]` `keep` std::max(0, current_scene()) in the control tick (fixes a Story 8.8 bug: the pointer was never written for Live Sets without a scene recall); seed() semantics

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "boot|sysd|settings|switch|setlist|live_set|control|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` and `python3 tools/ci/check_ui_strings.py` -- expected: exit 0

## Story 8.9 notes (appended)

- Open product decision: on a fresh data root the engine now persists an empty Live Set named "Live Set"; whether it should be kept in memory until written is undecided.
- The pointer format cannot represent "no scene recalled": a Live Set on its base is stored as scene 0 and a LAST_SOUND boot recalls scene 1.
- The 1.5 s sysd wait adds to boot time when sysd is absent (account for it in Story 8.10); a late-starting sysd boots LAST_SOUND (needs After=velora-sysd ordering); "never stalls" holds up to that bound.
- ws.v1 additive: `GetSetlists.include_all_live_sets` / `SetlistsReply.all_live_sets` feed the Boot into picker.

## Auto Run Result

Summary: BootTargetResolver (pure) + engine-host startup (bounded sysd wait, pointer, setlist load, scene recall, fallbacks, tracker seed), sysd UUID validation with legacy normalisation, ui Boot into row with a real All-Live-Sets picker through ws.v1, locale strings.
Verification: targeted tests (85) pass; boot and setlist smokes pass; full ctest passes except the 4 pre-existing failures; core deps and UI strings lint pass.

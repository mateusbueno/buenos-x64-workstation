---
title: 'Story 4.1: Tier profile read at boot (Mac logic)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '551534c34b7af00fd165cb21e915b0c731884b11'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md']
warnings: []
deferred:
  - 'Pro bank A/B behaviour (only the 16-part data model is enabled)'
  - 'The 16-part UI part strip (the UI still draws 8 parts)'
  - 'Board tier strap / OTP reader (TierSource has only the Mac no-strap implementation)'
  - 'Recalibrate all tier budgets and the organ/FM/VA Pro capacities on the board (TODO(board))'
  - 'Per-snapshot active_parts bound for the RT part loops; gate --tier out of production builds'
  - 'rtsan_arpeggiator_test (arpeggiator_traffic_test) leaves a test-tone voice stuck for some seeded arpeggiator-config streams (reproduced at the pre-4.1 baseline by perturbing the random stream); the test keeps its tuned 8-part traffic'
---

<intent-contract>

## Intent

**Problem:** One image must run v1 and Pro without a code fork. The tier (v1 or Pro) must be read once at boot and set part count, head budget, voice/cost budgets and enabled modules from a profile (FR17, AR24, NFR39, AD-27). Today part count, head budget and the arbiter budget are fixed defaults spread over several options.

**Approach:** Add a `TierProfile` data table in `engine/host` (no per-tier compile paths): v1 = 8 parts, head budget 5 GB (configured value inside the 4–6 GB range), v1 voice and cost budgets (the existing defaults: arbiter budget and voice floors); Pro (values decided with the user: scale 2x) = 16 parts, head budget 10 GB, voice/cost budgets 2x the v1 values (sample floor 512 voices), sequencer module flag ENABLED (no sequencer code). A `TierSource` port reads the tier ID once at start (board: strap/OTP via the port, not implemented on the Mac; the Mac host and a board with no readable strap use v1); a dev-only override (`--tier pro|v1` flag, with a prominent log line) selects Pro for testing. The profile is applied at start to the switcher/loader head budget, the voice arbiter/pool budgets and the maximum parts accepted by the Live Set model, and is published in the ws.v1 State for ui and sysd (additive field). Pro bank A/B behaviour is recorded as deferred (only the 16-part data model is enabled).

## Boundaries & Constraints

**Always:** Data-driven: one `TierProfile` struct and two constant instances (v1, Pro) with the numbers in ONE place with citations of the source (epics Story 4.1, AD-27, the user's 2x decision); every consumer reads the profile object, never `if (tier == pro)`. The tier is read once and immutable for the process lifetime. Snapshot/Live Set structures must already be sized for 16 parts (architecture review note: "Snapshot structures are sized for 16 parts and capped by the profile"): verify the current limit (find where 8 parts is hard-coded: Live Set parser/validator, snapshot, part router, ui) and make 16 the structural capacity with the profile as the cap, without changing v1 behaviour (a v1 profile rejects a 9th part with the existing typed error; Pro accepts exactly 16). Existing tests must keep passing with v1 as default; add tests for Pro (16-part Live Set loads and renders, v1 refuses it, budgets differ, State publishes the profile, override flag and absent strap use v1). Voice/cost budgets: locate the real constants (voice pool size, arbiter_budget, FX budget, sample/FM/VA floors from Stories 1.19, 12.5, 12.7) and scale them via the profile for Pro; do NOT claim they were measured on a board (placeholders to be recalibrated on the board, noted TODO(board)).

**Never:** No ui changes beyond reading the profile from State if trivially needed (UI part-strip count stays 8 in this story; note the 16-part UI as deferred), no sequencer, no Pro bank A/B, no motorized faders, no hardware strap/OTP reader, no sysd fields, no change to wire formats other than the additive State field.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| v1 default | no strap, no override | v1 profile applied, State publishes tier v1 | none |
| Dev override | --tier pro on the Mac | Pro profile, loud log line | invalid value → error exit with message |
| Strap says Pro | TierSource returns Pro | Pro profile applied once | unreadable strap → v1 + logged reason |
| 16-part Live Set on v1 | Live Set with 9..16 parts | refused with the existing typed error, current set keeps playing | typed error |
| 16-part Live Set on Pro | same | loads, all 16 parts render | none |
| Head budget | v1 vs Pro | switcher/loader budgets 5 GB vs 10 GB | over budget → existing kHeadBudgetExceeded |
| Voice/cost budgets | v1 vs Pro | arbiter budget and voice floors differ per the profile | none |
| Re-read | any later point | tier never re-read; profile immutable | none |
| Pro bank A/B | n/a | recorded as deferred; no behaviour | none |

## Code Map

- `engine/host/macos/main.cpp` (SwitcherOptions head_budget_bytes, StageControl head budget, arbiter_budget in LiveSetLoader options, plane/part limits, flags), `engine/host/include/velora/host/live_set_loader.hpp` (arbiter_budget, head_budget_bytes), `live_set_switcher.hpp` (head_budget_bytes), `control_plane.hpp/.cpp` (State assembly), `live_set_store.hpp`/`src/live_set_loader.cpp`/`proto/live_set.proto` (part limits and validation).
- `engine/core/include/velora/core/snapshot.hpp`, `part_router.hpp`, `voice_arbiter.hpp` (+ fm/va floors), `engine.hpp` -- structural part capacity and budgets.
- `proto/ws_v1.proto` (State), `proto/README.md`, `engine/ports/include/velora/ports/` (new tier_source.hpp port).
- Architecture docs: AD-27 (ARCHITECTURE-SPINE.md ~line 351) and the review notes (reviews/review-reconcile.md ~line 126).
- Tests: `engine/host/tests/live_set_loader_test.cpp`, `live_set_test.cpp`, `control_test.cpp`, `rtsan_*`, `engine/core/tests/`.

## Tasks & Acceptance

**Execution:**
- TierProfile + TierSource port + tier resolution at engine-host start, `--tier` override, apply to head budget, arbiter/voice budgets and part cap
- structural 16-part capacity audit and fix; profile as the cap; typed refusal on v1
- State publication (additive) and docs (AD-27 notes, deferred Pro bank A/B and 16-part UI)
- tests per matrix row, incl. a render of a 16-part Pro Live Set and v1 refusal; assert no tier branching outside the profile table (a static check script or test grepping for `== Tier::` outside the allowed file is acceptable)

**Acceptance Criteria:**
- Given a tier ID source, when the engine starts, then it reads the tier once and selects the profile (v1: 8 parts, 5 GB head budget, v1 budgets; Pro: 16 parts, Pro head and cost budgets, sequencer module enabled) and publishes it in the State.
- Given no readable strap or the Mac host, then v1 is used, and a dev-only override selects Pro for testing.
- Given the same image with each profile, then part count, head budget and cost budget differ as specified and no code path is compiled per tier.
- The Pro bank A/B behaviour and the 16-part UI are recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` Pro Live Sets with FM/VA parts could not load (arbiter floor+headroom exceeded engine capacity) — FM/VA capacity profile-driven; organ parts 8..15 silent on Pro — organ part count and capacity profile-driven; parallel-build race on generated params_gen.hpp (CI bug) — unique tmp name plus a proper generator target and dependencies
  - `[medium]` `patch` unstated v1 behaviour changes (channel aliasing removed, RT loops on 16 structural parts, CC on channels 9-16, memory growth) documented; tier hardening (static_asserts, unknown id, one-shot set_tier_profile); stronger branching lint; vacuous re-read test replaced
  - `[tracked]` arpeggiator stuck-voice bug (pre-existing, seeds 11111/424242) recorded in deferred-work.md; not fixed here

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "tier|live_set|switch|control|loader|arbiter|rt_traffic|sysd|boot" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: TierProfile table (v1: 8 parts, 5 GB, floors 256/128/64; Pro: 16 parts, 10 GB, 2x floors/budgets, sequencer flag), TierSource port + resolve_tier (read once, dev --tier override, unreadable strap -> v1), profile applied to head budget, loader options, part cap, FX budget and engine capacities, State.tier published, structural 16-part capacity, check_tier_branching lint.
Verification: targeted tests pass; clean parallel build of velora_core and velora_core_o2 works; full ctest passes except the 4 pre-existing failures and a load-sensitive librarian_crash_campaign (passes alone); core deps and branching lints pass.

---
title: 'Story 8.12: Startup reconcile of interrupted imports: fault injection and boot bound'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '52c6d2ce5fb7fbda5b662b576a5821e608d4d07d'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/librarian/README.md']
warnings: []
deferred:
  - summary: >-
      A catalog row that must be created or refreshed still runs verify_all (content read, unchanged Story 5.6 rule), so the 5 s / no-content-scan bound is proven for the steady-state boot only.
    evidence: |-
      Boot-bound test uses rows_added == 0; a post-crash boot with many missing rows scales with bundle size. Needs a decision on whether 5.6 may defer verify for recreated rows.
    severity: medium
  - summary: >-
      Librarian is ordered After=velora-engine.service only; the engine publishes no playable signal. No client handles RECONCILING yet; summary line path through service_main untested.
    evidence: |-
      Wire a readiness gate when the engine exposes one; clients retry on retry_after_ms.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** After a power cut during an import the library must come back clean, but the reconcile pass (rule owned by Story 5.6, lease-aware GC from 5.21) is not yet proven at every pipeline step as a hard guarantee, has no per-action summary log, no proven ≤5 s bound without content scanning on a 1 TB `/data`, and no defined ordering relative to engine-playable and command acceptance (FR67, AR28, AD-11).

**Approach:** Keep the reconcile rule unchanged; add what this story owns: (1) startup ordering so the reconcile pass finishes before the librarian accepts commands and starts only after the engine is playable (systemd ordering + a typed "reconciling" refusal until done), (2) a summary log with counts per action, (3) a fault-injection campaign that kills the librarian at each named import step 100+ times on random steps and asserts library set + catalog equal the before-state or the after-state (and a killed re-import leaves the previous bundle playing), (4) a boot-bound test on a synthetic 1 TB-apparent `/data` proving directory/manifest-level work only, ≤5 s, never touching a bundle referenced by `current`.

## Boundaries & Constraints

**Always:** Do not change the reconcile rule of Story 5.6 (delete staging leftovers, remove catalog rows whose library has no `current`, recreate missing rows from manifests under `current`) nor the 5.21 lease-aware GC; this story only orders, logs, proves and bounds it. Reconcile never reads bundle/sample contents (directory and manifest level only) and never touches a bundle referenced by `current`. The named kill steps are exactly: stage, verify, fsync, rename, flip `current`, catalog row. Reuse the existing crash-campaign harness (`librarian/tests/crash_campaign_test.cpp`, forked child killed at a seeded boundary; `VELORA_LIBRARIAN_KILL`) rather than writing a new one. Tests are deterministic (seeded).

**Never:** No new reconcile rule, no change to import semantics, no sample-content scanning, no engine changes beyond reading its existing playable signal if one is published (if none exists, order by systemd `After=` on the engine unit and document it).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Boot ordering | librarian starts | reconcile runs to completion before commands are accepted; commands arriving during it get a typed "reconciling" refusal | none |
| Summary | any reconcile | one log summary with counts per action (staging deleted, catalog rows removed, catalog rows recreated, GC) | none |
| Kill at each step | kill at stage / verify / fsync / rename / flip current / catalog row, 100+ random | after restart + reconcile: library set and catalog == state before OR after that import | invariant violation fails the test with seed |
| Interrupted re-import | kill during Import-again of an installed library | previous bundle still plays (catalog and `current` unchanged or fully new) | none |
| Large library | synthetic 1 TB-apparent `/data` (sparse/tiny files, many dirs, manifests) | reconcile ≤5 s, zero reads of sample/bundle content files | exceeding bound or any content read fails |
| Referenced bundle | bundle referenced by `current` | never deleted or modified | none |

## Code Map

- `librarian/src/library_reconcile.cpp` (+ header under `librarian/include/velora/`), `library_service.cpp`, `service_main.cpp` -- reconcile entry, service start, command acceptance.
- `librarian/tests/crash_campaign_test.cpp`, `library_service_test.cpp`, `lib_test_util.hpp` -- existing campaign (200 scripted imports) and fixtures to extend.
- `librarian/systemd/velora-librarian.service` -- ordering relative to the engine unit.
- `librarian/README.md` -- document ordering, log summary, bound.
- `engine/host/include/velora/host/fs_port.hpp` (CountingFs) -- pattern for asserting no content reads; the librarian may have its own fs hook (check).

## Tasks & Acceptance

**Execution:**
- `librarian/src/library_reconcile.cpp`, `library_service.cpp`, `service_main.cpp` -- per-action counts + summary log; accept commands only after reconcile; typed "reconciling" refusal before that; do not alter the rule
- `librarian/systemd/velora-librarian.service` -- order after the engine unit (playable), document
- `librarian/tests/crash_campaign_test.cpp` (or a sibling test) -- 100+ seeded random kills over the six named steps incl. interrupted re-import; assert before-or-after state
- new test -- 1 TB-apparent synthetic `/data`: ≤5 s, zero content reads (counting hook or permission/unreadable content files); never touches `current`-referenced bundles
- `librarian/README.md` -- document

**Acceptance Criteria:**
- Given librarian start, then the reconcile pass completes with the unchanged rule before commands are accepted and logs a per-action summary.
- Given kills at each named step 100+ times, then library set and catalog equal the before or the after state and a killed re-import leaves the previous bundle playing.
- Given a 1 TB-apparent `/data`, then reconcile finishes in ≤5 s without reading sample contents.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~40 findings — patches 5 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` service thread touched the catalog concurrently with reconcile — gated to accept/refuse only; handshake from cached schema version
  - `[medium]` `patch` failure path untested/no summary/throwing hooks — fixed + failed_pass_test
  - `[medium]` `patch` RECONCILING retry hint and broader refusal coverage; test hardening (timeouts, alarm, cleanup, per-point coverage, root/sparse guards)
  - `[medium]` `defer` verify_all content read on recreated rows vs 5 s bound; engine-playable gate; client handling
  - `[low]` `reject` harness-reuse claim (hooks reused, helpers local), systemd Wants, summary field nits

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "librarian" --output-on-failure` -- expected: all pass

## Auto Run Result

Summary: reconcile timed with per-action summary log; service accepts-and-refuses (typed RECONCILING with retry hint) until the pass finishes; 132-kill seeded campaign over six steps incl. re-import; 1.1 TB-apparent boot-bound test (0.05 s, no content reads); systemd After= engine.
Verification: 27 librarian tests pass; remaining full-suite failures pre-existing (tokens_verify, protected_keyless_test, sysex_receiver_test, library_resolver_test) plus one load-flaky setlist_controller_tests that passes alone.
Residual risks: see deferred.

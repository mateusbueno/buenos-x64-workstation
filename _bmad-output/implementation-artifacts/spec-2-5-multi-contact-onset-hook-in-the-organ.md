---
title: 'Story 2.5: Multi-contact onset hook in the organ'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '367b3cc150cacacb8f90f37c18909719bbb14d91'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 2.5 "Multi-contact onset hook in the organ" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1540; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints (clean-room, same rules as Stories 2.2-2.4 and `engine/core/ORGAN.md`):** define the vendor per-note `contact_k` controller format in UMP (document it in `engine/core/README.md`/ORGAN.md and in `proto/README.md`; it is the format the Epic 3 keybed reader will emit): a MIDI 2.0 per-note assignable/registered controller (or another documented UMP per-note controller) carrying (channel, note, contact index k = 1..3 or the number the keybed has, closed/open state) so that a keybed with 2 or 3 contacts per key maps onto the organ's contact groups; the `BlockRenderer`/`apply` path must route these UMP events to the engine at their exact in-block frame offset (the core still accepts only UMP) and must not count them as "ignored"; implement in `OrganEngine`: key voice starts sound on contact 1 closing (before any Note On) at that event's in-block offset; each later contact closure brings in its drawbar busbar group at that contact's time (documented mapping of contacts to footages/busbar groups in ORGAN.md following the public description of the multi-contact key behaviour of the reference console: e.g. contact groups of the 9 busbars/footages; the mapping must be an explicit, documented table, with 3-contact and 2-contact variants); key release reverses: busbar groups drop out in reverse order at each contact's time and the voice ends when the last contact opens; a Note On/Off-only source (no `contact_k`) keeps working: all busbars start together at Note On and stop at Note Off (the hook is optional per source); a Note On arriving after contact events for the same key must not start a second voice or retrigger percussion (the voice already exists) and does not change timing; Note Off after contact opens must be idempotent; protect against malformed sequences (open without close, close of contact 3 without 1, duplicate closes, contact index out of range) with defined, tested behaviour (ignore or clamp) and no crash/UB; interplay: percussion single-trigger fires on the first contact closure (key press) rather than the Note On when contact events are used; key click: onset click at contact 1, release click when the last contact opens; arbiter voice allocation happens at contact 1 (a voice started by contact_k is the same voice later bound to the Note On: handled by (channel, note) identity, and by the arbiter protection already in place); bus group onset ramps must be click-free but short enough that timing is exact: busbar group onset lands on the exact frame (use per-group gain ramps of a few samples, documented, or a gate with a defined 1-2 sample rise) and a unit test with synthetic sequences at 1 ms, 5 ms and 20 ms spacing proves the group onset lands on the exact frame of its contact event; extend the offline renderer so a MIDI-like input can carry contact events: define a simple text or SMF-based convention for the test files (e.g. SMF channel-voice events cannot carry it: add a small deterministic `.contacts` event-list input option `--events file` for tests only, documented) and add a committed small golden (bit-identical across runs, null-tested) using fast-playing/smear/half-press sequences; RTSan test with contact_k traffic.

**Never:** add rotary, effects, UI, keybed reader or MCU code; change the sample engine or other goldens; read GPL organ code.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.5 (~line 1540) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.5 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` open of a non-closed contact with a higher one closed dropped contact 3 early, contradicting the doc — ignored and counted; test added, golden regenerated
- `[medium]` `patch` contact-started keys bypassed the arbiter key/pedal bookkeeping (stale deferred release could cut a re-pressed voice) — contact_down/contact_up in the arbiter, `ContactEffect` out-parameter, pedal tests
- `[medium]` `patch` generation binding for contact-started voices unverified and taken even when no voice started — bound only on kStarted, ledger test
- `[low]` `patch` contact index aliasing (>255), foreign per-note controllers routed as contact_k, set_contact_count with live voices, unsorted merge, contact events extending the render, RTSan bound derivation — fixed with tests
- `[low]` `defer` the contact→footage groups table (3 contacts: 16'/5⅓'/8', 4'/2⅔'/2', 1⅗'/1⅓'/1'; 2 contacts: first four, rest) is a design value to be confirmed against the M-solo reference recordings (Stories 11.13/2.13)

## Auto Run Result

Status: done. `contact_k` UMP controller format (documented), contact events routed by `BlockRenderer` at their exact in-block frame, `OrganEngine` multi-contact voices (start at contact 1 before any Note On, busbar groups ramped in over 8 frames at each later contact, reverse release, idempotent Note Off, defined malformed-sequence behaviour, Note On/Off-only sources unchanged), arbiter bookkeeping for contact keys, `velora-offline-render --events` test input and a committed golden. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

---
title: 'Story 1.19: Voice pool arbiter with the sample floor'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '774b3f45fa9b4784c74908880ed42d5015bcc2a8'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Engines (test tone, sample) currently allocate voices from their own private pools and have no stealing. Dense playing must never drop the notes that matter, and the sample engine must be guaranteed at least 256 stereo voices (organ/FX reservations come first).

**Approach:** Add a `VoiceArbiter` in `engine/core` that is the single allocator and stealer. It owns a budget in cost units and, when a Live Set loads, reserves in order: the FX budget, the organ+rotary reservation (zero when no organ), each engine's floor (sample >= 256 stereo voices), and shares the remainder. Engines get voices only through the arbiter. When the pool is full it picks victims across the whole Live Set by the documented policy, fades stolen voices out over a short ramp, and keeps their cost counted until they are silent. It runs in the serial phase of the block loop on the RT plane.

## Boundaries & Constraints

**Always:** C++20, no OS deps, no allocation/locks/blocking in the RT path (pre-allocated tables; RTSan test); the arbiter is the only code that allocates or steals voices (engines request through it; engine-side private pools are removed or become arbiter-owned slots); cost units come from `Engine::cost_units_per_voice()`; reservation order FX -> organ+rotary -> engine floors -> shared remainder; configuration of budget/FX/organ/floors is passed at "Live Set load" time via a plain struct (outside the RT path; applying it must not allocate on RT); steal policy: steal across the whole Live Set; never steal the lowest held note or pedal-sustained notes while any other candidate exists; prefer newer and quieter voices (define the exact scoring order, document it, and keep it deterministic: ties broken by voice id); a stolen voice fades out over a short ramp (a few ms, configurable constant, documented) and keeps counting against the budget until silent; each steal increments a steal counter (global and per engine); floors are guaranteed: a note of engine E below its floor can steal from engines above their floor, never from an engine at or below its floor; steal metadata needed from engines is exposed through the existing engine interface (extend `VoiceInfo` minimally with what is needed — note number, channel, held vs pedal-sustained vs releasing — and document; interface changes must keep FM/VA pluggable with no further change); the existing `BlockRenderer` flow, test-tone and sample engine tests, and null tests against sfizz must keep passing unchanged in outcome (sample suite residuals must not regress; the sfizz-oracle behaviour for non-stolen voices is unchanged).

**Never:** Add parallel rendering (Story 1.18), organ DSP, FX DSP, UI, or Live Set loading code (the load-time reservation is a function on the arbiter that tests call); change the bundle format; remove the "PoolExhausted" typed status (it still applies when no victim exists); steal from reserved organ/FX budgets.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Reservation | budget B, FX f, organ o, floors | floors satisfiable -> reservation succeeds; shared remainder = B - f - o - sum(floors) | infeasible -> typed error and no partial state |
| Sample floor | pool full of sample voices | a new sample note always gets a voice (steals) while sample voices <= floor never stolen by other engines | none |
| Full pool, normal | all voices sounding, held bass note, pedal down, mixed velocities | victims chosen per policy; lowest held note and pedal-sustained voices are last resort | none |
| No candidate | every voice protected | PoolExhausted typed status; nothing dropped silently | typed status |
| Steal ramp | stolen voice | fades over the ramp, still counted until silent, then freed; never clicks (monotone ramp to 0) | none |
| Cost accounting | engines with different cost per voice | budget math in cost units, no overflow, integer arithmetic | rejects invalid config |
| Counters | each steal | increments global and per-engine steal counter | none |
| Determinism | same event script twice | identical victims and identical audio | none |
| RT safety | 10,000 blocks of dense traffic | RTSan clean | n/a |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/engine.hpp`, `block_renderer.hpp`, `engine_registry.hpp`, `test_tone_engine.hpp`, `sample_engine.hpp` + sources -- the Engine interface (`cost_units_per_voice`, `voice_info`, start/stop/render), BlockRenderer serial phase and event routing, engines' private voice tables (to be arbiter-owned or arbiter-gated)
- `engine/core/tests/` -- existing engine, sample-engine and RTSan tests (style: bare main, exit codes, RTSan tests wired in `engine/core/CMakeLists.txt` under `VELORA_HAVE_RTSAN`)
- `tools/offline-render/tests/sample_suite/` -- sfizz null cases that must not regress
- `tools/ci/check_core_deps.py`, `tools/ci/docker-check.sh` -- validations; **macOS skips RTSan tests, only Docker runs them: always run docker-check**
- `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md` AD-22 (~line 296) and `_bmad-output/planning-artifacts/epics.md` Story 1.19 (~line 1280) -- read-only

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/voice_arbiter.hpp` + `src/voice_arbiter.cpp` -- arbiter: config struct (budget, FX, organ+rotary, floors per engine type), `reserve()` returning typed result, voice request/steal path, ramp bookkeeping, counters -- the story
- `engine.hpp` / engines -- minimal interface additions (steal ramp request, extended VoiceInfo) and conversion of TestToneEngine and SampleEngine to arbiter-allocated voices -- integration
- `block_renderer` -- route note-ons through the arbiter in the serial phase; expose counters -- wiring
- `engine/core/tests/voice_arbiter_test.cpp` -- scripted scenarios: full pool with a held bass note, sustain pedal down, mixed velocities, floors (sample 256 voices guaranteed against organ/other pressure), reservation order and infeasible configs, steal counters, determinism, ramp behaviour, cost accounting with mixed costs -- coverage
- `engine/core/tests/rtsan_arbiter_test.cpp` -- 10,000 blocks of dense traffic with stealing under RTSan -- RT safety
- README updates (engine/core) documenting policy, scoring order and reservation -- docs

**Acceptance Criteria:**
- Given a Live Set configuration, when reservations are made, then FX, organ+rotary and engine floors are reserved in that order and the sample engine has >= 256 stereo voices guaranteed
- Given a full pool, when a new note arrives, then the victim follows the documented policy (never the lowest held note or pedal-sustained voices while other candidates exist; newer and quieter first) and each steal increments the counter
- Given a stolen voice, then it fades over the ramp and keeps counting against the budget until silent
- Given the existing suites, then sample null cases and engine tests still pass; macOS ctest and `tools/ci/docker-check.sh` (including RTSan) pass

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (this is the only place the RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~55 findings — patched 9 groups, rest deferred
- findings:
  - `[high]` `patch` sustain-pedal threshold treated MIDI values 32..63 as pedal down (0x40000000 vs 0x80000000) — corrected with boundary tests
  - `[high]` `patch` a victim whose fade request was refused was still marked fading and counted — undone, next victim tried
  - `[medium]` `patch` below-floor sample note could be refused when the engine slot table was full of live+fading voices — reserve() requires capacity >= floor + 64 fade headroom; dense-steal test proves the guarantee within that headroom
  - `[medium]` `patch` stale pedal/key/deferred state across reserve/clear, bind after reserve, cost/capacity drift, registry/arbiter bind mismatch — reset and rejected
  - `[medium]` `patch` stolen notes' note-offs polluted kVoiceNotFound; deferred releases lost or sent to the wrong engine after re-route — benign counter, per-note engine id, dropped counter
  - `[medium]` `patch` production code could lower the 256-voice floor via config — constexpr floor, test-only setter under VELORA_ARBITER_TEST_HOOKS
  - `[low]` `patch` counters racy for telemetry readers; attack-voice check counted fading voices; tests missing for real-engine ramp vs note-off, sample release trigger + pedal, multi-channel pedal, determinism with sample engine, invariant under RTSan — fixed/added
  - `[low]` `defer` acquire has no commit/abort protocol if an engine fails to start after a steal; per-acquire full scan cost unmeasured (needs the board and incremental counters); "lowest held note" is global by note number (not per channel/engine); victims chosen without cost-waste matching; organ never-steal flag not needed until the organ engine exists; slot tables stay in engines (arbiter decides); fade headroom of 64 is a documented limit, not unbounded
  - `[intent gap noted]` Nothing calls `reserve()` yet outside tests — the Live Set load path is Epic 4; until then BlockRenderer runs unreserved (first-free, PoolExhausted when full)

## Auto Run Result

Status: done. Added `VoiceArbiter` in `engine/core` (reserve order FX → organ+rotary → engine floors → shared remainder; victim policy releasing → held → pedal-sustained → lowest held note, quieter then newer then lower id; per-engine floors; 4 ms steal ramp counted until silent; steal counters), `VoiceAllocator`/`begin_steal_fade` in the engine interface, conversion of both engines to arbiter-granted slots, CC64 sustain-pedal handling in `BlockRenderer`, scripted-scenario and RTSan tests. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity check passes. Follow-up review recommended: false.

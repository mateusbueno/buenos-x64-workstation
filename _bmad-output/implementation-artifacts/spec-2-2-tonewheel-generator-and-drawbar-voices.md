---
title: 'Story 2.2: Tonewheel generator and drawbar voices at full polyphony'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: 'c96ae5dd76b601f88905eb0b0bdf02c4193756ce'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/engine/core/README.md'
  - '{project-root}/proto/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The product must ship a tonewheel organ as a mandatory v1 engine: 73 keys, 9 drawbars simultaneously, never voice-stolen. No organ engine exists.

**Approach:** Implement a clean-room `OrganEngine` in `engine/core` behind the existing single `Engine` interface: a per-part **shared tonewheel generator** that runs in block phase 2 (one `begin_block`-style hook before voices render) producing the full wheel set at the tonewheel frequency ratios of a real tonewheel organ, and per-key **voices** (phase 3) that sum their 9 drawbar footages (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1') from the shared wheels with foldback at the top and bottom of the range. Drawbar levels 0-8 are performance parameters (from the 2.1 registry, via UMP/CC), smoothed per block without zipper noise and applied at the event's in-block offset. The arbiter gets an organ+rotary reservation that is never stolen.

## Boundaries & Constraints

**Always:** Clean-room: write the DSP from public physical/music facts only (tonewheel gear/pinion ratios and the equal-temperament-based wheel frequencies as documented physical facts, harmonic footage ratios, foldback rule); do NOT read or copy setBfree or any other GPL organ source; record in `engine/core/README.md` (or an `ORGAN.md`) exactly what facts the tables come from so the clean-room claim is auditable; setBfree may only be used as an unshipped oracle (not in this story); `engine/core` stays OS-free, RT-safe (no allocation/locks/blocking in render; RTSan test over 10,000 blocks); voices write only their own state and their worker's scratch buffer (AD-6), the shared generator writes shared per-part state only in the phase-2 hook; the generator output is a fixed-size wheel table (91 wheels or the structure you document) updated per block; all 73 keys x 9 drawbars sound at once with no voice dropping; organ keys are never steal victims and the arbiter reserves the organ+rotary cost for 73 simultaneous keys ahead of every engine floor (extend `VoiceArbiter`/`LiveSetConfig` minimally: an engine-level `protected`/never-steal flag and the organ reservation computed from the organ engine's declared costs; keep existing arbiter tests passing); cost units: declare per-voice and per-shared-generator cost in cost units (placeholders to be measured on the dev board later; add a host micro-benchmark that prints figures labelled "host, not representative"); drawbar smoothing is per block (document the time constant), applied at the event's in-block offset; drawbar parameters come from the existing registry (organ drawbar ids per part in `proto/params.json`; add only what is missing, keep ids stable); user-visible and log strings must never contain "Hammond", "Leslie" or "B-3" (add an automated grep test over the sources of the organ engine and registry strings); the engine name is "tonewheel organ"; `EngineTypeId::kOrgan` (2) gets bound through the existing registry/`bind_engine` path, FM/VA stay unavailable.

**Never:** Add percussion, key click, vibrato/chorus scanner, leakage, rotary, multi-contact onset (Stories 2.3-2.6), effects, UI, or Live Set loading; read GPL organ code; implement stealing for organ voices; change existing engine outputs (sample null tests must still pass).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Single key | note 60, drawbars 888000000 | fundamental 8' sounds at the wheel frequency for that key; spectrum has the drawbar footages at their harmonic ratios | none |
| Full chord | 73 keys held, 9 drawbars | all 73 voices sound, none dropped, bounded output (no clipping beyond defined headroom/normalisation) | none |
| Foldback top | high key whose higher footages exceed the top wheel | footage folds back down an octave per the foldback rule (documented, tested at both ends) | none |
| Foldback bottom | low key 16' etc. below the lowest wheel | folds up per rule | none |
| Drawbar CC while holding | CC at in-block offset k | level changes at the smoothed ramp starting at frame k, no step discontinuity | none |
| Drawbar range | values 0..8 mapped to defined levels (documented dB curve) | monotonic; 0 = silent; 8 = full | out-of-range clamped |
| Phase relation | all keys share the same wheels | same wheel -> same phase for all voices using it (shared generator, not per-voice oscillators) | none |
| Pool full of sample voices + 73 organ keys held | arbiter active | all 73 organ keys sound; samples are stolen, organ never | none |
| RT safety | 10,000 blocks note traffic + drawbar changes | RTSan clean | n/a |
| Strings | grep for forbidden names | none found | test fails |

</intent-contract>

## Code Map

- `engine/core/include/velora/core/engine.hpp`, `block_renderer.hpp`, `voice_arbiter.hpp`, `sample_engine.hpp`, `test_tone_engine.hpp` + sources -- Engine interface (cost_units_per_voice, voice_info with channel/note, begin_steal_fade, start/stop/render, set_param), arbiter reserve order (FX, organ+rotary, floors) and policy, how engines are bound (`bind_engine`), how performance param events reach engines (see Story 2.1: `param_events.hpp`, `snapshot.hpp`, last-value table, `BlockRenderer` adoption of generations)
- `proto/params.json` -- organ drawbar ids (per part) and other organ params already declared; add only missing ones; run the generator through CMake
- `engine/core/tests/` -- test style (bare main), RTSan tests wired in `engine/core/CMakeLists.txt` under `VELORA_HAVE_RTSAN`; arbiter tests for reserve/steal; **macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them**
- `tools/offline-render/` -- renderer; extend with `--engine organ` plus drawbar settings so organ renders can be produced offline (needed later for A/B)
- `_bmad-output/specs/spec-arm64-stage-workstation/engines.md`, `_bmad-output/planning-artifacts/epics.md` Story 2.2 (~line 1452) -- read-only

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/organ_engine.hpp` + `src/organ_engine.cpp` (+ `tonewheel_tables` if clearer) -- generator, voices, drawbar smoothing, foldback, costs, voice_info, never-steal flag -- the engine
- `engine.hpp`/`block_renderer` -- a minimal per-block pre-render hook (phase 2) for engines that need a shared generator (default no-op), invoked once per engine per block before voices render -- phase structure
- `voice_arbiter` -- organ+rotary reservation and never-steal protection for protected engines, tests -- AD-22
- performance-param routing -- drawbar param events for a part reach that part's organ engine (via the existing 2.1 paths) with per-block smoothing at the event offset -- parameters
- `tools/offline-render` -- `--engine organ` with `--drawbars 888000000` -- offline renders
- tests: wheel frequency table vs documented ratios, harmonic content per drawbar (FFT or Goertzel in the test), foldback both ends, full 73-key chord without drops and with bounded level, no-zipper drawbar change (max sample-to-sample step bounded during ramp), shared phase, arbiter protection (pool full of sample voices + 73 organ keys held), RTSan 10,000 blocks, forbidden-string grep test, determinism -- coverage
- a host micro-benchmark executable printing cost per voice / generator ("host, not representative") -- cost record
- docs: ORGAN.md/README (facts used for tables, smoothing time constant, drawbar curve, foldback rule, reservation) -- clean-room audit trail

**Acceptance Criteria:**
- Given a part assigned the organ, when 73 keys are held with 9 drawbars set, then all voices sound from the shared wheel set, none are dropped, and voices write only their own state
- Given a drawbar CC while keys are held, then the level changes smoothly at the event's in-block offset without zipper noise
- Given an arbiter with the organ part loaded and a pool full of sample voices, then all 73 organ keys sound and no organ voice is ever a steal victim
- Given the organ sources and registry strings, then no "Hammond", "Leslie" or "B-3" appears
- Given macOS and Docker Linux, all tests pass including RTSan (Docker only)

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-18 — Review pass (2 reviewers: edge-case hunter + verification gap, to save usage)
- findings:
  - `[medium]` `patch` generation-delivered drawbar values (presets) were never verified to reach the organ — test added incl. "unchanged slot does not overwrite a live move"
  - `[medium]` `patch` offline-render organ test could not detect a silent or ignored `--drawbars` — content check added (non-silent, differs per registration)
  - `[medium]` `patch` pedal behaviour on the organ untested — BlockRenderer-level pedal test (hold, release at pedal-up, re-strike)
  - `[medium]` `patch` arbiter accepted an organ reservation smaller than the real cost; bind_engine lost drawbar values set before binding; disabled parts leaked voices; slot exhaustion dropped notes while releasing tails held slots; offline-render channels 8..15 played test tone and ignored full queues — fixed with tests
  - `[low]` `patch` ramp wording (fixed 240-frame per-sample ramp) and per-part state ownership documented; organ_bench now asserts finite/positive figures and a loose bound
  - `[low]` `defer` generator runs all 91 wheels for every enabled part even when idle (~29 µs/part/block on host); wheels exactly equal-tempered (no gear-ratio deviations); output of multiple parts summed above 1.0 until the master section (Story 2.7); declared cost units are placeholders until the board measures them; forbidden-string test scans sources and registry JSON, not compiled names

## Auto Run Result

Status: done. Clean-room `OrganEngine` ("tonewheel organ") in `engine/core`: 91-wheel shared generator in the phase-2 hook, per-key voices summing nine drawbar footages with foldback at both ends, 0-8 drawbar curve with a 5 ms smoothing ramp at the event's in-block offset, never-steal protection and organ+rotary reservation in the arbiter, `velora-offline-render --engine organ --drawbars`, clean-room audit trail in `ORGAN.md`, host micro-benchmark. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok (58/66 tests), purity and notices checks pass. Costs are placeholders until measured on the dev board. Follow-up review recommended: false.

---
title: 'Story 2.4: Vibrato/chorus scanner and tonewheel leakage'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '090fcd0a41d71598675d735d240cc1f11c98f403'
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

**Problem and approach:** Story 2.4 "Vibrato/chorus scanner and tonewheel leakage" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1510; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints (clean-room, same rules as Stories 2.2/2.3 and `engine/core/ORGAN.md`; no GPL organ source consulted; record the public facts used, e.g. the scanner as a tapped delay line swept at the public reference rate range ~6-7 Hz, tap counts and depth for V1-V3/C1-C3, in ORGAN.md):** implement in `OrganEngine` (per-part, applied to the part's summed organ signal) the vibrato/chorus scanner V1, V2, V3, C1, C2, C3 as a tapped delay line (pre-allocated fixed-size delay buffers, no allocation in render) with chorus settings mixing dry and scanned signals; switching settings (including on/off via `vibrato_chorus_on` / `vibrato_chorus_type`) while keys are held must not click (crossfade or ramp over a few ms; test with a max sample-to-sample step bound); leakage parameter (`leakage`, 0 = off): each sounding key adds low-level contributions from neighbouring wheels with a fixed per-unit random pattern (seeded, reproducible, per-wheel/per-neighbour fixed table generated at construction from a documented seed), scaled by the leakage parameter; with leakage 0 a single drawbar on a single key renders as a pure tone within the null-test tolerance against a synthetic sine reference (write that test using the existing null-test harness functions or an equivalent residual measure; document the tolerance reasoning); scanner state is per part (one part on one worker); all parameters already exist in `proto/params.json` (`vibrato_chorus_on`, `vibrato_chorus_type`, `leakage`); add CC mappings only if missing and keep ids stable; extend `velora-offline-render --engine organ` with `--vibrato off|v1|v2|v3|c1|c2|c3` and `--leakage 0..1` and add a small deterministic golden (render twice bit-identical, null-test against the committed golden like Story 2.3) with a MIDI file cycling scanner settings under a held chord; RTSan test: cycle all scanner settings during a 73-key chord for many blocks (extend `rtsan_organ_test` or add one) with no allocation, lock or blocking; unit tests: delay-line sweep rate and depth per setting measured on a test tone (frequency modulation spectrum or delay variation), chorus dry/wet mix, no click on switching, leakage determinism and level scaling, leakage 0 purity, determinism of the whole render.

**Never:** add rotary speaker, multi-contact onset, percussion/key click changes, effects chain, UI; read GPL organ code; change the sample engine or goldens of other stories.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.4 (~line 1510) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.4 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` silent-part scanner skip and delayed-tail drain were untested; disabled parts froze the sweep and kept a stale line — tail drain, skip also for disabled parts, tests (bit-identical restart vs an engine silent for the same time)
- `[medium]` `patch` leakage-move click test had no hard-switch contrast — rewritten against an instant-apply splice (ramped step 1.8e-5 vs 1.9e-3)
- `[medium]` `patch` offline renderer submitted vibrato type v1 while the registry default is c1; frame-0 setup used a 5 ms crossfade — default type fixed, immediate mode while the part is silent, ctest for a CC-81-only file
- `[low]` `patch` leakage_pattern out-of-range reads guarded; leakage-in-voice-gate/1'-cancel claim now tested
- `[low]` `defer` sweep rate 6.87 Hz, tap spacing/depths and leakage level (max gain 0.02) are design values from public figures, to be refined by the Story 2.13 A/B; leakage doubles the multiply-adds per live drawbar (cost units are still placeholders)

## Auto Run Result

Status: done. Vibrato/chorus scanner V1-V3/C1-C3 (tapped delay line, 6.87 Hz triangle sweep, 5 ms crossfade on switching, fixed storage per part), seeded tonewheel leakage (neighbouring wheels, reproducible table, max −34 dB), CC 87 for the scanner type, `velora-offline-render --vibrato/--leakage`, committed scanner golden (−100 dB null test). Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

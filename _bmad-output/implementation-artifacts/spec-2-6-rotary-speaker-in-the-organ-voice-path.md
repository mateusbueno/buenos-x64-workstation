---
title: 'Story 2.6: Rotary speaker in the organ voice path'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: 'd7df18eefeb3bd71e495562a03ea189218a67b1e'
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

**Problem and approach:** Story 2.6 "Rotary speaker in the organ voice path" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1573; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints (clean-room, same rules as Stories 2.2-2.5 and `engine/core/ORGAN.md`; no GPL organ/rotary source consulted; record the public facts used (typical horn/drum crossover ~800 Hz, speed figures slow ~0.7/0.8 Hz drum..., fast ~6-7 Hz horn, acceleration/braking time ranges, Doppler from rotating-source geometry) as design values in ORGAN.md):** implement a `RotarySpeaker` DSP unit in `engine/core` (fixed storage, no allocation in render, per-part instance, reusable later as an insert in Story 2.9 so put the DSP in its own header/source usable outside the organ) with: crossover splitting the signal into horn and drum bands; for each rotor a rotation model with amplitude modulation, Doppler (small fractional delay modulated by rotor angle using a fixed-size delay line) and stereo spread (two virtual microphones / left-right positions); independent acceleration and braking time constants per rotor (horn faster than drum); speed states SLOW, FAST, STOP with ramped transitions; STOP brakes both rotors to a standstill without a click (define the standstill rest angle behaviour: freezes at current angle with gain smoothing, or settles to a defined position; document and test click-free transition via a sample-to-sample step bound); speed change at the event's in-block offset; the current speed state (`SLOW`, `FAST`, `STOP`) is published in the engine state (a lock-free readable state per part, e.g. in the RT last-value table or an atomic status word, plus a ws.v1 state read path if a natural extension of the existing `GetPerformanceValues`/state messages exists; keep ws.v1 additions additive) so a UI can show `ROTARY SLOW/FAST`; parameters: a rotary speed performance parameter with values slow/fast/stop and a rotary on/off in the registry (`rotary_*` ids already exist in `proto/params.json` — read it, use them, add CC mappings if missing, keep ids stable, add only what is missing such as an explicit enum for stop/slow/fast); the organ part output passes through the rotary stage (output becomes stereo-capable: widen the per-part organ output to stereo as needed while keeping existing mono-organ goldens passing if the rotary is off/bypassed — rotary off must be bit-exact to current behaviour); the rotary cost is part of the organ reservation from Story 2.2 (use the existing `reservation_cost_units`/`kRotaryCost` placeholders and test that the reservation includes it); names: effect named "rotary speaker", UI label ROTARY, never "Leslie" (extend the forbidden-strings test to the new sources and registry); extend `velora-offline-render --engine organ` with `--rotary off|slow|fast|stop` and ability to change speed mid-file via the mapped CC; add a committed small golden (deterministic, null-tested) of a held chord switching slow → fast → stop → slow; RTSan test: cycle speeds under a 73-key chord for many blocks. Unit tests: crossover reconstruction (bands sum flat within tolerance when rotors stand still), AM depth and rate at slow/fast measured on a test tone, Doppler pitch deviation bounded, stereo spread (L/R decorrelation or level difference) at fast, acceleration/braking time constants measured separately for horn and drum, stop standstill and click-free, state publication matches the commanded state and the actual ramp (state value transitions defined: reports target state immediately; document), determinism, off = bit-exact bypass, reservation includes rotary.

**Never:** add the insert-chain mechanics (Story 2.7), the other effects, ROTARY buttons/UI (Epic 3/4), speakers/cabinet EQ beyond what the rotary model needs, or read GPL code.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.6 (~line 1573) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.6 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` radius clamp allowed a Doppler delay far above the 128-sample line (tap index wrap) — clamp 0.25 m with a static_assert-derived bound, hostile-config test asserts max delay < 128
- `[medium]` `patch` STOP left rotors at different angles (comb/level dip at the crossover) — both creep to a common rest angle, response pinned to a fresh unit to 1e-4
- `[medium]` `patch` disabled-part rotary path, rotary on→off bit-exact bypass and the CLI `--rotary` mapping were unverified — tests (rig comparison, memcmp L==R after off, byte-equal CC-vs-flag renders)
- `[medium]` `patch` stage peak reached ~3.0 on a saturated chord — existing soft limit applied to the stage output; RTSan bound now explicit
- `[low]` `patch` stale published Hz for disabled parts, tail-expired state not cleared with the stage on, non-finite speed, accessor bounds, same-frame event ordering — fixed with tests
- `[low]` `defer` all numbers (800 Hz crossover, slow 0.8/0.67 Hz, fast 6.8/5.7 Hz, depths, radii, accel/brake times) are design values from public facts, to be refined by the Story 2.13 A/B; actual horn/drum Hz are not in ws.v1 (only the commanded state is); cost 24 units is a placeholder

## Auto Run Result

Status: done. Reusable `RotarySpeaker` DSP (800 Hz LR4 crossover, horn and drum rotors with AM, Doppler and ±60° stereo spread, independent acceleration/braking, STOP to a common rest angle), per-part rotary stage in the organ voice path (bit-exact bypass when off, 5 ms crossfade on/off, tail drain, soft-limited output), published rotary state, `rotary_on` (id 201, CC 89) and CC 79 mapping, `velora-offline-render --rotary`, committed golden. Verification: macOS ctest 131/131 pass (2 env-gated skips), Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

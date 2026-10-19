---
title: 'Story 2.8: EQ and compressor as inserts and in the master section'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '808a617554a15284cb83ca1a6bd72a95a6900462'
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

**Problem and approach:** Story 2.8 "EQ and compressor as inserts and in the master section" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1631; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** implement the real DSP of the `eq` and `compressor` effect types in `engine/core` behind the `Effect` interface from Story 2.7 (replace the pass-through placeholders for these two types in `make_effect`), reuse them as the master EQ and master compressor in the `Mixer` (replace the master placeholders; master EQ then master compressor after the send returns, each with its own bypass; per output bus Main and Aux as the Mixer already has per-bus master chains). Everything fixed-storage, no allocation in render, stereo-linked where appropriate. **EQ:** Low shelf, Mid peak with Mid freq, High shelf; gains ±15 dB; RBJ-style biquads (public cookbook formulas) designed at a fixed 48 kHz (derive from kSampleRate), coefficient smoothing per block (or per-sample interpolation) so parameter changes do not click; magnitude response within ±0.5 dB of the analytic design curve at test frequencies (write the test by measuring with sine sweeps or by evaluating the biquad's transfer function and comparing to the ideal analog-prototype/design formula; document what the "design curve" is and the test frequencies); the part EQ bands are the same parameters as the fixed encoder functions LOW, MID, MID FREQ, HIGH (map insert params 1..4 of the `eq` type to Low gain, Mid gain, Mid freq, High gain with the documented normalised→physical mappings, and make the master EQ use the master_eq_* registry params (ids 500-503) with the physical units in the registry; keep ids stable, add only what is missing; document the mapping table in one place). **Compressor:** threshold, ratio, attack, release, makeup gain; feed-forward peak/RMS detector with a documented law; the gain-reduction value (dB) is published in the engine state as a lock-free readable value per instance (part insert slot and master) — expose it in the RT last-value table or an atomic status the control thread/ws.v1 `GetPerformanceValues` can read (extend additively; document); a step-input unit test shows the measured attack and release times within 10% of their settings (define attack/release time as the time to reach 1-1/e? use a documented standard such as 63%/ -10 dB definition; document it and test it); master section always present: master EQ and master compressor in that order with separate bypass flags (add `master_eq_bypass`/`master_comp_bypass` registry params, structural, if missing, additive). Effect costs: declare measured-on-host placeholder cost units (document that board measurement is Story 2.12). Smoothing and bypass changes must be click-free (cross-fade bypass, test with a step bound). Offline renderer: extend the routing options so inserts/master EQ/comp parameters can be set (e.g. `--inserts` already exists; add master EQ/comp flags) and commit a golden per effect (EQ and compressor each: short deterministic render, null-tested at tight tolerance; bit-identical across two runs). Update the routing null tests so that the default path remains bit-exact (master section default settings are identity: EQ 0 dB, compressor ratio 1 with no makeup must be exactly transparent — define "transparent" as bypass-equivalent and bit-exact if the effect is at identity settings or bypassed; test). RTSan test cycling parameters, bypass and compressor levels for many blocks.

**Never:** implement other effects (2.9-2.11), UI/panel, measure on the board.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.8 (~line 1631) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.8 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` mid-peak response deviated 0.5-2.8 dB from the design curve above ~2 kHz (bilinear warp) and the test had been restricted — peak redesigned with matched-magnitude coefficients, test covers mid 200-8000 Hz, ±15/±6 dB, 20 Hz-20 kHz (≤0.5 dB up to 16 kHz, 1 dB at 20 kHz for the extreme case)
- `[medium]` `patch` Aux master EQ/compressor/bypass/gain reduction unverified, 5th insert param (makeup) unverified through ws.v1 — tests added
- `[medium]` `patch` published gain reduction ignored fading chains; stale compressor state after bypass; non-finite compressor input passed through; bypass flag edge values; cost constants could drift; duplicate/non-finite CLI options; defaults not pinned — fixed with tests
- `[low]` `defer` insert EQ default mid frequency 1265 Hz (log mapping) differs from the master default 1000 Hz (documented and pinned); compressor detector is peak-only (no RMS option); effect costs are host placeholders until Story 2.12; higher EQ cost can make previously accepted chains exceed the FX budget

## Auto Run Result

Status: done. Real EQ (low shelf 100 Hz, mid peak with frequency, high shelf 4 kHz, ±15 dB, per-block smoothing, bit-exact when flat) and compressor (threshold, ratio, attack/release at the 63.2% points, makeup, stereo-linked peak detector, published gain reduction) as insert effects and as the per-bus master section (EQ then compressor, separate bypass with 5 ms cross-fade), registry additions (insert_param_5, master_eq_bypass, master_comp_bypass), ws.v1 gain-reduction readout, offline-render options and two committed goldens. Verification: macOS ctest 168/168 (2 env-gated skips), Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

---
title: 'Story 2.9: Chorus/phaser, EP amp/drive and rotary speaker inserts'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '9c81b0a40ab008dfe6c6031afbe4f74150256822'
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

**Problem and approach:** Story 2.9 "Chorus/phaser, EP amp/drive and rotary speaker inserts" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1656; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** implement the three remaining real insert effects in `engine/core` behind the `Effect` interface (replace the pass-through placeholders in `make_effect` for `chorus_phaser`, `ep_amp`, `rotary`; follow exactly the conventions of the EQ/compressor from Story 2.8: fixed storage, no allocation in render, params as normalised `insert_param_1..5` with one documented mapping table in `engine/core/README.md`, smoothing per block, bit-exact identity/bypass where defined, non-finite input -> 0, bypass reset, `VELORA_NONBLOCKING` on overrides, declared cost units as host-measured placeholders, resets on slot bypass, click-free bypass/cross-fade):
- **Chorus/phaser:** a chorus mode and a phaser mode with rate, depth, feedback (phaser) and mix; mode changes cross-fade without a click (test with a step bound); stereo (decorrelated LFO phases); documented public-fact design (e.g. modulated delay lines 5-30 ms for chorus; 4-6 all-pass stages swept by an LFO for phaser); mode is one insert param (enum-like mapping of a normalised value with hysteresis-free thresholds, documented, boundary tested).
- **EP amp/drive:** drive (nonlinearity such as a soft clipper/tanh-like waveshaper, **with oversampling (e.g. 2x or 4x with a documented half-band/FIR filter) where the nonlinearity needs it**, test that aliasing is reduced vs non-oversampled by measuring a high sine through drive), tone (a tilt/low-pass), amp/cabinet colouring (a fixed documented filter set: e.g. high-pass + presence peak + cabinet low-pass), output level; declares its cost (oversampling included) to the FX budget (update `effect_cost_units` and the master/fixed cost accounting; note the budget tests that sum costs).
- **Rotary insert:** reuse the existing `RotarySpeaker` DSP (Story 2.6) as the `rotary` insert on a non-organ part with its own speed parameter (insert params: speed slow/fast/stop mapping, plus the required params documented; speed change ramps like the organ's); an organ part keeps its built-in rotary and does **not** get a second instance unless the user inserts one (test that an organ part with built-in rotary on and no insert has exactly one rotary instance, and with an insert present there are two, i.e. the user's choice; no automatic duplication); published rotary state for inserts is optional (document if not provided). The insert version must handle stereo input (the organ's built-in rotary takes a mono part signal; the insert takes the part bus stereo: design the mono/stereo handling explicitly — e.g. sum to mono then rotary — and document).
Each new effect: unit tests (frequency/time-domain behaviour appropriate to the effect, identity/bypass bit-exactness, determinism, no-click switching, NaN input), RTSan coverage (extend `rtsan_routing_test`/`routing_traffic_test` to cycle the new insert types and params), the routing null tests keep passing (default path bit-exact; update pass-through tests that used these types), and a **committed golden render per effect** (offline renderer `--inserts` options already exist; add goldens under `tools/offline-render/tests/fx_chorus_phaser/`, `fx_ep_amp/`, `fx_rotary_insert/` with `.case` null-test files, two renders bit-identical, wrong-setting renders must fail the null test). The registry may need additive entries only if something is missing (keep ids stable); ws.v1 additive only.

**Never:** implement reverb/delay (2.10/2.11), UI, board measurement, or change organ built-in rotary behaviour or goldens of other stories.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.9 (~line 1656) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.9 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` rotary insert overran its 64-sample scratch arrays for blocks > 64 frames; EP amp silently left the tail of oversized blocks unprocessed — all three effects now process in 64-frame chunks, block-split invariance tests (64/200/1/7 frames, oversampler history included)
- `[medium]` `patch` EP amp overflow to inf poisoned filter state; cabinet filters started cold when the blend rose from 0; chorus/phaser mix-0 path skipped NaN sanitising, stale phaser coefficient and mode weight after identity; rotary reset() left ramps unsettled — fixed with tests
- `[medium]` `patch` running-instance parameter smoothing (the live-automation path) untested for all three effects — tests compare a running instance after set_params with a fresh instance (EP amp within 0.1 dB, rotary width ×2, chorus/phaser rate/depth/feedback by delay track and notch response)
- `[low]` `patch` effects assumed 48 kHz/64 frames in prepare() — refuse (pass-through, prepare_ok() false) otherwise; dead test code removed
- `[low]` `defer` Effect::prepare signature unchanged (would touch EQ/compressor); EP amp aliasing at full drive still ≈ −27 dBc (4x oversampling limit); control-plane parameter changes rebuild the chain (the live smoothing path is only used by tests until performance routing of insert params exists); effect costs are Release host placeholders until Story 2.12; rotary insert publishes no rotary state

## Auto Run Result

Status: done. Real insert effects behind the `Effect` interface: chorus/phaser (chorus 5-25 ms Lagrange delay, 4-stage phaser, rate/depth/feedback/mix, mode cross-fade), EP amp/drive (4x oversampled Padé shaper, tilt, presence, cabinet, level; declared cost 24), rotary insert (Story 2.6 `RotarySpeaker` on the mono sum with a WIDTH param; the organ's built-in rotary is never duplicated automatically); three committed goldens (`fx_chorus_phaser`, `fx_ep_amp`, `fx_rotary_insert`, −100 dB null tests). Verification: macOS ctest 198/198 (2 env-gated skips), Docker gcc 197/197 and clang-20 RTSan 206/206, purity and notices checks pass. Follow-up review recommended: false.

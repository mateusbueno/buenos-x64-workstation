---
title: 'Story 2.7: Fixed routing topology with insert chains, sends, master and output buses'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: 'c1592f857ba1576382ec55fcc7c7e3b3d61f7d03'
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

**Problem and approach:** Story 2.7 "Fixed routing topology with insert chains, sends, master and output buses" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1606; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** this is the mixing/FX-chain framework; the DSP of each effect lands in Stories 2.8-2.11 (EQ/compressor, chorus/phaser/EP amp, reverb, delay) and the rotary DSP exists already (`RotarySpeaker`, Story 2.6), so in this story every effect type is a **placeholder pass-through** behind the final effect interface (only the rotary may be wired to the existing `RotarySpeaker` as the "rotary" insert if that is natural — otherwise leave it a pass-through with a TODO for Story 2.9; decide, document, and keep the organ's built-in rotary stage unchanged). Implement in `engine/core` (OS-free, RT-safe, no allocation in render) a `Mixer`/`FxRouter` phase-5 stage that runs after voices render: each part's engine output is summed into its **part bus** (stereo float, 64-frame blocks), processed by its **insert chain** (up to 5 slots, types none/rotary/chorus_phaser/ep_amp/eq/compressor from `proto/params.json`, each with a bypass flag and up to 4 normalised params), then (a) per-part send levels (the performance params `send_reverb_level`, `send_delay_level`) feed two global send buses (Reverb and Delay placeholders: pass-through returns with unity gain for the routing tests so sums can be verified analytically; real DSP in 2.10/2.11), (b) the part bus plus send returns are summed into the **Main** or **Aux** bus per the part's `part_output` (structural), then the Main/Aux go through the **master section** (placeholder pass-through for master EQ/comp until 2.8; master level trim; **line-in** mixed into the master only at a `line_in_level` parameter — add a registry parameter `line_in_level` (master scope, performance or structural as fits AD-2; keep ids stable) if missing). The final output API gives Main L/R and Aux L/R buffers per block to the host adapters (CoreAudio adapter in `engine/host/macos` plays Main for now, and takes the optional line-in from CoreAudio input into a `LineInSource` port in `engine/ports` — silent default port on the dev board/Linux; keep macOS input optional: if no input device or permission, line-in is silent and engine-host still starts). Insert chain mechanics as structural state: add/remove/reorder/bypass an effect while notes sound — the control thread (already existing `ControlPlane` with `SetInsertChain` etc. from Story 2.1) publishes a new snapshot generation with **effect instances pre-allocated off the RT thread** (an effect pool object per instance created by the control thread and handed over via the snapshot; freed by the control thread under the AD-23 lifetime rules), and RT cross-fades from the old chain to the new one over a short ramp (e.g. 5-10 ms equal-power or linear), no click, no allocation on RT; define the effect-instance interface (`Effect::process(StereoBlock&)`, prepare/reset, cost units, type) and the factory used only by the control thread; the available effect types are only the listed ones (no plugin loading). FX budget: the arbiter reserves the **FX budget** for the configured inserts, sends and master **before** the organ reservation and engine floors (extend `LiveSetConfig`/`VoiceArbiter::reserve` so the FX reservation is computed from the effect instances' declared costs, order FX -> organ+rotary -> floors already exists as an input field; wire the computation), and a control command that would exceed the FX budget is **refused with a typed error** (closed-enum `ErrorCode`, additive in `ws_v1.proto`; update tests). Routing tests in the offline renderer: with pass-through effects, Main/Aux assignment, send levels and line-in-to-master are verified by null tests against analytically mixed references (e.g. render parts separately and sum them in the test harness, or generate expected WAVs analytically) — add the offline-render options to select part output / send levels / insert chains / line-in file for tests (e.g. a small JSON or flags), keep existing goldens bit-exact when no routing options are used (default path unchanged: all parts to Main at unity with empty chains must reproduce the current outputs exactly). Unit tests: bus summation, Main/Aux split, send levels, line-in only to master, chain add/remove/reorder/bypass with a continuous tone under test (max sample-to-sample step bound during cross-fades, no allocation under RTSan), FX budget refusal (typed error, nothing changes), generation lifetime of effect instances (freed only after RT stops using the old chain; test with held notes), RTSan test: 10,000 blocks with chain changes every N blocks and send changes, determinism.

**Never:** implement the effects' DSP (2.8-2.11), UI, physical output mapping (Epic 3), Live Set loading, or plugin loading; change existing engine goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.7 (~line 1606) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.7 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` send returns of an Aux part leaked into Main — per-bus send accumulators (Aux part leaves Main untouched), declared FX cost doubled for per-bus real DSP
- `[medium]` `patch` adapters without an Aux output silently dropped Aux parts; half-set Aux buffers; render_split default put everything on part 0; channels 8..15 aliasing — Aux folded into Main with a counter, rejected half-buffers, pure-virtual render_split, offline renderer rejects channels > 7 with routing
- `[medium]` `patch` master level, line-in level, insert param changes, render_block_buses and the routed sample engine were unverified end to end — tests added through the control plane
- `[medium]` `patch` mixer robustness (null registry lookups, non-finite line-in, first generation after silent blocks, leaked pending refs) and FX budget refusal blocking cost-lowering commands — fixed with tests
- `[low]` `patch` CoreAudio line-in (atomic pointer, chunked capture, mono duplicate, rate guard, start guard, cleanup guard) — implemented, compile-checked only (no input device here)
- `[low]` `defer` engine-host reserves the flat FX budget (instead of per-instance costs) because commands can raise the cost up to the budget; ring logic of CoreAudioLineIn not extracted/tested; effect costs are placeholders until Story 2.12; rotary insert is a pass-through until Story 2.9; Aux has no physical output until Epic 3

## Auto Run Result

Status: done. Phase-5 `Mixer`: per-part buses, insert chains (≤5 slots, pass-through placeholders, 5 ms cross-fade, instances pre-allocated by the control thread and carried in snapshot generations), two global sends with per-bus returns, Main/Aux assignment, master section (placeholder EQ/compressor + level trim), line-in to master only (`LineInSource` port, opt-in `--line-in` on macOS), FX budget reserved first and enforced with typed `ERROR_CODE_FX_BUDGET_EXCEEDED`, `velora-offline-render` routing options with analytic null tests. Verification: macOS ctest 100% pass, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

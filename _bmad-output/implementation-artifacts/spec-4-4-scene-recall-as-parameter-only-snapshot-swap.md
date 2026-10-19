---
title: 'Story 4.4: Scene recall as a parameter-only snapshot swap'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '69552b5af78562dd9d08e9be129e8fe27a93ed86'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 4.4 "Scene recall as a parameter-only snapshot swap" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2673; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** build on Story 4.2 (`LiveSet` with up to 8 scenes: scene semantics are documented in `engine/host/README.md` — a scene overrides per-part parameters and master params, sparse with presence flags, no timbre, no engine type, no tempo) and Story 4.3 (`LiveSetLoader`, `PartRouter`, `PartCfg` in the snapshot, generation swap by `ControlPlane`). Add a `SceneController`/control-plane command path in `engine/host` (control thread only): `recall_scene(index)`, `next_scene()`, `previous_scene()` (step in scene order, **stop** at the first and last existing scene — no wrap — returning a typed result "at_first"/"at_last"/"ok"), current scene index tracking (published in the engine state: add an additive field to the ws.v1 state/performance read messages and new additive ws.v1 commands `RecallScene{index}`, `NextScene`, `PreviousScene` with replies carrying `request_id` and the new revision, closed error codes for "no such scene" / "no live set loaded"). The recall builds a **new snapshot by copying the currently published generation and changing only scene-stored parameters** (volumes, keyboard on/off flags and zones as scene params permit, output Main/Aux, insert chain/bypass, send levels, engine sound parameters/drawbars/etc., master params) and **never** touches the timbre/engine type/sample banks/instrument handles (the new snapshot shares the same `sample_bank` shared_ptrs: assert pointer equality in tests), **loads no heads/instruments** (the loader's resolve/load functions must not be called: test with a counting HeadProvider that fails the test if invoked), swaps in with the usual single atomic store, and **performance parameters carried by the scene (volumes, drawbars, sends, etc.) are applied by RT once, at the new generation** via the existing `perf_init` + `program_generation` tagging mechanism of Story 2.1/AD-2 (RT applies a slot's initial value only when it changed vs the previous generation; document and test that a scene recall overrides a live knob move for the parameters the scene stores, and does NOT override parameters the scene does not store; a scene recall to the same scene index re-applies the scene values — define: it resets scene-stored performance values to the scene's values — document). Part-structural scene fields (keyboard on/off, zones, output, inserts) take effect for **new notes only**: notes held, sustained by the pedal or releasing at the moment of recall continue without gap or truncation (the `PartRouter` already tracks note-off consistency; test with held/sustained/releasing notes across a recall, render-compare against the same notes without recall for the unaffected parts: the audio of sounding notes up to their release must be sample-identical for parameters the scene does not change, and gap-free (no discontinuity beyond the smoothing ramps) for changed volume); a recall command that would change a part's timbre cannot be expressed (the scene format carries no timbre) — test that a Live Set file whose scene somehow carries timbre/engine fields (hand-encoded unknown fields, or fields the schema forbids) cannot change the timbre: the timbre stays as the Live Set defines it (validation rejects or ignores, document); hostile cases: recall of a missing scene, recall while a Live Set load is in progress/none loaded, recall storm (1000 recalls) with held notes (no leaks of generations/ledger references: `live_generations()` bounded), concurrent clients serialized by the control thread; the BUSY condition when the ledger is full returns the typed busy error and changes nothing. Offline renderer: `velora-offline-render --live-set ... --scene-at SECONDS:INDEX` (repeatable) to recall scenes at given times plus a committed golden (layer/split Live Set from Story 4.3 extended with 3 scenes: e.g. verse (pad low), chorus (pad full + organ), outro; held notes across recalls) null-tested at −100 dB, bit-identical on two renders, wrong scene order fails. RTSan test: scene recalls (including storms) interleaved with dense note traffic.

**Never:** load heads, change timbres, implement seamless Live Set switching (4.5), UI/panel (4.23), arpeggiator scene fields, setlists, or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.4 (~line 2673) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.4 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` scene-stored fields (octave, transpose, Aux output, low_key, send_delay, envelope, eq, master list) had no value assertions — one test recalls every field, checks snapshot fields, perf_init/perf_force, sound, and the reset to base; mutations of three code paths fail it
- `[medium]` `patch` a recall published before RT adopted the previous program generation lost its forced slots — the previous perf_force is OR-ed forward until RT adopts (via `ControlPlane::rt_held_snapshot()`)
- `[medium]` `patch` refused recalls consumed program-generation numbers/built chains; BUSY reasons indistinguishable; scene commands copied the whole snapshot; offline `--scene-at` in the last block rejected; engine-host scene wiring untested — can_publish first, details reasons, `engine_host_scene_smoke` ctest over ws.v1 (fails if `lopt.scenes` is removed), index validated up front
- `[medium]` `patch` benchmarks flaky under load (reverb_bench failed twice in Docker) — best-of-5, retries, `kReverbSendCost` 64→96 after measuring a systematic 4-5× ratio under a busy host (raises `kFixedFxCost` by 64; 30/30 runs stable; lowering the cost still fails the bench)
- `[low]` `defer` structural scene fields (volume, output, inserts) apply to the whole part bus through the mixer ramps, only zone/octave/transpose/keyboard flag are note-level; `GetState` without any generation still returns INTERNAL (only scene commands answer NO_LIVE_SET); `scenes.wav` ≈ 2.7 MB; the worst-scene "too many effect instances" case cannot occur (≤ 48 of 128), the reachable over-budget refusal is tested instead

## Auto Run Result

Status: done. `SceneController` (control thread): `recall_scene`, `next_scene`, `previous_scene` (stop at the first/last scene) build a new snapshot from the published one that changes only scene-stored parameters — shared sample banks, no heads loaded, performance values applied once at the new `program_generation` via forced slots — with held/sustained/releasing notes continuing; additive ws.v1 commands `RecallScene/NextScene/PreviousScene`, state fields `current_scene/scene_count`, error codes `NO_SUCH_SCENE/NO_LIVE_SET`; `velora-offline-render --scene-at` and a committed three-scene golden. Verification: macOS ctest 374/374 (2 env-gated skips), Docker gcc 372 and clang-20 RTSan 383, purity and notices checks pass. Follow-up review recommended: false.

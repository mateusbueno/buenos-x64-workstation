---
title: 'Story 4.10: ASSIGN maps with a global default'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '32a285e49a46f7e471131dd389a320e06dba4f85'
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

**Problem and approach:** Story 4.10 "ASSIGN maps with a global default" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2866; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** build on 4.2 (`LiveSet.assign` 8-slot ASSIGN map of `param_id`s), 4.6 (`SetlistStore`: persisted `assign_default.pb` global default with a built-in factory default, copied into new Live Sets at creation — the read side already exists; this story adds the editing commands and the encoder routing), 4.9 (`EditSession`: ASSIGN map edits are Live Set edits: dirty "Live Set map", the hook left for ASSIGN in Whole Write) and 2.1 (parameter registry, UMP performance events, last-value table, `ControlPlane`). Implement: (1) **commands** (additive ws.v1, control thread, typed errors) `SetAssignSlot{target: CURRENT_LIVE_SET | GLOBAL_DEFAULT, slot 1..8 (K1..K8), param_id}` and `ClearAssignSlot{target, slot}` (clear = 0/unset): validation: slot range, param_id must exist in the registry and be a **performance** parameter with a numeric/enum/bool type that an encoder can drive (reject structural params and master-only parameters that are not meaningful? define the rule: performance class, any scope except those the registry marks non-assignable — add an additive `assignable` flag in `proto/params.json` defaulting to true for performance parameters and false for fence/internal ones; regenerate); duplicates in different slots allowed? (decide + document: allowed, since ids may repeat intentionally; or refuse with DUPLICATE_ASSIGN — choose the least surprising: allow duplicates and document). Editing the **current Live Set's map** updates the in-memory working copy through `EditSession` (marks the Live Set dirty with change item key `assign.slot` (the dedicated ChangeItem.slot field from 4.9), and it survives Write (Whole Live Set Write persists the assign map — verify via the saved file) and a **power cycle** (Write, restart the store/session on the same data dir, reload: the map is there); editing the **global default** writes `assign_default.pb` through the SetlistStore's atomic writer immediately (this is a settings edit, not a Live Set edit: document that Write is not needed; typed error on failure with nothing partial) and **does not change existing Live Sets** (new Live Sets copy the default at creation — test that editing the default after creating a Live Set leaves that Live Set unchanged and that a Live Set created later has the new default); a command targeting CURRENT_LIVE_SET with no Live Set loaded -> NO_LIVE_SET. (2) **Engine-side encoder routing in ASSIGN mode:** define in `engine/core` the input event for encoder steps — **the panel MCU frames deliver relative signed steps per encoder (Story 3.10 will produce them)**; here define the UMP shape for "encoder n turned by d steps" as an assignable per-part-independent controller event, e.g. `EncoderStep{encoder 1..8 (0..7), delta int16}` mapped to a vendor UMP (assignable controller, channel 15 reserved range like the fence: document the reserved ids in one place) and an engine mode flag **ASSIGN mode active** (performance-state flag set by a new UMP/ws.v1 command `SetEncoderMode{PART | ALL | ASSIGN}` — Story 4.13 does the PART/ALL functions; implement only the mode flag + ASSIGN routing now, leave PART/ALL as no-ops with a counter) held on the RT plane and published in State. When ASSIGN mode is active and encoder n turns: the RT plane looks up slot n of the **current Live Set's map** (carried in the snapshot generation: add the map to the snapshot `live_set` data additively; scenes don't change it) and applies the relative steps to the mapped parameter's **live value** (the RT last-value table): value += delta × step_size where step_size is derived from the parameter's range and type (float: range/128 per step by default, clamp to [min,max]; enum: ±1 index per step clamped (no wrap); bool: toggles on positive delta true/negative false; int: ±1) — implement `apply_relative_step(ParamDef, current, delta)` once in core with exhaustive unit tests; **no pickup** (steps apply directly to the current live value); an **empty slot does nothing** (counted); a mapped param whose engine/part does not exist (e.g. part-scope param on a part with no organ) still updates the live value (the engine ignores it) — document; the step is applied as the same kind of performance event as a CC (so engines/mixers receive it through the existing perf-event path and smoothing/ramps apply; `part` for part-scope parameters: slot maps to `param_id` only, so define the target part: the **selected/current part** — Story 4.13/4.22 define part selection; for now apply to part 0 unless the ASSIGN map entry carries a part: extend the Live Set ASSIGN slot representation additively? The 4.2 map is `repeated uint32 param_id`; keep it, and apply part-scope parameters to the **current part** which is an RT-published value `selected_part` (default 0) settable via an additive ws.v1 command `SelectPart{part}` — implement just the minimal state and command)). Large steps (delta up to ±127), storms, saturation at range ends, and rapid direction changes behave deterministically. The change is visible to the edit session (the live value differs from the saved one -> dirty summary item). Offline renderer: `--encoder-at SECONDS:ENC:DELTA` (repeatable) and `--assign-mode` plus a Live Set with an ASSIGN map; commit a golden (an organ drawbar and reverb send mapped to K1/K2 turned during a held chord), null-tested −100 dB, bit-identical on two renders. Tests: command validation and persistence across Write and restart, default-copy independence, routing for each parameter type (float/enum/bool/int), empty slot, no-pickup, mode flag gating (PART/ALL do nothing yet), concurrency, RTSan (encoder storms interleaved with dense traffic and generation changes).

**Never:** implement the MCU panel reader (3.10), PART/ALL encoder functions (4.13), LED rings, UI (4.27), pickup for faders, or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.10 (~line 2866) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.10 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` audio-thread safety of the ASSIGN step path: entries failing the shared assignability rule rejected before computing the last-value slot; steps before any generation exists (no crash, counted); assign ids looked up whole (`find_assignable`), never narrowed to 16 bits (tested with 0x10001, 0x10000+102, 0xFFFFFFFF)
- `[medium]` `patch` files saved before an id became non-assignable (e.g. `line_in_level`) must still load — validators warn, the slot stays in the file and out of the generation; editing other slots of such a default works; commands still answer NOT_ASSIGNABLE
- `[medium]` `patch` refused ASSIGN edits could leave the Live Set dirty or the working copy changed — publish first, then mutate; ledger_full and publish_refused tests leave working copy, dirty state, revision and generation unchanged
- `[medium]` `patch` SetEncoderMode/SelectPart queue-full behaviour, stale State after a repeated command, enum clamp, hidden bad-event and no-part step counters, large-range whole-step rounding — fixed with tests (`events_rejected`, `steps_no_part` in State.encoders)
- `[low]` `defer` encoder steps are not a ws.v1 command (they come from the panel, Story 3.10; the offline renderer submits them directly), so the engine-host smoke can only check mode/part reach RT through the Write fence; encoder mode is not persisted (restart starts in PART); `line_in_level` is non-assignable by choice; float steps may differ in the last bit on FMA platforms (goldens absorb it); the "drop the rollback" mutation cannot be run (no rollback code remains); State.encoders.assign shows the generation's map which can differ from the file's

## Auto Run Result

Status: done. Per-Live-Set ASSIGN maps (8 slots) with a persisted global default: additive ws.v1 `SetAssignSlot`/`ClearAssignSlot` (current Live Set edits go through the edit session, dirty as `assign.slot`, persisted by Whole Write and surviving restart; the default is written atomically at once and never changes existing Live Sets), registry `assignable` flag, reserved channel-15 UMPs (`EncoderStep`, `SetEncoderMode`, `SelectPart`, fence) in one table, `apply_relative_step` (float range/128, int/enum ±1 without wrap, bool by sign, no pickup), RT routing of encoder steps in ASSIGN mode to the mapped parameter's live value (empty slot does nothing), `velora-offline-render --assign-mode/--encoder-at` with a committed golden. Verification: macOS ctest 448 (2 env-gated skips), Docker gcc 442 and clang-20 RTSan 456, purity and notices checks pass. Follow-up review recommended: false.

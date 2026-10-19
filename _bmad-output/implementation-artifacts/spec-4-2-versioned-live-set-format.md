---
title: 'Story 4.2: Versioned Live Set format with parts, scenes and ASSIGN map'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '1bda4c3a1ff3c8de0678a9fad4a8112266b22a41'
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

**Problem and approach:** Story 4.2 "Versioned Live Set format with parts, scenes and ASSIGN map" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2552; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** define the persisted `LiveSet` (protobuf) in `proto/live_set.proto` (package `ws.v1` or a sibling package `velora.liveset.v1` — choose per the architecture conventions (`PascalCase` messages, `snake_case` fields) and document) with `format_version` and implement in `engine/host` (control-thread side, portable C++, protobuf allowed here but **not** in `engine/core`) a `LiveSetStore`: read, write, validate, migrate-stub for N/N−1, UUIDv4 minting (inject a random source so tests are deterministic; real source = OS random; no RTC dependency), and atomic persistence (temp file in the same directory on the journaled partition, fsync file, rename, fsync directory; only the control thread writes Live Set files — document and enforce by API: the store has a single writer entry point). Message content exactly per the story: id (UUIDv4 string), name, tempo, parts (the tier's part count: 8 for v1, 16 for Pro — the message holds N parts; validation enforces the tier part count passed in), each part: timbre reference `(library_id, instrument_key)` or empty, low key, high key, octave, transpose, keyboard on/off, volume, output Main/Aux, insert chain (reuse the insert slot message shape from `ws_v1.proto`), send levels, **engine data** = engine type ID (Story 1.8's `EngineTypeId`) + sound parameters as a list of `(param_id, value)` keyed by registry id — no engine-specific message per engine so FM/VA fit unchanged —, envelope, EQ (reuse registry param ids); up to 8 scenes (each: name and every Live Set parameter except timbre assignment — i.e. a per-part overridable set of the same parameters + live-set-level ones, with presence flags so a scene can be sparse or full: define and document the exact scene semantics: scene = full parameter snapshot excluding timbre; default for an absent field = value in the base Live Set); an 8-slot ASSIGN map of `param_id`s; the arpeggiator fields Epic 7 will use defined now and left unset (a message `ArpSettings` with fields enabled, pattern, rate, gate, octave range, hold/latch, swing as optional fields; add no migration) — see Epic 7 stories in `epics.md` (7.2-7.4) to choose field names; unknown-field preservation: protobuf preserves unknown fields on parse/serialise by default — **test it** with a file written by a newer schema (simulate by adding fields via a second compiled test proto or by hand-encoding extra field numbers) round-tripping unchanged; N/N−1 reading: a file of version N or N−1 loads, an unknown major version is refused with a typed error (closed error enum in the store API, not exceptions across the boundary); current `format_version` = 1 now; to exercise N−1 create a tiny v0-compat path only in tests (e.g. a test-only version constant) — keep it honest: since no older release exists, the test writes a file with `format_version` = current−1 using the same schema and asserts it loads; a **compatibility corpus** directory (e.g. `engine/host/tests/corpus/live_sets/`) seeded with Live Sets saved by this release (several: minimal, full 8-part with scenes and ASSIGN, a FM-type part, a missing-library part, a Pro-sized 16-part set) plus a ctest that opens **every file in the corpus** on every build and a regeneration target/script; a test Live Set holding a part of a reserved engine type with no compiled engine (FM or VA in v1) round-trips through read and write **byte-for-byte unchanged** (deterministic serialization: use the library's deterministic option or sort map fields; document) and the loader helper reports that part as "engine not available" (silent, never substituted) — implement the *validation/resolution report* function `resolve_parts(LiveSet, engine availability)` returning per-part state (ok, engine-not-available, timbre-empty) without loading anything; ID collision rule: a Live Set loaded from import/restore whose ID collides with an existing one gets a new UUIDv4 and keeps its content (provide `adopt_with_collision_check(existing_ids)`); limits and validation: key ranges 0..127, low <= high, octave/transpose ranges (document), volumes, tempo range 20..300, name length, scene count <= 8, ASSIGN slots == 8 (empty slot = 0/unset), param_ids must exist in the registry with the right class (performance/structural) when they are referenced — unknown param ids are preserved but flagged (not rejected: forward compatibility), out-of-range values rejected with typed errors; hostile input (truncated file, garbage, huge repeated fields, deep nesting) never crashes (bounded file size, e.g. 1 MiB, enforced). Tests: round-trip, golden serialized bytes for one fixed Live Set (pins the wire format; hard-code a hex or commit a binary golden file), every validation rule, atomic write behaviour (simulate crash by killing between temp write and rename with a fault-injection hook, like the librarian: old file or new file, never partial; leftover temp files cleaned up by a reconcile call), single-writer enforcement, UUIDv4 format and collision handling, unknown-field preservation, version policy, corpus test, fuzz-ish test on random garbage/mutations of a valid file (deterministic seed) that never crashes and never returns OK for a corrupted CRC-less file unless it parses validly, RTSan not needed (control thread). Registry: add a stable scene-able marker? Not needed.

**Never:** implement scene recall, Live Set loading into the engine, setlists, UI, ws.v1 commands for Live Sets (later stories), or put protobuf in engine/core.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.2 (~line 2552) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.2 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` OsRandom fell back to a pid/address seed (repeating ids; adopt_with_collision_check could loop forever) — fails closed, 16-draw cap, `AdoptResult`
- `[medium]` `patch` a copied/renamed file silently shadowed another Live Set (embedded id vs filename) — refused; non-UTF-8 names/keys could be written but not re-read; instrument_key could carry `..`/absolute/backslash paths; empty TimbreRef made the set invalid — validated/normalised
- `[medium]` `patch` scene key overrides only validated when both were overridden; scene insert slots lacked a test — effective pair checked against the base; test added
- `[medium]` `patch` forward-compat leniency: unknown insert types and enum values beyond this build's range were rejected despite the proto promising preservation — preserved with warnings, round-trip tested
- `[medium]` `patch` durability reporting: rename ok but dir fsync failed reported as plain failure; reconcile returned kOk with leftovers; no I/O failure tests; thread-id reuse; dangling registry reference in availability — `kCommittedNotDurable`, kIo from reconcile, read-only directory test, `rebind_control_thread()`, engine ids copied
- `[low]` `patch` registry coupling (tempo id/range from the generated registry), list class/scope checks implemented as the README claimed, determinism wording corrected
- `[low]` `defer` envelope/EQ lists are `ParamEntry` lists with stand-in ids until the registry defines per-part envelope/EQ ids; scenes carry no tempo (Story 7.4: tempo never stored in a scene); fuzz test prints ~800 harmless protobuf UTF-8 lines; master-list class rule (allow send_reverb_master, reject line_in_level) is one condition to adjust

## Auto Run Result

Status: done. `proto/live_set.proto` (package `velora.liveset.v1`: LiveSet, Part, Scene/ScenePart, TimbreRef, EngineData, ArpSettings fields defined and unset) and `LiveSetStore` in `engine/host`: validated read/write with typed errors, N and N−1 version policy, unknown-field preservation, UUIDv4 minting and collision adoption, `resolve_parts` (FM/VA parts silent, never substituted), atomic write (temp, fsync, rename, directory fsync) with single-writer enforcement (flock + control thread), fault-injection tests, 6-file compatibility corpus and golden wire bytes. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

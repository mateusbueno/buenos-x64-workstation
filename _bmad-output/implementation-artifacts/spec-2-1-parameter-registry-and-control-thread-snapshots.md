---
title: 'Story 2.1: Parameter registry and control-thread snapshots for engine settings'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '84b7af4d331bd5de78e451b8e7d906dad4e06f73'
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

**Problem:** Organ, rotary, insert, send, master and output parameters need exactly one owner and one way to be edited by every client (UI, panel, Velora Studio). There is no parameter registry, no control thread, and no way to hand structural settings to the RT threads safely.

**Approach:** (1) A parameter definition file in `proto/` plus a generator that emits C++ tables for `engine/core` and `engine/host` and the matching `ws.v1` protobuf enums, failing the build on duplicate ids or a missing class. (2) A `ws.v1` Protocol Buffers schema (commands/state for parameters, handshake with version and features, closed error-code enum) served over a Unix domain socket by a **control thread** in `engine/host` — the only writer of structural settings — which validates commands against the registry, builds a new immutable snapshot generation and swaps it in with one atomic pointer store, replying with `request_id` and the new revision. (3) Performance parameters reach the RT threads only as timestamped UMP events (a defined mapping from param_id+part to a UMP per-note/assignable controller), RT owns the live value and publishes it in a lock-free last-value table the control thread reads. (4) First generation loaded from a committed dev config file (up to 8 parts: timbre reference, insert chain, send levels, output), and generation lifetime per AD-23.

## Boundaries & Constraints

**Always:** `engine/core` stays OS-free and RT-safe (RTSan clean: 10,000 parameter changes and 100 snapshot swaps with no allocation, lock or blocking on the RT path); protobuf is the wire format (`ws.v1` package, `PascalCase` messages, `snake_case` fields, only grows by addition, version+feature handshake first on every connection, closed error-code enum, errors as {code, message, details}); the generator is deterministic and runs in the CMake build (Python 3 only; generated files are build outputs, not committed, unless a committed golden copy is needed for review); protobuf toolchain: use `protoc` + libprotobuf (Homebrew `protobuf` on the Mac, `protobuf-compiler` + `libprotobuf-dev` in the Linux Docker image `tools/ci/docker/Dockerfile`, update the Dockerfile and `docker-check.sh` if needed); installing the Homebrew package on this Mac is authorized for this story; keep protobuf strictly in `engine/host`, `proto/` and tools — `engine/core` must not depend on it (purity check must pass, update it only if needed for the generated headers location); only the parameters this epic needs are declared (organ drawbars 1-9 per part, percussion, vibrato/chorus, key click and the other organ settings you can justify from `_bmad-output/specs/spec-arm64-stage-workstation/engines.md`, rotary speed/stop/slow-fast, inserts (type + params), sends (reverb/delay levels), master (EQ/comp), per-part output), each with a stable numeric id, class (structural | performance), type, range, default, unit; ids are never reused (a reserved/deprecated list in the definition file); new structural commands: set part insert chain, set send levels, set part output, set master params, set timbre reference (opaque id for now); an invalid command returns a closed-enum error code and changes nothing (revision unchanged); AD-23 lifetime: a snapshot generation is freed only after the RT side reports no live voice references it (epoch/hazard-style reclamation; freed by the control thread, never on RT); the control thread is the only writer of snapshots; "Save waits on a control->RT fence" is out of scope but the fence primitive may be provided.

**Never:** Add organ or effects DSP (Stories 2.2+), the librarian/sysd/ui processes, authentication/TCP/USB-NCM bridging, Velora Studio, Live Set formats (Epic 4), or protobuf dependencies in `engine/core`; hand-edit generated outputs; let RT threads read from structures the control thread is mutating.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Generator ok | valid definition file | C++ tables (engine/core, engine/host) + `ws.v1` enums emitted; build passes | none |
| Duplicate id | two params same id | generator and build fail with the ids named | build error |
| Missing class | param without class | build fails | build error |
| Reused id | id in the reserved/deprecated list reused | build fails | build error |
| Handshake | client connects | server sends/expects version+feature handshake first; unknown version rejected with closed code | closed error code |
| Structural command | valid set insert chain for part 2 | new snapshot revision N+1 swapped by one atomic store; reply with request_id and revision; state broadcast to subscribers | none |
| Invalid command | unknown param id, out of range, part index > 7, wrong class (performance param sent as structural), malformed protobuf, oversized frame | closed-enum error reply, no state change, revision unchanged, connection survives except for framing violations | closed error code |
| Performance param | drawbar 3 of part 4 via CC or ws.v1 | becomes a timestamped UMP for RT; RT owns the value; last-value table readable by the control thread without locks | none |
| Held note across swap | note sounding while a new generation is swapped in | old generation not freed until the voice releases and RT reports no references | none |
| RT safety | 10,000 param changes + 100 swaps | RTSan clean | n/a |
| First generation | engine starts with the committed dev config | published as generation 1 before audio runs | invalid dev file -> engine refuses to start with a clear error |
| Concurrent clients | two clients send structural commands | serialized by the control thread; revisions strictly increasing; no lost update | none |

</intent-contract>

## Code Map

- `proto/` (new top-level dir, Structural Seed) -- params definition file (YAML or JSON; choose and document, parse with Python stdlib or a vendored-free parser: use JSON or a restricted YAML subset to avoid new dependencies), `ws_v1.proto`, generator script, README
- `engine/core/` -- block renderer/UMP (`ump.hpp`), engines; add generated param tables, immutable snapshot types and the RT last-value table (lock-free, no allocation) here; core stays free of protobuf and OS
- `engine/host/` -- control thread, UDS server, protobuf codec, snapshot publication (atomic pointer + reclamation), dev-config loader; tests via a UDS client in the test process
- `engine/host/macos` `engine-host` -- start the control thread and UDS server (socket path option) and load the dev config; keep the existing audio/MIDI behaviour
- `tools/ci/check_core_deps.py`, `tools/ci/docker-check.sh`, `tools/ci/docker/Dockerfile` -- validations; **macOS skips RTSan tests; always run docker-check**
- `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md` AD-2 (~line 81), AD-8, AD-9, AD-23 (~line 311), conventions (~line 410-425); `_bmad-output/planning-artifacts/epics.md` Story 2.1 (~line 1421); `_bmad-output/specs/spec-arm64-stage-workstation/engines.md` -- read-only

## Tasks & Acceptance

**Execution:**
- `proto/params.json` (or yaml) + `proto/gen_params.py` + CMake integration -- definition, validation (duplicate ids, class required, reserved ids, range/default consistency), emission of `engine/core` tables, `engine/host` tables and `ws_v1_params.proto` enums -- registry
- `proto/ws_v1.proto` -- handshake, structural commands (set insert chain, send levels, part output, master params, timbre ref), performance param set command, state/revision messages, error enum, request_id -- wire schema
- `engine/core` snapshot types (8 parts: timbre ref, insert chain, sends, output; master) immutable + RT last-value table + param event mapping to UMP -- core data
- `engine/host` control thread + UDS server + framing (length-prefixed protobuf) + validation + generation swap + reclamation + dev-config loader + `engine/host/dev/engine-dev.json` committed -- control plane
- tests: generator failure cases (run the generator on bad fixtures and expect failure), control thread tests over a real UDS client for every matrix row, concurrency test (many clients), held-note-across-swap lifetime test, RTSan test (10,000 param changes + 100 swaps) wired like the other RTSan tests -- coverage
- READMEs (proto, engine/host, engine/core) -- docs; Dockerfile gets protobuf packages

**Acceptance Criteria:**
- Given a definition file with a duplicate id or a missing class, then the build fails naming the problem
- Given a valid structural command over the UDS, then a new immutable generation is swapped atomically and the reply carries request_id and the new revision; an invalid command returns a closed error code and changes nothing
- Given a performance parameter change, then it reaches RT only as a timestamped UMP, RT owns the live value and the control thread reads it from the last-value table without locks
- Given a held note across a snapshot swap, then the old generation is not freed until no voice references it
- Given macOS and Docker Linux, all tests pass, including RTSan (Docker only)

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (this is the only place the RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass (record protobuf, BSD-3-Clause, in THIRD_PARTY_NOTICES.md/manifest as a build dependency if the notices script requires it)

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~70 findings — patched 10 groups, rest deferred
- findings:
  - `[high]` `patch` use-after-free on the audio thread: adopt_generation read the previous generation through the moved hazard pointer — RT-owned copy of last applied initial values; deterministic poisoned-generation test
  - `[high]` `patch` hazard-pointer clause of safe_to_free never isolated by a test; tautological assertions; test dereferenced a possibly freed generation — deterministic tests at core and control-plane level
  - `[medium]` `patch` control socket: permission race, unlink of non-socket paths, EAGAIN treated as stale, no client cap/handshake deadline, EMFILE busy-spin, dead clients still processed, oversized replies, subscribe without initial state — hardened with limits and tests (fragmented frames, slow subscriber, mid-frame disconnect, 0600 mode)
  - `[medium]` `patch` voice_info contract that generation reclamation depends on was unverified for the real engines — contract tests for tone and sample engines (multi-region notes)
  - `[medium]` `patch` hardware CC64 (sustain) was dropped by the new CC branch so the arbiter never saw pedals — forwarded again with a test
  - `[medium]` `patch` generator crashed with tracebacks on bad shapes/non-finite numbers, missed constant clashes, reserved ids not in the proto enum, structural params without snapshot storage, insert param contiguity — fixed with tests including header compile and protoc runs
  - `[medium]` `patch` apply_insert_chain null deref, duplicate/partless performance_init entries (ParamValue.part now optional) — fixed
  - `[low]` `patch` semantics undocumented (Ack = queued, send levels apply on generation change only, float quantisation, thread ownership, MIDI-first tie-break) — documented, tie-break tested; flaky fixed sleeps replaced by deadline polling
  - `[low]` `defer` a sounding note pins its generation (32-slot ledger -> BUSY; per-key retrigger pinning; no panic/eviction path); CC changes are not pushed to subscribers (poll via GetPerformanceValues); protobuf 3.21 (Ubuntu) vs Homebrew skew only checked by the Docker build; absolute dev-config path baked into engine-host; schema additive-only golden check; protocol negotiation is exact-match; subscriber broadcast serialises per subscriber
  - `[intent gap noted]` AC2/AC4 "engine-host running": engine-host composes the control plane (flags `--socket`, `--config`) and was run by hand answering a Hello; automated tests drive the same components through a synthetic-clock rig, not the CoreAudio/CoreMIDI process

## Auto Run Result

Status: done. Added `proto/` (parameter registry JSON with 37 parameters, deterministic generator, `ws_v1.proto` with handshake, structural and performance commands and closed error enum), `engine/core` snapshots (immutable generations, atomic pointer swap, hazard pointer + reference ledger lifetime, lock-free last-value table, parameter-event UMP mapping), `engine/host` control plane + Unix-socket server + dev-config loader, `engine-host --socket/--config`. Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok (10,000 param changes and 100 swaps RTSan-clean), purity and notices checks pass. Follow-up review recommended: false.

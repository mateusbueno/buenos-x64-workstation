---
title: 'Foundation: sysd config service (host process) and engine config client, prerequisite of Stories 7.7, 3.6, 3.11, 8.9'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'd924fd8b66cd03344ef637c0b85f9bf8dec67954'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/format-versions-audit-8-5.md', '{project-root}/proto/README.md']
warnings: []
deferred:
  - summary: >-
      Not covered: behaviour for clock source, velocity curve, pedal calibration and Boot-into (Stories 7.7, 3.6, 3.11, 8.9 consume the config callback); ui SystemConfig JSON not migrated and ui is not a sysd client; no systemd unit (example in README), RAUC mark-good not wired (--slot-good is a dev flag); no TCP/USB bridge or pairing (Stories 9.3/9.6).
    evidence: |-
      See sysd/README.md.
    severity: medium
  - summary: >-
      Known limits: F_FULLFSYNC on the single poll thread stalls the server for the fsync duration; revision restarts at 0 after defaults; stale-socket replacement TOCTOU across two sysds sharing a socket path (operator error); handshake slot exhaustion for 5 s; client does not validate received Config; unknown fields in a patch dropped silently; N-1 stamp path duplicates the slot_good gate; live-set-id format is not UUID (tighten in 8.9); pedal minimum span (3.11); the 6 s engine_host_sysd_smoke flake was not reproduced (fix rests on larger timeouts and diagnostics).
    evidence: |-
      Recorded in sysd/README.md and the review triage log.
    severity: low
---

<intent-contract>

## Intent

**Problem:** Several stories (7.7 clock source, 3.6 velocity curves, 3.11 pedal calibration, 8.9 Boot-into) say the setting lives in the `sysd` config and reaches `engine` and `ui` at start and on change (AR17, AR39, AD-12). No `sysd` exists, so there is no owner and no channel (decision confirmed with the user: build a host `sysd` process owning the config; architecture says "System config: sysd; others, at start and on change", config is "Protobuf with schema_version, owned by sysd").

**Approach:** Create the `sysd/` component as a host executable (`velora-sysd`, C++20, becomes a systemd unit later) that owns one persistent config protobuf with `schema_version` (atomic writes, N/N-1 reader and unknown-field round trip using the Story 8.5 helpers, migration-runner ready), and serves it over ws.v1 on a Unix domain socket: Hello handshake (as the engine's control server), `GetConfig`, `SetConfig` (typed partial update with validation and a monotonically increasing revision), `SubscribeConfig` (pushes the full config on subscribe and a delta/full snapshot on every change). Provide a client library for the engine (`engine/host`: `SysdConfigClient`, background thread, reconnect with backoff, callbacks on the control thread side, last-known config cached, works with sysd absent) wired into engine-host behind `--sysd-socket PATH`, and a dev CLI `velora-sysdctl` (get/set/watch). The config message carries the fields the dependent stories need (additive, behaviour is NOT implemented here): clock_source, velocity_curve, pedal calibration (sustain/expression min/max per pedal, polarity), boot_into (mode + default_live_set_id).

## Boundaries & Constraints

**Always:** ws.v1 is the single IPC schema: add config messages to `proto/ws_v1.proto` additively (new ClientMessage/ServerMessage body numbers, closed ErrorCode additions, schema grows by addition only) and a `proto/sysd_config.proto` for the persisted `SysdConfig` (schema_version = 1, field numbers never reused, `reserved` where appropriate). The engine's own control server must keep rejecting config messages with its existing unknown-command behaviour (do not mix servers). Persistent file lives under a data dir (`<data>/config/sysd_config.pb`, 0644, dir owned by the sysd user per AD-9); written atomically (temp, fsync, rename, dir fsync) with the existing atomic helper pattern (liveset::atomic_replace_file or the same stage hooks) and fault-injection hook; missing/corrupt/unknown-major file ⇒ defaults + a logged reason, never a crash; N-1 files load and are rewritten as N only through the 8.5 migration policy (do not write on read). Defaults: clock_source INTERNAL, velocity_curve NORMAL, pedals uncalibrated, boot_into LAST_SOUND (as in the stories). Validation on SetConfig: enum ranges, calibration min<max within the pedal resolution, default_live_set_id format, rejects with typed ErrorCode and changes nothing. Single writer: only sysd writes the file; SetConfig is serialized in one thread. Subscribers get the config within 100 ms of a change (test with a fake clock/loop, no sleeps in logic). The engine client must never block the RT plane or the control tick (all I/O on its own thread; applying a received config to engine state is exposed via a callback the stories will use, with a version/revision guard). Tests run without root and with temp dirs; sockets in temp dirs. Keep `tools/ci/check_core_deps.py` green (engine/core must not depend on this).

**Never:** No behaviour for clock source / velocity / pedals / Boot-into (the stories do that), no migration of the ui SystemConfig JSON (theme, language, brightness etc. stay in ui for now; document this as a follow-up), no systemd unit file beyond a documented example, no TCP/USB bridge, no pairing, no RAUC/MCU duties of sysd.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| First start | no config file | defaults served, file created on first change only | none |
| Load N / N-1 | file at N or N-1 | loads; N-1 read-only until migration allowed | too old / unknown major ⇒ defaults + logged typed reason |
| Corrupt file | bad bytes | defaults, reason logged, bad file kept aside (renamed .corrupt) | no crash |
| SetConfig valid | partial update | validated, applied atomically, revision+1, persisted, pushed to subscribers | none |
| SetConfig invalid | out-of-range / min>=max | typed error, nothing changed, nothing pushed | typed error |
| Subscribe | client subscribes | immediate full config push, then pushes on each change | none |
| Persist failure | ENOSPC / rename fail | change rejected with typed error (or applied in memory with a not_durable flag: choose and document, like Story 4.6's committed-not-durable) | no corrupt file |
| Kill mid-write | kill at each write stage | next start reads old or new, never corrupt | none |
| Engine client | sysd absent at start, appears later | client retries with backoff, applies config on connect, keeps last-known on disconnect | no crash |
| Revision guard | stale or duplicate push | callback not re-fired for the same revision | none |
| Single writer | two concurrent SetConfig | serialized; revisions strictly increasing | none |

## Code Map

- `proto/ws_v1.proto`, `proto/README.md`, `proto/CMakeLists.txt`, `engine/host/CMakeLists.txt` (protoc wiring) -- add config messages and the persisted proto.
- `engine/host/include/velora/host/control_server.hpp`, `src/control_server.cpp` (framing, Hello, poll loop, subscriber push), `live_set_store.hpp` / `liveset::atomic_replace_file`, `boot_into_store.cpp` (Story 8.8: atomic write + stage hook + load fallback pattern), `migration_runner.hpp` and `format_versions_test.cpp` (Story 8.5 N/N-1 helpers), `tools/diag/` (UDS client + CLI pattern from Story 1.21) -- patterns to reuse.
- `engine/host/macos/main.cpp` -- engine-host wiring (`--data-dir`, `--virtual-panel` flags).
- Top-level `CMakeLists.txt` -- add `sysd/` subdirectory; new: `sysd/CMakeLists.txt`, `sysd/include/velora/sysd/config_store.hpp`, `sysd/src/config_store.cpp`, `sysd/src/config_server.cpp`, `sysd/src/main.cpp`, `sysd/README.md`; engine client `engine/host/include/velora/host/sysd_config_client.hpp` + src; `tools/sysdctl/`.
- Tests under `sysd/tests/` and `engine/host/tests/`.

## Tasks & Acceptance

**Execution:**
- protos -- SysdConfig (schema_version=1; clock_source, velocity_curve, pedal calibrations, boot_into) + ws.v1 config messages and error codes (additive)
- `sysd/` -- config store (load policy, validation, atomic persist, revision), UDS server (Hello, Get, Set, Subscribe), main with `--socket`, `--data-dir`
- engine client + engine-host `--sysd-socket` wiring; `velora-sysdctl` CLI
- tests -- every matrix row incl. kill-at-each-stage, N/N-1/unknown-major, corrupt file, validation, subscribe latency with fake loop, reconnect with a late-starting sysd (real UDS in temp dir), revision guard, engine unaffected when sysd is absent, engine server rejecting config messages, an end-to-end smoke (sysd + sysdctl + engine-host --no-audio client)
- docs -- sysd/README.md (ownership, file layout, ws.v1 messages, how dependent stories add fields and behaviour, what is not covered), proto/README.md

**Acceptance Criteria:**
- Given `velora-sysd` running, when a client sets a valid config value, then it is validated, persisted atomically and pushed to every subscriber (engine client callback fires once per revision).
- Given the engine started before sysd, then it runs with defaults and applies the config when sysd appears, never blocking audio or the control tick.
- Given a corrupt, too-old or unknown-major config file, then sysd serves defaults and logs a typed reason without crashing; a kill at any write stage leaves the old or the new file.
- Given the dependent stories, then each can add its behaviour by consuming the callback without further protocol changes.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer; foundational code)
- findings:
  - `[high]` `patch` data loss: too-large/unreadable file overwritten by the first SetConfig; aside rename before replace left no primary file on a failed write; aside overwrote earlier asides — hard-link aside, unique names, primary stays until the atomic replace
  - `[high]` `patch` server dropped buffered frames on EOF; client blocking connect could hang stop(); backoff reset without a valid Config — fixed + tests
  - `[medium]` `patch` field-wise patch semantics for pedals and boot_into (avoid clearing fields in 3.11/8.9), racy and vacuous client tests, latency measurement including fsync, smoke flake diagnostics and timeout
  - `[low]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "sysd|config|control|compat|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: velora-sysd (config store with atomic persist, flock single-writer, N/N-1 policy, validation, revision; ws.v1 UDS server with Get/Set/Subscribe), proto/sysd_config.proto + additive ws.v1 messages and ErrorCode 41, engine SysdConfigClient (own thread, reconnect, revision guard, last-known cache) wired behind engine-host --sysd-socket, velora-sysdctl CLI.
Verification: sysd/config/control/smoke tests pass (smoke 20/20 repeat); full ctest passes except the 4 pre-existing failures; core deps check passes.

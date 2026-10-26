---
title: 'Foundation: sysd ws.v1 TCP bridge (9.3 Mac logic, pairing-lite) and Velora Studio device client, prerequisite of Stories 9.8 and 9.13'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'f768a42bc96111a031948c67591d4f125a1b719b'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/sysd/README.md', '{project-root}/desktop/src-tauri/src/main.rs']
warnings: []
deferred:
  - summary: >-
      Board work not done: nftables single-port policy, NCM/link-local addressing, USB gadget, TLS (the physical USB link is the trust boundary), unit-side pairing UI/lifecycle (Story 9.6: reopen window, Forget, cooldown), engine-host --studio-port dev listener. Studio GUI (pairing dialog, CSP) not exercised in a real webview; the editor still renders simulated Live Sets (Write to Velora disabled while paired) until Story 9.8 maps real State.
    evidence: |-
      sysd/README.md, desktop/README.md.
    severity: medium
  - summary: >-
      Known limits: library path policy (absolute device paths) deferred, bidi characters in computer names, ConfigService holds its mutex across an fsync, pairing denial-of-service by burning the 5 global attempts (per-source cooldown), dev flags not compiled out of release builds, u64 precision in JSON State, engine_host_bridge smokes are load-sensitive in parallel full runs (pass alone and repeated).
    evidence: |-
      Recorded in the READMEs and the review triage log.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Velora Studio (Tauri app in `desktop/`) has no real connection to the unit: its "device" is an in-memory simulation (`make_default_device_state`), so Stories 9.8 (Live Set view) and 9.13 (Setlists view) have no state snapshot or deltas to show. Architecture: the desktop app uses the same ws.v1 schema, framed on one TCP port over USB NCM and bridged by `sysd`; `sysd` forwards commands to the owning process over UDS and relays state back (Stories 9.3, 9.6, 9.7). Decision with the user: build the `sysd` bridge (Mac/localhost logic) plus a Rust client in Studio, pairing reduced to a fixed test code on the Mac.

**Approach:** (1) `sysd` bridge (`sysd/`, new component alongside the config service): listens on ONE TCP port (default 127.0.0.1 on the Mac; the bind address is configurable; the NCM interface, nftables and link-local addressing are board work) speaking length-prefixed ws.v1 frames (same framing as the UDS servers, 4-byte big-endian length, 65536 max). Handshake: `Hello {client_version, features[], client_id}` → reply `{device_version, firmware_version, features[], device_id, model}` with the negotiated feature set (additive ws.v1 messages/fields; reuse the existing Hello/HelloAck where possible by adding fields); messages for features not in the set → `UNSUPPORTED_FEATURE` (closed ErrorCode addition). An unpaired client may only call Hello and the pairing messages (the allow-list is a table); everything else → `NOT_PAIRED`. Pairing (reduced, Story 9.6 Mac scope): `PairRequest{code, computer_name}` / `PairReply`; the bridge has a pairing window with a code from `--pairing-code CODE` (test) or random 6 digits logged to stdout; 5 wrong codes close the window; `--dev-bypass-pairing` pairs every client (dev host, model "DEV HOST"); paired computers persist (id, name, last connected) in a small file under the sysd data dir (atomic write, same helper pattern as the config store). A paired session forwards commands: Live Set / setlist / scene / edit / state messages to the engine control UDS, library and catalog messages to the librarian UDS (if the librarian socket is configured), config messages to the sysd config service in-process; relays State snapshots and pushed messages back, never interpreting Live Set content; `request_id` is preserved end to end (client assigns; bridge maps (client_id, request_id) to an upstream id and back). (2) Studio Rust client (`desktop/src-tauri`): a `device` module with a blocking TCP client on its own thread using prost-generated ws.v1 types (already linked), connect/disconnect/reconnect with backoff, Hello, pairing, Subscribe to engine State, a request/response helper, and Tauri commands/events to the JS layer: `device_connect(host, port)`, `device_pair(code, name)`, `device_disconnect`, `device_status`, and events `device://status` (disconnected, connecting, unpaired, paired, version-mismatch), `device://state` (the latest State as JSON-serializable struct or the raw message converted via serde), plus a generic `device_request` command for later stories. The existing simulated device stays as the offline fallback (not removed): the title-bar connection indicator and the offline explanation follow the real status when a device client is configured. JS: minimal wiring so `isConnected`/status reflect `device://status`, a pairing dialog hook (can be a plain prompt for now; the full dialog design is Story 9.6) and a state store that 9.8/9.13 will read.

## Boundaries & Constraints

**Always:** ws.v1 additions are additive (new body numbers, ErrorCode additions NOT_PAIRED / UNSUPPORTED_FEATURE if missing, new fields); never reuse numbers; keep `proto/README.md` current; Studio's Rust build keeps compiling for macOS/Windows/Linux (stdlib networking only; no OS-specific code). The bridge never interprets Live Set content and applies a per-connection rate/size limit (frame cap, max clients 8, idle/handshake timeouts); an unpaired client can do nothing but Hello/pair. The pairing code is compared in constant time, 5 wrong codes close the window, the window closes after 2 minutes (injected clock for tests); `--dev-bypass-pairing` and the fixed `--pairing-code` are dev-only flags documented as such (the binary logs a loud warning). Replies from upstream carry the original client request_id. Reconnect: the Studio client reconnects with backoff after the unit drops and re-subscribes; version mismatch (client_version vs device protocol N/N-1 policy of Story 8.5) is reported as a status, not a crash. Tests run without root, sockets in temp dirs / ephemeral localhost ports, no real network beyond 127.0.0.1. Existing sysd, engine, desktop (cargo test) and backup/restore tests must keep passing.

**Never:** No nftables/NCM/link-local address assignment (board), no USB gadget, no TLS (the physical USB link is the trust boundary per the architecture; note it), no full pairing dialog UI or device-side pairing UI (Story 9.6 device section), no Live Set editing commands beyond forwarding, no removal of the offline simulation, no changes to the engine's own protocol behaviour other than what forwarding needs (none expected).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Hello | client Hello with features | reply with device info and negotiated feature set | unknown major version → typed error, close |
| Unpaired command | any non-Hello/pair message | NOT_PAIRED, nothing forwarded | none |
| Unsupported feature | message of a feature not negotiated | UNSUPPORTED_FEATURE | none |
| Pair ok | correct code within window | PairReply ok; client stored in paired list; session becomes paired | none |
| Pair wrong x5 | five wrong codes | window closed, further PairRequest refused until reopened | typed error |
| Pair window expiry | >2 min | refused | typed error |
| Dev bypass | --dev-bypass-pairing | Hello pairs immediately, model "DEV HOST" | loud warning |
| Forward | paired client sends GetState/Subscribe | forwarded to the engine UDS, State relayed back with the original request_id | engine down → typed UPSTREAM_UNAVAILABLE |
| Push relay | engine pushes State to a subscribed client | relayed to the TCP client | slow client: drop oldest / disconnect with cap |
| Oversize / malformed frame | >65536 or bad proto | connection closed, counted | none |
| Persistence | restart sysd | paired computers remembered (no re-pair needed) | corrupt file → empty list + log |
| Studio client | connect/pair/subscribe through the bridge to a real engine-host --no-audio | status events and State events delivered; reconnect after the bridge restarts | none |
| Offline | no device | app shows the existing offline UI | none |

## Code Map

- `sysd/` (config_server.cpp framing/poll loop, config_service.cpp, main.cpp, README), `proto/ws_v1.proto` (Hello/HelloAck, ErrorCode, ClientMessage/ServerMessage numbering), `engine/host/src/control_server.cpp` (UDS framing, Hello, subscribers), `librarian/src/library_service.cpp` + `proto/library_api.proto` (separate UDS API; forward opaquely by a per-feature route table).
- `desktop/src-tauri/src/main.rs`, `Cargo.toml`, `build.rs` (prost-build of proto/), `desktop/src/app.js` (connection indicator, offline UI, isConnected), `desktop/src/index.html`.
- `tools/sysdctl/*` (blocking client library pattern), `tools/diag/` (UDS client), `engine/host/macos/engine_host_sysd_smoke.cpp` (smoke pattern with real processes).
- Tests: `sysd/tests/*`, a new `sysd_bridge_tests`, `desktop/src-tauri` cargo tests (client against an in-process fake bridge), an end-to-end smoke (engine-host --no-audio + velora-sysd bridge + a small test client or the Rust client binary/test).

## Tasks & Acceptance

**Execution:**
- protos (additive): hello/device info/features, PairRequest/PairReply, NOT_PAIRED, UNSUPPORTED_FEATURE, UPSTREAM_UNAVAILABLE; README
- sysd bridge: TCP listener, framing, handshake/feature negotiation, allow-list table, pairing (window, code, limits, persistence), forwarding to engine/librarian UDS with request_id mapping and push relay, config handled in-process, limits and timeouts, flags (--bridge-port, --bind, --engine-socket, --librarian-socket, --pairing-code, --dev-bypass-pairing)
- Studio Rust client + Tauri commands/events; JS wiring (status indicator, state store, prompt-based pairing placeholder); offline fallback intact
- tests: every matrix row (bridge with fake upstream UDS servers and with the real engine-host), Studio client against the real bridge, reconnect, version mismatch status; cargo test in desktop/src-tauri
- docs: sysd/README.md (bridge design, port, flags, what is board work), desktop README note, proto/README.md

**Acceptance Criteria:**
- Given a TCP connection to the bridge, when the client sends Hello, then it gets device info and the negotiated feature set; unpaired clients can only Hello/pair; others get NOT_PAIRED.
- Given the pairing code, then a correct code pairs and persists; five wrong codes close the window; the window expires after 2 minutes.
- Given a paired session, then commands are forwarded to the engine/librarian and state is relayed back with request_id preserved.
- Given Velora Studio, then it connects through the bridge to a real engine-host, shows the real connection status and receives State events; the offline mode still works without a device.
- nftables single-port policy, NCM addressing and the real device-side pairing UI are recorded as deferred (board / Story 9.6).

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Security review pass (single focused reviewer)
- findings:
  - `[high]` `patch` PAIRING BYPASS: a Hello with a known client_id was paired without the code — added a 256-bit pairing token (hash persisted, constant-time compare, window closes on success, file 0600/dir 0700), no std::rand fallback, ids not logged
  - `[high]` `patch` default-allow routing of every ClientMessage and opaque library pass-through (path-taking and internal report messages reachable by a paired computer) — total message table with a descriptor-walking test, explicit library allow-list; hardware/panel commands denied
  - `[medium]` `patch` blocking upstream connect stalling all sessions, unpaired slot exhaustion, unbounded upstream queues, post/stop race, reconnect storm, main-thread blocking in Tauri commands, no CSP, window.prompt unavailable in WKWebView
  - `[medium]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "sysd|bridge|control|smoke|desktop" --output-on-failure` -- expected: all pass
- `cd desktop/src-tauri && cargo test` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: velora-sysd TCP bridge (single port, Hello/features, token-based pairing with persisted hashed tokens, total message table, explicit library allow-list, request_id mapping, push relay, limits), ws.v1 additive messages (PairRequest/Reply, LibraryRequest/Reply, ErrorCode 42-46, Hello/HelloAck fields), Studio Rust device client (reconnect, keepalive, tokens per device_id, Tauri commands/events, strict CSP), JS status/pairing wiring, offline simulation kept.
Verification: sysd/bridge/control/smoke tests pass (smokes pass alone and repeated; two were load-sensitive in a full parallel run), cargo test 20/20, full ctest otherwise only the 4 pre-existing failures plus a load-sensitive librarian_crash_campaign; core deps, colour literal and private key lints pass.

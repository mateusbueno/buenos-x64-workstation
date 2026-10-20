---
title: 'Story 4.11: Live Set favorites and Recent in the catalog'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '621193779207f2b2fe879bf98fd444985e9c3ab7'
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

**Problem and approach:** Story 4.11 "Live Set favorites and Recent in the catalog" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2798; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** create the minimal `librarian` **service** (Structural Seed: `librarian/` is the sandboxed import/convert/index/catalog service; today `librarian/` holds the import library and CLI — add a service executable `librarian-service` (C++20, host process on the Mac; a systemd unit file `librarian/systemd/velora-librarian.service` for the board is delivered as a documented file only, no board testing) that serves a **`ws.v1` Library API** over a **Unix domain socket** (reuse the framing, handshake (version+features), closed error-code style of the engine's ControlServer in `engine/host/src/control_server.*` — read `proto/ws_v1.proto` and `proto/README.md`: define the Library API as an additive set of messages in a new `proto/library_api.proto` (package `ws.v1` per AD conventions: "Protobuf package ws.v1, messages PascalCase, fields snake_case"; if mixing files in the same package is awkward, put the Library API in its own file within package `ws.v1` with distinct message names `LibraryClientMessage`/`LibraryServerMessage` and the same `ProtocolVersion`/`Hello`/`HelloAck`/`Error` conventions) containing now: handshake (Hello/HelloAck with version + features), `AddLiveSetFavorite{live_set_id}`, `RemoveLiveSetFavorite{live_set_id}`, `GetFavoritesAndRecent`, `ReportLiveSetActive{live_set_id}` (the engine/ui reports a Live Set became active; the service records it in Recent), `LiveSetDeleted{live_set_id}` (reconcile trigger) and server pushes `FavoritesChanged`/`RecentChanged`/`State{favorites, recent, catalog_schema_version}` to subscribers ("the favorite set changes and is published to `ui`": `Subscribe{enable}`); the service opens a **SQLite WAL catalog** (`<data>/catalog.db`, `PRAGMA journal_mode=WAL`, `PRAGMA user_version`/a `meta` table holding `schema_version` = 1) with **only** the tables this story needs: `live_set_favorites(live_set_id TEXT PRIMARY KEY, added_seq INTEGER)` and `live_set_recent(live_set_id TEXT PRIMARY KEY, last_played_seq INTEGER)` (use a monotonically increasing sequence number from a persisted counter in `meta`, not wall-clock time, since the unit has no RTC); **only `librarian` writes the catalog** (single-writer: flock on `<data>/catalog.lock`, a second service instance is refused; document AD-9); SQLite is a build dependency: system `libsqlite3` on macOS (Homebrew/system) and `libsqlite3-dev` in the Docker image (update `tools/ci/docker/Dockerfile`; if SQLite headers are unavailable, FATAL_ERROR with a clear message); record SQLite in `THIRD_PARTY_NOTICES.md` + `tools/ci/third_party_manifest.json` as a build/runtime dependency (public domain: check how the notices script treats an entry, follow the established pattern from protobuf, keep `check_third_party_notices.py` passing); wrap SQLite in a small RAII C++ class (`Catalog`) with typed `Status` results (no exceptions across API), prepared statements, busy timeout, integrity on open (`PRAGMA integrity_check` quick on schema_version mismatch only), schema migration policy: an unknown newer `schema_version` is refused with a typed error, an older one is migrated forward (stub for v1 only). **Recent semantics:** "a Live Set becomes active during a performance" — `ReportLiveSetActive` updates the **in-RAM** recent list immediately (most recent first, deduplicated, capped at 20) and the service writes it to SQLite only at an **idle-safe moment** (no write during playing): define "idle-safe" by an explicit API: the service has an `idle` flag fed by the engine/ui via `SetPlaying{playing}` (additive message) — writes of Recent are deferred while `playing == true` and flushed when playing becomes false or after a configurable safe-flush call (`FlushRecent`) and at orderly shutdown; **favorites** are explicit user commands (written immediately, fsync via SQLite WAL commit; document). Recent lists the **last 20 Live Sets, most recent first**. **Deletion reconcile:** `LiveSetDeleted{id}` (and a `Reconcile{existing_live_set_ids[]}` message that takes the list of existing Live Set ids and removes favorite/recent rows whose Live Set no longer exists) removes the rows and publishes the change. Integration: the engine-side wiring is out of scope except two thin hooks: (a) a small C++ `LibraryClient` in `engine/host` (portable, UDS client) that can `report_active(id)` / `set_playing(bool)` asynchronously with reconnect/backoff and never blocks the control thread (a bounded queue with drop-oldest and a counter), (b) `LiveSetSwitcher`/`EditSession` call sites left untouched except optionally wiring the client when `--library-socket PATH` is given to engine-host (default off; keep all existing smoke tests unchanged). **Tests:** catalog unit tests (schema creation, version policy, favorites add/remove idempotent, ordering by added_seq, recent ordering/dedupe/cap 20, persistence across reopen, single-writer lock, corrupt/foreign db file → typed error, delete cascade), service tests over a real UDS client (handshake, version mismatch, every message, push to subscribers, concurrent clients serialised, malformed frames, oversize frames), the "no write while playing" test using an injected `Catalog` write counter (SQLite file mtime/size unchanged and counter 0 across a simulated hour of Live Set switches while `playing=true`, then exactly one flush after `playing=false`), `LibraryClient` tests (reconnect, drop-oldest, never blocks), crash-injection: kill -9 the service process mid-flush (fork + _exit hook) and reopen: catalog consistent (WAL recovery), plus a smoke test starting the real `librarian-service` binary on a temp data dir and socket (like the engine-host smoke tests; no audio needed).

**Never:** implement the library/instrument catalog tables (5.6), import API, preflight, sandboxing (5.9), UI/Setlist Browser (4.30), search, tags (those live in the setlist store from 4.6), or any engine changes beyond the optional client wiring.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.11 (~line 2798) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.11 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` a refused catalog file was modified before being refused (WAL pragma ran first, `-wal/-shm` created) — read-only inspection first (immutable open; private copy when a non-empty `-wal` exists); foreign files, files with only views/triggers, `meta` without a valid version and newer schemas are refused byte-identical; schema created only when `sqlite_master` is empty; a missing `next_seq` row is rebuilt
- `[high]` `patch` an empty or truncated `Reconcile` list wiped all favorites and Recent; long lists exceeded the frame — empty refused unless `allow_empty`, chunked reconcile (`more`, applied by the final chunk, ≤100000 ids), full 61-row Recent reconcile; `kMaxFavorites` 4096→2048 so a full state fits one 262144-byte frame
- `[high]` `patch` `playing=true` could stick forever when the engine or ui died and a failed delete left RAM and SQLite different — playing is a lease owned by the connection (released on close with a Recent flush, optional `playing_lease_ms`), failed cascades roll back RAM; clients that send then close have their frames applied; the LibraryClient flushes on stop, re-sends the last Live Set after a reconnect and never evicts the item being sent
- `[medium]` `patch` storage-failure, favorites-limit, slow-subscriber and client-cap paths untested; engine-host "performing" detection untested; systemd unit could not start (namespace/install permissions) and the socket mode contradicted the unit comment — tests added, `--library-playing-idle-ms` and a hidden note injector make the smoke deterministic, root-run ExecStartPre, `--socket-group` (mode 0660, default 0600)
- `[low]` `defer` the LibraryClient sends no heartbeats (the lease timeout is a server option, default off); the systemd unit and the board path are untested; the engine does not yet send `Reconcile`/`LiveSetDeleted`; engine-host's "performing = a note in the last 3 s" is a stand-in for the real playing signal; bind/stale-socket code is copied from ControlServer instead of shared; `FlushRecent` is honoured even while playing (the caller asserts safety)

## Auto Run Result

Status: done. Minimal `librarian-service` (host process; systemd unit delivered as a file) serving a `ws.v1` Library API over a Unix socket (`proto/library_api.proto`: handshake, favorites add/remove, recent reports with idle-safe writes, `SetPlaying`, `FlushRecent`, `LiveSetDeleted`, chunked `Reconcile`, subscriptions and pushes), with a SQLite WAL catalog (`catalog.db`, `schema_version` 1, only `live_set_favorites` and `live_set_recent`, sequence counter instead of wall-clock, single writer by flock, foreign/newer files refused untouched), Recent kept in RAM (last 20, most recent first) and written only when not playing; `LibraryClient` in `engine/host` (bounded drop-oldest queue, reconnect, never blocks) wired by `engine-host --library-socket`. Verification: macOS ctest 494 (2 env-gated skips), Docker gcc 487 and clang-20 RTSan 502, purity and notices checks pass. Follow-up review recommended: false.

---
title: 'Story 8.5: Format versions N/N-1 and migrations only after the slot is marked good'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: 'dbfc4e7df21cb7e0d074b156150d37573d528f8b'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md']
warnings: []
deferred:
  - summary: >-
      migration_runner has no production caller and no real SlotProbe (RAUC mark-good) or concrete Unit: wiring into librarian startup / control thread is dev-board work.
    evidence: |-
      Runner, probe interface and synthetic unit are tested; see format-versions-audit-8-5.md.
    severity: medium
  - summary: >-
      Bundle and catalog readers were not changed (catalog migrates older schemas forward; librarian protocol handshake still exact match; save_to_file of SystemConfig is not an fsync'd atomic write); AC4 wording about an older release reading migrated data is ambiguous and is covered only through a newer reader at N-1.
    evidence: |-
      Deviations documented in the audit; tests/compat/releases is empty until a tagged release adds files.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Every persisted form must open on the next firmware (version N and N-1), reject older or unknown-major data with a typed error, round-trip unknown fields, and any irreversible migration must wait until the A/B slot is marked good. Today N/N-1 is proven only per format (Live Set, setlist files), there is no compatibility corpus gate across forms, and no migration runner (FR49, FR69, AR29, NFR44, AD-12).

**Approach:** (1) Audit every persisted form present in this repo (bundle `format_version`, Live Set and setlist files, catalog schema, ws.v1 handshake version, MCU protocol version, ui/sysd-style config `schema_version` where one exists) and close each gap: reader accepts N and N-1, rejects older/unknown major with a typed error, round-trips unknown fields on read-modify-write, with one unit test per format. (2) Make a top-level compatibility corpus `tests/compat/` (Live Sets, setlists, config files; reuse/relocate the existing corpora without losing their goldens) with a CTest gate that every file opens on the current build, plus a script the release job uses to add a release's files. (3) Add a migration runner in the control-thread/librarian layer: data is read through the N-1 reader and kept read-only until a "slot is good" probe says so (injected; the RAUC hook is dev-board work), then a migration unit writes the migrated form atomically (temp, fsync, rename, dir fsync) and always writes N, never N+1; a rollback before mark-good leaves data byte-identical.

## Boundaries & Constraints

**Always:** Accept exactly N and N-1 (min_readable = N-1, as `setlist_store.hpp` `min_readable_version` already does); typed errors for older / unknown major; unknown fields are preserved byte-for-byte semantically on write-back. Reuse existing format code (live_set_store, setlist_store, formatlib bundle reader, librarian catalog, ws.v1 `PROTOCOL_VERSION_1`, MCU `kProtocolVersion`); tests set N=2 to exercise N-1 (as existing tests do). The migration runner is generic with an injected probe and a registry of migration units; ship one synthetic test migration only (no real irreversible migration exists yet). Migration writes are atomic. The corpus gate must fail the build if any file fails to open.

**Never:** No real RAUC integration, no sysd daemon (dev-board work: provide the probe interface and document it), no new on-disk format changes to shipped data, no deletion of existing corpus files or goldens, no inventing config schemas that do not exist in the repo (list them as deferred in the audit instead).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| N and N-1 | file at version N, and at N-1 | opens; N-1 flagged `migrated`/legacy where applicable | none |
| Too old | version < N-1 | typed error (e.g. kTooOld) | no partial load |
| Unknown major | version > N or unknown major | typed error (e.g. kTooNew/kUnknownMajor) | no partial load |
| Unknown field | field written by a newer minor | survives read-modify-write | none |
| Corpus | every file in `tests/compat/` | opens on current build; gate fails otherwise | CTest failure names the file |
| Before mark-good | new release boots on N-1 data, probe says not good | reads via N-1 reader, writes nothing | none |
| After mark-good | probe says good | migration unit runs, atomic write of N | failure keeps old file intact |
| Rollback | probe never good, previous firmware reads data | data byte-identical to before | none |
| Old release on migrated data | N-1 reader opens migrated (written as N) data | opens | none |
| Kill mid-migration | process killed between write steps | old or new file, never corrupt | none |

## Code Map

- `engine/host/include/velora/host/live_set_store.hpp`, `setlist_store.hpp` (`min_readable_version`, `parse_library`, unknown-field handling), `engine/host/tests/corpus/{live_sets,setlists}`, `live_set_corpus_gen.cpp`, `setlist_corpus_gen.cpp`, goldens -- existing N/N-1 + corpus patterns (Stories 4.2, 4.6).
- `formatlib/include/`, `formatlib/src` bundle reader (`format_version`), `formatlib/tests/bundle_test.cpp`.
- `librarian/src/catalog.cpp`, `librarian/tests/catalog_test.cpp` -- catalog schema version (`schema_version()`).
- `proto/ws_v1.proto` (`PROTOCOL_VERSION_1`, Hello), `engine/host/src/control_plane.cpp`; `mcu/common/include/mcu/mcu_frame.h`, `mcu_frame.hpp` (`kProtocolVersion`).
- `ui/src/system_config.cpp` (config JSON, check whether it carries a schema_version).
- Top-level `CMakeLists.txt`, `tools/` for the corpus script and CTest gate; new `tests/compat/` dir and migration runner (`engine/host/include/velora/host/migration_runner.hpp` + src, or a shared location both control thread and librarian can use) with tests.

## Tasks & Acceptance

**Execution:**
- audit -- write `_bmad-output/implementation-artifacts/format-versions-audit-8-5.md` listing each persisted form, its current version constant, reader policy, unknown-field behaviour, typed errors and test, and gaps closed here vs deferred (e.g. no sysd config exists)
- per-format readers/tests -- close gaps found (bundle, catalog, ws.v1 handshake, MCU protocol, config where a schema exists): N/N-1 accepted, older/unknown-major typed error, unknown-field round-trip, one unit test each
- `tests/compat/` + CMake gate + `tools/` add-release script -- consolidated corpus of Live Sets, setlists and config files opened by one gate test; document how a release adds its files
- migration runner + tests -- injected mark-good probe, read-only before good, atomic migrate after, writes N never N+1, rollback leaves data byte-identical, kill-mid-write never corrupt, N-1 reader opens migrated data

**Acceptance Criteria:**
- Given every persisted form, then readers accept N and N-1, reject older/unknown major with a typed error, and an unknown field survives read-modify-write (one test per format).
- Given `tests/compat/`, then CI opens every file on the current build and a failure to open any file fails the gate.
- Given a release needing a migration, then it reads read-only until the slot is good, migrates atomically after, a rollback before mark-good leaves data unchanged, and the older release still reads the migrated data.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~45 findings — patches 8 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` migration runner: stale temp, post-rename status contract, lost update, throwing callbacks, validate hook, overflow-safe unit validation — fixed + tests
  - `[medium]` `patch` untested ws.v1 refusal contract; add_release.py not atomic / missing-gate traceback; gates (dotfiles, case, errors, ui gate non-vacuity self-test); SystemConfig npos guard, known-key test, verbatim wording; README/CMake path fixes
  - `[medium]` `defer` no production caller/probe; bundle/catalog/librarian-handshake deviations
  - `[low]` `reject` HelloAck echo of N-1 without a dialect, duplicate unknown key, large-integer precision, load() reset-to-default

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "compat|migration|live_set|setlist|catalog|bundle|control|version" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: migration runner (injected mark-good probe, read-only before good, atomic N-never-N+1, rollback-safe), ws.v1 N/N-1 handshake with typed refusals, SystemConfig N/N-1 + unknown-field preservation, tests/compat corpus + gates + add_release.py, audit document.
Verification: targeted and full ctest pass except 4 pre-existing failures (tokens_verify, protected_keyless_test, sysex_receiver_test, library_resolver_test); librarian crash/checkpoint tests are load-sensitive and pass alone; core deps check passes.
Residual risks: see deferred.

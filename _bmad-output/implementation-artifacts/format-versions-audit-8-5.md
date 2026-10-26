# Format versions audit (Story 8.5)

Policy (FR49, AR29, NFR44, AD-12): a reader at version N accepts N and N-1 (`min_readable = max(1, N-1)`), refuses older and newer with a typed error and
no partial adoption, keeps unknown fields on read-modify-write. Irreversible migrations run only after the slot is marked good (migration runner below).
Tests set N=2 (or 3) to exercise N-1 where the shipping N is 1.

| Form | Version constant (N) | Reader policy | Unknown fields | Typed errors | Test | Status |
|------|----------------------|---------------|----------------|--------------|------|--------|
| Live Set file (`proto/live_set.proto`) | `liveset::kFormatVersion = 1` | N, N-1; `parse()` migrates N-1 in memory | protobuf unknown fields kept, deterministic write | `kUnsupportedVersion` (older and newer) | `live_set_tests`, `format_versions_test` | already met |
| Setlist library, default ASSIGN map (`proto/setlist.proto`) | `setlist::kFormatVersion = 1` | N, N-1 (`min_readable_version`) | kept (library, tags, setlists, ASSIGN) | `kUnsupportedVersion` | `setlist_tests`, `format_versions_test` | already met |
| Sample bundle `.vlb` / protected `.vlrp` | `format::kFormatVersion = 2`, minor 1, `kMinReadableVersion = 1` | N, N-1; minor above `kFormatMinor` refused | read-only form (the librarian never rewrites a bundle); unknown minor refused rather than half read | `Error::kUnsupportedVersion` | `formatlib/tests/bundle_test.cpp` (older, far, major 3, minor above) | met; unknown-field round trip not applicable (never rewritten) |
| Library catalog (SQLite, `librarian/src/catalog.cpp`) | `kCatalogSchemaVersion` | newer refused untouched; older migrated forward step by step in one transaction per step (a rebuildable index, so it keeps its own forward chain rather than the N-1 rule) | table columns survive: migrations only add | `CatalogStatus::kSchemaTooNew`, `kFaultInjected` | `librarian/tests/catalog_test.cpp` (too new, migrate, fault in migration) | met, deviation documented: older schemas migrate, not refused |
| ws.v1 handshake (`PROTOCOL_VERSION_1`, `ControlPlane::kProtocolVersion = 1`) | 1 | **gap closed**: `check_protocol_version()` accepts N and N-1 (was `== N`); the ack answers with the version the client asked for | protobuf unknown fields kept and ignored | `ERROR_CODE_UNSUPPORTED_VERSION`, details `supported`, `supported_min`, `requested`, `reason = too_old | too_new` | `format_versions_test` (N=1,2,3, 0 and 0xFFFFFFFF, unknown fields) | closed here |
| Library protocol handshake (`LIBRARY_PROTOCOL_VERSION_1`) | 1 | exact match today | protobuf | handshake error | `librarian/tests/library_service_test.cpp` | **deferred**: a second protocol version does not exist yet; align to N/N-1 when it does |
| MCU frame protocol (`MCU_PROTOCOL_VERSION`, major.minor nibbles) | `0x10` | N, N-1 major accepted, major 0 and N+1 rejected (`mcu_version_accepted`) | frames carry no free-form fields; unknown types are counted and dropped | `MCU_EV_VERSION_REJECTED` / `Event::kVersionRejected` | `mcu/common/tests/mcu_frame_test.cpp`, `mcu_wrapper_test`, `format_versions_test` (host decoder at 2.1) | already met |
| sysd / ui config (`ui/src/system_config.cpp`, `schema_version`) | **gap closed**: `kConfigSchemaVersion = 1` | **gap closed**: was no check at all; now N, N-1 load (N-1 reported `migrated`, held as N), refused files change nothing | **gap closed**: unknown top-level members were dropped on save; now kept verbatim | `ConfigLoadStatus::TooOld / TooNew / ParseError` | `ui/tests/config_version_test.cpp`, `compat_config_test` | closed here |
| Boot-into pointer (`boot_into_store`) | n/a, a fixed tiny record | not versioned | n/a | n/a | `boot_into_tests` | out of scope (no schema) |
| sysd daemon config (`proto/sysd_config.proto`, `sysd/`) | `sysd::kSchemaVersion = 1` | N, N-1 (`sysd::min_readable_version`); N-1 loaded read-only, rewritten as N only by `ConfigStore::migrate()` through the migration runner after the slot probe | protobuf unknown fields kept (file, Config, pedals) | `LoadReason::kTooOld / kTooNew / kCorrupt / kInvalidContent` (defaults + logged reason) | `sysd_config_tests` (N = 1 and N = 2/3) | added by the sysd config foundation |
| RAUC mark-good | does not exist in this repo | | | | | **deferred** (dev-board work): the probe interface is `migration::SlotProbe`; sysd takes it as `StoreOptions::slot_good` |

## Gaps closed in this story

1. ws.v1 handshake accepted only an exact version; it now accepts N and N-1 and refuses older / newer with `reason`.
2. sysd config had a `schema_version` it never checked and dropped unknown members on save; both fixed (the on-disk shape is unchanged for known members; an
   absent `schema_version` is read as N, as before).
3. No compatibility corpus gate across forms: `tests/compat/` (Live Sets, setlists, ASSIGN maps, config files; the Story 4.2 / 4.6 corpora moved there with git
   history and their goldens in `engine/host/tests/golden/` untouched), `compat_corpus_test` + `compat_config_test`, `compat_gate_selftest`,
   `tools/compat/add_release.py` for a release job.
4. No migration runner: `engine/host/include/velora/host/migration_runner.hpp`.

## Migration runner contract

- Units migrate one file from N-1 to exactly the running N (any other target is `kBadUnit`: N+1 is never written).
- Before the slot is good: reads only. No file, temp file, rename or removal; outcome `kDeferred`. A rollback to the previous firmware finds the bytes identical.
- After `SlotProbe` says good: transform, verify the output is exactly version N, then the data root's atomic replace (temp, fsync, rename, directory fsync).
  Failure (refusal, wrong version, ENOSPC, kill at any stage) leaves the old file; a kill after the rename leaves the new one; the next run finishes or is a no-op.
- The probe is asked at most once per run and only when a file needs it. An absent probe means "never good".
- Data written as N opens on this release and, as its N-1, on the next one.
- **Dev-board wiring (not done here)**: the librarian at startup (after reconcile) and the control thread at an idle-safe moment (never while playing) call
  `migration::run()` with a probe backed by the init system's mark-good state (RAUC slot status `good` after the update client commits), once per boot and again on the
  mark-good event; the real unit registry is empty until a format needs an irreversible change. Only a synthetic unit (Live Set 1 -> 2) exists, in the tests.

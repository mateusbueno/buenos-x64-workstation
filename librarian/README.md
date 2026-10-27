# librarian

POSIX only (uses `flock`, `fsync`, `rename`, symlinks; its tests also use `fork`): macOS and Linux, not Windows.

Role: imports an SFZ library or a SoundFont 2 file into an immutable formatlib bundle and publishes it atomically under
`<root>/lib/<library_id>/` (Story 1.13, SF2 in Story 5.3). Runs on the Mac and on the board; since Story 5.6 the `librarian-service` runs these imports behind the
Library API and keeps the libraries catalog (below); the import UX and categories UI are later stories. There is no encryption anywhere in the librarian (user
imports are never encrypted; Epic 6 protects bundles elsewhere). Story 6.4 adds the verified install of protected vendor bundles ("Installing protected bundles" below): the librarian
checks signatures and hashes and still runs no cipher. Target `velora_librarian`, CLI `librarian`.

```
build/librarian/librarian import <file.sfz | file.sf2 | dir>... --root <data root> [--base DIR] [--category NAME] [--report FILE|-]
```

Exit code 0 = imported (or identical bundle already current), 1 = nothing published for at least one file (reasons on
stderr, one line each, `file:line: ...`), 2 = usage. `--base` is the directory the `instrument_key` is relative to (default: the SFZ's own
directory, so `Piano.sfz` gets the key `Piano.sfz`). `--category` takes a category key (`category.piano`), its slug (`piano`) or an English name (`E.Piano`, `Mallet & Bells`, `FX/Amb`), case-insensitive,
and rejects `none` and unknown names (exit 1, nothing written); it is stored as `<bundle_dir>/category`, written inside the staging directory so it is part of the atomic publish
(an identical re-import refreshes it before the flip). Since Story 5.5 the file holds the **stable key** (`category.piano\n`) whatever spelling was passed; bundles written
before it hold free text such as `Piano\n`, which `parse_category_file` (formatlib) still reads, and a bundle without a category file stays byte-identical (the manifest is
not touched, so no format bump). `librarian suggest-category [--table FILE] <name>...` prints `<name><TAB><key|none>` per argument. `--root` must be an existing directory
(a mistyped root is an error, never created); the file argument must come first. **While a `librarian-service` owns the data root the CLI refuses to import**
(exit 1, "the librarian service is running on <root>...": the service holds `<root>/lib/.service.lock` exclusively). The CLI creates that file if needed and holds it shared for its whole run, so a
service that starts during a CLI import is refused too (its reconcile would otherwise delete `.staging` under the running import). With no service the CLI works standalone as before (it leaves the empty
`lib/.service.lock` next to `.staging`), and the next service start lists what it published (see "Startup reconcile").

Identity: `library_id` = UUIDv5 of the SFZ's canonical absolute path (formatlib `set_source_id`), so changing a sample
keeps the `library_id` and yields a new `bundle_id`; moving the SFZ makes a new library. `bundle_id` is the manifest
hash. `instrument_key` is the normalized `.sfz` path relative to the base directory.

Batch import and the import report (Story 5.1): several files and/or directories (searched recursively for `*.sfz` and `*.sf2`,
extension case-insensitive, sorted, duplicates dropped) are a batch. One SFZ is still one library with its own bundle and
`library_id`; a file that is skipped or fails never stops the others (`import_batch`, `velora/librarian/batch.hpp`). With one
plain file the CLI prints the Story 1.13 text; for a batch it prints one line per file and a summary line. `--report FILE`
writes the machine-readable report, `--report -` writes it to stdout instead of the text output; an unwritable report file
is exit code 1. The report is JSON Lines (UTF-8, stable key order, no trailing spaces): one `file` object per file in
processing order, then one `summary` object:

```
{"type":"file","file":S,"status":"imported|unchanged|skipped|failed","reason":"none|unsupported_features|damaged",
 "failing_path":S,"offenders":[{"opcode":S,"file":S,"line":N,"message":S}],
 "allowlisted_count":N,"allowlisted":[{"opcode":S,"file":S,"line":N}],
 "ignored_values_count":N,"ignored_values":[{"opcode":S,"file":S,"line":N}],
 "messages":[S],"library_id":S,"bundle_id":S,"samples":N,"regions":N,"detail":S}
{"type":"summary","files":N,"imported":N,"unchanged":N,"skipped":N,"failed":N}
```

Reason codes are a closed set. `skipped` always carries one of the two codes below and nothing of that file is written (no
library directory, no staging leftovers, an earlier `current` stays); `failed` (reason `none`, `detail` = the status text)
is an infrastructure problem (I/O error, busy lock, failed verification, bad arguments).

- `unsupported_features`: the SFZ uses something outside the subset: scripting (`script`), effects (`<effect>`, `effect1`),
  `lfo*`, `eg*`, `*_oncc*`, any other opcode or header that is not in the table in `formatlib/README.md`, a value outside
  the subset (`trigger=legato`, `fil_type=lpf_4p`, a fifth xfin controller...), or a sample in an audio format the importer
  does not have (8-bit or float64 WAV, a rate outside 44.1/48/88.2/96 kHz, a non-WAV container such as `.flac`; these
  appear in `offenders` with opcode `sample`). `offenders` lists every offender with `file:line` (also in `messages`).
- `damaged`: the SFZ cannot be parsed or opened, a value is malformed or out of range, a region is inconsistent (inverted
  ranges, loop or offset beyond the sample), a sample file is missing, unreadable, not a valid WAV or truncated (a WAV
  whose data chunk is shorter than its header says is damaged, never imported shorter). `failing_path` is the missing or
  unreadable file (or the SFZ). If a file has both kinds of problems it is `damaged` (the list of unsupported constructs
  may then be incomplete; `offenders` still lists those found).

Non-sounding opcodes on the allow-list (`label_cc<N>`, `label_key<N>`, `*_label`, `image`, `hint_*`; the table is in
`formatlib/README.md`) never skip a file: they are accepted, ignored, logged in `allowlisted` and counted in
`allowlisted_count`.

Story 13.2: an unparseable or out-of-range `lokey`/`hikey`/`key` value (`lokey=-1`, `lokey=c-2`, `hikey=128`) falls back to
the opcode default, as in sfizz, and is logged in `ignored_values`; the file still imports. `hikey=-1` marks a region that
no note triggers; it is not written to the bundle (logged in `ignored_values`) until Story 13.7 adds controller-triggered
voices. `<curve>` blocks are parsed and stored inert.

Supported SFZ subset: see `formatlib/README.md` (authoritative opcode table: supported, allow-listed, everything else
unsupported). It now includes keyswitches, CC crossfades, loops and basic filters (Story 5.1). A bundle that uses none of the
new features is byte-identical to what Story 1.13 produced; the new features set format 2.1 (see `formatlib/README.md`).
Any opcode, header or value outside the subset skips the whole file with a per-file reason listing every offender (e.g.
`Piano.sfz:12: unsupported opcode 'amp_veltrack'`); nothing is published and the previous `current` stays. The engine does
not play the new features yet (Story 5.2): until then an instrument with keyswitches sounds all its regions. The importer never resamples or requantizes: samples must be WAV, 16/24-bit PCM or 32-bit
float, mono/stereo, 44.1/48/88.2/96 kHz, otherwise the file is skipped with the sample path as the reason. SFZ loops: a missing `loop_start` is 0, a missing `loop_end` the last frame, `loop_mode=loop_continuous|loop_sustain` without points loops the whole sample; `loop_start > loop_end` is an error. Sample paths must stay inside the SFZ's directory. Missing
samples, missing/recursive `#include` and region values beyond a sample's length are reasons too.

Publish protocol (crash safe): flock `<root>/lib/.staging/.lock`, reconcile leftovers, build the bundle in
`<root>/lib/.staging/<unique>/` (must be on the destination's filesystem, else refused), verify (reopen, `verify_all`,
region table equal to the parsed one, every sample bit-identical to a fresh decode of its source WAV), fsync file and
directory, rename the directory to `<library_id>/<bundle_id>`, fsync, flip `current` via symlink-then-rename, fsync. Killed at
any point, `current` is the previous or the new complete bundle, never a partial one; the next run removes staging
leftovers and, in the directory of the library being imported only, stale `.current.tmp.*` links. Old bundle directories are kept (engine leases), identical re-imports add
nothing. Fault injection (`FaultInjection::on_stage`, `corrupt_staged_before_verify`, `pretend_cross_device`) is used by the
tests; only the test build of the CLI (`librarian_test_cli`, `-DVELORA_LIBRARIAN_TEST_HOOKS`) honours `VELORA_LIBRARIAN_FAULT=<staged|verified|fsynced|renamed|link-created|flipped>` (`_exit(137)`); the shipped `librarian` ignores it. `link-created` fires only when a link was actually created.

Tests: `librarian_opcode_test` (Story 5.1: one fixture per opcode, parsed region table compared field by field with the
bundle's after the round trip, Epic 1 version word for bundles without extensions, every unsupported category, the
allow-list, each damaged cause including a truncated and an unreadable WAV, nothing written for skipped files, mixed batch,
JSON report and CLI); `librarian_test` (every I/O-matrix row with synthetic SFZ and generated WAVs: field-by-field region comparison,
resolver opens the result, real `fork`+`_exit` kill at every stage with and without a previous bundle, recovery, corrupt
staged sample, unsupported audio/opcodes, include cycles, cross-device, lock, CLI).

## SoundFont 2 import (Story 5.3)

`.sf2` files go through the same batch, publish protocol and report as SFZ files (`import_sf2`; `import_file` picks the importer by extension).
**One SF2 file is one library with one instrument per preset**: `library_id` = UUIDv5 of the file's canonical absolute path (the rule of the SFZ
importer, so re-importing a changed file keeps the library and makes a new bundle), `instrument_key` = `bank:preset` in decimal (`0:0`, `128:0` for
percussion; a repeated bank:preset is damage), the instrument name is the preset name. `--base` does not apply (keys do not come from paths). The mapping,
the samples (kept at their source rate and bit depth, 16-bit or 24-bit with `sm24`, bit identical after the bundle round trip) and the ignored features
are described in `formatlib/README.md` and in the header of `formatlib/include/velora/format/sf2_import.hpp`. The offline renderer plays a preset with
`velora-offline-render --engine sample --bundle <bundle.vlb> --instrument 0:0`.

- **Skipped files** (nothing is written, the batch goes on): reason `damaged` for bad RIFF/chunk sizes, tables whose sizes or indexes do not fit,
  samples or loops outside `sdta`, a duplicate bank:preset, a file without any region, an unreadable file (`failing_path` is the file, the detail is in
  `messages`); reason `unsupported_features` for a SoundFont version other than 2, a ROM sample or a sample rate outside 44.1/48/88.2/96 kHz
  (`offenders` lists `{"opcode":"ifil"|"sample", ...}`).
- **Features outside the model** (non-default modulators, LFOs, the modulation envelope, effects sends, `scaleTuning`, `exclusiveClass`, sample end
  offsets, the default velocity -> filter cutoff modulator of a filtered preset, ...) never skip a preset: they are ignored and each is listed per preset as a **warning** with the code `ignored_sf2_feature`: `{"code":"ignored_sf2_feature",
  "preset":"0:7","opcode":"modLfoToPitch","file":S,"line":0,"message":S}` (for a modulator `opcode` holds `modulator src=0x0093 dest=modLfoToPitch
  amount=100 amtsrc=0x0000 trans=0`). The file line of an SF2 ends with `"format":"sf2","presets":N,"ignored_generators":N,"ignored_modulators":N` (the
  file-level counters of those warnings; `warning_count` counts them all). Two more codes use the same shape: `ignored_zone` (a zone whose start offset or loop lies outside its sample, or that the model cannot hold, is dropped; the rest imports) and `ignored_empty_preset` (a preset without a sounding region is not published as an instrument). The rule is documented in the importer header and covered by
  `librarian_sf2_test` and `format_sf2_test`.

Test `librarian_sf2_test` (`tests/sf2_import_test.cpp`): the General-MIDI-style fixture (16- and 24-bit) through `import_sf2` and the published bundle (keys,
regions, every sample bit for bit, library identity, re-import, a changed file, extension dispatch), a single-instrument file, damaged and unsupported
files (nothing written), the warning report (JSON Lines and `ImportResult`), a mixed batch that continues past damaged and unsupported files, the CLI,
verification failure, abort/recovery, category and lock. The render comparison against FluidSynth is `tools/offline-render/tests/sf2_suite/README.md`.

Real-library acceptance (manual, performed by the founder): set `VELORA_SFZ_DEMO_DIR` to a directory containing the
Salamander Grand Piano and/or a VSCO2 instrument and run

```
VELORA_SFZ_DEMO_DIR=~/libs ctest --test-dir build -R librarian_demo_acceptance --output-on-failure
```

It imports every `*.sfz` below it into a temporary root and checks the bundle's region count equals the `<region>` count of
the SFZ sources (parser count and an independent text scan). Without the variable the test is reported as skipped
(exit code 77). A library using opcodes outside the subset (for example `amp_veltrack`) is reported as a failure with the
offending file:line, which is the intended behaviour of the subset.

## Category suggestion (Story 5.5)

`suggest_category(SuggestInput{file_name, folder_names[], metadata_strings[]}) -> optional<Category>` (`velora/librarian/category_suggest.hpp`) suggests ONE of the 17
categories (`formatlib/README.md`, "Categories and locale catalog") or none. It is a suggestion only: `ImportResult::suggested_category` (and `"suggested_category"` in each
report file line, a category key or `"none"`) carries it, the import never stores it and still needs the caller's explicit category (Story 5.7 preflight and the Import
surface use it; `import_file` fills it for SFZ from the file name, the three nearest folders and the file's `label_*` values and `//` comments, for SF2 from the preset names; a skipped or damaged file still carries the suggestion from its name and folders). Paths with a `/` are made absolute and normalised first and volume or account names (`/Volumes/<vol>`, `/media/<user>/<vol>`, `/Users/<user>`) are not folders. The SFZ scan reads a regular file only (a FIFO or device gives nothing), at most 1 MiB and 4096 bytes per line; label values (64) and comments (32) have separate caps, `//` starts a comment only at a line start or after whitespace (never inside `sample=dir//a.wav`) and `/* */` blocks are skipped. If the embedded table failed to parse (`KeywordTable::embedded_error()`), nothing is ever suggested, a message goes to stderr (assert in debug) and `suggest-category` exits 3.

Rules, in this order:

1. **Tokenise** each text: UTF-8, accents folded (PT-BR and ES diacritics, NFD combining marks dropped), lower-cased, split on non-letters, underscores, camelCase and acronym
   boundaries (`EPiano` -> `e piano`, `XMLFile` -> `xml file`); digits are dropped (`Rhodes_Mk1` -> `rhodes mk`).
2. **Match** the keyword table: a phrase is a sequence of whole tokens (no substring matches: `airbass` is not `bass`). Candidates are taken by phrase length in tokens (longest first), then weight (heavier first), then position in the text, and may not overlap, so
   `bass clarinet` (Woodwind) consumes `bass`; `synth brass` is Synth Poly, as in the Categories table of the UX spec. Per category the weights of the distinct matched
   phrases add up.
3. **Precedence** file name > metadata > folder names. The first source with any match decides and its decision is final: an explicit exception phrase (category `none`,
   e.g. `viola caipira`, `viola de cocho`) gives none; otherwise the highest total wins and a tie for the top gives none. A source without a match falls through.
4. No match, a tie or an exception: **none**. The module prefers none to a wrong guess (`Piano Pad`, `Choir Pad`, `Contrabass`, `Yamaha_C3_Grand` are none).

The table is data: `librarian/data/category_keywords.json`, `{"table_version": N, "entries": [{"category": slug|"none", "weight": 1..100, "phrases": [..]}]}` with phrases in
English, PT-BR and Spanish (plain ASCII; accents are folded on load, so `orgao` matches `Órgão`). A phrase listed twice (also under another category) is a load error. It is
loaded at runtime with `KeywordTable::load_file` (CLI `--table`) and **embedded** into the library at CMake configure time (`category_keywords_embedded.cpp`, regenerated
when the JSON changes), so the `librarian` binary needs no file; `KeywordTable::embedded()` is the default and a test proves it equals the file.
**Versioning policy:** `table_version` is bumped by 1 on every change of the data file (new words, weights, exceptions); changing the algorithm above is a code change
that must keep `librarian/tests/category_fixtures.tsv` passing. Words are added only when unambiguous: a word that names more than one family (`horn`, `gaita`,
`contrabass`, `grand`) stays out and the file ends up none, which the user then confirms in the UI.

Tests: `librarian_category_test` (tokenizer, table parse errors and embedded-equals-file, `category_fixtures.tsv`: 95 rows of real-world style names, Salamander / VSCO2 /
Sonatina / Pianobook / PT-BR / ES / SF2 General MIDI names, each category at least once, 13 rows expected `none` with a precision gate that no `none` row may get a category,
the three Flow 2 examples, precedence and tie rules, metadata scan, `import --category` spellings, `none` and unknown names refused with nothing written, SF2 preset-name
suggestion, the report field and the CLI). The fixtures are added to by appending rows; an expectation is written from the instrument, never copied from the output.

## Service, catalog and Library API (Story 4.11)

`librarian-service` is the host process of the `librarian` (a systemd unit on the board, `systemd/velora-librarian.service`: delivered as a documented file only, not tested on the
board; the Mac runs the binary directly). It owns the SQLite catalog and serves the **Library API** (`proto/library_api.proto`, package `ws.v1`) on a Unix domain socket.

```
build/librarian/librarian-service --data DIR --socket PATH     # DIR must exist; SIGTERM / SIGINT = orderly stop
```

Exit codes: 0 orderly stop, 1 could not start (another service holds the catalog, the catalog is damaged / foreign / newer than this build, the socket is taken by a live
listener or is not a socket), 2 usage. Targets: `velora_librarian_service` (catalog + service, links only SQLite, protobuf and threads: not formatlib), `librarian-service`.
SQLite is a build dependency: the system `libsqlite3` (macOS SDK or Homebrew; `libsqlite3-dev` in `tools/ci/docker/Dockerfile`); a missing one is a configure-time FATAL_ERROR with the
install hint. It is public domain, recorded in `THIRD_PARTY_NOTICES.md` and `tools/ci/third_party_manifest.json` (`Public-Domain` is on the license allow-list).

**Catalog** (`catalog.hpp`, `<data>/catalog.db`, `PRAGMA journal_mode=WAL`, `synchronous=FULL`, `PRAGMA user_version=1`). Only the tables of this story: `meta(key, value)` holding
`schema_version` (1) and `next_seq`, `live_set_favorites(live_set_id PRIMARY KEY, added_seq)`, `live_set_recent(live_set_id PRIMARY KEY, last_played_seq)`. Ordering uses sequence
numbers from the persisted `next_seq` counter, never wall-clock time (the unit has no RTC). The instrument/library tables are Story 5.6. `Catalog` is an RAII class with typed
`CatalogStatus` results (no exception crosses the API), prepared statements, a busy timeout and one transaction per write (a committed write is fsynced through the WAL; a process
killed mid-write leaves the previous committed state, recovered at the next open). Version policy: a `schema_version` newer than the build is refused (`kSchemaTooNew`) and left
untouched; an older one is migrated forward after a `PRAGMA quick_check` (the only integrity check; version 1 is the only version, so the migration is a stub); a foreign file, a SQLite database that holds any object (table, view, index, trigger) but no catalog `meta`, or one with a newer schema is refused (`kNotACatalog` /
`kCorrupt` / `kSchemaTooNew`) **before anything is written**: the file is inspected read-only (immutable, or on a private copy when a non-empty `-wal` exists), so a refused
file stays byte-identical with no `-wal`/`-shm` created; the schema is created only in a file whose `sqlite_master` is entirely empty. A missing `next_seq` row is rebuilt above
everything stored. Live Set ids are 1..64 bytes of `[A-Za-z0-9._-]` (UUIDs are the intended ids).

**Single writer (AD-9).** Only `librarian` writes the catalog: `Catalog::open` takes an exclusive `flock` on `<data>/catalog.lock` for its lifetime; a second service or catalog on the
same directory (same or another process) gets `kBusy`, and the service exits 1. The engine and the UI are clients; engine/UI never open `catalog.db`. The data directory
`/data/catalog` on the board is owned by the librarian's service user (the systemd unit creates it).

**Library API** (framing and closing rules of the engine's ControlServer: 4-byte big-endian length + protobuf, handshake first, a framing violation / wrong first message / unsupported
version / late handshake closes the connection after the error reply, an unparsable frame or invalid command after the handshake gets an error reply and the connection stays; largest
frame 262144 bytes). Handshake: `LibraryHello{version=1, features, client_name}` -> `LibraryHelloAck{version, features, catalog_schema_version}`.
Commands: `AddLiveSetFavorite`, `RemoveLiveSetFavorite` (idempotent; answer `LibraryAck{changed}`), `GetFavoritesAndRecent` (answers `LibraryState`), `ReportLiveSetActive`,
`LiveSetDeleted`, `Reconcile{existing_live_set_ids}`, `SetPlaying{playing}`, `FlushRecent` (answers `FlushReply{written}`), `LibrarySubscribe{enable}`. A subscriber gets the current
`LibraryState` right away and then `FavoritesChanged{favorites}` / `RecentChanged{recent}` after every change ("the favorite set changes and is published to `ui`"). Errors are the closed
`LibraryErrorCode` (`INVALID_ID`, `LIMIT_REACHED` at 2048 favorites (sized so a full list of 64-byte ids and Recent always fit one frame), `STORAGE_FAILED` with `details.reason`, and the framing codes); a refused command changes nothing. Concurrent
clients are serialised on the single service thread.

**Favorites** are explicit user commands: written immediately (one WAL transaction) and published. They list in the order they were added (oldest first; a re-added favorite goes last).

**Recent** lists the last 20 Live Sets, most recent first, deduplicated. `ReportLiveSetActive` updates the in-RAM list at once and publishes `RecentChanged`. The write to SQLite
(one transaction replacing the table) happens only at an *idle-safe moment*, defined by the `SetPlaying` flag the engine/UI feeds: while `playing` is true Recent is never written
(`recent_unwritten` in `LibraryState` says so); it is written when `playing` becomes false, at once when a report arrives while not playing, on an explicit `FlushRecent`
(the caller asserts the moment is safe, so it is honoured even while playing) and at orderly shutdown. A service killed with `kill -9` while playing loses the unwritten Recent
(never the favorites). **Deletion:** `LiveSetDeleted{id}` and `Reconcile{ids that exist}` (reconcile covers every recent row, not only the 20 listed) remove the favorite row at once, remove the id from the RAM Recent and from SQLite with the same
rule as any Recent write (deferred while playing; when idle both tables are cleaned in one transaction), and publish the change. Sending them is the engine's/UI's job (the engine-side wiring of
deletion is out of scope of this story; `engine-host --library-socket` only reports the active Live Set and the performing state).

Tests (no timing assertions; waits are bounded polls for an event): `librarian_catalog_test` (schema, version policy, favorites idempotence and order, Recent order / dedupe / cap 20,
persistence across reopen, the lock in the same and another process, corrupt and foreign files, delete cascade, fault injection, crash recovery by fork + `_exit`/`SIGKILL` inside a
write), `librarian_service_test` (a real UDS client: handshake and version mismatch, every message and every bad id, pushes, 8 concurrent clients, malformed / oversize / late frames,
the no-write-while-playing test with an injected write counter and the SQLite files unchanged across 3600 simulated switches then exactly one write, deletion reconcile, single writer,
stale sockets, kill of the service process mid-flush), `librarian_client_test` (the engine-side `LibraryClient`), `librarian_service_smoke` (the real binary, SIGTERM and SIGKILL,
exit codes), `engine_host_library_smoke` (engine-host `--library-socket`, macOS).

**Reconcile safety.** An empty `existing_live_set_ids` list while favorites or Recent exist is refused (`INVALID_COMMAND`, `details.reason=empty_reconcile`) unless the message sets
`allow_empty`, so a failed or truncated listing cannot wipe the catalog. Long lists are sent in chunks: every chunk but the last sets `more`; the ids accumulate on the connection
(at most 100000) and the final chunk (`more=false`) applies the union; a connection that closes first applies nothing; a bad id drops the accumulation.

**Playing is a lease.** `SetPlaying(true)` is owned by the connection that sent it: when that connection closes (the engine/UI exited or crashed) `playing` becomes false and the
pending Recent is written. `LibraryServiceOptions::playing_lease_ms` (default 0 = off) also ends it that long after the last `SetPlaying(true)` (tested on an injected clock). A peer
that sends a command and then closes or half-closes still has its buffered commands applied and answered. A failed storage write answers `STORAGE_FAILED` with `details.reason`, leaves
RAM equal to SQLite, keeps unwritten Recent marked `recent_unwritten`, and is retried at the next idle-safe moment.

**Socket access.** Default mode 0600 (dev). `--socket-group NAME` (`LibraryServiceOptions::socket_group`) sets the socket's group and mode 0660; the systemd unit passes `velora`
(its `/run/velora` directory is 0750, group velora). An unknown group refuses to start. The unit is untested on the board.

## Libraries catalog, import API and atomic publish (Story 5.6)

The `librarian-service` is the only writer under `<data>/lib` and of the catalog (AD-9). It now imports: an `ImportRequest` on the Library API queues a job, one worker
thread runs jobs serially through the Epic 1 importer, and the service writes the catalog rows only after the bundle is published. Proto: `proto/library_api.proto`
(additive messages and error codes, see `proto/README.md`).

```
build/librarian/librarian-service --data DIR --socket PATH [--catalog-dir DIR] [--socket-group NAME]
```

`DIR` is the library root (`DIR/lib/<library_id>/...`, the layout the engine's resolver reads); `catalog.db` and `catalog.lock` live in `--catalog-dir` (default `DIR`; the
board: `--data /data --catalog-dir /data/catalog`, see `systemd/velora-librarian.service`).

**Catalog schema 2** (`PRAGMA user_version=2`, `meta.schema_version=2`; migration 1 -> 2 is forward-only, one transaction, keeps every favorite and recent row; a newer schema is
refused untouched as in 4.11): `libraries(library_id PRIMARY KEY, name, source_kind 'sfz'|'sf2', source_path, bundle_id, instrument_count, imported_seq)` and
`instruments(library_id -> libraries ON DELETE CASCADE, instrument_key, name, category CHECK 1..17, sample_count, region_count, bytes, PRIMARY KEY(library_id, instrument_key))`.
Category 0 ("none") and anything above 17 are refused by the table itself. A library and its instruments are written in one transaction (`Catalog::publish_library`); identical
content writes nothing; `imported_seq` (an import counter, no wall clock) is the list order. `bytes` is the decoded PCM size of an instrument's samples. No search tables yet (5.16).

**Library API additions.** `ImportRequest{source, items[]{path, category, name, instruments[]{key, name, category}}}`: `source` is an .sfz/.sf2 file or a folder, `items` are the files
to import (each its own library, Story 5.1), each needs a category 1..17; an SF2's `category` is the default of its presets and `instruments[]` overrides category and/or name per preset
key. The reply `ImportAccepted{job_id, item_count, bytes_total, queue_position}` is synchronous; `ImportProgress` events (job id, `event_seq` growing by one, state
QUEUED/RUNNING/PUBLISHED/FAILED/CANCELLED, counters, `current_item`, bytes, and on the event that finishes an item an `ImportItemResult` with status imported/unchanged/skipped/failed/cancelled,
the Story 5.1 reason `unsupported_features`/`damaged`, offenders `file:line`, `failing_path`, messages) go to the requesting connection and to every `LibrarySubscribe` subscriber.
The last event of a job is terminal: PUBLISHED = at least one item reached the catalog (others may be skipped), FAILED = none did (**an all-skipped or all-failed job ends FAILED**), CANCELLED, DRIVE_REMOVED (Story 5.8: the source disappeared; see the last section). A request holds at most 1000 items (the whole request is validated on the service thread, so a UI splits a bigger import), and every string that comes from the file system in an event or list reply is made valid UTF-8 (bad bytes become U+FFFD) and bounded; an item result keeps at most 100 offenders and 100 messages of at most 400 bytes each (the rest is summarised as `... N more`), so a progress frame stays far below the 262144 byte limit. `ListLibraries` and
`ListInstruments{library_id}` page (`offset`, `limit`, `total`, `more`; `ListInstruments` is ordered by `instrument_key`; a reply is also cut at `reply_budget_bytes`, 200000 by default, always with at least one entry, and then `more` is set: ask again from offset + size), `CancelImport{job_id}`, `ImportStatusRequest{job_id}` (latest event). **Refusals happen before anything is
written or queued, as typed errors:** `CATEGORY_REQUIRED` (no items, or a category that is missing, `none` or unknown; `category` is an `optional` field so presence is known: `details.reason` is `missing` when it is not set, `none` when it is set to 0, `unknown` for an id above 17), `INVALID_SOURCE` (missing or unreadable
source or item, not .sfz/.sf2, item outside the source, an item twice), `QUEUE_FULL` (more than `max_queued_jobs` = 16 waiting), `UNKNOWN_JOB`, `UNKNOWN_LIBRARY`. An override whose instrument key is not in the file is not imported: that item fails with `unknown_instrument_key` before anything is written. A re-import of unchanged
content is `unchanged` ("already imported"): no bundle, no new `current`, the catalog row is untouched unless the name or category differ.

**Order of one item (AR28).** stage in `<data>/lib/.staging/<unique>/` -> samples written -> bit-exact verification (reopen, `verify_all`, region table and every sample compared with a fresh
decode of the source) -> fsync -> rename into `<library_id>/<bundle_id>` -> flip `current` (symlink then rename) -> **then** the catalog rows in one transaction -> the progress event. A failure at any point
publishes no catalog rows. The importer itself is Stories 1.13/5.1/5.3 (`import_file`); the service reuses it and only adds `catalog.meta` and the hooks. Worker and service thread talk through a mailbox: the
worker never touches SQLite, and waits until the service thread has written the rows of an item before it starts the next one. A cancel takes effect at the next safe point before the rename (an aborted
staging directory is removed at once); a published item stays published. Shutdown does the same.

**`catalog.meta`** (`library_index.hpp`) sits in the bundle directory next to `bundle.vlb` and the legacy `category` file, is written inside the staging directory before the rename (so it is part of
the atomic publish) and records the library name, source kind and path and, per instrument key, its display name and category. This is how the catalog is rebuilt from bundles alone. It is deliberately not
in the binary manifest: the manifest is hashed into `bundle_id` and read by the engine, adding names and categories would bump the bundle format and change the ids of every bundle, and display data may be
refreshed by an identical re-import (like the Story 5.5 `category` file) without making a new bundle; it is rewritten atomically (tmp + rename) and `bundle.vlb` stays immutable. It is a small text
file (percent-escaped, `format_version 1`; unknown record types are ignored, a newer version is treated as absent) so the CLI links no protobuf. Only the service writes it (`ImportOptions::write_catalog_meta`);
CLI imports keep their exact old bytes.

**Startup reconcile** (`library_reconcile.hpp`, run by `start()` before the socket is bound, also exposed as `reconcile_libraries` / `rebuild_libraries`): (1) deletes `<data>/lib/.staging/*` (not the lock) and orphan
`.current.tmp.*` links in every library directory; (2) lists every library directory whose `current` names an existing bundle; (3) adds or refreshes the catalog row of each from the bundle alone (after `verify_all`;
a bundle that fails is not cataloged) and removes the rows of libraries with no usable `current`, so the catalog and `<data>/lib` agree and no partial library is listed. A bundle without `catalog.meta` (Epic 1
demo bundles, CLI imports) is listed with the instrument names of its manifest and the category of its legacy `category` file (`read_category_file`); one with no valid category is **not cataloged**
(reported in `ReconcileReport::skipped`, left on disk). The pass is idempotent and crash safe (a kill inside it just repeats it). It never deletes a bundle directory except non-`current` ones that still carry the Story 5.8 checkpoint marker (non-`current` bundles are otherwise the engine's
leases). **Only a definite absence removes a row** (`ENOENT`, a `current` that is not a link or is dangling): any other error while examining `lib/` or resolving a `current` (EACCES, EIO...) is recorded in
`ReconcileReport::errors` and the whole pass removes no row; a bundle that cannot be opened or whose `catalog.meta` is unreadable, oversized, malformed or from a newer format keeps its existing row (the legacy `category`
file is NOT used as a fallback then, it would rewrite the categories) and is reported in `skipped`; the index checks the manifest's sample ranges before using them. `rebuild_libraries` drops every library row first (tested: the result equals what the service wrote).

**Startup ordering, summary and boot bound (Story 8.12).** (1) *Ordering.* `velora-librarian.service` is `After=velora-engine.service` (ordering only; the engine publishes no separate "playable" signal, so the unit order is the contract and nothing in the engine changed). Inside the service, `start()` takes the locks, binds the socket and starts serving, then runs the reconcile pass: during it the socket answers the handshake but **every command gets the typed refusal `LIBRARY_ERROR_CODE_RECONCILING` (19, `details.reason=reconciling`)**; the import worker does not exist yet. `start()` returns, and commands are accepted, only after the pass finished with the unchanged Story 5.6 rule (and the Story 5.21 GC); a pass that fails stops the service as before. (2) *Summary log.* After every pass `LibraryServiceOptions::log` receives one line, `reconcile_summary(report)`, which `librarian-service` prints on stdout (journal): `reconcile summary: staging_deleted=N tmp_links_deleted=N catalog_rows_removed=N catalog_rows_recreated=N catalog_rows_updated=N catalog_rows_unchanged=N gc_bundles_deleted=N not_cataloged=N errors=N libraries=N elapsed_ms=N`. (3) *Boot bound.* The pass works at directory and manifest level only: it lists directories, reads the symlink `current`, the bundle header and manifest (`describe_bundle`) and `catalog.meta`, and removes staging leftovers and marked non-`current` bundles; it never reads sample or bundle content except `verify_all` of a bundle whose row it has to create or refresh (the existing rule), and it never modifies a bundle that `current` references. Proven in `librarian_startup_reconcile_test`: on a synthetic data root of 1.1 TB apparent size (512 libraries, a 2 GiB sparse file and a FIFO named like a sample in every bundle directory, so any content read fails by blocking) the pass takes about 0.05 s on a laptop against the 5 s bound. **Limits of that proof:** the bound is proven for the steady-state boot only (catalog already agrees with the bundles, so no row is created or refreshed); a row that must be created or refreshed still runs `verify_all` on its bundle, which reads the bundle's sample content (the unchanged Story 5.6 rule), and that cost is not bounded by the 5 s test. The `After=velora-engine.service` line is ordering only, not a gate on the engine being playable (no such signal exists). The RECONCILING error carries `details.retry_after_ms=250`; while the pass runs the service thread only accepts, answers handshakes and refuses (it does not touch the catalog: no counters sync, playing-lease expiry or Recent flush). A failed pass logs `reconcile: <status> (summary...)`, removes the socket and releases every lock, so a new service can start.

**Single writer.** `start()` takes `catalog.lock` (a second service gets exit 1) and then an exclusive flock on `<data>/lib/.service.lock`; the `librarian import` CLI always holds that file shared for its run, so it
refuses while a service holds it and a service refuses to start (before it reconciles or touches `.staging`) while a CLI import holds it. **Shutdown:** `stop()` first refuses new imports (`SHUTTING_DOWN`), cancels the running
item at its next safe point and every queued job (each ends with a terminal CANCELLED event), then stops; whatever was published but not yet cataloged is listed by the next start's reconcile.

**No encryption.** (Story 6.4: still no cipher; the one allowed libcrypto use is Ed25519 signature VERIFICATION of protected bundles, see "Installing protected bundles".) `tools/ci/check_librarian_no_crypto.py` (ctest `librarian_no_crypto`, with `librarian_no_crypto_selftest`) checks the link line collected at configure time and the symbols (`nm`) of
`librarian`, `librarian-service` and their static libraries for AES / OpenSSL / mbedTLS / libsodium / OP-TEE / CommonCrypto / SQLCipher; without `nm` it skips (exit 77) after the link-line part; an `nm` that fails or prints nothing for a target is a FAIL for that target. The link-line walk records any name it cannot resolve as `UNRESOLVED:<name>` (the script fails on it); only the imported third-party targets of other directories (FLAC, protobuf/abseil, SQLite) are exempt there, their code being covered by the symbol scan. The Homebrew-protobuf/vendored-abseil link of `velora_librarian_service` is exercised by the real binary in `librarian_service_smoke` (it starts `librarian-service`, also with `--catalog-dir`, and talks to it) and by every service test.

**Tests.** `librarian_import_service_test` (a real socket client; beyond the rows below: `--catalog-dir` split, progress to subscribers and not to bystanders, cancel of a three-item job after the first commit, `stop()` while a job runs, paging and the reply budget, bounded and non-UTF-8 reports, unknown override keys, a CLI import blocking a service start, reconcile with injected I/O errors, unreadable `catalog.meta`; the exact boundary order with the catalog and file system observed from outside at each point, category-required refusals with nothing
written, duplicates/changed source/metadata refresh, progress monotonicity and skip reasons with offenders, SF2 per-instrument categories and paging, cancel of a running and a queued job with a gate (no sleeps),
the bounded queue, unreadable/invalid sources, single writer incl. the CLI, concurrent list and import clients, startup reconcile, rebuild from bundles alone, legacy/uncategorised/corrupt bundles),
`librarian_catalog_test` (migration 1 -> 2 keeping favorites and recent, a migration failed by name through `migrate.before_commit`, constraints, cascade, fault and kill inside the library transaction), `librarian_crash_campaign` (below), `librarian_no_crypto*`.

**Interrupted-import campaign** (`librarian_startup_reconcile_test`, Story 8.12): the same forked-service mechanism as the crash campaign (a boundary hook that `_exit(137)`s), seeded (`kSeed`), 132 kills at the six named steps (stage = `stage-created`/`samples-written`, verify, fsync, rename, flip `current` = `link-created`/`link-flipped`, catalog row), half on a fresh library and half on a re-import of an installed one (changed source). After the restart (the service's own startup reconcile) the library set (`current` per library) and the catalog must equal the state before that import or the state after it; a failure prints the seed, run index, step and point. For a killed re-import the previous bundle must still be byte-identical and `current` the previous or the whole new bundle. The same test covers the RECONCILING refusal, the summary counts and the 1 TB boot bound. About 16 s.

**Crash campaign** (`tests/crash_campaign_test.cpp`): a seeded script of 200 imports (SFZ and SF2, single and multi-instrument, re-imports of unchanged and of changed sources, changing categories and names,
damaged and unsupported files). Each import runs in a forked child hosting the real service, killed with `_exit(137)` by `import_point_hook` at a seeded boundary (stage-created, samples-written, verified,
fsynced, renamed, link-created, link-flipped, catalog-row-written, catalog-committed, job-finished) or inside its own startup reconcile. After each kill the parent restarts (reconcile) and checks that every catalog
library has a verifying `current` bundle and matching instrument rows, every published library is cataloged, nothing is left in staging, and the pass is idempotent; at the end the whole script is replayed without
faults on the crashed tree and on a fresh one and both end in the same published state. The script is deterministic (xorshift seed in the file); it takes about 20 s on a laptop (process starts and service poll
intervals, tiny sources), so it runs in the default ctest. Deleting the "add missing row" step of reconcile makes it fail at the first step (checked by hand while writing it).

**Deviations from the story text (kept on purpose).** The service lock is `<data>/lib/.service.lock` (inside `lib/`, so the board sandbox needs write access to that tree only) instead of a separate `lib.lock`;
`catalog.meta` is a small text file, not protobuf (the CLI links no protobuf); instrument `bytes` is decoded PCM size; `--catalog-dir` splits the catalog from the library root (`/data/catalog` vs `/data/lib`).

## Import preflight: batch scan, free space and duplicates (Story 5.7)

Before anything is written the unit shows what is on a drive, what will be skipped and whether it fits. `PreflightRequest{source, volume_label, max_files}` on the Library API
scans a mounted volume or folder for `.sfz` / `.sf2` **without converting anything**, on the worker thread (`import_worker.*`: the service thread keeps answering; a scan runs
between jobs and between the items of a running import, is queued behind at most `max_queued_preflights` (4) others, and is cancelled by `CancelPreflight{request_id}`, by the
requester disconnecting, or by the service stopping). The scanner itself is `preflight.hpp/.cpp` (`preflight_scan`), pure file system work with no catalog.

- **Libraries and rows.** A library of the preflight is the top-level folder of the source (its files at any depth); the files directly in the source form the first library, named
  `volume_label` (default the source folder's name; on the Mac the tests pass a folder plus an explicit label). Rows are one per timbre: an SFZ file, or one preset of an SF2 file
  (`instrument_key` = the file name for an SFZ, `bank:preset` for SF2: the importer's keys). A file that cannot be parsed is one row with an empty key. The IMPORT unit stays
  Story 5.1's: every file is its own catalog library; only the display grouping is by folder. Names starting `._` (macOS resource forks) are ignored.
- **Status** is the closed set `ready` (a Story 5.5 category is suggested), `needs_category` (none is: the user must pick one, FR21), `skipped_unsupported_features`,
  `skipped_damaged` (the importer's own Story 5.1 classification, reproduced from headers: unsupported opcodes/headers/values, unsupported sample formats; missing, unreadable or
  truncated samples, a loop or offset that does not fit its sample, a damaged SF2) and `already_imported`. The scan decides the first four; the service adds `already_imported`
  from the catalog: the row's `library_id` is minted by formatlib's `library_id_from_source` (the one function `BundleWriter::set_source_id` also calls: UUIDv5 of the canonical absolute
  path) and the catalog must hold that library with that `instrument_key`. The default `action` of a row is IMPORT for `ready`/`needs_category` and SKIP for everything else.
- **Import again.** `ImportItem.action` (additive): DEFAULT (0) imports exactly as in 5.6; IMPORT is the explicit "Import again" on an `already imported` row; SKIP leaves the item out
  (no category required, no space counted, `ImportAccepted.skipped_count`); a request whose items are all SKIP is `INVALID_COMMAND` `details.reason=nothing_to_import`. Re-importing an
  unchanged source ends `unchanged`; a changed source is replaced in place by a new bundle (Story 5.8, below). The action is per file: an SF2 is one item, so all its preset rows share one action.
- **Headers only.** An SFZ is parsed as text (formatlib parser) and each sample is examined through a RIFF chunk walk with `pread` (format chunk, the data chunk's declared size against
  the file size); an SF2 is parsed over a counting byte source, which reads the `pdta` tables and the referenced sample headers but not the sample words. `PreflightDone.bytes_read` counts
  what the scan read; the test reads 1.4 KB for an SFZ whose WAV holds 64 MiB (sparse) plus an SF2.
- **Bounds.** At most 20000 `.sfz`/`.sf2` files (`max_files`, never above `kPreflightMaxFiles`), 400000 directory entries and 32 levels; hitting one sets `truncated` and the listing is
  partial. Symbolic links are never followed: a linked folder is skipped, a linked file is used only when its target lies inside the source root (it is then identified by its canonical
  target), and a sample that is a link leaving the root makes its SFZ `skipped_damaged`. `symlinks_skipped` counts them. Non-UTF-8 names are made valid (bad bytes become U+FFFD) in every
  reply, as in 5.6. The result is kept in the service (newest 3) under `preflight_id` and paged with `ListPreflightRows{preflight_id, offset, limit, library}` (cut at
  `reply_budget_bytes`, `more`); `PreflightDone` lists at most 200 library summaries (`libraries_total` says how many exist).
- **Space.** An import of items with total decoded PCM size P (worst case, uncompressed: frames x channels x bytes per sample, per timbre, from the headers) needs
  `required = 2 x P + 256 MiB` (`kImportPcmFactor` 2 = the published bundle plus the staging copy; `kImportReserveBytes` = the catalog, manifests, file system metadata and slack; 0 when
  there are no items). `free` is `statvfs` `f_bavail x f_frsize` of the data directory (`LibraryServiceOptions::free_bytes_fn` replaces it in tests; a failing probe means "unknown": the check
  is skipped). `PreflightDone` reports `pcm_bytes`, `required_bytes`, `reserve_bytes`, `free_bytes`, `fits` for the rows whose default action is IMPORT; an `ImportRequest` whose required bytes
  exceed the free bytes is refused before anything is written or queued with `INSUFFICIENT_SPACE` and `details.required_bytes` / `details.free_bytes` (decimal). The request's estimate uses the
  preflight's per-file figure while the file's size and mtime are unchanged, else a headers-only estimate made on the service thread at request time (cost: parsing the SFZ and the WAV headers
  of the items, at most 1000 items).
- **Duration.** `PreflightDone.duration_ms` is the scan wall clock. The 1,000-file synthetic volume test asserts counts and correctness, never time, and prints the figure (about 0.3 s on
  the development Mac with tiny files and a warm cache); **host figures are not representative of the board** (USB flash, cold cache): measure there.

Tests: `librarian_preflight_test` (statuses incl. all five, root-label library, library_id equals the importer's, already-imported default Skip then Import again, space refusal with both numbers
and the exact boundary, symlink escapes, truncation cap, headers-only byte count, 1,000-file volume, cancel while the service stays responsive, bad sources, non-UTF-8 names where the file system allows).

**Review round (Story 5.7), rules worth knowing.**
- *Identity.* `library_id` derives from the **canonical absolute path** of the file: the same drive mounted at another path (`/Volumes/USB` vs `/Volumes/USB 1`) is a different library, so its rows are not `already imported`. Golden vectors for `library_id_from_source` are in `formatlib/tests/hash_test.cpp`.
- *Chunks.* One ImportRequest holds at most 1000 items (`kMaxItems`) while a scan lists up to 20000 files (50000 rows, `kPreflightMaxRows`): a UI imports a big selection in chunks of at most 1000 items. Each chunk is checked for space on its own and the reserved bytes of the chunks that are still queued or running (`reserved_bytes_`, released when a job ends or is cancelled) are held against free space, so many chunks cannot jointly overcommit the drive.
- *Selection.* `PreflightDone.required_bytes` is the figure for the **default** selection (everything that is not `already imported` or skipped). A different selection must add up `PreflightRow.pcm_bytes` with the same formula; "Import again" on already imported rows needs the full reserve too (the old bundle stays until the new one is current).
- *Free space unknown.* `free_known=false` in `PreflightDone` and `ImportAccepted`: `fits` is then false (it only says "known and enough"), the import is NOT refused (the check is skipped); show the state, do not block.
- *Estimate for an ImportRequest* is not confined to the source root (the importer follows sample links wherever they lead), uses the preflight's figure only while every file it depends on (.sfz, includes, samples) still has the same size and mtime, charges a file the estimate calls skipped its own size (never zero), and is made on the service thread for at most 50 files per request (`LIMIT_REACHED` `details.reason=estimate_budget`: run a preflight on the source first, its figures are then cache hits, or split the request). Cache hits cost one `stat` per dependency (at most 200000 per request, then they count as misses).
- *Remaining service-thread work.* Finishing a scan reads the whole catalog once (`list_libraries` with instruments: bounded by the size of the catalog) and then does set lookups per row; no file is stat'ed there (the worker captured size and mtime). A scan holds at most 50000 rows and the service keeps the newest 3 results.
- *Names.* A file whose name is not valid UTF-8 is listed with the bad bytes shown as U+FFFD; that sanitised path cannot be opened, so such a file cannot be imported through the API (rename it). A link to a file inside the source is an alias and is not listed (the file is, once, under its own name); links leaving the source are skipped; a sample link leaving the source makes its SFZ `skipped_damaged` in the scan although the importer itself would follow it (the one deliberate divergence from the importer, asserted in the parity table of the tests). FIFOs and devices are never read (opened non-blocking and refused unless regular).
- *Limits.* At most 4 scans wait for the worker, at most 2 per connection (`QUEUE_FULL` `details.reason=per_client`). `detail` of a skipped row never quotes a file outside the source (only that one has a problem). `ImportItem.action` outside DEFAULT/IMPORT/SKIP is `INVALID_COMMAND` `details.reason=unknown_action`.

## Checkpoints, drive removal and Import again (Story 5.8)

A long import must survive a pulled drive and a power cut: what finished stays, the timbre in progress is discarded whole, and no library is ever half-written.
A **timbre** is the unit: an SF2 preset, or an SFZ file (one file = one library = one timbre, Story 5.1; it is published as soon as it is finished, so the interval
below changes nothing for SFZ files).

**Checkpoints (an SF2 import).** While a SoundFont is converted preset by preset, every `checkpoint_timbres` finished timbres (default 10) or `checkpoint_bytes` of decoded PCM
(default 20 GiB), whichever comes first, the timbres finished so far are published as a NEW immutable bundle of the same library with the whole Story 1.13 protocol (stage, bit-exact
verification, fsync, rename into `<library_id>/<bundle_id>`, atomic flip of `current`); the last bundle, with every timbre, is published at the end of the file (no extra checkpoint
when the last timbre ends the file). Every checkpoint bundle holds the first k timbres of the file, bit for bit what the final bundle holds for them (checked at publish against the previous
checkpoint), has its own `catalog.meta` and is never modified. **A superseded checkpoint is deleted** as soon as the next bundle of the run is current (and the catalog row follows): the engine's lease on a bundle is an open file descriptor, which survives the unlink, so a reader that already resolved it keeps reading it; only a resolver call that races the flip can miss it and retries. Every bundle of a first-time SF2 import carries a `checkpoint` marker file (inside the staging directory, part of the atomic publish) that is removed once the last bundle of the run is current; the startup reconcile deletes every non-current bundle that still carries it (a superseded checkpoint or an unflipped publish that a kill left) and clears the marker of a current one. Bundles without the marker (final bundles, SFZ, Import again) are never pruned: they are the leases of earlier imports. The one thing a kill can still leave behind is the complete, unmarked bundle of a library that already had a `current` (Import again killed between rename and flip): it is verified, harmless and kept like any older bundle. The service writes the catalog rows of a checkpoint
(`ImportProgress.checkpoints` counts them, the event has no `item`) before the next timbre is converted. Configuration: `LibraryServiceOptions::{checkpoint_timbres, checkpoint_bytes}`,
`librarian-service --checkpoint-timbres N --checkpoint-bytes N`, `librarian import ... --checkpoint-timbres N --checkpoint-bytes N` (whole numbers of at least 1; tests use 1..3). The byte
figure of a timbre is its decoded PCM size (`checkpoint_size_fn` injects sizes in tests).

*A library that already has a `current` bundle is never checkpointed* (Import again, or the partial bundle a crashed run left): its old bundle stays current until the whole new
bundle is verified, so a reimport can never shrink a library that was complete. **The way back after a pulled drive** therefore is: the library holds the finished timbres (the preflight lists them `already imported`, default action Skip, the others `ready`); "Import again" of the file converts it whole and publishes the complete bundle in one step (no checkpoints on the way: a second removal during it publishes nothing and the partial library stays as it was). `librarian_checkpoint_test` runs exactly this sequence (removal, a second removal on the retry, preflight, Import again) and checks the result equals a clean import.

**Space.** Checkpoints do not raise the Story 5.7 requirement: the peak is the current checkpoint (at most P, the decoded size of the timbres so far) plus the next one being staged (at most P), then the old one is deleted; `required = 2 x P + 256 MiB` already covers that, and the reservation of the job is held for its whole life. The rewrite cost (every checkpoint writes the bundle again) is time and flash wear, not space.

*Hard links: not possible with this bundle format, and not used.* The story suggests hard-linking the verified sample data of the previous bundle into the new directory. A bundle is one
file (`header | manifest | index | blocks`, see `formatlib/README.md`); a bigger bundle has a different manifest and index in front of its blocks, so it cannot share bytes with a smaller
one. Instead the `BundleWriter` keeps the verified encoded blocks of the finished timbres in memory (the import already holds the whole bundle in memory), so a checkpoint re-reads
nothing from the source and re-encodes nothing, and writes the file again; the cost is writing the bundle once per checkpoint (O(n^2 / interval) bytes). The consequence: no
hard-link-or-copy fallback exists to test. Changing that needs a format change (a block store separate from the manifest), which is out of this story.

**Drive removal.** The job ends with the new terminal state `IMPORT_STATE_DRIVE_REMOVED` (the story's `drive_removed`) when the source disappears at ANY point of an item. Detection, in order of strength:
the worker's built-in probe (the job's source folder or file can no longer be `stat`ed with ENOENT/ENOTDIR/ENXIO/ENODEV/EIO/ESTALE, or sits on another device than when the job started) and the explicit signal
`LibraryServiceOptions::source_gone(source)` (the board's unmount event; tests) are trusted and polled before each timbre, after each sample read and whenever a parse, read or verification fails. An **I/O error**
(`kIo` of an SF2 sample read, an SF2 parse or verification read, an SFZ sample load) is only *suspect*: it is confirmed by a stronger probe, once (list the source folder, read one byte of the source file again, and
the file system must answer EIO/ENXIO/ENODEV/ESTALE/ENOTCONN, or the injected probe says gone). Confirmed: drive removal. Not confirmed (a bad file on a drive that still answers): that item fails
(`Status::kIo`, item FAILED; an SFZ sample stays `damaged`) and the job goes on with the next item. A **short read** (`kTruncated`) is damaged data, never removal, unless the probe says the source is gone.
Then: the timbre in progress is dropped whole from the writer (`BundleWriter::discard_last_instrument`) and nothing of it reaches staging; the timbres finished so far are published as the last bundle (a first-time
import) and cataloged; the rest of the job is not started: each item after it gets a result of its own (`SKIPPED`, reason `IMPORT_REASON_DRIVE_REMOVED`, counted in `items_skipped` and `items_done`); earlier
libraries are untouched (their bundles and rows are never rewritten). The final event carries the counts: `items_total`/`items_done`/`items_imported` and `timbres_total` (starts as one per file and grows
when an SF2 is opened and its preset count is known; a file never opened counts 1), `timbres_imported` (timbres in a published bundle whose catalog row was written), `checkpoints`; `bytes_done` adds only the PCM that was
converted of the removed item; the item that was running has status `IMPORT_ITEM_DRIVE_REMOVED` with `timbres` (published) and `timbres_total`. A removal before the first timbre finished publishes nothing (the library does not
exist, `timbres_imported` 0); a removal during **Import again** publishes nothing and the old bundle stays current and intact. The SFZ path converts a failed verification into a removal only when no bundle
was renamed into place yet. The CLI stops its batch at the file that hit the removal and reports `drive removed: N of M timbre(s) published` on stderr (exit 1). A cancel between two timbres keeps the checkpoints already published (the item ends `cancelled`).
A client that did not list `import.checkpoints.v1` in its hello does not know the new enum values: it receives `FAILED` with `message = "drive_removed"` (and the item as `FAILED`, detail naming the removal) instead of `DRIVE_REMOVED`,
also in the answer to `ImportStatusRequest`.

**Partial libraries (the contract).** An SF2 item that ends in any state other than imported/unchanged AFTER a checkpoint (cancelled, skipped as damaged, failed on a verification or an I/O error, drive removed) leaves the
timbres of its last published bundle published, cataloged and playable. Its `ImportItemResult` says so: `timbres` > 0 and `partial = true`. A job in which no item reached the end still ends `FAILED`/`CANCELLED`
(`PUBLISHED` means at least one item finished) while the catalog holds the prefix; "Import again" completes it. `ImportProgress.checkpoint_error` is set while the catalog rows of a checkpoint could not be written (the
bundle is published; `timbres_imported` does not count it yet); the next checkpoint or the end of the item writes the row again and clears it.

Interpretation worth knowing: the story says the librarian "publishes the timbres finished so far" on removal; this is done for ALL finished timbres, also when no checkpoint interval
had been reached yet (6 timbres, interval 10, the drive goes at the 4th: the first 3 are published as one bundle).

**Import again** (`ImportItem.action = IMPORT` on an `already imported` row). The file is converted again; if it differs, a new bundle is published and `current` flips to it, with the same
`library_id` (UUIDv5 of the source path) and the same `instrument_key`s; the converted timbres that did not change are bit-identical in the new bundle (an SF2 is one item, so every preset is
converted again from the source and compared by the identical encoder), the old bundle directory stays (leases). A Live Set only holds `(library_id, instrument_key)`, so the
engine's `LibraryResolver` (which the Live Set loader calls per part) resolves the re-converted timbre without any edit of the Live Set; the test resolves through it. An unchanged source ends
`unchanged`.

**Crash campaign** (`librarian_crash_campaign`, second part): 50 SIGKILLs of a real process (`librarian_test_cli`, told by `VELORA_LIBRARIAN_KILL=<point>@<n>` to kill itself) importing a 7-timbre
SoundFont with checkpoints of 2. The 45 named points of the pipeline (7 stage points x 4 publishes, `timbre-start` and `timbre-done` x 7, `checkpoint-published` x 3) are each hit once in a seeded
shuffle, plus 5 repeats drawn from the same seed; a kill that does not land (the process exits normally) fails the test. Every kill runs on a fresh root; after each: the catalog opens, the reconcile pass
runs and is idempotent, the Story 5.6 invariants hold (rows match bundles, no staging leftovers, no orphan temp links), only the current bundle is left under the library (no lease exists in the campaign; the chain keeps unmarked bundles of earlier runs) and it verifies and holds the first k timbres
of the clean import bit for bit, the catalog lists the library iff it has a `current` bundle and names it, and a completed re-run ends with `current` = the clean import's bundle id (the content hash) and
a catalog row naming it. A chain of 10 kills on ONE root and 20 SIGKILLs of the service process (timbre, stage and catalog points, `import_timbre_hook`) follow the same checks. The test CLI's
`VELORA_LIBRARIAN_SOURCE_GONE=<point>@<n>` makes the source vanish at a point instead (the real drive-removal path of the importer); neither variable exists in the shipped `librarian`.

**Tests.** `librarian_checkpoint_test` (beyond the list: removal on the 2nd sample of a 3-sample timbre equals a clean import of the prefix file, injected I/O errors and short reads through a fault `ByteSource`, the partial-library contract for cancel / damaged / verification failure after a checkpoint, a failed checkpoint catalog write, checkpoint naming with interval 1, the old-client downgrade, removal twice then preflight then Import again, the real `librarian-service` flags, the shipped `librarian` ignoring the kill variables, reconcile pruning): checkpoint boundaries (interval 1; 3 with a remainder; an exact multiple; 11 timbres; the default 10 with 6 and 12 timbres; an interval larger than the
file; the byte threshold with injected 10 GiB sizes, mixed sizes and a 1-byte limit; the count winning), every checkpoint verified, immutable and a prefix of the final bundle; no checkpoint for a library
that is already published; removal before the first timbre finishes / after a checkpoint / before the first interval / during Import again / of an SFZ / a parse that fails because the source is gone
(`damaged` stays `damaged` with a live drive) / an abort between timbres; Import again with the resolver; the service (catalog rows observed at each checkpoint through a second connection, DRIVE_REMOVED
with the counts and earlier libraries untouched, with the injected probe and with the built-in stat probe by renaming the source folder, removal before anything, Import again through `ImportAction`);
the CLI flags and bad values. Env-gated: `VELORA_TEST_DISK_IMAGE=1` (macOS) creates a `hdiutil` image, imports from it and `hdiutil detach -force`s it in the middle of the 5th timbre; without the variable, off
macOS, or when `hdiutil` cannot attach an image the part reports "skipped" and passes. On the board the same path is the unmount event / `ENODEV` of a pulled USB drive (verify with a physical drive).

Not in this story: the power-cut campaign on the board (Epic 8), the read-only mount and sandbox (Story 5.9).

**Corrupted PCM before a removal publish.** When the source is gone the last bundle cannot be compared with it: the removal publish verifies the bundle's structure (`verify_all`: every block hash and the per-sample PCM
hashes the writer computed from the PCM it was given), the instrument keys, names, region tables, sample counts, formats and lengths against what was parsed, and that the first timbres equal the previous checkpoint.
It does NOT prove that the PCM the source returned before the removal was right (a flaky read that returned wrong bytes without an error would be encoded as given); only a publish with the source present
(checkpoints and the final bundle) compares every new sample bit for bit with a fresh read.

## Installing protected bundles (Story 6.4: FR31 install, FR32, AR28 partial, NFR25)

A vendor pack is the output directory of `tools/content-pack` (Story 6.3): `bundle.vlrp` (format 3, AES-256-CTR blocks, signed manifest), `catalog.meta`, `catalog.meta.sig`,
`wrapped-key.<device key id>.bin` (`wrapped_key_v1`, 105 bytes) and files the librarian never reads (`content-key.vault`, `pack.json`: the vault entry is the vendor's, it never
reaches a device). The librarian **verifies and installs** it; it never decrypts a block and never holds a content key (below).

```
librarian install-protected <pack dir> --root <data root> [--bundle FILE] [--wrapped-key FILE] [--library-id UUID] [--trust-list FILE]
```

Exit 0 = installed (or the same bundle was already installed: `unchanged`), 1 = refused or failed (`rejected (<reason>): ...: nothing published` on stderr), 2 = usage. It takes the same locks as
`import` (refuses to run while a service owns the root; the staging lock serialises it with any importer). The CLI does not write the catalog; the service's next start lists the library
(reconcile). Over the Library API: `InstallProtectedRequest{source_dir | bundle_path, wrapped_key_path, catalog_meta_path, catalog_meta_seal_path, expected_library_id}` (proto/library_api.proto);
it answers `ImportAccepted` (`item_count` 1, `required_bytes` = pack size + reserve) and runs as a **job on the same worker** as imports (FIFO with them, one at a time, the single catalog writer), with the
same `ImportProgress` events, `CancelImport`, `ImportStatusRequest`, reservation of space and terminal states: `PUBLISHED` (installed or unchanged), `FAILED` (rejected: the item result is `FAILED` with
`install_reason` and a `detail`), `CANCELLED`, `DRIVE_REMOVED` (the volume went away while the bundle was read; for clients that listed `import.checkpoints.v1`, else `FAILED` with message `drive_removed`).
Refusals before a job exists (nothing queued): `INVALID_SOURCE` (reason `bad_path` for a relative path, `source_missing`, `not_a_directory`, `bundle_missing`, `source_unreadable`), `INVALID_COMMAND` (`install_target`: name
exactly one of `source_dir` / `bundle_path`; `invalid_library_id`), `INSUFFICIENT_SPACE`, `SHUTTING_DOWN`, `QUEUE_FULL`. The hello features gain `install.protected.v1` and `library.kind.v1`.

**What is checked, in this order** (`install.hpp`; every failure is a typed `InstallReason` / `ws.v1.InstallReason`, nothing is published and the staging area is cleaned):

1. The bundle opens as a protected bundle. **Bounded before any allocation:** the header's sizes are capped (manifest 64 MiB, index 512 MiB; a claim beyond them, or beyond the file, is `corrupt_bundle`), and the 128-byte seal is read first and its **signer's key id checked against the trust list before the manifest and the index are allocated, read or hashed** (`untrusted_key` costs a header and a seal read). Then header layout, manifest and index hashes, **the Ed25519 signature over the manifest statement verifies** with the strictness of `KEY-HIERARCHY.md` section 6 (`signature_invalid`), the hash-list root matches the index (`hash_list_mismatch`). An unprotected (older) format is `not_protected`, an unknown version `unsupported_version`, anything structurally damaged or truncated `corrupt_bundle`.
2. The vendor `library_id` is the expected one: `expected_library_id` if given, else the name of the pack directory when that is a UUID (`library_id_mismatch`).
3. **The wrapped-key blob is well formed, by structure only** (`wrapped_key_invalid`): exactly one candidate (a named one, or the only regular file `wrapped-key.*.bin`), 105 bytes, magic `VWK1`, algorithm byte `0x01`, a non-zero ephemeral key, and - when the file is named `wrapped-key.<16 hex>.bin` - that recipient key id equal to bytes 5..12 of the blob. It is **never unwrapped**; the librarian has no device key. It cannot tell a damaged payload (recipient id, ephemeral key, nonce, ciphertext, tag) from a good one - only the device can, by the GCM tag at unwrap (Story 6.5) - and the blob has no library id inside (it is bound as AAD at unwrap). A corrupted blob, or a blob made for another library, can only make that library fail to unlock, never play wrong audio (`KEY-HIERARCHY.md`); the installed copy is byte-identical to the one delivered and is written `0600` (the rest of the bundle directory `0644`, directories `0755`). The test pins the limit exactly: of the 105 single-bit flips of a blob, the 13 in the magic, the algorithm and the recipient id are refused and the other 92 install as delivered.
4. **catalog.meta is bound to the bundle** (`catalog_meta_invalid`, `untrusted_key`): see below.
5. What is installed. A `current` bundle that is unprotected means the id is a user library (`library_id_in_use`). **Replay protection:** the pack carries a signed, monotonic `pack_revision` (in `catalog.meta.sig`; `velora-content-pack pack --revision N`, default 1); the installed `current` bundle's own sealed revision is read, and a pack with a **lower** revision is a `downgrade`, as is the **same revision with another bundle** and a lower format minor. A deliberate vendor rollback is therefore a new, **higher** revision that carries the old content. A `current` bundle that cannot be examined (damaged, signer no longer trusted) leaves the history unknown: the install proceeds and says so in the result's `detail`. The same `bundle_id` in place and intact is **idempotent**: `unchanged`, reason `already_installed` (if `current` does not name it - an install cut short between rename and flip - `current` is flipped to it, status installed). If the delivered wrapped-key blob differs from the stored one (the vendor re-delivered the pack for this device), the stored `wrapped-key.bin` is replaced atomically (temp file, fsync, rename, fsync) after the same validation and the result says `wrapped_key_updated` (`ImportItemResult.wrapped_key_updated`); the bundle itself stays unchanged. A damaged copy of the same `bundle_id` is moved into the staging area (the next run empties it) and the normal publish follows. **At most two protected bundles stay:** once the flip is durable the bundle directories of that library other than the new one and the one that was current before are removed (an open descriptor on them - a lease - survives the unlink); a kill during that removal leaves the extra directories until the next install of the library.
6. Space: `required = pack bytes + 256 MiB reserve` (`required_install_bytes`; the copy is renamed, not duplicated) against the free bytes of the data file system; short = `INSUFFICIENT_SPACE` / status `insufficient space` with `required_bytes` and `free_bytes`; unknown free space skips the check. The install request reserves its bytes like an import until it ends (a running install and an import beside it share the reservation). The previous bundles are already on disk; the check counts the staged copy plus the reserve, so a library of size P briefly holds the previous, the current and the staged bundle (about 3P) before the oldest is removed.
7. The bundle is copied into `<data>/lib/.staging/<unique>/` **in bounded chunks** (256 KiB) and then **every ciphertext block of the staged copy is hashed against the signed index** (`block_hash_mismatch`), one block (at most 1 MiB) at a time. The file is never loaded whole. This is **measured**: `librarian_install_test` replaces `operator new` and installs a 139 MB bundle (4,000+ blocks) with a peak of live allocations under 1 MB and a largest single allocation of 256 KiB (asserted limits: 8 MiB and 1 MiB); a counting byte source shows the largest read of the source is the copy chunk. `InstallResult::peak_buffer_bytes` is only a diagnostic. An out-of-memory or any exception inside the install or the description of a bundle is caught (`io_error` / an unreadable bundle whose row is kept), never thrown into the worker thread. Verification is of the **staged copy**, so a copy error cannot slip through, and the staged bundle must be the very `bundle_id` that was opened.

Publish then follows the user-import order and primitives: fsync the files and the staging directory, `rename` to `<data>/lib/<library_id>/<bundle_id>/` (fsync the parents), flip `current` by symlink-then-rename (fsync), then the catalog rows (service). The bundle directory holds `bundle.vlrp`, `wrapped-key.bin`, `catalog.meta`, `catalog.meta.sig`; a protected bundle has no `bundle.vlb` and no legacy `category` file. The kill points are those of imports (`stage-created`, `samples-written`, `verified`, `fsynced`, `renamed`, `link-created`, `link-flipped`, `catalog-row-written`, `catalog-committed`, `job-finished`), the reconcile and lease semantics are identical (a bundle that is not `current` is a lease and stays).

**Sources.** The pack is read from any readable absolute path the caller names (a mounted volume, a folder); only regular files are read - a FIFO or a device in place of any file is a typed refusal, never a hang (`O_NONBLOCK` open, `fstat`, `S_ISREG`; the Library API also checks the optional paths: `not_a_file`). The error text names the path it tried: whoever can talk to the service socket (mode 0600 / the `velora` group) can ask it to probe readable paths and read the typed reason (a file-existence oracle); the socket permissions are the control, and the service only ever reads from the source, never writes to it.

**catalog.meta binding (decision).** The names and categories the catalog shows live in `catalog.meta`, which is deliberately not in the signed manifest (it can be refreshed without making a new bundle). Rather than trust an unsigned file, the pack carries a **detached seal**, `catalog.meta.sig` (84 bytes: `VCM2`, algorithm 1, the `pack_revision` (u32 LE), the signer key id, an Ed25519 signature) over the statement `"velora/catalog-meta/v1" 0x00 || bundle_id || SHA-256(catalog.meta) || signer key id || pack_revision` (`formatlib` `protected_format.hpp`, made by `tools/content-pack`, checked by `verify_catalog_meta_seal`). So one flipped bit in `catalog.meta` or in the seal breaks the signature, a seal made for another bundle (even of the same signer) does not fit, and the signer must be the bundle's own key. The install also requires that `catalog.meta` parses (assignable categories only) and names **exactly the bundle's instruments**. `describe_bundle` re-checks the seal every time it describes a protected bundle (reconcile; the signer must be the bundle's signer and the parsed bytes are the verified bytes), so an edit after the install is noticed (`catalog.meta` unreadable: the row is kept, the library is reported in `skipped`). Tests: every byte of both files flipped.

**Factory rows.** The catalog is **schema 3**: `libraries.kind` (`user` | `factory`, `CHECK`), additive migration 2 -> 3 (every existing row becomes `user`, favorites and Recent untouched; a file whose version was edited back is tolerated). `LibraryRecord::kind`, `LibraryInfo.kind` (`LIBRARY_KIND_USER` = 0 default, `LIBRARY_KIND_FACTORY` = 1) in `ListLibraries`. The kind is derived from the bundle (a `bundle.vlrp` is Factory), so a catalog rebuilt from bundles alone gets it back; `rebuild_libraries` without trusted keys keeps the Factory rows (it cannot rebuild them) instead of dropping them. Rows of an installed library come from the manifest (instrument keys, sample and region counts, decoded sizes) and the sealed `catalog.meta` (names, categories) **without decrypting**; user and Factory libraries coexist in `ListLibraries`/`ListInstruments` ordered by `imported_seq`.

**The user import path never touches a Factory library.** An `ImportRequest` item whose derived `library_id` is a Factory library is refused at the request: `LIBRARY_ERROR_CODE_FACTORY_LIBRARY_PROTECTED` (18; details `path`, `library_id`, `reason=factory_library_protected`) before anything is queued; the request check looks at the file system as well as the catalog (any `<bundle_id>/bundle.vlrp` under the library directory, so the window between an install and its catalog row is covered), and the importer itself refuses too (`Status::kFactoryProtected`, text `factory_library_protected`, nothing published) - **early, before any sample is read or converted**, and again at publish time - for the CLI, a request that raced an install, and the SF2 checkpoint path. Ordinary user imports are unchanged: still unencrypted, path-identified (FR24).

**Trusted keys.** `LibraryServiceOptions::trusted_keys` / `InstallOptions::trusted` / `ReconcileOptions::trusted` / `DescribeOptions::trusted` (`velora::format::TrustedKeys`) are injectable. The shipped `librarian` and `librarian-service` embed the keys of their image variant: `tools/keys` generates `velora_trusted_keys_<variant>.hpp` at configure time from `tools/keys/trust/<variant>.pub-list` (CMake cache variable `VELORA_LIBRARIAN_TRUST_VARIANT`, default `dev`; `VELORA_DEV_KEYS_DIR` adds a developer's own local dev key to the dev variant; a dev key can never be embedded into a production variant - the configure step refuses) and `trust.hpp` exposes them. The committed `dev.pub-list` is empty, so a stock dev build trusts nothing and refuses every install (`untrusted_key`) until `--trust-list FILE` (the `*.pub-list` format of `tools/keys/README.md`) or `VELORA_DEV_KEYS_DIR` names a key. **Configure refuses a dev trust variant in a `Release` / `MinSizeRel` / `-DVELORA_PRODUCTION=ON` build** (ctest `librarian_dev_trust_refused_in_release`), and `--trust-list` is compiled out of every other variant (`resolve_trusted_keys_for_variant` refuses it unless the variant is `dev`; tested per variant, and the real `librarian-service --trust-list` is started by `librarian_install_service_test`). With no trusted keys, the startup reconcile does not examine the Factory libraries on disk: their rows are kept and each is reported in `skipped`.

**No cipher, proven on the real binaries.** The librarian links `velora_format_verify` (libcrypto's `EVP_PKEY_new_raw_public_key`, `EVP_PKEY_free`, `EVP_MD_CTX_new/free`, `EVP_DigestVerifyInit`, `EVP_DigestVerify` for Ed25519, nothing else) and never `velora_format_decrypt` or the protected writer. ctest `format_no_decrypt` and `librarian_binaries_no_cipher` (`tools/ci/check_format_no_decrypt.py`) scan the symbols of `librarian`, `librarian-service`, `librarian_test_cli` and their libraries for any cipher/decrypt symbol and for any crypto-library import outside the six verification calls, and require that the scanned code really defines `ed25519_verify_strict` and the install path (not vacuous); `librarian_binary_links_verify_only` does the same for the shipped CLI alone; `librarian_no_crypto` (`tools/ci/check_librarian_no_crypto.py`) walks the link line (`OpenSSL::Crypto` as a shared libcrypto is the one allowed crypto library, also as a `$<LINK_ONLY:...>` private dependency) and the symbols; `content_pack_flagged_by_no_decrypt_scan` shows the same scanner flags the content-pack tool. The generated trust header's `kTrustedContentKeys` (public signing keys) is not a content key and is exempt from the name rule.

**Tests.** `librarian_install_test` (library and CLI): the happy path (files byte for byte, vault and report not copied, `current`), idempotence and revisions, downgrade / unprotected / user library / missing pack, trust (untrusted, empty, mismatched or duplicated list, a flipped signature bit, the trust-list parser), the library id, the wrapped key (every structural defect, missing, ambiguous, named; the documented payload limit), the **flipped-bit sweep** (1,078 single-bit flips of `bundle.vlrp`: every byte of the header, manifest, index and seal and a stride through the blocks; every byte of `catalog.meta` and `catalog.meta.sig`; 12 truncations and an appended byte) with the EXACT reason per class (seal key id `untrusted_key`, signature `signature_invalid`, magic / algorithm / reserved / header / manifest / index `corrupt_bundle`, version word `not_protected` / `unsupported_version` / `corrupt_bundle`, blocks `block_hash_mismatch`, the two meta files `catalog_meta_invalid` only) and, after EVERY flip, nothing published, no staging file and an empty catalog; validly re-signed bundles that are still wrong (`hash_list_mismatch`, `unsupported_version`, `corrupt_bundle`); headers that claim 2 GiB manifests / 1.5 GiB indexes and untrusted signers with 60-384 MiB claims, with the largest allocation counted (under 1 MiB); FIFOs in place of each of the four files; the staging lock held (`busy`); unknown free space; a relative bundle path; replay / same-revision / higher / rollback-as-higher revisions; pruning to two bundles and a lease surviving it; a damaged copy replaced; per-device wrapped-key redelivery; the catalog.meta binding (another bundle's seal, a validly sealed list that does not match the instruments, another signer, missing files), bounded memory, space (one byte short / exact), cancel, drive removal (versus a plain I/O error), a fault at every stage and the recovery, Factory rows through `describe_bundle` and reconcile (incl. block damage kept out of the catalog, a catalog rebuilt from bundles), and the CLI exit codes. `librarian_install_service_test` (a real socket client): the job and its events (RUNNING with bytes), Factory next to User rows with `kind`, idempotent re-install, every rejection with its `install_reason` and no change to catalog or tree, the request refusals (incl. FIFOs), space, QUEUE_FULL, a queued install cancelled before it starts, space reserved beside a running install and an import, SHUTTING_DOWN, cancel, DRIVE_REMOVED (and the old-client downgrade), `FACTORY_LIBRARY_PROTECTED` (an SFZ, an SF2 with checkpoints refused before conversion, a request whose second item clashes, the window before the catalog row, the real `librarian import` CLI process), the single-writer guard of the install CLI, restart with and without trusted keys, the schema 3 migration with a failure injected into the 2 -> 3 step and the tolerated already-migrated column, and the real `librarian-service --trust-list` binary. `librarian_e2e_pack_install_test` runs the real `velora-content-pack` and `librarian install-protected` processes on a tiny SFZ with generated dev keys and lists the Factory row from a service. **Crash campaign** (`librarian_crash_campaign`, last part): 40 SIGKILLs of an install - 28 at the stage points and boundaries (below) and 12 mid-copy (`copy-chunk` kill points over a seven-chunk bundle, restarted through the real `LibraryService::start`) - the CLI at each of the 7 stage points on a fresh root and over the previous revision, the service at each of its 10 boundaries (4 over the previous revision) - each followed by a restart, the import campaign's invariants (a listed library has a verifying `current`, every library with a usable `current` is listed, no staging or `.current.tmp` leftovers, reconcile idempotent), bundle directories byte-identical to the pack, `current` the old or the new bundle, and a completed re-run ending in the new bundle.

**Limits.** The wrapped key is checked by structure only (above). Verification is of ciphertext integrity and authenticity: whether a key unlocks the library is first known when the engine opens it (Story 6.5: `wrong_content_key`, `key_unavailable`; `engine/host/README.md`, "Protected libraries"). `current` flips to the newest installed revision; there is no rollback command (a rollback is a new, higher pack revision). The install runs on the worker, so a long install delays imports (and the reverse), like any job.

## Additive import options for the content-pack tool (Story 6.3)

`ImportOptions::library_id` (a canonical UUID) replaces the path-derived library id; `ImportOptions::bundle_sink` is a build-only mode: the source is parsed,
loaded, encoded and verified bit for bit exactly as for a user import, but in memory, with no data root, lock, staging directory, rename or `current` flip,
and the verified `BundleWriter` (plus the `catalog.meta` text) goes to the sink. `tools/content-pack` uses both and finalizes the writer as a protected bundle, so
vendor content takes the same import path as user content without a plaintext bundle ever touching the disk. The librarian service and CLI never set either
option, so user imports are unchanged (they stay unencrypted and path-identified, FR24), and the librarian still links no cipher.

## Known limitations (Story 5.8)

- **No hard links / shared block store.** A bundle is one file, so each checkpoint rewrites the finished timbres (time and flash wear grow with timbres squared over the interval). A block store separate from the manifest
  would allow hard links and incremental checkpoints; it is a format change, deferred.
- **Memory.** The whole converted library is held encoded in RAM until it is published (no streaming finalize): the peak is about twice the library (the encoded blocks and the bytes being written). The default byte threshold
  of 20 GiB means "at most one checkpoint per 20 GiB of PCM" and does NOT bound RAM; the board's default must be lowered at integration (or the writer made streaming).
- **Resolver-level Import again test.** Import again is tested through `LibraryResolver` (what the Live Set loader calls per part), not through a full `LiveSetLoader` load.
- **Leases are not enumerated.** Deleting a superseded checkpoint relies on the lease being an open descriptor; a process that only knows the path (a resolver call racing the flip) retries.

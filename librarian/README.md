# librarian

POSIX only (uses `flock`, `fsync`, `rename`, symlinks; its tests also use `fork`): macOS and Linux, not Windows.

Role: imports an SFZ library or a SoundFont 2 file into an immutable formatlib bundle and publishes it atomically under
`<root>/lib/<library_id>/` (Story 1.13, SF2 in Story 5.3). Runs on the Mac and on the board; the full import UX, categories UI,
crypto and streaming are later stories (Epic 5 and beyond). Target `velora_librarian`, CLI `librarian`.

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
(a mistyped root is an error, never created); the file argument must come first.

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

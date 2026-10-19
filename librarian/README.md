# librarian

POSIX only (uses `flock`, `fsync`, `rename`, symlinks; its tests also use `fork`): macOS and Linux, not Windows.

Role: imports an SFZ library into an immutable formatlib bundle and publishes it atomically under
`<root>/lib/<library_id>/` (Story 1.13). Runs on the Mac and on the board; the full import UX, categories UI,
SF2, crypto and streaming are later stories (Epic 5 and beyond). Target `velora_librarian`, CLI `librarian`.

```
build/librarian/librarian import <file.sfz | dir>... --root <data root> [--base DIR] [--category NAME] [--report FILE|-]
```

Exit code 0 = imported (or identical bundle already current), 1 = nothing published for at least one file (reasons on
stderr, one line each, `file:line: ...`), 2 = usage. `--base` is the directory the `instrument_key` is relative to (default: the SFZ's own
directory, so `Piano.sfz` gets the key `Piano.sfz`). `--category` is stored as `<bundle_dir>/category`, written inside the staging directory so it is part of the atomic publish
(placeholder until the Epic 5 catalog; an identical re-import refreshes it before the flip). `--root` must be an existing directory
(a mistyped root is an error, never created); the file argument must come first.

Identity: `library_id` = UUIDv5 of the SFZ's canonical absolute path (formatlib `set_source_id`), so changing a sample
keeps the `library_id` and yields a new `bundle_id`; moving the SFZ makes a new library. `bundle_id` is the manifest
hash. `instrument_key` is the normalized `.sfz` path relative to the base directory.

Batch import and the import report (Story 5.1): several files and/or directories (searched recursively for `*.sfz`,
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

Real-library acceptance (manual, performed by the founder): set `VELORA_SFZ_DEMO_DIR` to a directory containing the
Salamander Grand Piano and/or a VSCO2 instrument and run

```
VELORA_SFZ_DEMO_DIR=~/libs ctest --test-dir build -R librarian_demo_acceptance --output-on-failure
```

It imports every `*.sfz` below it into a temporary root and checks the bundle's region count equals the `<region>` count of
the SFZ sources (parser count and an independent text scan). Without the variable the test is reported as skipped
(exit code 77). A library using opcodes outside the subset (for example `amp_veltrack`) is reported as a failure with the
offending file:line, which is the intended behaviour of the subset.

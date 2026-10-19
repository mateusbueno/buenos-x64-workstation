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
directory, so `Piano.sfz` gets the key `Piano.sfz`). `--category` is stored as `<bundle_dir>/category`, written inside the staging directory so it is part of the atomic publish
(placeholder until the Epic 5 catalog; an identical re-import refreshes it before the flip). `--root` must be an existing directory
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

# librarian

POSIX only (uses `flock`, `fsync`, `rename`, symlinks; its tests also use `fork`): macOS and Linux, not Windows.

Role: imports an SFZ library into an immutable formatlib bundle and publishes it atomically under
`<root>/lib/<library_id>/` (Story 1.13). Runs on the Mac and on the board; the full import UX, categories UI,
SF2, crypto and streaming are later stories (Epic 5 and beyond). Target `velora_librarian`, CLI `librarian`.

```
build/librarian/librarian import <file.sfz> --root <data root> [--base DIR] [--category NAME]
```

Exit code 0 = imported (or identical bundle already current), 1 = nothing published (reasons on stderr, one line each,
`file:line: ...`), 2 = usage. `--base` is the directory the `instrument_key` is relative to (default: the SFZ's own
directory, so `Piano.sfz` gets the key `Piano.sfz`). `--category` is stored as `<bundle_dir>/category`, written inside the staging directory so it is part of the atomic publish
(placeholder until the Epic 5 catalog; an identical re-import refreshes it before the flip). `--root` must be an existing directory
(a mistyped root is an error, never created); the file argument must come first.

Identity: `library_id` = UUIDv5 of the SFZ's canonical absolute path (formatlib `set_source_id`), so changing a sample
keeps the `library_id` and yields a new `bundle_id`; moving the SFZ makes a new library. `bundle_id` is the manifest
hash. `instrument_key` is the normalized `.sfz` path relative to the base directory.

Supported SFZ subset: see `formatlib/README.md`. Any opcode, header or value outside it skips the whole file with a
per-file reason listing every offender (e.g. `Piano.sfz:12: unsupported opcode 'cutoff'`); nothing is published and the
previous `current` stays. The importer never resamples or requantizes: samples must be WAV, 16/24-bit PCM or 32-bit
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

Tests: `librarian_test` (every I/O-matrix row with synthetic SFZ and generated WAVs: field-by-field region comparison,
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

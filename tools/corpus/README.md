# tools/corpus

Role: Story 5.4, the reference corpus job (SoundFont libraries since Story 13.11, see "SF2 libraries"). It fetches (or reuses a verified cached copy of) each real SFZ library named in
`manifest.json`, imports it with the host importer (`librarian import`), renders a per-library MIDI test suite through the sample engine
and through the sfizz oracle, null-tests the two renders within the library's recorded tolerance and writes a report. It reuses the existing
tools (`build/librarian/librarian`, `velora-offline-render --engine sample`, `velora-null-test`, `tools/oracle/render-oracle.sh`); there is no
second importer or second oracle. Test tooling only: nothing here is linked into a shipped target.

```
python3 tools/corpus/corpus.py check [--require-pins] [--local ID=PATH ...]   # manifest valid? (offline)
python3 tools/corpus/corpus.py fetch [--pin] [--deep] [--only ID ...] [--local ID=PATH ...]
python3 tools/corpus/corpus.py run   [--only ID ...] [--out DIR] [--pin] [--deep] [--keep-work] [--timeout S] [--local ID=PATH ...]
python3 tools/corpus/corpus.py ids   [--no-local]            # ids of the included entries (the nightly workflow uses --no-local)
python3 tools/corpus/corpus.py guard                         # fails if corpus audio is in the repository tree
python3 -m unittest discover -s tools/corpus/tests           # offline unit tests (fake archives, no network)
```

`run` needs `build/` (cmake targets `librarian`, `velora-offline-render`, `velora-null-test`) and the oracle
(`tools/oracle/build-sfizz-render.sh`, or `VELORA_SFIZZ_RENDER`). A missing tool is a **failure**, never a skip. Exit code 0 only when every included
library fetched, hash-verified, imported (every `.sfz` that is not listed in `skip_files`) and passed every null test. Cache: `~/.cache/velora-corpus`
(`VELORA_CORPUS_CACHE` or `--cache`), always outside the repository. Report: `build/corpus-report/report.md` and `report.json` (no audio; the
renders are deleted after each null test, the logs stay in `work/`).

## Manifest (`manifest.json`)

One entry per library. Included entries have `source_url` (https, allow-listed hosts only: `codeload.github.com`, `github.com`,
`raw.githubusercontent.com`, `objects.githubusercontent.com`), `version`, `commit`, `sha256` (of the archive), `license`, `license_url`, optional
`tolerance_db` (default `default_tolerance_db`, -60), `skip_files` (`path` + `reason`), `null_files`, `null_exclude` (`path` + a `reason` of at least 40 characters), `limits`. An excluded entry has `excluded.reason`
(at least 10 characters) and is never fetched; the report lists every exclusion with its reason. "No corpus file is skipped except entries explicitly listed
with a reason" is enforced three ways: an `.sfz` that fails or skips on import fails the job unless it is in `skip_files`; a stale `skip_files` entry fails; an
entry that is neither complete nor excluded is a manifest error.

**Scope rule for `skip_files`** (Story 13.1, `_bmad-output/planning-artifacts/sfz-scope-decision.md`, ratified by the founder, 2026-10-28): `skip_files` lists
only files that need an SFZ tier marked **out of scope** in that record, and the `reason` names the tier and the opcodes the importer reports as
`unsupported_features` (FR22). A file that fails import for any other reason (an opcode of a supported or deferred tier that is not implemented yet, a damaged file)
is not listed: it fails the job, so the gap stays visible. The corpus requirement reads "imports and plays with mapping intact for every file in a supported tier".

## Why a library does not import (import-failure reasons)

For every file that does not import, the report classifies each importer message (the librarian's per-file `messages`, falling back to `offenders`) and shows, per
library, a table of counts by reason with an example `file:line` (relative to the library; an include file is named as such). `report.json` carries the same data as
`import_failure_reasons: {files_failed, rows: [{category, title, detail, count, files, example}]}` (`count` = issues, `files` = distinct instrument files).
Categories, in table order:

| Category | Detail column | Derived from |
|---|---|---|
| unsupported opcode | the opcode (or `header <name>`) | `unsupported opcode` / `unsupported header` issues |
| unsupported value | the opcode whose value is outside the subset (e.g. `ampeg_dynamic`, `polyphony`, `on_locc64`) | `unsupported value 'v' for opcode 'o'` |
| sample path escapes the SFZ directory | `..` relative (slash or backslash separators) or absolute path | `path '...' leaves the SFZ directory` (the importer files it under *damaged*) |
| sample format not supported | the extension of the referenced sample (`.flac`, `.ogg`, ...), or `.wav (format outside the subset)` | `unsupported audio` messages, or a missing sample whose extension is not `.wav` (only WAV is read) |
| missing sample file | `sample= file does not exist` | `sample file not found` with a `.wav` extension |
| include fragment | `no <region>` | `no regions in SFZ file`: the file is not a standalone instrument (e.g. `includes/*.sfz`) |
| damaged / parse | the importer's wording (syntax, invalid value, region without `sample=`, damaged WAV) | the remaining *damaged* kinds |
| other | the raw message | anything not recognised, so a new importer message is visible instead of hidden |

The table replaces the old opcode histogram line (`unsupported_histogram` stays in `report.json`, built from the log text). SF2 libraries use the same table
(their messages mostly land in *damaged / other*). Tests: `tests/test_import_reasons.py` (synthetic report lines).

## Local sources (`source_kind: "local_dir"`)

Some libraries are supplied by the founder from local disk (the Pianobook packs: no scripted download, no licence statement to rely on). Such an entry
has `source_kind: "local_dir"`, `local_hint` (a machine-independent description, never an absolute or home path; the manifest check refuses `/...`,
`/Users/` and `/home/`), `version`, `license`, `license_url`, optional `source_page`, and `expected_tree_sha256` instead of `source_url` / `sha256`.

- **Where the files are**: `--local ID=PATH` (repeatable) or the environment variable `VELORA_CORPUS_LOCAL_<ID>` (id upper-cased, every non-alphanumeric
  character becomes `_`, e.g. `VELORA_CORPUS_LOCAL_PIANOBOOK_MANTHEY_KLAVIANO`). The flag wins. A path inside the repository tree is refused, and so is a cache inside it.
- **Copy, never use in place**: only `.sfz` and audio files (not dot-files) are copied into `<cache>/<id>/<tree digest>/tree`. Images, `.dspreset`, readmes,
  `.DS_Store` and everything else stay behind. Hardening matches archive extraction: a symlinked top directory, any symlink or special file anywhere below
  it (even an unneeded one), unsafe/over-long/duplicate (case- and Unicode-insensitive) names, and the per-file / total / member limits fail the entry;
  sources are opened `O_NOFOLLOW` and must still be regular files of the planned size, targets are `O_EXCL | O_NOFOLLOW`, mode 0644, sizes re-checked while
  streaming. The source folder is only ever read (a unit test compares size, mtime and hash before and after).
- **The pin** is a deterministic tree digest: sha256 over the lines `relative path NUL size NUL sha256(file) LF` of the copied files, sorted by relative path.
  Trust on first use exactly as for archives (`fetch --pin` writes `expected_tree_sha256` once, never overwrites; nobody attested the first copy), then any
  difference is `HASH MISMATCH`, a hard failure. `fetch --deep` recomputes the tree digest from the cache and re-hashes every byte.
- **A machine without the files**: the entry fails with `local source not available on this machine (set VELORA_CORPUS_LOCAL_<ID> or pass --local ...)`. It is
  never skipped silently. The only way to leave such an entry out is to list it under `excluded` with a reason in the manifest (or select others with `--only`).
  One deliberate convenience: when a verified cached copy for the pinned digest exists (listing check, `--deep` for all bytes) it is used without the source, since
  its bytes are already proven equal to the pin.
- **Licence field**: records what the package states (nothing, for the two current entries), where the work comes from, and that distribution needs a licence
  the founder must confirm from the pack page. No licence is invented. These libraries are for local internal testing, never redistributed or committed.

## SF2 libraries (`"format": "sf2"`, Story 13.11)

An entry with `"format": "sf2"` (default `sfz`) is a library of SoundFont 2 files. Everything about getting the files is unchanged: the same `source_url` (allow-listed https
hosts) with an archive hash, or `source_kind: "local_dir"` with a tree digest, the same trust-on-first-use pin, the same hardened extraction and local copy (`.sf2` is copied
like audio, never committed; `corpus.py guard` and `.gitignore` treat it as audio). What differs is the runner (`Runner.library_sf2`):

- **Import**: every `.sf2` of the library (except `skip_files`) goes through `librarian import`, i.e. the SF2 reader of Story 5.3, in one run. A file that is skipped or fails fails the
  job, with the importer's own message in the report (for example `sample rate 35135 Hz (the importer never resamples ...)`); a stale `skip_files`, `null_exclude` or
  `tolerance_overrides` path fails too.
- **What is null-tested**: presets, not files. The instrument key of an SF2 preset is `bank:program`; the runner lists them from the file's `phdr` chunk (`sf2_presets`) and tests up to
  `max_null_instruments` of them, spread evenly over the sorted (file, bank, program) list of `null_files` (default: all imported files), or exactly the ones named in
  `null_presets: ["dir/file.sf2#bank:program", ...]` (a name that is not an imported preset fails the job). All six probes run per preset.
- **Oracle**: the existing FluidSynth oracle of Story 5.3 (`tools/oracle/render-fluidsynth.sh`, FluidSynth 2.6.1 built by `build-fluidsynth.sh`, or `VELORA_FLUIDSYNTH`); no other
  fluidsynth is used, no second oracle exists. A preset needs at least one probe that FluidSynth does not render as silence (all six silent = the stimulus does not reach the preset = failure), duplicate `bank:program` keys, a bank above 16383
  or a program above 127 fail, and a run that null-tests no preset at all fails. A run that selects an SF2 entry needs the oracle (missing = failure); a run of only SF2 entries does not need sfizz and the reverse.
- **MIDI**: the same six probes as for SFZ, written for FluidSynth's player (`midi_suite.build_fluid`): 375 PPQ (1 tick = 64 frames), every event rounded to a multiple of 3 ticks (4 ms),
  the preset selected on channel 0 with MMA bank select (CC0 = bank / 128, CC32 = bank % 128), a program change and CC7 = 127; the engine gets the same MIDI one tick later, because FluidSynth
  applies events one block late (`tools/offline-render/tests/sf2_suite/README.md`, "MIDI timing"). The sfizz half-frame offsets do not apply.
- **Comparison**: engine `--gain-db 0` (the oracle's `-g 0.8409` already undoes its filter gain), the oracle render trimmed after its last non-zero frame plus 100 ms (the player appends 2 s of
  silence; the engine gets the matching `--tail-seconds`), `velora-null-test --max-offset-frames 1 --check-excess`, no `--candidate-quantize` (FluidSynth dithers its 16-bit output; sfizz does not).
  A probe that FluidSynth renders as digital silence for a preset (its zones do not cover the probe's keys) is marked `oracle silent` and passes only if the null test passes on it.
- **Tolerance**: `tolerance_db` is **mandatory** on an SF2 entry (the -60 dB default of the SFZ entries does not apply) and is recorded with the measurement in `notes`; per-file values go in
  `tolerance_overrides` with a reason. What the measurement is up against: the Story 5.3 suite reaches -40 dB only because its fixtures are written for FluidSynth (zones with attacks of at least
  15 ms, `for_oracle` rewrites of the oracle's copy). A real library is not rewritten, so FluidSynth's departures from the SoundFont specification (attenuation x0.4, sustain depth, decay and release
  time on a 96 dB range, a 0.2-frame filter delay that grows with frequency, a 64-frame amplitude ramp at the start of a voice) are in the residual. Trial runs on three small packs
  from the founder's disk (not in the manifest, no pin, no licence statement) measured -26 to -33 dB on the six probes (a generated sine SoundFont: -28 to -41 dB), high-register `range` and `sustained_loops` being the worst.
- **Report**: `report.md` / `report.json` list SF2 and SFZ entries in one table with a Format column; an SF2 instrument line names the file, `bank:program` and the preset name. The exit code is 1
  when any entry, of either format, fails to fetch, import or null-test.
- **No entry yet**: the manifest has no SF2 entry, because none can be pinned yet (no SF2 pack with a licence statement and a scripted source exists, and the founder's local packs are not
  pinned). Add one with `fetch --pin` / `run --pin` exactly as for a Pianobook pack (`source_kind: "local_dir"`).

## Hash pinning is trust-on-first-use

The pin is the SHA-256 of the **archive bytes**. Sources are GitHub `codeload` tarballs addressed by commit SHA, so the content is already
content-addressed by git; the archive hash additionally protects against a changed tarball encoding or a tampered mirror. The first download of an
entry without a pin (`fetch --pin` / `run --pin`) writes the hash it saw into the manifest (never over an existing pin). That first download is trusted
as is: nobody independently attested it. From then on a different hash is a hard failure ("HASH MISMATCH"). GitHub does not promise that its
generated tarballs stay byte-identical forever; if a pinned archive stops matching, investigate (compare the extracted tree digest in the cache stamp), then
re-pin in a reviewed commit. Review the pin lines of the manifest like any dependency lock.

## Fetch and extraction hardening

- https only, no credentials, redirects re-checked against the allow-list, the download streams to `.part` and is limited by `max_archive_bytes`
  (a server that lies about `Content-Length` is stopped while streaming).
- The archive is treated as untrusted. Its member list is validated completely before anything is written: no absolute paths or drive letters, no `..`,
  no backslashes, no NUL, no symlinks, hardlinks or special files (any link is refused, even a harmless one), no duplicates (case-insensitive, because
  macOS merges `A.wav` and `a.wav`), no file below a file, member count, per-file, total and compression-ratio limits.
- Files are created with `O_EXCL | O_NOFOLLOW` into a directory this process just created, with normalised modes (no setuid, no execute bits); sizes
  are re-checked while writing. Nothing from an archive is ever executed, imported as code or run from the cache; the tools read it only as data.
- Extraction happens in a private `.stage-<pid>` directory and becomes `<cache>/<id>/<archive sha256>/` by one `rename`, so a crash leaves no tree that looks
  complete. A stamp records the archive hash, a listing digest (paths and sizes) and a content digest.
- Reuse: the listing digest is re-checked on every run (catches added, removed, truncated or replaced-by-other-size files and planted symlinks); `--deep`
  re-hashes every byte. Same-size edits behind our back are only caught by `--deep`.
- Residual (documented, not solved): the cache directory is trusted to belong to the current user. A local attacker who can write there between the
  listing check and the import (time-of-check / time-of-use) is outside the model; use a private cache directory (mode 0700) on shared machines.
  The importer and the oracle read the same files that were verified, so the window is the run itself.

## The per-library MIDI suite (`midi_suite.py`)

Generated in code, six probes, library-independent: `range` (keys 21..108 in steps of 3), `velocity` (keys 36/60/84 at velocities 1..127), `round_robin`
(ten repeats on 48/60/72), `keyswitches` (every key 0..47 pressed briefly, each followed by two notes), `releases` (short, long and pedal-held notes, release
triggers), `sustained_loops` (chords and notes held 8-10 s). Each starts with CC7 = 127. For each library the runner null-tests up to `max_null_instruments`
instruments (the smallest, a middle one and the largest by region count, or the ones named in `null_files`) with all six probes; **every** `.sfz` of the
library is imported. Rendering every instrument of a 100-file library six times is a nightly-length cost the founder can opt into by raising the number.

## Null test and tolerance

Per render: `velora-offline-render --engine sample --gain-db -7.35 --oracle-block-frames 1` against `render-oracle.sh` (sfizz 1.2.3, Hermite, 48 kHz,
`VELORA_SFIZZ_BLOCKSIZE=1`), compared with `velora-null-test --max-offset-frames 0 --check-excess --reference-decode sfizz-s16 --candidate-quantize sfizz-s16`, as the Story 1.15 / 5.2
harness except for the block size and the candidate quantisation (see below). The tolerance is recorded per library in the manifest (`tolerance_db`, default -60 dB) together with the
reason for any value other than the default. A tolerance is never changed to make a run pass without a written reason in the manifest and the story spec.

What the VSCO 2 CE triage (Story 5.4) found about the comparison itself, so that a residual is read correctly. None of it is an engine defect; each item is
handled in the harness, not by loosening the tolerance:

- **Event times sit half a frame after a frame boundary.** `sfizz_render` advances its MIDI clock by adding 1/48000 s per frame and fires an event on the first
  frame whose accumulated time is at least the event time. For an event exactly on a frame boundary the double rounding decides between frame N and N-1, so
  with the old 1/960 s grid (50 frames per tick, every event on a boundary) some notes started one frame early (4800 right, 14400 one frame early) and nulled at
  -12 dB on a bright bell transient. `midi_suite.py` now uses one tick per frame (24000 PPQ) and a first tick of 1.5 frames, so every event is at k + 0.5 frames; sfizz
  and the engine (exact integer arithmetic in `smf.cpp`) both use floor and agree. A unit test checks the property.
- **Block size 1 for the oracle.** sfizz keeps a voice's read position as a float32 cumulative sum per block (`Voice::fillWithData`), which makes its playback speed about
  0.6 ppm off for a 44.1 kHz sample at 48 kHz: 0.27 frames after 10 s (a simulation of exactly that sum gives 0.249, the measurement 0.27; the engine, which accumulates in double,
  tracks an exact Hermite resample to 0.003 frames). A 10 s bell therefore nulls at -53.5 dB at the oracle's block size 64, -60.8 at 16, -70.8 at 2 and -72.2 at 1.
  The block size is a command line option of `sfizz_render` (the oracle itself is untouched), so the corpus job runs the oracle and the engine's `--oracle-block-frames` at 1.
  At block size 1 the block-relative note delay is always 0, which also removes sfizz's `ampeg_dynamic` defect (below). The 64-frame Story 1.15 / 5.2 suite is unchanged.
- **`ampeg_dynamic=1` in sfizz** (`ADSREnvelope::getBlock` in dynamic mode calls `updateValues(0)` on its first chunk, which overwrites the start delay of a note that does not begin
  on a block boundary with 0): the amplitude attack of such a note is cut short (VSCO `ampeg_attack` 1 ms, note at frame 4832: residual -34 dB, the same SFZ without the flag -61 dB).
  By the SFZ definition the flag only makes the envelope re-read `ampeg_*_oncc` / `vel2` values while a voice sounds. Since Story 13.5 the importer reads `ampeg_*_oncc` once at voice start and rejects a region that combines them with `ampeg_dynamic=1`; without an `_oncc` the flag changes nothing (`vel2` reads the constant trigger velocity), so the engine's specified attack is right.
- **Controllers that move while a note sounds (Story 13.5).** sfizz re-evaluates `amplitude_oncc`, `volume_oncc`, `pan_oncc` and `tune_oncc` (and the `_cc` / `_curvecc` forms) every block; the
  engine reads the controller once when the note starts (`engine/core/README.md`, "CC at voice start"). The corpus probes move CC64 (a sustain pedal that goes down while the keys are held and
  lifts later) and nothing else, so only an instrument that maps CC64 to one of those targets diverges, as soon as the pedal moves under a sounding voice; gates (`locc64`/`hicc64`) and
  `ampeg_*_oncc64` are read at the note on in both and agree. Such an instrument is imported and counted, and is kept out of the automatic null picks with a manifest entry
  `"null_exclude": [{"path": "...", "reason": "..."}]` (the reason must name the controller, the target and the probe; the report prints it; a stale entry fails the job; an
  instrument cannot be in `null_files` and `null_exclude` at once). `null_files` still forces a pick when the divergence itself is what is being measured.
- **The engine tail follows the oracle.** `sfizz_render` renders until silence (VSCO `ampeg_release` is up to 15 s); the runner measures the oracle file and gives the engine the same tail, otherwise the comparison covers a truncated tail.
- **Random layers are null-tested with the sfizz random replay (Story 13.10).** An instrument that chooses a layer with `lorand`/`hirand` (19 of the 75 VSCO files) cannot agree with sfizz by seeding:
  sfizz draws from a fixed-seed Numerical Recipes LCG (`Random::randomGenerator`, seed 0, no setter) once per note-on, note-off and controller, the engine from its own seeded xorshift64*. The
  corpus render of such an instrument therefore passes `--sfizz-random-replay` to `velora-offline-render` (test only, off everywhere else): the renderer computes from the MIDI events the number sfizz
  uses for each note event and, just before the event, re-seeds the engine through the public `SampleEngine::set_seed` so that the engine's first draw is that number. The production generator is not
  changed. The replay is exact for the layer choice only; an instrument whose SFZ also uses `off_by` (choke groups: an extra draw per choke) or any `*_random` opcode (per-voice draws) is not
  aligned, is still imported, and is left out of the automatic picks while another instrument exists (the report lists it with the opcode); `null_files` can still force it. The manifest of VSCO 2 CE lists
  `FluteStac.sfz` and `TrumpetStac.sfz` in `null_files`. Details: `_bmad-output/implementation-artifacts/spec-13-10-random-layer-parity-with-the-null-oracle.md`.
- **16-bit oracle floor, and why the candidate is quantised.** sfizz's output is 16-bit, a rounding noise of about -101 dBFS per sample. Relative to a quiet instrument that alone is the whole residual:
  the VSCO oboe `round_robin` probe (mean level about -47 dBFS) shows -54.4 dB with every real difference below 0.3 LSB, the GM percussion about -38 dB. `--candidate-quantize sfizz-s16` runs the engine's float
  output through the same dr_wav conversion before the comparison (a deterministic model of the oracle's output stage, nothing statistical): the integers are then identical wherever the two float renders agree
  to well below one LSB (99.89 % identical samples in that oboe probe, -71.5 dB). A real error of one LSB or more still shows in full.

## Licences

Local Pianobook entries record "no licence stated" (see Local sources). Every other entry carries the licence name and the URL of the licence text at the pinned commit. Sonatina Symphonic Orchestra is Creative Commons Sampling Plus 1.0
(restrictions recorded in the manifest notes). The audio is used for internal tests only and is never committed, redistributed or shipped.

## Corpus audio is never committed

`.gitignore` ignores the cache-like directories and every audio extension under `tools/corpus/`; `corpus.py guard` (run in the nightly workflow and by the unit
tests) fails when git would add an audio file outside the small test-fixture directories, or one above 8 MB anywhere.

## CI

`.github/workflows/corpus-nightly.yml` is `workflow_dispatch` only (CI is manual-only for this project). `local_dir` entries cannot run on a hosted runner (the founder's
files are not there): the workflow runs only the entries printed by `corpus.py ids --no-local`, says so with a `::notice::` listing the local entries it did not run, and
the `only` input can name entries explicitly (a named local entry then fails with "local source not available"). Run those entries on the founder's Mac with the commands above. The nightly `schedule:` is present as a comment to enable when decided.

The workflow builds only the sfizz oracle, so `corpus.py ids --no-local` also leaves out every `format: "sf2"` entry and prints a `::notice::` naming it on stderr; naming one in the `only` input fails
its preflight (no FluidSynth) until a build step for `tools/oracle/build-fluidsynth.sh` is added to the workflow. None exists yet (not changed in Story 13.11; CI is not triggered).
The local equivalent is the commands above.

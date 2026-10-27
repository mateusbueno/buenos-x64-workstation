# tools/corpus

Role: Story 5.4, the reference corpus job. It fetches (or reuses a verified cached copy of) each real SFZ library named in
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
`tolerance_db` (default `default_tolerance_db`, -60), `skip_files` (`path` + `reason`), `null_files`, `limits`. An excluded entry has `excluded.reason`
(at least 10 characters) and is never fetched; the report lists every exclusion with its reason. "No corpus file is skipped except entries explicitly listed
with a reason" is enforced three ways: an `.sfz` that fails or skips on import fails the job unless it is in `skip_files`; a stale `skip_files` entry fails; an
entry that is neither complete nor excluded is a manifest error.

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

Per render: `velora-offline-render --engine sample --gain-db -7.35 --oracle-block-frames 64` against `render-oracle.sh` (sfizz 1.2.3, Hermite, 48 kHz),
compared with `velora-null-test --max-offset-frames 0 --check-excess --reference-decode sfizz-s16`, exactly as the Story 1.15 / 5.2 harness. The tolerance is
recorded per library in the manifest (`tolerance_db`, default -60 dB) together with the reason for any value other than the default. A tolerance is never
changed to make a run pass without a written reason in the manifest and the story spec.

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
Local equivalent: the commands above.

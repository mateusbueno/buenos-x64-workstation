---
title: 'Story 5.4: Reference corpus imports and plays with mapping intact (Mac)'
type: 'feature'
created: '2026-10-27'
status: 'in-progress'
baseline_revision: '64cf453'
review_loop_iteration: 0
followup_review_recommended: true
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-1-15-sample-engine-ram-resident-and-sfizz-null-test.md', '{project-root}/_bmad-output/implementation-artifacts/spec-5-1-full-sfz-v1-opcode-subset-in-the-importer.md', '{project-root}/_bmad-output/implementation-artifacts/spec-5-2-sample-engine-keyswitches-crossfades-filters.md', '{project-root}/_bmad-output/implementation-artifacts/spec-11-12-license-check-of-the-vendored-sfizz-parser-and-dependencies.md', '{project-root}/tools/corpus/README.md', '{project-root}/tools/oracle/README.md']
warnings: ['THE STORY IS NOT MET. The pipeline (manifest, fetch, hash pin, safe extract, import, per-library MIDI suite, sfizz null test, report, nightly job definition) is built and was run on real libraries, and the job FAILS, which is what the AC asks of a failing corpus: 9 of the 10 fetched libraries cannot be imported at all (opcodes outside the Story 5.1 subset), and the one that imports completely (VSCO 2 CE, 75 of 75 files) passes 0 of its null-tested renders. Nothing was skipped or loosened to hide that. Pianobook: 2 of >=5 delivered as founder-supplied local folders (kamoepiano301, Manthey Klaviano; both state no licence; both fail import); the other five candidates stay excluded (login gate, no licence).', 'NOT RUN: the nightly workflow (CI is manual-only and was not triggered), the corpus job on Linux, and the real-library RTSan/Docker paths. Everything was run on the Mac only.']
deferred:
  - summary: >-
      Importer/engine coverage of the opcodes the real corpus needs. 9 of 10 libraries (all of Sonatina 747 files, Salamander, Scarypiano, Meatbass, Weresax, Bear Sax, Osiris, Steel Drum, Headroom) are rejected on import by features outside the Story 5.1 subset. By number of FILES affected the families are: CC modulation (`*_oncc<N>` / `*cc<N>` forms of amplitude, pan, tune, ampeg_*, offset, `locc/hicc` region gates, `set_cc/set_hdcc`) in nearly every Karoryfer / Salamander / Osiris / Headroom file; `group` + `off_by` + `off_mode` + `off_time` (mono / choke groups, ~25 of 48 Bear Sax, 21 of 39 Meatbass, 135 of 747 Sonatina); `amp_velcurve_<N>` (33 of 39 Meatbass, 27 of 48 Bear Sax); `amp_veltrack` and `ampeg_vel2*`; `offset_random`; `fil_keytrack/keycenter/veltrack`, `group_volume`, `gain_cc<N>` (Sonatina, 289-373 files each); LFOs (`lfo<N>_*`, `amplfo_*`), `eg<N>_*`, `eq<N>_*`; `note_polyphony`, `bend_up/down`; the `<curve>` header; `lokey=-1 / hikey=-1` (a region that never plays by key; rejected as a damaged value, Salamander). Each of these is a feature of the engine as well, not only the importer. The per-library histograms are in the report (`unsupported_histogram`).
    evidence: |-
      build/corpus-report-rest/report.json and the import logs under build/corpus-report-rest/work/<id>/import.log (not committed); counts reproduced by `python3 tools/corpus/corpus.py run`.
    severity: high
  - summary: >-
      Null test of VSCO 2 CE: 0 of 48 renders (8 instruments x 6 probes) pass the recorded -60 dB tolerance. Residuals -54.4 .. +1.9 dB. Two causes were found, neither fixed: (1) instruments that choose their round robin with `lorand/hirand` (FluteStac, TrumpetStac: residual about 0 dB, the output is a different random layer) cannot be null-tested against sfizz because the two RNGs are not the same: the engine's seeded RNG is reproducible but not sfizz's; the comparison needs either the same generator or a comparison that is independent of which layer was drawn; (2) on instruments with long samples and retriggered notes (TubularBells, ViolaEnsSusVib) the residual is small (about -55 to -60 dB, at the 16-bit floor of the oracle) for some notes and about -10 dB for others at equal level, zero lag and gain 0.98-1.0: at the onset of the diverging notes sfizz's output rises about 7x faster than its own first note and than the engine (ratio sfizz note 2 / sfizz note 1 = 6.8, 4.9, 3.3, ... -> 1 over about 40 samples, where `ampeg_attack` is 1 ms = 48 samples). It is not the voice count (`--voices 512` unchanged), not `ampeg_dynamic` (removing it from the SFZ changes nothing), not the oracle block size (32 / 128 / 1024 give the same bad notes), not a rate problem (the 44.1 kHz stereo 16-bit samples play right on the first note), and the oracle is deterministic (two renders are byte-identical). The root cause is open: either an sfizz envelope behaviour on retriggered voices that the engine does not reproduce, or an engine bug. The remaining instruments (Contrabass, Oboe, Tuba, FHorn, GM percussion) show -15 .. -47 dB with the same signature in part; not analysed one by one.
    evidence: |-
      build/corpus-report-vsco8/report.md (per instrument and probe), scratchpad analysis of TubularBells `round_robin` / a single-key retrigger MIDI at 0.4 s and 2 s spacing; the engine and sfizz first-note samples are identical to 4 digits.
    severity: high
  - summary: >-
      The Pianobook selection (AC: >=5 instruments chosen by the founder) is 2 of >=5 delivered, and neither delivered pack imports. Delivered (founder-supplied local folders, new manifest kind `local_dir`, pinned by tree digest, copied into the cache): kamoepiano301 (washiyamagiken, v1.0) and Manthey Klaviano (Michele Pedrazzi, v1.1). Both packages state NO licence; recorded as local-test use only, never redistributed or committed, licence to be confirmed by the founder from the pack page before any distribution. Still excluded with a reason (login gate, no licence on the page): antz, dead-man-orchestra, kantele-thefinnishzither, jouhikko-thefinnishbowedlyre, accordion-la-melodiosa. Import results: kamoepiano301 rejected (`cutoff_oncc131` x56 and `cutoff_curvecc131` x61 = velocity-as-CC filter modulation through <curve> tables; 5 x header `<curve>`; once each `amp_veltrack`, `ampeg_vel2attack`, `sustain_cc`, `ampeg_release_shape`, `ampeg_releasecc64`, `ampeg_release_curvecc64`; and `lokey=c-2`, an out-of-range note name). Manthey Klaviano rejected (`end` x153, and `trigger=release_key` x1). No importer change was made: nothing there is inert or small and clearly correct. `end` is inert for 152 of 153 regions (end equals or exceeds the sample frame count) but truncates one region (samples/70_p1.wav: end=645179, 701743 frames), and the engine has no sample-end; `release_key` differs from `release` in sfizz (it ignores the sustain pedal), and the importer maps no such mode. Null tests of both: not run (nothing imported). Three further >=5 picks must be named by the founder, or other freely available instruments accepted.
    evidence: |-
      tools/corpus/manifest.json entries `pianobook-*` (excluded.reason) and `kind: extra`.
    severity: medium
  - summary: >-
      Sonatina Symphonic Orchestra licence: Creative Commons Sampling Plus 1.0 (retired by Creative Commons). Verbatim copying is permitted only non-commercially; partial use as samples is permitted, also commercially; the whole work may not be used as a derivative except as an insubstantial portion; advertising use is excluded. The corpus uses it internally for tests and never redistributes it. Whether Velora may ever ship or bundle it is a licence decision that was not taken here. The original torrent and the Zynthian mirror were not used: the maintained GitHub release (Peter Eastman, `peastman/sso` v4.0) was reachable and the author's own continuation.
    evidence: |-
      tools/corpus/manifest.json `sonatina-symphonic-orchestra` license / notes; LICENSE at the pinned commit.
    severity: low
  - summary: >-
      Only up to three instruments per library are null-tested (the smallest, a middle and the largest by region count; `max_null_instruments`, `--max-null N`), while every `.sfz` is imported. A library such as Sonatina (747 files) or VSCO (75) is therefore proven for a sample of its instruments. Rendering all of them is a configuration change (the nightly can afford it), not a code change, but it was not run.
    evidence: |-
      tools/corpus/corpus.py pick_null; manifest `max_null_instruments`.
    severity: low
  - summary: >-
      The per-library MIDI suite is library-independent (it sweeps keys 0..108, velocities 1..127, the pedal, long holds) rather than derived from each instrument's own region map. Keyswitch ranges above key 47, controller-dependent articulation (CC1 / CC11 sweeps; `demo_features.mid` of Story 5.2 does that and is not part of this suite) and release-trigger ranges outside the probes are not exercised. A probe that lands on keys without any region produces silence in both renderers and a meaningless residual (seen as -22 dB on a three-sample synthetic instrument); the report does not flag such renders.
    evidence: |-
      tools/corpus/midi_suite.py; synthetic-suite trial recorded in this spec's triage log.
    severity: low
  - summary: >-
      Hash pins are trust-on-first-use of GitHub `codeload` tarballs; GitHub does not promise byte-stable generated archives. The sources are addressed by commit SHA, which is the stronger pin, but a changed tarball encoding would fail the hash (safe) and need a reviewed re-pin. No independent attestation of the first download exists.
    evidence: |-
      tools/corpus/README.md "Hash pinning is trust-on-first-use".
    severity: low
---

<intent-contract>

## Intent

**Problem:** "Imports and plays with mapping intact" had only been shown on a synthetic library and on an env-gated demo test that had never met a real library (FR20, NFR19, AR2 partial, EXT5 dependency).

**Approach:** One tool, `tools/corpus/`, reuses the existing importer (`librarian import`), renderer (`velora-offline-render --engine sample`), harness (`velora-null-test` with the Story 1.15 settings) and oracle (`tools/oracle/render-oracle.sh`, sfizz 1.2.3); there is no second importer or oracle. It adds a pinned manifest, a fetch with cache and hash verification and hardened extraction, a generated six-probe MIDI suite, a runner that fails on any import or null-test failure, a report, offline unit tests and a manual-only workflow definition.

## Boundaries & Constraints

**Always:** audio never enters the repository (cache outside the tree, `.gitignore` rules, `corpus.py guard` in ctest and in the workflow); https only from the allow-listed hosts; an archive is untrusted data and nothing from it is executed; every exclusion carries a reason and appears in the report; a missing tool is a failure; tolerances are per library in the manifest and are never changed to pass without a written reason.

**Never:** sign in anywhere, accept any site's terms, use credentials or random mirrors; trigger CI; commit corpus files; mark the story done while an AC is unmet.

## Acceptance status

| AC | Status |
|----|--------|
| Manifest pins Salamander, VSCO2, Sonatina, Karoryfer libs, a named Pianobook selection (>=5), each with source URL, version, content hash | MET for Salamander, VSCO2 CE, Sonatina (maintained release), Scarypiano, Meatbass, Weresax, Bear Sax (URL, version, commit, SHA-256 of the archive, licence and licence URL). PARTLY MET for Pianobook: 2 of >=5 delivered (kamoepiano301, Manthey Klaviano; local_dir entries pinned by tree digest; NO licence stated by either package, local-test use only); NOT MET for the rest: five candidates stay explicit exclusions (login gate, no licence on the page) and three more picks are needed. |
| Job fetches or reuses a cached copy, verifies the hash, imports with the host importer; no file skipped except listed with a reason | Fetch / cache / hash: MET (10 libraries, 6.9 GB cache, second run reused the cache). Import: NOT MET: only VSCO 2 CE imports (75 of 75); the other 9 libraries are rejected on import (about 930 files). The files are NOT listed as skipped in the manifest: that would hide the gap, so the job fails. |
| Per-library MIDI suite rendered; each render passes the sfizz null test within its recorded tolerance; report with per-library results and residuals | Suite and report: MET (six probes, per-render residual and worst 100 ms window, exclusions and failures listed). Null tests: NOT MET (0 of 48 VSCO renders pass; the other libraries never reach it). |
| Any import or null-test failure fails the job; corpus audio never committed | MET: `corpus.py run` exits 1 (shown on the real corpus); the guard fails on a planted audio file (unit test and ctest `corpus_no_audio_guard`). |

## Code Map

- `tools/corpus/manifest.json` (10 included, 6 excluded), `corpus_lib.py` (manifest validation, download, safe extract, cache stamps, TOFU pin), `corpus.py` (check / fetch / run / guard, report), `midi_suite.py` (six probes), `tests/test_corpus_lib.py` (36 offline tests), `README.md`.
- `.github/workflows/corpus-nightly.yml` (`workflow_dispatch` only, schedule left as a comment), root `CMakeLists.txt` (ctest `corpus_tooling_unit_tests`, `corpus_manifest_check`, `corpus_no_audio_guard`), `.gitignore`.
- `formatlib/src/sfz_import.cpp`, `formatlib/tests/sfz_test.cpp`, `formatlib/README.md`: `ampeg_dynamic` accepted (see triage).

## Review Triage Log

### 2026-10-27 - Self review (adversarial, same author)
- findings:
  - `[high]` `patch` path traversal / absolute / backslash / drive-letter / NUL member names: the whole member list is validated before anything is written and a refused archive leaves no target directory (tests for tar and zip, including the case where the evil member comes second).
  - `[high]` `patch` symlinks, hardlinks, FIFOs and zip symlink attributes: every link is refused, even a harmless one (a link inside the tree is not needed by any corpus library and an escaping one is the classic attack); files are created with `O_EXCL | O_NOFOLLOW` under a directory this process just created.
  - `[high]` `patch` zip bombs: absolute limits per file, total, member count and a compression-ratio cap; the listing pass of a tar.gz decompresses the stream once, so a capped reader refuses a stream that decompresses beyond the total limit plus 64 MB before any header is trusted; sizes are re-checked while writing (a member larger than announced aborts and removes the target). A truncated gzip, an encrypted zip member or a corrupt stream become a `CorpusError`, not a traceback.
  - `[high]` `patch` hash pinning: a pinned hash that does not match is a hard failure and leaves no staging directory; an unpinned entry is refused unless `--pin` is given, and a pin is never overwritten. Trust on first use is documented in the README and here: the first download is trusted as seen.
  - `[medium]` `patch` redirects: every redirect target is re-checked against the https allow-list (host, scheme, no credentials); `Content-Length` is checked up front and the stream is capped while reading (a server that lies about the length is stopped).
  - `[medium]` `patch` duplicate members that merge on macOS (case and Unicode normalisation: `café` against `café`) are refused; a file below a file is refused.
  - `[medium]` `patch` cache TOCTOU: the tree becomes visible by one `rename` of a private staging directory, a stamp holds the archive hash and the listing and content digests, reuse re-checks the listing (catches added / removed / resized files and planted symlinks) and `--deep` re-hashes every byte. A same-size edit is only caught by `--deep` (tested, documented). A local attacker who can write to the cache between the check and the import is outside the model (private cache directory advised); the importer and the oracle read the verified files, so the window is the run itself.
  - `[medium]` `patch` the guard used a path walk that listed the git-ignored oracle checkout (`tools/oracle/.work/...` holds sfizz test WAVs); it now asks git for what would be added (tracked plus untracked, ignores applied) and allows only audio below 8 MB in the test-fixture directories.
  - `[medium]` `patch` the runner matched import report lines by the path it passed; relative tree paths produced "no report line" for every file. Paths are made absolute and compared by `relpath`.
  - `[medium]` `patch` the instrument picker with `--max-null` N returned only three indices; it now spreads N picks from the smallest to the largest instrument.
  - `[medium]` `patch` importer: VSCO 2 CE failed 75 of 75 files on one opcode, `ampeg_dynamic` (111 lines, nothing else outside the subset). sfizz uses the flag only to re-read the `ampeg_*` values while a voice sounds; the values it could change come from `ampeg_*_oncc<N>` and `ampeg_vel2*`, which are unsupported and reject the file on their own. It is now allow-listed (accepted, logged in `allowlisted`, never stored), with a comment, the README table row and a test that the flag imports while `ampeg_release_oncc72` and `ampeg_vel2release` in the same file still reject. Checked that removing the flag from a VSCO SFZ does not change the oracle's render. This is a loosening of the importer's input policy for one inert flag, not of any tolerance.
  - `[low]` `patch` workflow: manual-only like ci.yml, the nightly `schedule` is a comment; it needs `brew` packages and the oracle build, and uploads only the report.
  - `[high]` `defer` the opcode coverage gap, the VSCO null-test divergence, the Pianobook selection (see `deferred`).
  - `[low]` `defer` concurrent `fetch --pin` runs are not locked (the staging directory is per process, the publish is a rename and a loser gets a `CorpusError`, but two processes pinning the same manifest at once could interleave the JSON rewrite).
  - `[low]` `defer` Python 3.9 on the Mac (Xcode's) is the lowest supported; `tarfile` on 3.12+ has `extraction_filter`, which this code does not rely on (it extracts by hand).
- trial on the synthetic suite (17 instruments of `tests/sample_suite`, two null-tested with all six probes): the harness passes the probes that stay inside an instrument's range at -67 .. -68 dB and fails (-21 .. -27 dB) those where the instrument has no region, because both renders are near-silent and the residual is the oracle's 16-bit floor. Recorded under `deferred` (probe coverage).

## Verification

**Commands:**
- `python3 -I -m unittest discover -s tools/corpus/tests` -- expected: 51 tests OK (1 skipped on case-insensitive filesystems) (offline)
- `python3 tools/corpus/corpus.py check --require-pins` and `guard` -- expected: exit 0
- `python3 tools/corpus/corpus.py fetch --pin` (done once; network) and `python3 tools/corpus/corpus.py run` -- expected on the CURRENT state: exit 1 (the job fails honestly)
- `cmake --build build -j8 && ctest --test-dir build -j4 --output-on-failure` -- expected: all pass

## Auto Run Result

Summary: `tools/corpus/` exists and was run against real libraries. Fetched, hash-pinned and cache-verified (SHA-256 of the archive): Salamander Grand Piano V3 (CC BY 3.0, `f36bef84...`), VSCO 2 Community Edition 1.1.0 (CC0, `f59b0362...`), Sonatina Symphonic Orchestra v4.0 from `peastman/sso` (Sampling Plus 1.0, `8fc8a7a7...`), Karoryfer Scarypiano, Meatbass, Weresax and Bear Sax (CC0, GitHub repositories at pinned commits) and three `extra` stand-ins (Osiris Piano CC0, Steel Drum Unlicense, Headroom Piano CC BY 4.0): 6.9 GB cached outside the repository. Imports: VSCO 2 CE 75 of 75 files (3163 regions, after allow-listing the inert `ampeg_dynamic` flag); all other libraries rejected on import by opcodes outside the Story 5.1 subset (Sonatina 747 of 747, Bear Sax 48, Osiris 74, Meatbass 39, Weresax 13, Headroom 4, Steel Drum 2, Salamander 1, Scarypiano 1). Null tests: VSCO 2 CE, 8 instruments x 6 probes: 0 of 48 pass at -60 dB (-54.4 .. +1.9 dB). The corpus job exits 1. Pianobook: six SFZ packs recorded as explicit exclusions (login gate, no licence on the page); none fetched.
Verification: 36 offline unit tests pass (path traversal, absolute names, links, special files, duplicates, case and Unicode collisions, size / count / ratio / decompression caps, truncated and corrupt archives, hash mismatch, TOFU pin and no overwrite, cache hit without download, cache tampering, planted symlink, deep check, exclusion handling, guard on planted audio, deterministic MIDI); full `ctest -j4` 707 tests, 705 passed and the same two skipped as before (`librarian_demo_acceptance`, `sample_demo_null_test`), including the new `corpus_*` tests and `format_sfz_test` with the `ampeg_dynamic` case; `check_third_party_notices`, `check_core_deps`, `check_no_private_keys`, `check_ui_strings`, `check_no_color_literals` exit 0. Real runs: `build/corpus-report/report.md` (official, 3 instruments per library) and `build/corpus-report-vsco8/report.md` (8 VSCO instruments); both report failures and exit 1. Not run: the nightly workflow, Linux, Docker, any CI. Addendum: 51 offline tests now (local_dir tree digest determinism, tamper and mismatch, TOFU pin, symlink / fifo / symlinked-top refusal, limits, missing source, env var and flag resolution, repository refusal, source never modified). `corpus.py run --only pianobook-kamoepiano301 pianobook-manthey-klaviano` on the Mac: both FAIL at import as described; `fetch --deep` verified both cache copies byte for byte; `guard` ok.
Residual risks: the story is not met (see `deferred`); the VSCO divergence has an unresolved root cause that may be an engine bug (retriggered notes) or an sfizz behaviour; random round robin (`lorand/hirand`) cannot be null-tested against sfizz as built; the Pianobook requirement needs the founder; Sampling Plus 1.0 terms for Sonatina need a licence decision before any distribution; hash pins are trust on first use of GitHub tarballs; a local writer to the cache directory is outside the threat model. Status stays in-progress. Pianobook addendum: 2 of >=5 delivered via `local_dir` (kamoepiano301, Manthey Klaviano); both fail import (opcodes recorded in `deferred`), so no null test ran for them; licences unstated and unconfirmed; the local entries cannot run on CI.

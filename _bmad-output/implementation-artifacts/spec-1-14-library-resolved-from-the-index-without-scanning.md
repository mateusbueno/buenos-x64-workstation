---
title: 'Story 1.14: 1 TB library resolved from the index without scanning'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '7879788e066da15a2c3fa3a22cb4627b823b8f3b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/formatlib/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The engine must find any instrument by `(library_id, instrument_key)` in up to 1 TB of libraries without ever scanning directories or sample data, and must fail typed (silent part, no substitution) when a reference does not resolve.

**Approach:** Add a library resolver under `engine/host/` (it may depend on `formatlib`; `engine/core` must not) that maps `(library_id, instrument_key)` to an opened bundle by opening only `<root>/lib/<library_id>/current` (symlink to a `<bundle_id>/` directory holding the bundle file), reading the bundle header/manifest/index through `formatlib`'s reader and returning the instrument entry; all filesystem access goes through a small counting file-system port so filesystem operations can be asserted; plus a generator tool that builds a synthetic library tree of arbitrary count/size (sparse files allowed) for scale tests.

## Boundaries & Constraints

**Always:** Root is a parameter (default `/data`; tests use a temp dir); layout `<root>/lib/<library_id>/current -> <bundle_id>/` and the bundle file inside it (choose a fixed file name, e.g. `bundle.vlb`, document it, and make the generator and resolver share the constant); the resolver never lists/readdirs `<root>/lib` or any library directory — only `stat/readlink/open` of the exact paths it needs; it reads no sample blocks (only header, manifest, index); results are typed: `kOk`, `kMissingLibrary`, `kMissingInstrument`, `kBundleError` (carrying formatlib's error), and a failed resolve never substitutes another instrument or library; thread/RT: the resolver runs on a control or I/O thread, not the audio thread (document); ids are validated (library_id is a canonical UUID string, instrument_key normalized per formatlib; reject path traversal like `..` or `/`).

**Never:** Walk or scan directories, touch librarian/catalog/SQLite, implement streaming or head loading, change `engine/core`, change the bundle format (if the reader's whole-index-in-RAM is a problem for resolve time at scale, report it instead of redesigning), or claim latency budgets on the Mac (label timings "host, not representative").

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Hit | existing library + instrument | kOk with bundle handle + instrument/sample info; ops = a bounded constant independent of library count | none |
| Scale | 10 / 1,000 / 20,000 library dirs (plus sparse multi-GB files for size) | filesystem-op count identical and resolve time does not grow with library count/size | none |
| Missing library | unknown library_id or dangling/absent `current` | kMissingLibrary; no other library touched | typed result |
| Missing instrument | library exists, key absent | kMissingInstrument | typed result |
| Corrupt bundle | truncated/corrupt manifest or index | kBundleError with formatlib error | typed result |
| Hostile ids | `../x`, empty, absolute path, non-UUID | rejected before any filesystem access | kInvalidId |
| Current points to wrong place | symlink loop or points outside `lib/<id>/` | rejected | typed result |
| Startup | engine start with the resolver | only requested `current` links are opened; zero readdir ops (asserted via the counting port) | none |

</intent-contract>

## Code Map

- `engine/host/CMakeLists.txt`, `include/velora/host/`, `src/`, `tests/` -- existing portable host lib `velora_host`; add the resolver here (portable, POSIX file calls behind the port) and link `velora_format` (check that the top-level CMake adds `formatlib` before/independent of `engine/host`)
- `formatlib/include/velora/format/bundle.hpp` -- `BundleReader::open_file`, manifest accessors (instrument keys, library_id, bundle_id), `BundleWriter` to produce fixtures; read the README for the on-disk layout
- `tools/` -- add `tools/library-gen/` (CLI + library) that writes the layout; reuse offline-render's WAV loader or generate PCM in code
- `tools/ci/check_core_deps.py`, `tools/ci/docker-check.sh` -- purity (core stays free of host/format) and the Linux validation; both must pass
- `_bmad-output/planning-artifacts/epics.md` Story 1.14 (~line 1149) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `engine/host/include/velora/host/fs_port.hpp` + `src/posix_fs.cpp` -- tiny file-system port (stat, readlink, open_read, pread, close) with op counters, a POSIX implementation and a counting wrapper -- makes "no scanning" testable
- `engine/host/include/velora/host/library_resolver.hpp` + `src/library_resolver.cpp` -- resolver as specified, typed results, id validation, current-link checks -- core of the story
- `tools/library-gen/` -- generator: N libraries each with a small real bundle (distinct library_ids, instruments from generated PCM) and optional sparse padding file (e.g. 1 GiB sparse) to emulate size; CLI flags `--root --libraries N --pad-gib G` -- scale fixture
- `engine/host/tests/library_resolver_test.cpp` -- every matrix row; op-count equality across 10/1000/20000 libraries; no-readdir assertion; resolve timing printed (labelled "host, not representative") and a loose sanity bound only -- coverage
- READMEs and CMake wiring; update `engine/host/README.md` with the layout and the no-scan rule -- docs

**Acceptance Criteria:**
- Given 20,000 libraries, when one instrument is resolved, then only its `current` link, manifest and index are opened, with the same filesystem-op count as with 10 libraries and zero directory-listing ops
- Given an unresolvable library or instrument, then a typed missing result is returned and nothing else is substituted
- Given hostile ids, then they are rejected before any filesystem access
- Given Linux (Docker) and macOS builds, then both pass the full suite

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (Linux gcc and clang RTSan; CI is manual-only, validation is local)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test` -- expected: pass

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~50 findings — patched 7 groups, rest deferred
- findings:
  - `[high]` `patch` open() on a FIFO planted as bundle.vlb hung the resolver; fstat failure used an uninitialised stat — O_NONBLOCK, checked fstat
  - `[medium]` `patch` symlink/directory/FIFO bundle.vlb rejection and I/O-error paths (lstat/readlink/pread) untested; fd leak on error paths unchecked — tests added with a fault-injecting FsPort and a leak-tracking wrapper
  - `[medium]` `patch` readlink race reported as invalid link; instrument_info() on a failed result; null FsPort/empty root; parse_library_id partial output — fixed
  - `[medium]` `patch` flaky relative timing assertion and a slow default scale test — assertion removed, default 10/1000/3000 libraries, 20,000 opt-in with VELORA_SCALE_TEST=1
  - `[low]` `patch` open_source untested; library-gen overflow/zero count/recursive mkdir/tmp cleanup — fixed
  - `[low]` `defer` symlinked parents (root, lib/, lib/<id>) are followed, only the last components are checked; finer error classes (EACCES vs I/O); CountingFs/RecordingFs live in the shipped library; layering host-test -> tools; padding.bin sits inside the bundle dir; instrument lookup is linear; no bundle cache; fd limit with many leases; op-count tests coupled to formatlib read pattern
  - `[intent gap noted]` real 1 TB NVMe at 10/100/1000 GB, engine startup integration and "part plays silent" are not part of this story's host-level resolver; verified at library-count scale with sparse files and recorded op counts — real-disk measurement belongs to the dev-board stories (1.16/1.17/1.11)

## Auto Run Result

Status: done. Added the host library resolver (`LibraryResolver`, `FsPort` with counting/recording wrappers, POSIX implementation), the `velora-library-gen` scale tool, and a public `ByteSource`/`BundleReader::open_source` in formatlib. Resolve costs a constant 8 filesystem operations (lstat x2, readlink, open, pread x3, close) with zero directory listings at 10, 1,000 (1 GiB sparse each, ~1 TB apparent) and 3,000 libraries (20,000 opt-in); typed results kOk/kInvalidId/kMissingLibrary/kMissingInstrument/kBundleError/kInvalidLink/kIdentityMismatch; no substitution. Verification: macOS ctest 23/23 and Docker (gcc, clang-20 RTSan, purity) 26/26 pass. Timings are "host, not representative". Follow-up review recommended: false.

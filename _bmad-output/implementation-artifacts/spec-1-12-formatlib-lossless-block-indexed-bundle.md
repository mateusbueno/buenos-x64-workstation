---
title: 'Story 1.12: formatlib lossless block-indexed bundle'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '7f67cfd1d58dc67bd4231a790a99b1870c8a2d61'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/_bmad-output/specs/spec-arm64-stage-workstation/library-format.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The sample engine needs a library container that stores samples losslessly, supports seeking to any frame by decoding one block, and mints stable identities. No such format exists.

**Approach:** Create `formatlib/` (C++20) with a bundle writer/reader: fixed-size blocks per sample coded with libFLAC (integer PCM 16/24-bit, mono/stereo, 44.1/48/88.2/96 kHz) or stored raw (32-bit float), an index table mapping every frame to its block, and a manifest with `format_version`, `bundle_id` (manifest hash), `library_id` (UUIDv5 of a source fingerprint for user imports) and per-instrument `instrument_key`. No crypto, no SF2, no SFZ parsing in this story.

## Boundaries & Constraints

**Always:** Decoded PCM is bit-identical to the source on every path; no resampling or requantization anywhere; typed errors (an error enum/result type, no exceptions needed on the read path, never a crash or garbage audio) for truncated blocks, corrupted index, unknown major `format_version`; reader accepts format_version N and N−1; only formatlib mints `bundle_id`, `library_id`, `instrument_key`; libFLAC is the only third-party dependency, linked as library only (no flac/metaflac binaries), found with CMake `find_package(FLAC)`/pkg-config (Homebrew has 1.5.0; the target is 1.5.0, accept >= 1.4 and record the found version in a CMake status line — do NOT download or vendor anything); SHA-256 and SHA-1 (for UUIDv5) are implemented in-house in a small tested unit with known-answer vectors; the reader gives a block-level API usable from an I/O thread (decode one block into caller-provided memory, no allocation inside the decode call beyond what libFLAC needs and documented); formatlib must not include anything from engine/core (and engine/core must not depend on formatlib).

**Never:** Add crypto/AES/signing, SF2, an SFZ parser, the librarian, catalog code, streaming/io_uring code, the engine integration, or network access. Do not weaken the bit-exactness guarantee for speed. No GPL/LGPL dependency.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Round trip int PCM | 16/24-bit, mono/stereo, each of 44.1/48/88.2/96 kHz | every frame decodes bit-identical | none |
| Silence | all-zero sample | round-trips; compresses well | none |
| Full-scale noise | worst-case incompressible | round-trips; block may fall back to verbatim FLAC | none |
| Odd length | length not a multiple of block size, length 1, length 0 | round-trips; last block short; length 0 allowed or typed error (documented) | typed error if rejected |
| Seek | frame k in the middle | one block decode yields frames incl. k, equal to the source | out-of-range frame -> typed error |
| Float 32 | 32-bit float source | stored uncompressed in the same container; bit-identical (including NaN payloads/-0.0) | none |
| Unsupported format | 8-bit, 5 channels, 11.025 kHz, 32-bit int | typed error at write | typed error |
| Truncated file | cut inside a block / inside index / inside manifest | typed error | typed error |
| Corrupt index | offsets out of file range, non-monotonic | typed error | typed error |
| Corrupt block | flipped bits in a block | typed error from the block hash/FLAC decoder, never silent garbage | typed error |
| Version | major unknown (e.g. N+1) | rejected with typed error; N and N−1 accepted | typed error |
| Identity | same inputs twice | identical bundle bytes, bundle_id, library_id; instrument_key from normalized relative path (forward slashes, no ./ or .., case preserved) | invalid path -> typed error |

</intent-contract>

## Code Map

- `formatlib/` -- new top-level dir (architecture Structural Seed: IDs, bundle read/write, lossless codec; crypto and SFZ parser come later)
- `CMakeLists.txt` -- add `add_subdirectory(formatlib)`; keep the existing subdirs and tests
- `tools/ci/check_core_deps.py` -- unaffected, must keep passing; `.github/workflows/ci.yml` -- jobs need libFLAC dev packages: macOS `brew install flac`, Linux `apt-get install libflac-dev` (all three build jobs and the RTSan job); keep jobs green
- `engine/core` is read-only here
- `_bmad-output/specs/spec-arm64-stage-workstation/library-format.md` -- container requirements (block-indexed, FLAC-style, float uncompressed, heads decoded later)
- `_bmad-output/planning-artifacts/epics.md` Story 1.12 (~line 1090) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `formatlib/include/velora/format/` + `formatlib/src/` -- public API: error/result types; `BundleWriter` (add instrument with name/key and samples from in-memory PCM + format descriptor, finalize to a byte buffer/file, deterministic); `BundleReader` (open from file/memory with validation; manifest accessors; per-sample info; `read_block(sample, block, out)`; `read_frames(sample, first, count, out)` convenience built on blocks); constants for block size (fixed, e.g. 4096 frames, documented) and `kFormatVersion` -- container
- `formatlib/src/sha256.*`, `sha1.*`, `uuid.*` -- hashes with known-answer tests, UUIDv5 -- identity minting
- `formatlib/src/wav_source.*` (optional, small) -- read PCM16/24 and float32 WAV into the writer input, so Story 1.13 can feed WAV files; tested -- source loading
- `formatlib/tests/` -- tests for every matrix row (fixture generator in code: silence, full-scale noise, sine, odd lengths, all formats), determinism, truncation at every byte offset of a small bundle (never crashes, always typed error), index/block corruption, version N/N−1/N+1 -- coverage
- `formatlib/CMakeLists.txt`, `README.md` -- target `velora_format` (static), ctest registration, role and dependency rule line -- wiring and docs
- `.github/workflows/ci.yml` -- install libFLAC dev packages on every job -- keep CI green

**Acceptance Criteria:**
- Given any supported format, when a bundle is written and every sample decoded, then PCM is bit-identical to the source
- Given a frame k, when read via the index, then exactly one block is decoded and returns the correct frames
- Given a truncated or corrupted bundle, then a typed error is returned and no crash occurs (also with the truncation sweep)
- Given identical inputs, then bundle bytes, bundle_id and library_id are identical; given format_version N+1 the reader rejects with a typed error, N and N−1 are accepted
- Given 32-bit float sources, then they are stored uncompressed and bit-identical

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test` -- expected: pass
- Report the libFLAC version found by CMake

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~50 findings — patched 10 groups, rest deferred
- findings:
  - `[high]` `patch` block bytes depended on the libFLAC version (vendor-string comment block) so bundle bytes could differ across CI machines — only STREAMINFO kept; determinism scoped to the same libFLAC version; library_id and PCM hashes are libFLAC-independent, golden literals added
  - `[medium]` `patch` verify_all skipped block hashes when verification was off — always verifies block SHA-256 and PCM hash
  - `[medium]` `patch` reader accepted duplicate instrument keys; moved-from BlockDecoder null deref; unbounded index entry size; EINTR — fixed
  - `[medium]` `patch` PCM-hash check, decoder-only corruption (kCorruptBlock) and WAV header branches untested; `(void)e` sweep asserted nothing — tests added incl. forged valid-hash containers
  - `[low]` `patch` non-atomic finalize_to_file; Result/Error not [[nodiscard]]; libFLAC version check skippable; WAV data size 0xFFFFFFFF rejected; doc/code mismatch on library_id; decoder alloc failure misreported — fixed
  - `[low]` `defer` whole index table held in RAM (48 B/block; slim for Story 1.14); kMinReadableVersion 0 policy; hot-path allocation in read_block convenience overloads and per-block FLAC decoder init (a streaming reader must reuse decoders — Story 1.17); Unicode/case key normalization; fuzz/property tests; POSIX-only pread; named header-offset constants
  - `[unverified]` `defer` Linux libFLAC (apt, 1.4.x) build and tests first run in CI

## Auto Run Result

Status: done. Added `formatlib/` (`velora_format`): lossless block-indexed bundle writer/reader (128-byte header, manifest, index with per-block SHA-256, 4096-frame blocks, one-block FLAC streams for 16/24-bit PCM, raw float32), in-house SHA-256/SHA-1/UUIDv5, identity minting (`bundle_id`, `library_id`, `instrument_key`), WAV loader, typed errors, and tests (round trips for all formats and odd lengths, seek decodes one block, truncation and bit-flip sweeps, forged-container corruption, versions N/N−1/N+1). CI installs libFLAC dev packages. Verification: macOS build and ctest pass (libFLAC 1.5.0), purity check passes. Follow-up review recommended: false.

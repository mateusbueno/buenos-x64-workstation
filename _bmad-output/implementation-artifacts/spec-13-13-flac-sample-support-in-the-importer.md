---
title: 'FLAC sample support in the importer'
type: 'feature'
created: '2026-10-28'
status: 'done'
baseline_revision: 'a707940e7d588506198ae8b8f877ca6b82a3c75f'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/planning-artifacts/sfz-scope-decision.md', '{project-root}/_bmad-output/implementation-artifacts/epic-13-retro-2026-10-28.md', '{project-root}/formatlib/include/velora/format/wav_source.hpp']
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Reference corpus libraries such as Salamander Grand Piano V3 and Headroom Piano provide their samples in FLAC format (.flac). Today, the importer only supports WAV files; attempting to import FLAC files results in `unsupported audio format` / `damaged WAV file (bad_magic)` and causes import rejection.

**Approach:**
1. In `formatlib/src/wav_source.cpp` (or audio source): when loading audio files, detect FLAC magic (`fLaC`) or `.flac` extension and decode the audio using `libFLAC` (already linked into `velora_format`).
2. Provide `load_audio_file` (with `load_wav_file` maintaining compatibility) returning `WavData` with format, frame_count, and interleaved PCM samples.
3. In `librarian/src/preflight.cpp`: support inspecting FLAC headers (`flac_head`) to read format (channels, sample rate, bits per sample) and frame count without decoding all audio during preflight scan.
4. Support `.flac` in `has_audio_ext` / `has_wav_ext`.

## Boundaries & Constraints

**Always:** Supported formats remain 1-2 channels, 16-bit or 24-bit PCM, 44.1/48/88.2/96 kHz. Decoded PCM samples match the internal exchange convention.
**Never:** Re-encode samples or introduce external dependencies beyond the existing `libFLAC`.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Valid 24-bit stereo 48kHz FLAC | `rel13.flac` | Decoded into `WavData` with 24-bit PCM | None |
| Valid 16-bit FLAC | 16-bit stereo FLAC | Decoded into `WavData` with 16-bit PCM | None |
| Corrupt FLAC file | truncated/invalid FLAC bytes | Return Error::kCorruptHeader or Error::kTruncated | Reported as damaged sample |
| Preflight scan of FLAC | `flac_head` | Returns format and frames from STREAMINFO in <1ms | Error if STREAMINFO invalid |

</intent-contract>

## Code Map

- `formatlib/include/velora/format/wav_source.hpp` -- add FLAC support to audio loading.
- `formatlib/src/wav_source.cpp` -- implement FLAC decoder using `libFLAC`.
- `formatlib/tests/wav_test.cpp` -- test FLAC parsing and decoding.
- `librarian/src/preflight.cpp` -- add `flac_head` and recognise `.flac`.

## Tasks & Acceptance

- [x] 1. Declare `parse_flac` in `formatlib/include/velora/format/wav_source.hpp`.
- [x] 2. Implement `parse_flac` using `libFLAC` stream decoder in `formatlib/src/wav_source.cpp` and support FLAC detection in `load_wav_file`.
- [x] 3. Support fast FLAC STREAMINFO header extraction (`flac_head`) in `librarian/src/preflight.cpp`.
- [x] 4. Add unit tests for FLAC parsing in `formatlib/tests/wav_test.cpp`.
- [x] 5. Add integration test for FLAC sample import in `librarian/tests/librarian_test.cpp`.

## Review Triage Log

- Layer 1 (Sanity & Contract): Decoded FLAC samples strictly adhere to the internal integer PCM exchange convention (16/24-bit).
- Layer 2 (Preflight Performance): `flac_head` extracts format and frame count directly from the 42-byte STREAMINFO block without full audio decoding.
- Layer 3 (Tests & Verification): `format_wav_test` and `librarian_test` pass with 100% success.

## Auto Run Result

Status: done
Verification:
- `format_wav_test`: PASS
- `librarian_test`: PASS
- Real 24-bit 48kHz stereo FLAC (`rel13.flac` from Salamander Grand Piano): decoded and bundled successfully with exact frame count 21,901.


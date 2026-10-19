# tools/oracle

Role: the sfizz reference renderer used **only by test tooling** as the oracle of the sample-engine null tests (Story 1.15).

It is never linked into a shipped target, no sfizz source is vendored here (this directory holds scripts and this file; the sources live
in the git-ignored `.work/`), and `tools/ci/check_third_party_notices.py` fails if oracle sources are committed, `.gitignore` loses the
`/tools/oracle/.work/` rule, sfizz becomes a submodule, or a shipped CMake file references `tools/oracle` or `sfizz`. Only the vendored
BSD-2 SFZ parser (`formatlib/third_party`, see `THIRD_PARTY_NOTICES.md`) ships. License review: `_bmad-output/business/11-12-sfizz-license-check.md`.

## Scripts

- `build-sfizz-render.sh` -- clones https://github.com/sfztools/sfizz into `tools/oracle/.work/sfizz` (override with `VELORA_ORACLE_DIR`),
  checks out sfizz **1.2.3 = commit `4e70dc0bef53b41f2853ed46e26f5911114c92d0`** with submodules, verifies the commit, and builds the
  `sfizz_render` CLI (Release, no JACK/LV2/VST/tests). Prints the path of the binary
  (`.work/build/library/bin/sfizz_render`); a second run just prints it. Needs network, git, cmake and a C++17 toolchain, and about
  10 minutes the first time. Two build workarounds, both in the script: on arm64 Linux/macOS it passes `-DPROJECT_SYSTEM_PROCESSOR=aarch64`
  (sfizz 1.2.3 adds the 32-bit ARM flags `-mfpu/-mfloat-abi` for any "arm*" processor, which clang rejects on arm64), and it silences
  `-Wmissing-template-arg-list-after-template-kw` (an error in recent clang for sfizz's bundled atomic_queue).
- `render-oracle.sh <file.sfz> <file.mid> <out.wav> [quality]` -- `sfizz_render` at 48 kHz, 64-frame blocks, polyphony 64, sample
  quality 2 (Hermite, sfizz's default; 1 = linear). The output is sfizz's own 16-bit WAV, untouched. Uses `VELORA_SFIZZ_RENDER` if
  set, else builds the oracle; exits 77 when it is unavailable. The output size is capped (128 MB) because sfizz renders until silence and
  a voice that never releases (see the quirks in `engine/core/README.md`) would otherwise fill the disk.
- Goldens of the suite: `tools/offline-render/tests/sample_suite/regenerate_goldens.sh`. A regenerated golden that differs from the
  committed one means the oracle, the suite or the platform changed -- review before committing.

## What the oracle's output needs before a null test

`velora-null-test --reference-decode sfizz-s16` (case key `reference_decode`) undoes the dr_wav float->s16 conversion. The engine side
runs with `--gain-db -7.35 --oracle-block-frames 64`, and the test MIDI files send CC7 = 127 first; why is explained in
`engine/core/README.md` ("Known differences from sfizz") and `tools/offline-render/tests/sample_suite/README.md`.

# tools/oracle

Role: the reference renderers used **only by test tooling** as the oracles of the audio null tests: sfizz for SFZ (Story 1.15),
FluidSynth for SF2 (Story 5.3), and MSFA (Music Synthesizer for Android / Dexed core) for 6-operator FM synthesis (Story 12.2).

They are never linked into a shipped target, no oracle source is vendored here (this directory holds scripts and this file; the sources live
in the git-ignored `.work/`), and `tools/ci/check_third_party_notices.py` fails if oracle sources are committed, `.gitignore` loses the
`/tools/oracle/.work/` rule, sfizz or FluidSynth becomes a submodule, a shipped CMake file references `tools/oracle` or `sfizz`, or any
shipped CMake file mentions FluidSynth. Only the vendored BSD-2 SFZ parser (`formatlib/third_party`, see `THIRD_PARTY_NOTICES.md`) ships;
the SF2 reader and the 6-operator FM engine are clean-room code written from public specifications and mathematical models. License review of sfizz:
`_bmad-output/business/11-12-sfizz-license-check.md`. FluidSynth is LGPL-2.1 and MSFA is Apache-2.0: they are only ever *run as separate processes* on
fixtures, built into `.work/` on the machine that regenerates goldens, and never distributed, linked, vendored or copied from.

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

- `build-fluidsynth.sh` -- clones https://github.com/FluidSynth/fluidsynth into `tools/oracle/.work/fluidsynth` (override with
  `VELORA_ORACLE_DIR`), checks out **FluidSynth 2.6.1 = tag `v2.6.1` = commit `71e85b2ca6bf48641ba7e2261d8f2640ae473773`**, verifies the commit and
  builds the `fluidsynth` CLI (Release, static library, only libsndfile for file output; no audio or MIDI driver, network, readline, JACK, ...). Prints
  the path of the binary (`.work/fluidsynth-build/src/fluidsynth`); a second run just prints it. Needs network, git, cmake, a C++17 toolchain and
  libsndfile (`brew install libsndfile`, `apt-get install libsndfile1-dev`). Exit code 77 = a prerequisite is genuinely missing (git, cmake, network,
  libsndfile), any other non-zero code a real failure. A from-scratch build took about two minutes on an Apple M-series Mac.
- `render-fluidsynth.sh <file.sf2> <file.mid> <out.wav>` -- `fluidsynth -ni -F out.wav -r 48000 -g 0.8409 -R 0 -C 0 -o synth.midi-bank-select=mma`:
  fast file rendering at 48 kHz into FluidSynth's own 16-bit stereo WAV (untouched; interpolation is FluidSynth's default, 4th order), reverb and chorus
  off (their sends would add a wet signal the engine does not have), bank select in MMA mode (bank = CC0 x 128 + CC32, so the percussion bank 128 is
  selectable on any channel). Uses `VELORA_FLUIDSYNTH` if set, else builds the oracle; exits 77 when it is unavailable. If the oracle cannot be built,
  stop: goldens are never written by hand or taken from another renderer.
- Goldens of the SF2 suite: `tools/offline-render/tests/sf2_suite/regenerate_goldens.sh <build dir>` (needs the oracle). Its README
  (`tools/offline-render/tests/sf2_suite/README.md`) lists, with the FluidSynth source lines, every place where the oracle departs from the SoundFont
  specification and how the suite compensates (the `-g 0.8409` above is one of them).

- `build-fm-oracle.sh` -- clones https://github.com/google/music-synthesizer-for-android.git into `tools/oracle/.work/msfa`
  (override with `VELORA_ORACLE_DIR`), checks out commit `f67d41d313b7dc85f6fb99e79e515cc9d208cfff`, and builds the
  `fm_oracle_render` CLI with the test adapter `tools/oracle-adapter/fm_oracle_render.cpp`. Prints the binary path
  (`.work/bin/fm_oracle_render`); a second run returns immediately via `.msfa-built-commit`. Exit code 77 = compiler or network missing.
- `render-fm-oracle.sh <file.syx> <voice_slot> <file.mid> <out.wav>` -- renders the SysEx voice (1..32) and MIDI events
  at 48 kHz using MSFA's `Dx7Note` into a 48 kHz stereo WAV file with `1.0 / 2^27` normalization. Uses `VELORA_FM_ORACLE` if set,
  else builds the oracle; exits 77 when unavailable.
- Goldens of the FM suite: `tools/offline-render/tests/fm_suite/regenerate_goldens.sh`.

## What the oracle's output needs before a null test

`velora-null-test --reference-decode sfizz-s16` (case key `reference_decode`) undoes the dr_wav float->s16 conversion. The engine side
runs with `--gain-db -7.35 --oracle-block-frames 64`, and the test MIDI files send CC7 = 127 first; why is explained in
`engine/core/README.md` ("Known differences from sfizz") and `tools/offline-render/tests/sample_suite/README.md`.

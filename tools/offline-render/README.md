# tools/offline-render

Role: offline renderer and null-test harness driving `engine/core` without a live audio device.

Dependency rule: may depend on `engine/core` and `engine/ports`; nothing in `engine/` may depend on it.

## Tools

- `velora-offline-render --midi in.mid --out out.wav [--engine test-tone] [--voices N] [--tail-seconds S] [--detune-cents C]`
  Renders a Standard MIDI File (format 0/1, PPQ) through `BlockRenderer` to a 32-bit float, stereo, 48 kHz WAV.
  Tick-to-frame conversion uses the file's tempo map in integer arithmetic (default 120 bpm); each event is applied
  at its exact in-block offset. Output is bit-identical across runs. `--detune-cents` is test-only (used to prove
  the null test fails on a wrong render). A malformed SMF exits non-zero and writes no output.
- `velora-null-test --reference r.wav --candidate c.wav [--candidate ...] --tolerance-db X --max-offset-frames N`
  or `--case file.case` (key=value lines: `reference`, `tolerance_db`, `max_offset_frames`; the reference path is
  relative to the case file; flags override). Tries integer alignment offsets only within +-max, reports per-file
  residual dB (residual RMS relative to reference RMS, overlap only), the chosen offset, length difference and the
  worst 100 ms window, and exits non-zero on failure. A silent reference passes only against a silent candidate.

## Golden render

`tests/golden/` holds `golden.mid`, `golden.wav` and `golden.case`. ctest renders twice (byte-equality), null-tests a
fresh render against the golden, and requires a `--detune-cents 20` render to fail. Regenerate the golden after an
intentional engine change with `cmake --build build --target update-golden` and review the diff.

## Oracle rule

A reference oracle (for example sfizz) may only be used offline to produce reference renders for null tests. It is
never linked into any shipped target, and no oracle is part of this directory yet.

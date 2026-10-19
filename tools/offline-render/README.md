# tools/offline-render

Role: offline renderer and null-test harness driving `engine/core` without a live audio device.

Dependency rule: may depend on `engine/core`, `engine/ports`, `engine/host` (the bundle loader for the sample engine) and `formatlib`; nothing in `engine/` may depend on it.

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

- `velora-offline-render --engine sample --bundle B --instrument KEY [--seed N] [--gain-db G] [--interpolation hermite|linear] [--voices N]`
  (Story 1.15) renders the MIDI through `SampleEngine` playing instrument `KEY` (its `instrument_key`) of the bundle file `B`
  (`<root>/lib/<library_id>/<bundle_id>/bundle.vlb`, made by `librarian import`). All samples are decoded to RAM first; voices default to
  64; the report line counts unplayable regions and pool exhaustion. `--seed` seeds `lorand/hirand`; `--gain-db` scales the
  output; `--oracle-block-frames N` (test only) emulates sfizz's per-voice block-boundary hold (see `engine/core/README.md`).
  `--bundle/--instrument` are errors without `--engine sample`; an unknown instrument or an unreadable bundle exits 1 and writes nothing.
- `velora-null-test` also takes `--max-length-diff-frames N`, `--check-excess` (the energy outside the compared overlap, i.e. a tail in
  one file only, must stay below the tolerance too) and `--reference-decode sfizz-s16` (case keys `check_excess`, `reference_decode`).
  16-bit PCM WAVs are readable; `sfizz-s16` undoes `sfizz_render`'s float->s16 conversion (dr_wav maps silence to -1 and scales by
  32767.5, so the reference is decoded as `(s + 1) / 32767.5`), which would otherwise inject a -90 dBFS DC offset and a gain error of 1.5e-5.

- `velora-offline-render --engine organ [--drawbars 888000000]` (Story 2.2) renders the MIDI through the tonewheel organ on
  channels 0..7 (one organ part per MIDI channel; notes on channels 8..15 are refused as engine-not-available and stay silent). `--drawbars` is nine digits 0..8 for 16', 5 1/3',
  8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' (default `008000000`), applied to every part at frame 0.

- `--percussion second|third,fast|slow,soft|normal` (any subset, each category once, unnamed ones keep the registry default
  third, fast, soft) or `--percussion off`, and `--key-click 0..1` (default 0.5) (Story 2.3) set the organ's percussion and key click on every
  part at frame 0; both are errors without `--engine organ`. The MIDI file can change them too: control changes are now parsed and
  those mapped by the registry (`proto/params.json` `cc`: drawbars 70-78, percussion 80, key click 82, percussion harmonic/decay/volume
  84/85/86, ...) become performance parameter events on the channel's part; unmapped controllers (volume, pedal, ...) are dropped and do
  not extend the render.

## Organ percussion golden (Story 2.3)

`tests/organ_percussion/` holds `organ_percussion.mid` (made by `make_midi.py`: staccato and legato playing, all eight
harmonic/decay/volume combinations through CC 80/84/85/86, then key click through CC 82 with a repeated key), `organ_percussion.wav`
(about 4.85 s, 1.9 MB) and `organ_percussion.case` (null test, -100 dB). ctest renders twice (`organ_perc_bit_identical`),
null-tests the fresh render against the golden (`organ_perc_null_golden`; a null test rather than a byte compare because libm may differ
in the last bit between macOS and Linux) and requires a different key click level and a different registration to fail it. The render
command is `velora-offline-render --midi organ_percussion.mid --engine organ --percussion off --key-click 0.4 --tail-seconds 0.3`.
Regenerate after an intentional engine change with `cmake --build build --target update-organ-percussion-golden` and review.

## Golden render

`tests/golden/` holds `golden.mid`, `golden.wav` and `golden.case`. ctest renders twice (byte-equality), null-tests a
fresh render against the golden, and requires a `--detune-cents 20` render to fail. Regenerate the golden after an
intentional engine change with `cmake --build build --target update-golden` and review the diff.

## Sample engine against sfizz (Story 1.15)

`tests/sample_suite/` holds a synthetic SFZ library (generated by `generate_suite.py`, tiny 16-bit WAVs), MIDI files, and sfizz
renders of them as committed goldens (`golden/*.wav`, the untouched `sfizz_render` output, ~1.6 MB). ctest runs, per case,
`librarian import` (in process) -> `velora-offline-render --engine sample` -> `velora-null-test --case` against the golden
(`sample_null_<case>`), so **no sfizz is needed to run the tests**. Further tests: `sample_null_neg_gain` / `sample_null_neg_interp`
(a 1 dB gain error and the wrong interpolation must fail the same null test), `sample_render_cli_errors`, and the env-gated
`sample_demo_null_test` for real libraries (`VELORA_SFZ_DEMO_DIR`; skipped, exit 77, without it or without an oracle). Tolerances,
measured residuals and every deviation are in `tests/sample_suite/README.md`. Regenerating the goldens needs the oracle:
`tests/sample_suite/regenerate_goldens.sh` (see `tools/oracle/README.md`).

## Oracle rule

A reference oracle (sfizz's full engine) may only be used offline to produce reference renders for null tests. It is never linked
into any shipped target and no oracle source is in this repository: `tools/oracle/` fetches sfizz 1.2.3 into an ignored build
directory and builds its `sfizz_render` CLI there. `tools/ci/check_third_party_notices.py` enforces this.

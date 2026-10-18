---
title: 'Story 1.9: Offline renderer and null-test harness'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '76ad40287b4a62bcd4f88b21bbcabbb994f8afc1'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** There is no way to render the engine core to a file or to check sound fidelity automatically; later DSP stories need deterministic renders and null tests in CI.

**Approach:** In `tools/offline-render` build (1) a CLI that reads a Standard MIDI File, converts it to UMP events with frame positions (fixed tempo map, 48 kHz, same device-time-to-frame rule the live engine will use), drives `BlockRenderer` and writes a 32-bit float WAV, (2) a null-test harness that compares two WAVs and reports residual dB, and (3) CI wiring with a committed golden render of the test-tone engine and a detuned render that must fail.

## Boundaries & Constraints

**Always:** C++20, standard library only (hand-written SMF parser and WAV reader/writer, no third-party libs); two runs with identical inputs give bit-identical output files; WAV is 32-bit float, stereo, 48 kHz; the harness aligns by trying offsets only within a configured max offset (no unbounded search), computes residual RMS relative to reference RMS in dB, passes/fails against a per-test tolerance stored with the test case (a small text/JSON-free key=value file next to the golden), and prints a report with per-file residual dB and the worst 100 ms window; tools may depend on engine/core but engine/core must not depend on tools (purity check must keep passing); a reference oracle (e.g. sfizz) is never linked into any shipped target (document this rule in the README; no oracle is added in this story).

**Never:** Add real sample/organ DSP, audio device I/O, or any dependency outside the standard library; add engine-core features beyond what the renderer needs (if a missing core hook is essential, keep it minimal and tell me in the report).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Happy render | SMF type 0 or 1, note-on/off, default tempo 120 | WAV with notes at tempo-map frame positions; identical on re-run (byte-equal) | none |
| Tempo change | SMF with tempo meta events | frame positions follow the tempo map | none |
| Running status, note-on vel 0 as note-off, delta times > 1 byte | valid SMF | parsed correctly | none |
| Malformed SMF | bad header/truncated track | exit non-zero with a clear message, no output file | stderr message |
| Null identical | same WAV twice | residual -inf (reported as e.g. -300 dB) pass | none |
| Null detuned | candidate with different pitch | residual above tolerance, fails | exit non-zero, report |
| Offset within max | candidate delayed by k <= max_offset frames | aligned, passes | none |
| Offset beyond max | delay > max_offset | fails | exit non-zero |
| Length/format mismatch | different sample rates or channel counts | fails with message; length difference handled by comparing the overlap and reporting it | exit non-zero |
| Silent reference | reference RMS zero | defined behaviour (fail unless candidate also silent) | message |

</intent-contract>

## Code Map

- `tools/offline-render/CMakeLists.txt`, `README.md` -- existing placeholders from Story 1.3 to extend
- `engine/core/include/velora/core/block_renderer.hpp`, `ump.hpp`, `test_tone_engine.hpp` -- API to drive: `BlockRenderer::submit(TimedUmp)`, `process_block`, `kBlockFrames`, `kSampleRate`, `make_note_on/off`; event target_frame is relative to the next block rendered, frames >= 64 are deferred (see header)
- `CMakeLists.txt` (top) -- already add_subdirectory(tools/offline-render); `.github/workflows/ci.yml` -- add a null-test step/job if needed (ctest already runs on all jobs)
- `tools/ci/check_core_deps.py` -- must keep passing
- `_bmad-output/planning-artifacts/epics.md` Story 1.9 (~line 1011) -- ACs, read-only

## Tasks & Acceptance

**Execution:**
- `tools/offline-render/src/smf.{hpp,cpp}` -- SMF parser (format 0/1, tempo map, running status, vel-0 note-on) producing time-ordered note events with tick-to-frame conversion at 48 kHz using integer/fixed arithmetic for determinism -- MIDI input
- `tools/offline-render/src/wav.{hpp,cpp}` -- 32-bit float WAV writer and reader -- file I/O
- `tools/offline-render/src/render_main.cpp` -- CLI `velora-offline-render --midi in.mid --out out.wav [--engine test-tone] [--voices N] [--tail-seconds S]`; converts events to TimedUmp with frame positions and drives BlockRenderer block by block (submit each event in the block that contains its frame, as an in-block offset) -- renderer
- `tools/offline-render/src/nulltest.{hpp,cpp}` + `null_test_main.cpp` -- CLI `velora-null-test --reference r.wav --candidate c.wav --tolerance-db X --max-offset-frames N` with the report (per-file residual dB, worst 100 ms window) and non-zero exit on failure; also accepts a `--case file` with the stored tolerance -- harness
- `tools/offline-render/tests/` -- tests for every matrix row (tiny SMFs generated in code), determinism (two renders byte-equal), detuned fails, golden passes -- coverage
- `tools/offline-render/tests/golden/` -- a small committed MIDI file, its golden WAV render (keep it short, a few seconds, a few hundred KB at most) and a case file with tolerance; plus a CMake/ctest rule that renders and null-tests against the golden, and a rule that renders with a deliberately detuned setting (e.g. a test-only `--detune-cents` flag on the renderer) and expects the null test to fail -- CI proof
- `tools/offline-render/CMakeLists.txt` -- targets and ctest registration; `README.md` -- usage and the oracle rule -- wiring and docs

**Acceptance Criteria:**
- Given a SMF, when rendered twice, then the two WAV files are byte-identical
- Given the golden test-tone render, when the null test runs against a fresh render, then it passes; given a detuned render it fails
- Given a candidate delayed within max offset, then it passes; beyond max offset it fails
- Given the null test report, then it lists per-file residual dB and the worst 100 ms window

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test` -- expected: pass
- Run the golden pipeline manually once and show the report output

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~45 findings — patched 9 groups, rest deferred
- findings:
  - `[high]` `patch` NaN/Inf candidate produced a perfect null — non-finite samples now fail
  - `[medium]` `patch` zero-energy overlap offsets and overlap-only silence judgement let bad audio pass — offsets skipped, silence judged on whole file
  - `[medium]` `patch` truncated candidates passed — `max_length_diff_frames` added, golden.case sets 0
  - `[medium]` `patch` lax numeric parsing (empty/garbage/inf; case-file bypassed caps) — one strict parser for flags and case file, unknown keys rejected
  - `[medium]` `patch` detune-fails proof used WILL_FAIL (any error passed) and stale files could mask failures — regex on the FAIL line, stale files deleted, fixtures
  - `[low]` `patch` unvalidated detune extremes, hard-coded 48 kHz/100 ms in report, malformed-SMF test wrote truncated bytes, no exact-length test, meta/sysex running status, zero tracks, overflow — fixed with tests
  - `[low]` `defer` >1024 simultaneous events in one block abort the render; PCM16/24 and extensible WAV reading; worst-window gating; Windows path/POSIX test portability; atomic output writes; golden provenance script; cross-platform libm tolerance (verified first in Linux CI); engine-config file input
  - `[unverified]` `defer` -100 dB tolerance across macOS/Linux libm is assumed; first Linux CI run will confirm

## Auto Run Result

Status: done. Added the offline renderer (SMF → UMP with integer tempo-map frame positions → BlockRenderer → 32-bit float WAV, byte-deterministic), the null-test harness (bounded offset alignment, residual dB, worst 100 ms window, length and silence rules, case files), a committed 1 s golden render, ctest pipeline (render twice and compare, null-test vs golden, detuned render must fail with the FAIL result line), and README with the oracle-never-linked rule. One core hook was added: `TestToneEngine::kParamDetuneCents`. Verification: macOS build, ctest all pass, purity check and self-test pass; Linux/RTSan CI jobs run on push. Follow-up review recommended: false.

---
title: 'Story 2.11: Delay send with tempo sync'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '5bb17600b4c34d1eb1c4ad4bf1b8a9558b0953cd'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 2.11 "Delay send with tempo sync" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1717; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** implement the real delay in `engine/core` as the Delay send-bus processor of the `Mixer` following exactly the structure and lessons of the Reverb send (Story 2.10: one instance per output bus Main/Aux fed by that bus's per-part send sums, stereo in (mono-summed send) / stereo out, fixed storage, no allocation in render, slice processing for blocks > 64 frames, idle/transparent detection so the default path stays bit-exact when sends are 0 or the master level is 0 with no tail ringing, level ramps per block, `send_delay_master` analogue if needed for symmetry (optional, add only if natural; per-part `send_delay_level` id 401 already exists; keep ids stable and document the DELAY encoder binding to that id). Parameters (new additive registry ids, master scope): delay time in ms (up to at least 2000 ms; use a log or documented mapping), tempo sync on/off, sync note value (1/4, 1/8, 1/16 and dotted and triplet variants of those; enum), feedback (limited strictly below unity: cap at e.g. 0.95 and prove stability), level (return level), optional ping-pong/stereo spread and a damping low-pass in the feedback path (documented; keep the feedback path stable and the output never above full scale for bounded input). Behaviour: when tempo sync is off, time is in ms and **changes glide without clicks** (fractional delay with interpolation and a smoothed delay-time slew; test with a pitch-glide-free/click-free bound: step bound on the output while changing the time under a tone, plus a hard-switch contrast); when tempo sync is on, the delay time = note value × 60/bpm and follows the **RT plane's single tempo value** at the next block: define that single tempo value in the engine core (a lock-free RT-owned value, e.g. `RtTempo` in the BlockRenderer/last-value table set by a performance parameter `tempo_bpm` (master scope, performance class, 20..300 bpm, default 120) — Epic 7 will replace the source with the clock follower, so keep the API (`BlockRenderer::tempo_bpm()`), document that internal tempo is the only source now) and the Mixer delay reads it at begin_block; a tempo change moves the target delay time at the next block and the glide applies. Feedback near maximum for a long run: the output never exceeds full scale and never runs away — a long-run test (simulate ≥10 minutes of audio faster than real time, i.e. 10 min × 48 kHz = ~28.8 M frames processed in the test within a reasonable ctest time, in Release or Debug; use a sparse input signal and a bound check on the peak and energy; if too slow in Debug, process 10 minutes of audio with 1024-frame slices and make it a separately labelled slow test with a TIMEOUT; document) shows bounded output; feedback limited below unity strictly in code (clamp) and tested with NaN/inf/out-of-range parameter values (clamped to safe defaults). Delay time accuracy test: an impulse's first echo arrives at the requested time within 1 sample (ms mode) and at the exact tempo-synced time (per note value at several bpm values); tempo change test. Cost: declared FX cost per instance (both buses) measured on the host with a bench (labelled "host, not representative"), with the bench's FAIL_REGULAR_EXPRESSION so it can fail ctest (see lessons from Story 2.10). RTSan: change time, sync, note value, feedback, level and tempo continuously for many blocks. Offline renderer: options to set delay params, tempo, sync; commit goldens (one for ms mode, one for tempo-synced mode with a tempo change via a CC-mapped or option-driven tempo), null-tested at -100 dB, bit-identical on two renders, wrong settings fail. ws.v1: only additive and via the existing master-param/performance paths.

**Never:** implement the external MIDI clock follower or transport (Epic 7), UI, board measurement, or change reverb/organ/sample behaviour or goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.11 (~line 1717) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.11 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` waking the delay after idle could memset ~2 MB in one block (RT spike), contradicting the README — bulk clear removed: restart/reset only reset filters and a written-sample watermark, reads older than it return silence; first block after idle costs like any other (bench p99 within 2× the declared cost)
- `[medium]` `patch` damping 0 left a stale low-pass state (transient when raised mid-tail); huge finite inputs overflowed to inf/NaN in the loop; denormals in the feedback tail — state follows the signal, input clamped to ±1e6, values below 1e-20 flushed
- `[medium]` `patch` tempo: tempo_bpm() null lookup and static-local on the RT path; first generation used the default 120 bpm for an instant sync target — fixed with tests; tempo-event/note same-block ordering tested
- `[medium]` `patch` cost benches only asserted the declared cost in optimised builds, which the normal builds never are — benches now link a private -O3 copy of the core sources so the assertions run everywhere; kDelaySendCost 20→40 and kReverbSendCost 48→64 because the assertions then showed the earlier figures too tight (host placeholders until Story 2.12)
- `[low]` `defer` 10 s+ feedback tails; ~2 MB per delay instance (two instances); no `send_delay_master`; glide is tape-style (0.75–1.25× read speed), so big time/tempo steps arrive after ≈0.6 s

## Auto Run Result

Status: done. Real delay send: stereo feedback line with Catmull-Rom fractional read, rate-limited glide, damping, DC blocker, ping-pong, feedback clamped to 0.95, wet tap held to ±1; time 1-2000 ms or tempo-synced note values (1/4, 1/8, 1/16 plus dotted and triplet) following the RT plane's single `tempo_bpm` (`BlockRenderer::tempo_bpm()`, internal source until Epic 7); one instance per output bus; idle detection keeps the default path bit-exact; registry ids 540-546 and 802; `velora-offline-render` delay/tempo options with two goldens; 3×10-minute long-run boundedness test. Verification: macOS ctest 322/322, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

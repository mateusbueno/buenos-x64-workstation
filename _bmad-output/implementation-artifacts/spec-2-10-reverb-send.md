---
title: 'Story 2.10: Reverb send'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '1f7ca087303f53baaab69cbc83657bdac21a638f'
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

**Problem and approach:** Story 2.10 "Reverb send" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1688; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** implement the real reverb DSP in `engine/core` as the Reverb send-bus processor of the `Mixer` (replace the Reverb placeholder; Story 2.7 gave each output bus (Main, Aux) its own send accumulator and its own placeholder instance — keep that: one reverb instance per output bus, both fed by their bus's per-part send sums; declared FX cost covers both instances; read `engine/core/README.md` and `mixer.hpp` for the exact structure; stereo in (mono-summed send), stereo out). Fixed storage, no allocation in render, fully clean-room (public algorithms only: e.g. a Schroeder/Moorer/FDN (feedback delay network) design with damping, pre-delay and diffusion; document the public references/algorithm in the README; no GPL reverb code consulted). **Types:** at least Room, Hall, Plate (each a documented parameter set of the same engine, e.g. different delay-line lengths, diffusion, damping, modulation); **time** parameter sets RT60 (range e.g. 0.2 s..10 s) and the RT60 measured by an impulse-response test (Schroeder backward integration, T20/T30 extrapolated to RT60) tracks the time parameter within 15% at 1 kHz for each type at several time settings (document the measurement method and test points); changing type while a tail rings cross-fades without a click (two engines running during a short cross-fade, or parameter-set interpolation; test with a step bound on the output during a ringing tail); **level** (return level) parameter. Registry (additive, stable ids): structural or performance as fits AD-2 — `reverb_type` (enum room/hall/plate), `reverb_time`, `reverb_level` (master scope) and the **REV master send level** parameter exposed as one performance parameter (`send_reverb_master`, master scope, 0..1) that scales the reverb send of all parts together (so the 9th fader in MIXER can drive it later), in addition to the per-part `send_reverb_level` (id 400, already exists, smoothed per block); only that part's contribution changes when a part's send level changes (test: change part A's send, part B's wet contribution is unchanged); CC mappings only if natural (document). The ws.v1 path: new master params come through the existing `SetMasterParams`/GetState/performance mechanisms (additive only). Cost: declare per-instance cost units measured on the host with a worst-case test (all 8 parts sending at the longest time, 128-voice organ+samples background not needed): add a host benchmark test printing the worst block time labelled "host, not representative" and set the declared FX cost so it covers the measured worst block time ratio-wise relative to the other effects (final board measurement is Story 2.12; document that). Everything must keep the default path bit-exact when the reverb is idle (send levels 0 and master level 0: the Mixer's trivial/identity detection must treat an idle reverb as transparent; define and test: with all send levels 0 the output equals the pre-reverb behaviour bit-for-bit, and the reverb does no work). Offline renderer: options to set reverb type/time/level and REV master; commit a golden (short render of a note with the reverb tail, null-tested at -100 dB, bit-identical across two runs) per type or one combined golden covering the three types sequentially; wrong-setting renders must fail. RTSan test: change type/time/level/sends continuously for many blocks.

**Never:** implement delay (2.11), UI/panel, board measurement, convolution/IR-based reverbs, sample-rate conversion, or change organ/sample goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.10 (~line 1688) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.10 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` ReverbSend::process overflowed its stack arrays for blocks > 64 frames — processed in 64-frame slices (200/1/7/320-frame blocks bit-identical to 64-frame blocks)
- `[high]` `patch` reverb_bench / organ_bench used PASS_REGULAR_EXPRESSION so a failing cost assertion could never fail ctest — fail regexes added and verified by lowering kReverbSendCost (Release build fails as it should)
- `[medium]` `patch` level-0 clear dropped a pending/ongoing type change; idle path left a stale level ramp; REV master ramp stalled in the inactive branch and part sends could jump when the master left 0 — fixed with tests
- `[medium]` `patch` live level changes (1→0.3, 1→0), idle-time type/time changes (Mixer and ReverbSend) and level-0-with-sends behaviour untested — tests added
- `[low]` `defer` reverb cost 48 units is a host Release ratio (covers the cross-fade worst case) until the board measurement in Story 2.12; three per-type goldens instead of one combined (the offline renderer cannot switch type mid-render); each instance uses ≈0.3 MB fixed memory (2 instances); level 0 with sends up keeps the Mixer on the routed path (documented)

## Auto Run Result

Status: done. Real reverb send: 8-line FDN (Householder) with damping, pre-delay, Schroeder diffusers and modulated lines; types room/hall/plate; RT60 0.2-10 s measured at 1 kHz (worst deviation 8.0% at 0.3 s, ≈2% from 1.2 s up, limit 15%); 50 ms click-free type cross-fade; one instance per output bus; per-part send smoothing; `send_reverb_master` (REV master) and `reverb_type/time/level` registry parameters; idle detection keeps the default path bit-exact; three committed goldens; `reverb_bench`. Verification: macOS ctest 250+ pass (2 env-gated skips), Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.

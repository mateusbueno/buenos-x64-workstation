---
title: 'Story 2.3: Organ percussion and key click'
type: 'feature'
created: '2026-10-18'
status: 'done'
baseline_revision: '15f8c853330f9ce886aa9469d6fcee847649d50e'
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

**Problem and approach:** Story 2.3 "Organ percussion and key click" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1485; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints (clean-room, same rules as Story 2.2 / `engine/core/ORGAN.md`; no GPL organ source consulted, record the public facts used in ORGAN.md):** extend `OrganEngine` with percussion (2nd/3rd harmonic, fast/slow decay, normal/soft volume; single-triggered: sounds on a key press only when no other organ key of that part is held; 1' drawbar cancelled while percussion is on; normal volume lowers the drawbar level; document the exact time constants and gain factors with their public-fact justification) and key click (level parameter, 0 = off; short broadband click per contact event at the key voice's onset and release, scaled by the level and varying per key and per press from a seeded deterministic generator so results are reproducible and repeated notes are not identical). Parameters already exist in `proto/params.json` (`percussion_on/harmonic/decay/volume`, `key_click`); route them through the existing part-param path; add nothing unless missing and keep ids stable. RT-safe (no allocation in render), part state written only in the phase-2 hook or its own voice state per the Story 2.2 rules, tables fixed-size. The offline renderer needs `--engine organ` options to set percussion and key click (e.g. `--percussion third,fast,soft`, `--key-click 0.5`) or a MIDI-controllable path via the mapped CCs; commit a small MIDI file exercising percussion settings (legato vs staccato, both harmonics, both decays, both volumes) and key click, render it deterministically (bit-identical across runs, tested) and commit the golden WAV (keep it small, a few seconds at most) with a ctest that compares fresh render to the golden byte-for-byte or via the null-test harness with a very tight tolerance. Unit tests: single trigger (legato does not retrigger; releasing all keys then pressing retriggers), decay follows the selected constant (measure the envelope), 1' cancelled, normal vs soft level relation, harmonic selection by spectrum, key click present at onset and release, varies per key, level 0 silent, determinism.

**Never:** add vibrato/chorus, leakage, rotary, multi-contact onset hooks (later stories), change the sample engine or its null-test goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 2.3 (~line 1485) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 2.3 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` ramp tests could not detect hard steps — block-by-block monotonic/exact 240-frame test
- `[medium]` `patch` release click inside the onset burst overwrote it; click seed engine-wide (cross-part dependence); reclaim cut pending clicks; voice_info level ignored percussion/click — two additive bursts, per-part press counter, reclaim preference, level fixed
- `[medium]` `patch` golden covered 6 of 8 percussion combinations; CC events altered non-organ renders — all 8 combinations, CC handled only for the organ and channels < 8, queue overflow checked
- `[low]` `patch` release-click level, enum CC boundaries 63/64, pedal x percussion single-trigger untested — tests added
- `[low]` `defer` percussion tau/gain/-3 dB lowering are design values from rough public descriptions (to be refined by the Story 2.13 A/B); percussion per voice means legato keys never get a tone even if the first envelope still sounds

## Auto Run Result

Status: done. Organ percussion (2nd/3rd harmonic, fast/slow, normal/soft, single-triggered, 1' cancelled, drawbar lowering) and key click (seeded per key/press, onset and release bursts) in `OrganEngine`; CC 84-86 mapping; `velora-offline-render --percussion/--key-click`; committed 4.85 s golden covering all eight combinations (null-tested at -100 dB). Verification: macOS ctest all pass, Docker gcc 72/72 and clang-20 RTSan 80/80, purity and notices checks pass. Follow-up review recommended: false.

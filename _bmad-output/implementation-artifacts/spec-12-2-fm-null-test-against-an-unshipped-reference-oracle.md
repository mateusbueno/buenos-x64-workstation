---
title: 'Story 12.2: FM null test against an unshipped reference oracle'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '791fddd'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/fm_engine.hpp'
  - '{project-root}/engine/core/include/velora/core/fm_voice.hpp'
  - '{project-root}/engine/core/include/velora/core/fm_parameters.hpp'
  - '{project-root}/formatlib/include/velora/format/sysex_6op.hpp'
  - '{project-root}/tools/oracle/README.md'
  - '{project-root}/tools/oracle/build-fm-oracle.sh'
  - '{project-root}/tools/oracle/render-fm-oracle.sh'
  - '{project-root}/tools/offline-render/src/render.hpp'
  - '{project-root}/tools/offline-render/src/render.cpp'
  - '{project-root}/tools/offline-render/src/render_main.cpp'
  - '{project-root}/tools/offline-render/CMakeLists.txt'
  - '{project-root}/tools/ci/check_third_party_notices.py'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** To ensure clean-room 6-operator FM audio fidelity without violating licensing terms or shipping GPL code, the FM engine must be null-tested against a proven reference implementation (Dexed / MSFA 6-operator core) used strictly as an unshipped test oracle.

**Acceptance Criteria (from Epics - Story 12.2):**
1. **Unshipped Oracle Architecture (NFR22, NFR28, NFR27, AR2):**
   - Reference FM implementation (MSFA / Dexed core) is fetched and built only in test tooling (`tools/oracle/.work/`), never vendored into any shipped directory.
   - The license manifest check (`tools/ci/check_third_party_notices.py`) verifies no GPL oracle code is vendored or linked into shipped targets.
   - The oracle is driven as a black box through its native input format (155-byte / SysEx voice dump) via a test-only adapter in `tools/` that translates Velora FM parameters into oracle parameters.
2. **Committed FM Test Suite & Null Testing:**
   - A committed FM test suite exercises algorithms (serial, parallel carriers, complex feedback), feedback levels (0..7), operator envelopes (fast attack, slow attack, decay-sustain, long release), key scaling, and velocity scaling with LFO off.
   - The null-test harness compares Velora offline render with oracle render: residual is below −50 dB relative to reference RMS.
   - Decided tolerance (−50 dB) and its reasoning (clean-room floating-point DSP vs integer lookup tables / rounding in reference implementations) are recorded with the test suite.
   - The FM null tests run in CI on every test run.
3. **Negative Controls / Broken Change Detection:**
   - A deliberately broken FM change (e.g. wrong feedback scaling or algorithm mismatch) causes the null test to fail.

</intent-contract>

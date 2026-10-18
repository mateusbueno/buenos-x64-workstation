---
title: 'Story 1.3: Monorepo skeleton with host build and CI'
type: 'chore'
created: '2026-10-18'
status: 'done'
baseline_revision: '7d9685a0b976c17ac2e861f2d046a2dc7d63a20b'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The repo holds only planning docs. Every following story needs a place to land and an automated gate (unit tests, RealtimeSanitizer, core-purity check) from the first commit.

**Approach:** Create the minimal monorepo skeleton from the architecture Structural Seed, a top-level C++20 CMake build, a dependency-purity check for `engine/core/`, and a GitHub Actions workflow on macOS and Linux with a RealtimeSanitizer job proven by an expected-fail RT-unsafe test.

## Boundaries & Constraints

**Always:** Create only `engine/core/`, `engine/ports/`, `engine/host/`, `tools/offline-render/`, `tools/spikes/`, each with a README stating its role and dependency rule (core may include only core, ports and the C++ standard library). C++20, CMake, builds on macOS (Apple clang) and Linux (gcc and clang). RTSan test uses `[[clang::nonblocking]]`.

**Never:** Create other seed directories (`ui/`, `yocto/`, `formatlib/` etc.), add real DSP, add third-party dependencies beyond a vendored-free test harness (use a tiny in-repo test runner or CTest executables; no network fetches), or reference Linux/ALSA/OP-TEE/hardware headers from `engine/core/`.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Clean core | `engine/core` sources include only core/ports/std headers | purity check exits 0 | none |
| Violation | a core file includes `<alsa/asoundlib.h>` or `../host/x.h` or links a non-core target | purity check exits non-zero naming file and include | CI fails |
| RT-unsafe code | allocation inside a `[[clang::nonblocking]]` function in the RTSan test | RTSan build aborts that test; CTest expects failure (WILL_FAIL) so suite passes and proves the gate | n/a |
| Apple clang | no RTSan support | RTSan target is skipped with a clear CMake message, not a build error | message |

</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md` -- Structural Seed (line ~445), read-only
- `_bmad-output/planning-artifacts/epics.md` -- Story 1.3 ACs (line ~842), read-only
- Toolchain on this Mac: cmake at /opt/homebrew/bin, Apple clang 21 (no -fsanitize=realtime; Homebrew LLVM provides it if installed)
- Repo root has only `_bmad/`, `_bmad-output/`, `.gitignore`; no code yet

## Tasks & Acceptance

**Execution:**
- `CMakeLists.txt` -- top-level C++20 project, enable_testing, add the five subdirs, option VELORA_RTSAN -- single host build entry
- `engine/core/`, `engine/ports/`, `engine/host/` -- minimal header+source placeholder (e.g. a version constant) with INTERFACE/static libs, tests under each `tests/` -- gives targets to test
- `engine/core/README.md`, `engine/ports/README.md`, `engine/host/README.md`, `tools/offline-render/README.md`, `tools/spikes/README.md` -- role and dependency rule line each
- `tools/ci/check_core_deps.py` (or cmake script) -- scan `engine/core/` includes and CMake link libraries; fail on anything outside core/ports/std; plus a self-test fixture proving it fails on a violation
- `engine/core/tests/rtsan_unsafe_test.cpp` + CMake -- RT-unsafe allocation in `[[clang::nonblocking]]`; only built with clang that supports `-fsanitize=realtime`; registered WILL_FAIL under RTSan
- `.github/workflows/ci.yml` -- jobs: macOS (build+ctest), Linux gcc, Linux clang (>=20) with RTSan build+ctest, purity check
- `.gitignore` -- add `build*/`

**Acceptance Criteria:**
- Given a fresh clone, when `cmake -S . -B build && cmake --build build && ctest --test-dir build` runs on macOS, then it builds and all tests pass
- Given a core file with a forbidden include, when the purity check runs, then it fails; given the clean tree it passes
- Given a clang with RTSan, when the RTSan build runs ctest, then the deliberate RT-unsafe test is detected (expected fail) and the suite is green
- Given a push or PR, when CI runs, then macOS, Linux gcc and Linux clang/RTSan jobs and the purity check all run

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0; and its self-test shows non-zero on a violating fixture
- RTSan: if a clang with `-fsanitize=realtime` is available locally (e.g. Homebrew llvm), configure with it and run ctest; otherwise state it is verified only in CI

## Review Triage Log

### 2026-10-18 — Review pass
- verdicts: 5 reviewer layers, ~40 findings — patched 12, deferred the rest
- findings:
  - `[medium]` `patch` rtsan_unsafe_test passed on any non-zero exit — replaced WILL_FAIL with PASS_REGULAR_EXPRESSION "RealtimeSanitizer" and -O0
  - `[medium]` `patch` quoted non-core includes (e.g. "alsa/x.h") passed lexically — now require the file to exist under engine/core (realpath)
  - `[medium]` `patch` `$<...>` link items and target_link_options were skipped — now flagged (only -fsanitize= options allowed)
  - `[medium]` `patch` find_package/FetchContent/pkg_check_modules in core not detected — now flagged
  - `[low]` `patch` self-test accepted any non-zero exit; fixture lacked quoted-include and generator-expression cases — asserts exit 1, fixture extended
  - `[low]` `patch` empty ctest and missing Python in CI went green — `--no-tests=error`, Python REQUIRED when CI is set
  - `[low]` `patch` duplicate push+PR runs, no permissions — push limited to main, concurrency group, contents:read
  - `[low]` `patch` `build*/` ignore too broad — anchored `/build*/`
  - `[low]` `defer` line continuations, block comments, macro includes, #include_next not parsed — text scanner limitation
  - `[low]` `defer` engine/ports and host/tools README rules not enforced — only the core rule is a stated AC
  - `[low]` `defer` apt.llvm.org key pinning (signed-by, https), SHA-pinned actions, warning flags, sanitizer matrix — CI hardening for later
  - `[unverified]` `defer` RTSan syntax and package names `clang-20`/`libclang-rt-20-dev` only verifiable on first CI run

## Auto Run Result

Status: done. Added the monorepo skeleton (engine/core, engine/ports, engine/host, tools/offline-render, tools/spikes with READMEs), top-level C++20 CMake with five ctest tests, `tools/ci/check_core_deps.py` purity gate with self-test fixture, an RTSan safe/unsafe test pair (built only when the compiler supports `-fsanitize=realtime`), and `.github/workflows/ci.yml` (purity, macOS, Linux gcc, Linux clang-20 RTSan).
Verification: macOS Apple clang fresh configure/build/ctest 5/5 pass; purity check and self-test pass. RTSan path and Linux jobs are unverified locally (no RTSan clang here); they run on first push. Follow-up review recommended: false.

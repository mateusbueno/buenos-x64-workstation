---
title: 'Story 5.1: Full SFZ v1 opcode subset in the importer'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '2c9a8181c0d2f3ed3a3657a1ca01f9dd629bddab'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-5-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 5.1 "Full SFZ v1 opcode subset in the importer" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~3537; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** extend the Story 1.13 importer (`formatlib/src/sfz_import.cpp`, `instrument_model.hpp`, bundle manifest region table, `librarian/` CLI/`import_sfz`) — read `formatlib/README.md`, `librarian/README.md`, `_bmad-output/business/11-12-sfizz-license-check.md` (only the vendored sfizz parser closure may be used; no new vendored files without extending that table). Add to the instrument model and importer: keyswitches (`sw_lokey`, `sw_hikey`, `sw_last`, `sw_down`, `sw_up`, `sw_previous`, `sw_default`), CC crossfades (`xfin_locc<N>`/`xfin_hicc<N>`, `xfout_locc<N>`/`xfout_hicc<N>`, `xf_cccurve` gain|power), loops (all four `loop_mode`s and `loop_start`/`loop_end` already partially mapped in 1.13 — make sure complete incl. `loop_crossfade`? NOT in the list: keep strictly to the listed opcodes) and basic filters (`fil_type` lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p, `cutoff`, `resonance`); a region may have keyswitch ranges (`sw_lokey/sw_hikey` define the keyswitch range, `sw_last`, `sw_down`, `sw_up`, `sw_previous`, `sw_default`), and crossfades keyed by CC number (the opcode suffix is the controller number; support several CC crossfades per region up to a documented bound, e.g. 4 xfin + 4 xfout CCs). **Bundle format:** bump the bundle `format_version` **minor**: today `kFormatVersion` is a single integer (2): design the major/minor split minimally and compatibly (e.g. keep `format_version` major and add a `format_minor` header field, or introduce the region-record extension via a new record version byte) so that the reader accepts N and N−1, every Epic 1 demo bundle (format 2 without the extension) still opens and **renders bit-identically** (Story 1.13/1.15 goldens and the committed fixtures `tools/offline-render/tests/sample_suite`, `layer_split` etc. must keep passing unchanged; regenerate nothing existing); the extended region data lives in an additive section of the manifest (the region record stays 96 bytes for old regions; extensions in a separate per-region extension table indexed by region, so old readers can ignore it — document the exact layout and add golden hex literals like the 96-byte region record golden test). **Import report:** extend `librarian import` / the library API with a structured per-file import report (reason codes) — a file using an opcode outside the subset (scripting, effect buses, any unlisted opcode that affects sound) is **skipped** with reason code `unsupported_features` and the list of offending opcodes with file:line, and the other files of a batch still import (add batch support: `librarian import <dir-or-list of sfz files>`; each file is its own bundle/library as before, or a batch that continues past failures — keep one SFZ = one library per Story 1.13 and process many files; the batch result lists per-file status); an **explicit allow-list of non-sounding opcodes** (for example `label_cc`, `label_key`, `image`, `hint_*`, `sw_label`, `master_label`, `group_label`, `region_label`, `set_cc` ... decide a documented list of purely informational opcodes) is accepted and logged (counted in the report) instead of causing a skip; a file that cannot be parsed, references a missing sample, or points at an unreadable or truncated WAV is skipped with reason code `damaged` and the failing path, and **nothing of that file is written** (the stage/verify/publish discipline from 1.13 already ensures nothing partial; test it). Machine-readable report: JSON lines or a stable text format on stdout/`--report FILE` (document). Reason-code enum is closed. A unit test **per opcode** compares the parsed region table with the bundle's instrument table for a fixture SFZ using it (generate fixtures in code: small synthetic WAVs + SFZ text; compare field by field after a write/read round trip through the bundle), plus negative tests (each unsupported opcode category: scripting `<control>`+ `script=`, effect `<effect>` headers / `effect1`, `lfo`, `eg`, `amplitude_oncc` etc.; mixed batch). Keep the existing Story 1.13 opcode subset behaviour for files that remain within it (same region tables, same bundle ids where the extension table is empty: a bundle without extended features must be byte-identical to what 1.13 produced — test with the committed fixture sources). Docs: README updates (formatlib, librarian) with the supported opcode table (supported / allow-listed / unsupported) as one authoritative list generated or checked by a test against the implementation.

**Never:** implement playback of keyswitches/crossfades/filters in the engine (Story 5.2), SF2 (5.3), UI, categories (5.5), preflight (5.7), or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 5.1 (~line 3537) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 5.1 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` sound-affecting opcodes accepted but silently dropped (`fil_type`/`resonance` without `cutoff`) — still "no filter" but always reported as an `ignored_opcode` warning with file:line; non-canonical controller suffixes (`xfin_locc007`) are damaged; per-issue `failing_path`
- `[medium]` `patch` writer-side rejections reported as "damaged WAV" — `status_for_add_sample_error` classifies them (invalid argument / I/O => failed, unsupported format => unsupported_features)
- `[medium]` `patch` batch robustness: directory walk dropped files after the first error, duplicate inputs imported twice, invalid UTF-8 in the JSON report, unwritable `--report` hid the causes — manual recursion with per-directory failed entries, weakly_canonical dedupe, UTF-8 validation (U+FFFD), reasons still printed
- `[medium]` `patch` a minor-1 bundle with no extension records had two valid encodings — rejected as kCorruptManifest; batch CLI text, `failed` report lines and the overstated bundle_test comment fixed
- `[low]` `defer` until Story 5.2 an imported instrument with keyswitches/crossfades/filters sounds all its regions in the engine; a minor-1 bundle is refused with kUnsupportedVersion by readers that do not know minor 1 (the extension is not ignorable by old readers); `set_cc<N>` is deliberately not allow-listed (it affects crossfades); `status_for_add_sample_error` branches are covered by unit tests of the function only (not reachable from SFZ input)

## Auto Run Result

Status: done. Instrument model and importer extended with keyswitches (`sw_*`), CC crossfades (`xfin_*/xfout_*`, `xf_cccurve`, up to 4+4 controllers), the four loop modes and basic filters (`fil_type`, `cutoff`, `resonance`); bundle `format_version` is now `major | minor << 16` (major 2, minor 1 written only when an extended feature is used, so Epic 1 bundles stay byte-identical; reader accepts N and N−1); a documented allow-list of non-sounding opcodes; structured JSON Lines import report with closed reason codes (`unsupported_features`, `damaged`) and batch import (`librarian import <files or dirs>`) that continues past skipped files and writes nothing of a skipped file; a README opcode table checked against the implementation by a test. Verification: macOS ctest 449 (2 env-gated skips), Docker gcc 443 and clang-20 RTSan 457, purity and notices checks pass. Follow-up review recommended: false.

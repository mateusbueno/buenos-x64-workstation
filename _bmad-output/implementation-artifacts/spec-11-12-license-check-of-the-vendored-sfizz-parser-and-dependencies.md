---
title: 'Story 11.12: License check of the vendored sfizz parser and dependencies'
type: 'chore'
created: '2026-10-18'
status: 'done'
route: 'oneshot'
review_loop_iteration: 0
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Story 1.13 will vendor the sfizz 1.2.3 SFZ parser into `formatlib`, but no written license check exists for the files it pulls in or for their transitive dependencies (abseil, ghc filesystem, simde), so the closed product could ship code whose obligations are unmet.

**Approach:** Inventory the parser's exact transitive file set at sfizz tag 1.2.3 (commit 4e70dc0bef53b41f2853ed46e26f5911114c92d0) and its third-party dependencies, record a license table under `_bmad-output/business/`, add a root `THIRD_PARTY_NOTICES.md` carrying the required BSD-2/Apache-2.0/MIT notices, and add a CI check that the notices cover the table and sfizz is never linked into a shipped target.

</frozen-after-approval>

## Implementation Notes

- Closure computed from sfizz 1.2.3 (32 files); simde IS required (MathHelpers.h), abseil and ghc::filesystem too. No copyleft found.
- Added `_bmad-output/business/11-12-sfizz-license-check.md`, `THIRD_PARTY_NOTICES.md`, `tools/ci/check_third_party_notices.py` (+ manifest), wired into ci.yml and docker-check.sh.
- About-screen and image install of the notices belong to the stories building those surfaces.

## Review Triage Log

- medium (patched): regex missed `sfizz_parser`; now substring match, comments stripped, `tests/` exempt at any depth, files closed, self-tests added.
- medium (patched): `OpcodeCleanup.re` missing from table; added. Notices header/H1, SIMDe holders, Abseil change-notice wording fixed.
- low (rejected/deferred): manifest did not list the files and cannot compare the vendored tree (no vendored tree exists until Story 1.13); abseil file-level list added to the doc as a 1.13 vendoring condition.
- low (rejected): CI step naming, CC0 text, non-CMake build files; no such files exist yet.

## Code Review (2026-10-19)

- decision A applied: gate flags only sfizz dependency/link/fetch commands (CMake statements, multi-line aware); `sfizz_parser`/`sfizz::parser` allowed.
- patched: top-level-only `tools`/`build` exemption, `tests` at any depth; utf-8; 32-file count; verdict wording; sign-off pending; shipped notices header cleaned; self-test expanded (no bare assert).
- deferred items actioned: non-CMake build files scanned; manifest now carries file list and pins, checked against the table, notices headings and `vendor_dirs`. Image/About install tracked in deferred-work.md.

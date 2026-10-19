---
title: 'Story 5.5: Category model, localized labels and auto-suggestion'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: 'd5689a07bb06459eb6553349dfa827abcdb5b53f'
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

**Problem and approach:** Story 5.5 "Category model, localized labels and auto-suggestion" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~3651; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** (1) **Category type in `proto/`:** add `proto/category.proto` (package `velora.library.v1` or consistent with existing protos) defining enum `Category` with `CATEGORY_NONE = 0` (explicit "none": usable as a filter/"unassigned" sentinel in UI/queries but can **never be stored on an imported instrument**: enforce in the type helpers and in the librarian/`formatlib` writer path) and the 17 categories — Piano, E.Piano, Organ, Keys, Mallet & Bells, Strings, Brass, Woodwind, Choir & Vocal, Guitar, Bass, Synth Lead, Synth Pad, Synth Poly, Drums & Perc, World, FX & Ambient — with stable numeric ids 1..17 that only grow by addition (never renumbered; a reserved-list/guard test), plus a C++ header generated or hand-maintained in `formatlib/` (and generated protobuf in hosts) with constexpr helpers `is_assignable(Category)`, `key(Category)` (stable string key such as `category.piano`, `category.epiano`, `category.mallet_bells`, ...), count. The bundle manifest already carries a free-text category file/field from Stories 1.13/5.1? — check `librarian/README.md` (`--category`, `category` text file in the bundle dir): migrate to the new enum (numeric id stored in the manifest in an additive way: inspect the current storage and keep old bundles readable; do NOT change bytes of bundles without a category; if the manifest cannot carry it without a format bump, store it in the bundle dir `category` file as the stable key string and validate on read: choose the least invasive, document). `librarian import --category NAME` accepts keys/English names (case-insensitive) and rejects unknown names and `none`. (2) **Locale catalog:** the UX `EXPERIENCE.md` has a Categories table with full and short labels in EN, PT-BR and ES (find it: search the file for "Categories"); there may be no locale catalog code yet (Epic 10 owns the full i18n system, Story 10.7): create the minimal shared **locale catalog format** needed now, in a way Epic 10 can extend: `locale/en.json`, `locale/pt-BR.json`, `locale/es.json` (or `.po`-like; choose JSON with flat string keys per AD-20 "Externalized strings" in the architecture spine — read AD-20 and follow its rules: keys, English default, placeholders policy) containing the category keys `category.<id>.full` and `category.<id>.short` exactly as in the EXPERIENCE.md Categories table (copy the labels verbatim from the table; do not invent), loaded by a tiny `LocaleCatalog` C++ reader in `formatlib/` or a new `locale/` library (portable, no heap issues on the host: a simple JSON subset parser written in-house or reuse protobuf's JSON util in host-only code — keep `engine/core` free of it) with lookup by locale (`en`, `pt-BR`, `es`), fallback to English for missing keys (but see test), and a **test that fails if any category key is missing in any shipped locale or any short label exceeds 10 characters (count Unicode code points, not bytes)**, plus a test that the three locale files have exactly the same key set. (3) **Suggestion module in `librarian/`:** `suggest_category(SuggestInput{file_name, folder_names[], metadata_strings[] (SFZ `<control>` labels/comments, SF2 preset and bank names)}) -> optional<Category>`: tokenise (lowercase, strip accents (PT/ES diacritics) via a small built-in folding table, split on non-alphanumerics, underscores, camelCase, digits), match a **versioned keyword table in EN, PT-BR and ES** stored as a data file (`librarian/data/category_keywords.json` with `table_version`) with weights/priorities and conflict resolution rules (e.g. `rhodes`, `wurli`, `wurlitzer`, `epiano`, `electric piano`, `dx` no... only unambiguous words → E.Piano; `viola`, `violino`, `violin`, `cello`, `strings`, `cordas`, `cuerdas`, `orchestra` → Strings; `upright`, `baixo`, `bajo`, `bass` → Bass; `rhodes`... ), precedence file name > metadata > folder names, longest/most specific keyword wins, ties → none, never guess ambiguously: `Viola_Caipira` (a Brazilian guitar, ambiguous with the string instrument `viola`) must yield **none** — encode such exceptions explicitly (negative keywords/phrases like `viola caipira` → Guitar? The story says none: obey: none). The table is data, loaded at runtime from the data file (and embedded as a fallback string at build time via a CMake step so the librarian binary needs no file); document the format and the versioning policy. **Fixture test:** at least 60 real-world style file names (build the fixture file `librarian/tests/category_fixtures.tsv` with `name<TAB>expected` entries mixing names from well-known free libraries — Salamander Grand Piano, VSCO2 instrument names (e.g. "Violins Sustain", "Bass Clarinet", "Tuba"), Sonatina, Karoryfer instruments, Pianobook-style names (e.g. "Rhodes_Mk1", "Upright_Bass_Jazz", "Wurly_200A", "Church Organ", "Vox Humana"), PT-BR/ES examples ("Piano Cauda", "Violão Nylon", "Baixo Elétrico", "Cordas Ensemble", "Coro Feminino", "Sanfona", "Cuerdas Sintéticas"...), SF2 GM preset names ("Acoustic Grand Piano", "Church Organ", "Nylon Guitar", "Synth Strings 1", "Standard Kit")) where each expected value is a category key or `none`; the test asserts every row, prints mismatches, and an extra precision gate: no row expected `none` may get a category (the module must prefer none over a wrong guess). Include the three examples from the story: `Rhodes_Mk1` → E.Piano, `Upright_Bass_Jazz` → Bass, `Viola_Caipira` → none. Wire the module into the librarian import path as a **suggestion only** (the report/`ImportResult` carries `suggested_category`; the import itself still requires an explicit category from the caller (UI/CLI) — Story 5.7 uses it in preflight); expose a CLI `librarian suggest-category <name>...` for quick checks.

**Never:** UI, preflight (5.7), the catalog (5.6), the full i18n system/language switching (10.7), changing bundle bytes for existing bundles, or inventing label text (copy from EXPERIENCE.md).


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 5.5 (~line 3651) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 5.5 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` SFZ metadata scan could block forever on a FIFO/device or exhaust memory on a huge single line — regular files only (O_NONBLOCK + fstat), bounded chunked reads, per-line cap; overlong/surrogate UTF-8 no longer decodes as letters
- `[high]` `patch` locale catalog returned `string_view`s into storage that reloads could invalidate and could be left half-loaded — std::string copies, all-or-nothing load, tag normalisation (`pt_br` → `pt-BR`, regional fallback), number parsing locale-independent, input size cap
- `[medium]` `patch` suggestion robustness: label values starved by header comments, `//` inside opcode values, `/* */` blocks, silent empty embedded table (now loud: stderr, debug assert, `suggest-category` exit 3), SF2 parse failures lost the suggestion, relative paths/volume names leaked into the folder hints, non-ASCII junk accepted by `parse_category`, legacy category files indistinguishable from absent ones (`read_category_file` tri-state), control characters echoed raw by the CLI
- `[low]` `defer` the UX table has no label for "none" (no `category.none` key; `category_label(kNone)` is empty by design); the 95 fixture rows (13 expected none) were designed together with the table, so they overstate real-world accuracy (ambiguous words like `horn`, `gaita`, `contrabass`, `grand` deliberately excluded); SFZ metadata is a plain text scan (no `#include`), SF2 bank names are not in the model; Epic 10 will add keys, so the "exactly 34 keys" assertion must be relaxed then; the exit-3 path of the CLI is not tested; old bundles with free-text category files are readable but never rewritten to the key form

## Auto Run Result

Status: done. `proto/category.proto` (17 categories with stable ids 1..17 plus an explicit never-storable `none`), a C++ twin in `formatlib` with parsing helpers, the librarian's `category` file now holding the stable key, `locale/{en,pt-BR,es}.json` with full and short labels copied verbatim from the UX Categories table (tests: identical key sets, nothing missing, short labels ≤10 code points), a `LocaleCatalog` reader, and the librarian suggestion module (`librarian/data/category_keywords.json` EN/PT-BR/ES versioned table embedded at build time, precedence file name > metadata > folders, ties and exceptions give none, e.g. `Viola_Caipira` → none) wired into the import result/report and a `librarian suggest-category` CLI; 95 fixture rows with a no-wrong-guess precision gate. Verification: macOS ctest 489 (2 env-gated skips), Docker gcc 483 and clang-20 RTSan 498, purity and notices checks pass. Follow-up review recommended: false.

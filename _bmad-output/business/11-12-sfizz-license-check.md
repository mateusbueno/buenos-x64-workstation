# Story 11.12 — License check: vendored sfizz parser and dependencies

- **Date:** 2026-10-18
- **Upstream:** https://github.com/sfztools/sfizz, tag `1.2.3`, commit `4e70dc0bef53b41f2853ed46e26f5911114c92d0` (archived upstream; BSD-2-Clause per `LICENSE`, SPDX headers per file)
- **Scope:** the exact transitive file closure of upstream CMake target `sfizz_parser` (`SFIZZ_PARSER_HEADERS`/`SFIZZ_PARSER_SOURCES` + OpcodeCleanup.re/.cpp), computed by following every `#include` that resolves inside `src/sfizz`, plus the external libraries those files include. Story 1.13 must vendor exactly this set (or a subset); adding any file requires extending this table first.
- **Method:** include-closure script over the 1.2.3 tree, per-file `SPDX-License-Identifier` check, license files read at the pinned submodule commits.

## Verdict

No GPL/LGPL/AGPL or other copyleft code is in the vendored set. All components are BSD-2-Clause, MIT or Apache-2.0 (plus CC0 for two simde headers). Obligations are attribution/notice only; are met by the content of `THIRD_PARTY_NOTICES.md`; installing it in the image and showing it in Velora Studio's About is tracked as a follow-up (see below) and is not done yet. BSD-2-Clause has no endorsement clause, but the product still must not use sfizz or contributor names to imply endorsement; THIRD_PARTY_NOTICES.md says so.

## Dependency table

| Component | Version / pin | License | Copyright holder | Obligations met |
|---|---|---|---|---|
| sfizz parser (32 files below) | 1.2.3 / `4e70dc0b` | BSD-2-Clause | sfizz contributors (AUTHORS.md; maintainer Paul Ferrand) | yes — notice in NOTICES §1 (source retains copyright + conditions; binary reproduces them) |
| abseil-cpp (`absl/strings`, `container` flat_hash_map/set, `types` optional/span, `meta`, `memory`, `algorithm` and their internal deps: `base`, `hash`, `numeric`) | submodule `c2435f83` | Apache-2.0 | The Abseil Authors (Google LLC) | yes — NOTICES §2; license text shipped; repo has no `NOTICE` file (404 at pin); modified files must carry change notices |
| ghc::filesystem (`ghc/fs_std.hpp`, `ghc/filesystem.hpp`) | submodule `8a2edd6d` | MIT | Steffen Schümann, 2018 | yes — NOTICES §3 |
| SIMDe (`simde/simde-features.h`, `simde/x86/sse.h` and their includes) | submodule `2bf459d4` | MIT (`hedley.h`, `debug-trap.h` are CC0-1.0) | Evan Nemerson and SIMDe contributors | yes — NOTICES §4 |
| re2c-generated `OpcodeCleanup.cpp` | re2c 1.3 output | generated output is not covered by re2c's license (public-domain-style output exception); the `.re` source is BSD-2-Clause | sfizz contributors | yes — no re2c code shipped |

### Excluded on purpose (never vendored)

st_audiofile and its dr_libs/wavpack/libaiff code, the sfizz synth/renderer, fmidi, jsl, cxxopts, atomic_queue, cephes, threadpool, cmake/ and clients. sfizz as CI reference renderer is **test-only** (Story 1.15/1.9) and must never be linked into a shipped target; `tools/ci/check_third_party_notices.py` fails the build if a shipped build file (CMake dependency/link/fetch commands, `.gitmodules`, Dockerfiles, `.mk`, meson) references sfizz other than its parser; this is a text-level check, not a resolved link graph.

### Files in the sfizz closure

| File | Version | License | Copyright holder | Notice met |
|---|---|---|---|---|
| `src/sfizz/CCMap.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Config.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Defaults.cpp` | sfizz 1.2.3 | BSD-2-Clause (no per-file header; covered by repo LICENSE) | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Defaults.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/LFOCommon.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/LFOCommon.hpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/LFODescription.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/MathHelpers.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/OpcodeCleanup.re` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Opcode.cpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Opcode.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/OpcodeCleanup.cpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/Range.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/SIMDConfig.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/SIMDHelpers.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/SfzFilter.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/SfzHelpers.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/modulations/ModId.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/modulations/ModKey.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/modulations/ModKeyHash.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/Parser.cpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/Parser.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/ParserListener.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/ParserPrivate.cpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/ParserPrivate.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/parser/ParserPrivate.hpp` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/simd/HelpersScalar.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/utility/Debug.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/utility/LeakDetector.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/utility/Macros.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/utility/NumericId.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |
| `src/sfizz/utility/StringViewHelpers.h` | sfizz 1.2.3 | BSD-2-Clause | sfizz contributors | yes (NOTICES §1) |

Note: `Defaults.cpp` lacks a per-file SPDX header; upstream `LICENSE` (BSD-2-Clause) applies to the whole tree, so this is not a gap.

## Follow-ups (not blocking this story)

1. Story 1.13: vendor only the files above; keep this table in sync; consider replacing ghc::filesystem with `std::filesystem` (C++20) to drop one dependency.
2. Velora Studio About screen and the image's `/usr/share/licenses` copy of `THIRD_PARTY_NOTICES.md` are wired in the stories that build those surfaces; this story supplies the content.
3. Re-run the check whenever Story 1.13 changes the vendored set. The CI script verifies the manifest file list against the table above, notice headings against pinned versions, and any file under `vendor_dirs` against the list.
4. Open item (AC2, first Then): install `THIRD_PARTY_NOTICES.md` in the image and show it in Velora Studio's About. Tracked in `deferred-work.md`.

## Vendoring conditions for Story 1.13

- Vendor files unmodified with their per-file SPDX/copyright headers intact (BSD-2 source redistribution); record any modification in the file header.
- Abseil is vendored as the minimal closure of the headers listed in the table row; the file-level abseil list is produced when 1.13 vendors it and added here before merge.
- Sign-off: pending founder confirmation. This is an engineering license inventory, not legal advice; confirm with counsel before v1 launch if desired.

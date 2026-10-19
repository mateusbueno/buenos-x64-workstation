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

## Story 1.13 vendoring record (file-level lists)

Vendored on 2026-10-18 into `formatlib/third_party/` (one directory per upstream, upstream license file copied next to it). The sfizz list below is the 31-file closure table above plus `OpcodeCleanup.re` (32 files, unmodified, SPDX headers intact; `Defaults.cpp` has none upstream), plus upstream `LICENSE` and `AUTHORS.md`. The Abseil, SIMDe and ghc lists were produced by compiling the five parser sources and the Abseil sources below with `clang++ -MM` (arm64 and x86_64 macOS targets) and taking every header reached; this is the exact set vendored.

- **Abseil** (`formatlib/third_party/abseil-cpp/`, pin `c2435f83`, Apache-2.0, no file modified, upstream `NOTICE` does not exist at the pin): 131 files, of which the following 16 `.cc` files are compiled (the minimal set the static link of the parser needs; found by linking the parser against upstream `absl::strings` + `absl::flat_hash_map` and reading the linker map, plus `throw_delegate`, `int128`, `log_severity` as portability insurance for Linux):
  - `absl/base/internal/raw_logging.cc`
  - `absl/base/internal/throw_delegate.cc`
  - `absl/base/log_severity.cc`
  - `absl/container/internal/raw_hash_set.cc`
  - `absl/hash/internal/city.cc`
  - `absl/hash/internal/hash.cc`
  - `absl/hash/internal/low_level_hash.cc`
  - `absl/numeric/int128.cc`
  - `absl/strings/ascii.cc`
  - `absl/strings/charconv.cc`
  - `absl/strings/internal/charconv_bigint.cc`
  - `absl/strings/internal/charconv_parse.cc`
  - `absl/strings/internal/memutil.cc`
  - `absl/strings/match.cc`
  - `absl/strings/numbers.cc`
  - `absl/strings/str_cat.cc`

  All vendored Abseil files, headers and sources:
  - `absl/algorithm/algorithm.h`
  - `absl/algorithm/container.h`
  - `absl/base/attributes.h`
  - `absl/base/casts.h`
  - `absl/base/config.h`
  - `absl/base/const_init.h`
  - `absl/base/dynamic_annotations.h`
  - `absl/base/internal/atomic_hook.h`
  - `absl/base/internal/dynamic_annotations.h`
  - `absl/base/internal/endian.h`
  - `absl/base/internal/errno_saver.h`
  - `absl/base/internal/identity.h`
  - `absl/base/internal/inline_variable.h`
  - `absl/base/internal/invoke.h`
  - `absl/base/internal/low_level_alloc.h`
  - `absl/base/internal/low_level_scheduling.h`
  - `absl/base/internal/per_thread_tls.h`
  - `absl/base/internal/prefetch.h`
  - `absl/base/internal/raw_logging.cc`
  - `absl/base/internal/raw_logging.h`
  - `absl/base/internal/scheduling_mode.h`
  - `absl/base/internal/spinlock.h`
  - `absl/base/internal/thread_annotations.h`
  - `absl/base/internal/thread_identity.h`
  - `absl/base/internal/throw_delegate.cc`
  - `absl/base/internal/throw_delegate.h`
  - `absl/base/internal/tsan_mutex_interface.h`
  - `absl/base/internal/unaligned_access.h`
  - `absl/base/log_severity.cc`
  - `absl/base/log_severity.h`
  - `absl/base/macros.h`
  - `absl/base/optimization.h`
  - `absl/base/options.h`
  - `absl/base/policy_checks.h`
  - `absl/base/port.h`
  - `absl/base/thread_annotations.h`
  - `absl/container/fixed_array.h`
  - `absl/container/flat_hash_map.h`
  - `absl/container/flat_hash_set.h`
  - `absl/container/inlined_vector.h`
  - `absl/container/internal/common.h`
  - `absl/container/internal/common_policy_traits.h`
  - `absl/container/internal/compressed_tuple.h`
  - `absl/container/internal/container_memory.h`
  - `absl/container/internal/hash_function_defaults.h`
  - `absl/container/internal/hash_policy_traits.h`
  - `absl/container/internal/hashtable_debug_hooks.h`
  - `absl/container/internal/hashtablez_sampler.h`
  - `absl/container/internal/inlined_vector.h`
  - `absl/container/internal/layout.h`
  - `absl/container/internal/raw_hash_map.h`
  - `absl/container/internal/raw_hash_set.cc`
  - `absl/container/internal/raw_hash_set.h`
  - `absl/crc/crc32c.h`
  - `absl/crc/internal/crc32_x86_arm_combined_simd.h`
  - `absl/crc/internal/crc32c_inline.h`
  - `absl/crc/internal/crc_cord_state.h`
  - `absl/functional/function_ref.h`
  - `absl/functional/internal/function_ref.h`
  - `absl/hash/hash.h`
  - `absl/hash/internal/city.cc`
  - `absl/hash/internal/city.h`
  - `absl/hash/internal/hash.cc`
  - `absl/hash/internal/hash.h`
  - `absl/hash/internal/low_level_hash.cc`
  - `absl/hash/internal/low_level_hash.h`
  - `absl/memory/memory.h`
  - `absl/meta/type_traits.h`
  - `absl/numeric/bits.h`
  - `absl/numeric/int128.cc`
  - `absl/numeric/int128.h`
  - `absl/numeric/int128_have_intrinsic.inc`
  - `absl/numeric/internal/bits.h`
  - `absl/profiling/internal/sample_recorder.h`
  - `absl/strings/ascii.cc`
  - `absl/strings/ascii.h`
  - `absl/strings/charconv.cc`
  - `absl/strings/charconv.h`
  - `absl/strings/cord.h`
  - `absl/strings/cord_analysis.h`
  - `absl/strings/cord_buffer.h`
  - `absl/strings/escaping.h`
  - `absl/strings/internal/charconv_bigint.cc`
  - `absl/strings/internal/charconv_bigint.h`
  - `absl/strings/internal/charconv_parse.cc`
  - `absl/strings/internal/charconv_parse.h`
  - `absl/strings/internal/cord_data_edge.h`
  - `absl/strings/internal/cord_internal.h`
  - `absl/strings/internal/cord_rep_btree.h`
  - `absl/strings/internal/cord_rep_btree_navigator.h`
  - `absl/strings/internal/cord_rep_btree_reader.h`
  - `absl/strings/internal/cord_rep_crc.h`
  - `absl/strings/internal/cord_rep_flat.h`
  - `absl/strings/internal/cord_rep_ring.h`
  - `absl/strings/internal/cordz_functions.h`
  - `absl/strings/internal/cordz_handle.h`
  - `absl/strings/internal/cordz_info.h`
  - `absl/strings/internal/cordz_statistics.h`
  - `absl/strings/internal/cordz_update_scope.h`
  - `absl/strings/internal/cordz_update_tracker.h`
  - `absl/strings/internal/has_absl_stringify.h`
  - `absl/strings/internal/memutil.cc`
  - `absl/strings/internal/memutil.h`
  - `absl/strings/internal/ostringstream.h`
  - `absl/strings/internal/resize_uninitialized.h`
  - `absl/strings/internal/str_join_internal.h`
  - `absl/strings/internal/string_constant.h`
  - `absl/strings/internal/stringify_sink.h`
  - `absl/strings/match.cc`
  - `absl/strings/match.h`
  - `absl/strings/numbers.cc`
  - `absl/strings/numbers.h`
  - `absl/strings/str_cat.cc`
  - `absl/strings/str_cat.h`
  - `absl/strings/str_join.h`
  - `absl/strings/string_view.h`
  - `absl/synchronization/internal/create_thread_identity.h`
  - `absl/synchronization/internal/kernel_timeout.h`
  - `absl/synchronization/internal/per_thread_sem.h`
  - `absl/synchronization/mutex.h`
  - `absl/time/civil_time.h`
  - `absl/time/clock.h`
  - `absl/time/internal/cctz/include/cctz/civil_time.h`
  - `absl/time/internal/cctz/include/cctz/civil_time_detail.h`
  - `absl/time/internal/cctz/include/cctz/time_zone.h`
  - `absl/time/time.h`
  - `absl/types/internal/span.h`
  - `absl/types/optional.h`
  - `absl/types/span.h`
  - `absl/types/variant.h`
  - `absl/utility/utility.h`

- **SIMDe** (`formatlib/third_party/simde/`, pin `2bf459d4`, MIT; `hedley.h`, `debug-trap.h` CC0-1.0; no file modified), 13 headers pulled in by `MathHelpers.h`:
  - `simde/check.h`
  - `simde/debug-trap.h`
  - `simde/hedley.h`
  - `simde/simde-align.h`
  - `simde/simde-arch.h`
  - `simde/simde-common.h`
  - `simde/simde-constify.h`
  - `simde/simde-detect-clang.h`
  - `simde/simde-diagnostic.h`
  - `simde/simde-features.h`
  - `simde/simde-math.h`
  - `simde/x86/mmx.h`
  - `simde/x86/sse.h`

- **ghc::filesystem** (`formatlib/third_party/ghc/`, pin `8a2edd6d`, MIT): only `ghc/fs_std.hpp`. **Modified**: the `ghc::filesystem` fallback branch was removed so the header always selects `std::filesystem` (the modification is recorded in the file header); `ghc/filesystem.hpp` is therefore not vendored and ghc code is not compiled or linked.
- **sfizz** (`formatlib/third_party/sfizz/`): 32 files under `sfizz/sfizz/` plus `LICENSE` and `AUTHORS.md`. Built as `velora_sfizz_parser` with `-fno-char8_t` (upstream is C++17; `u8""` literals must stay `char`); no source change.

Not vendored and not linked: st_audiofile, dr_libs, wavpack, libaiff, the sfizz synth/renderer, fmidi, jsl, cxxopts, atomic_queue, cephes. Audio is read by formatlib's own WAV loader.

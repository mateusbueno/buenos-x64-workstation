# Vendored SFZ parser closure (Story 1.13; file list and licenses in
# _bmad-output/business/11-12-sfizz-license-check.md and THIRD_PARTY_NOTICES.md).
# Only the BSD-2-Clause sfizz *parser* is built here: no sfizz synth/renderer, no st_audiofile,
# no dr_libs. Its dependencies (abseil, SIMDe headers, ghc/fs_std.hpp -> std::filesystem) are
# vendored next to it, so nothing is downloaded and nothing is found on the system.
set(_abs ${CMAKE_CURRENT_SOURCE_DIR}/abseil-cpp/absl)
add_library(velora_sfizz_parser STATIC
  sfizz/sfizz/Opcode.cpp
  sfizz/sfizz/Defaults.cpp
  sfizz/sfizz/OpcodeCleanup.cpp
  sfizz/sfizz/parser/Parser.cpp
  sfizz/sfizz/parser/ParserPrivate.cpp
  ${_abs}/hash/internal/hash.cc
  ${_abs}/hash/internal/city.cc
  ${_abs}/hash/internal/low_level_hash.cc
  ${_abs}/container/internal/raw_hash_set.cc
  ${_abs}/strings/ascii.cc
  ${_abs}/strings/charconv.cc
  ${_abs}/strings/internal/charconv_bigint.cc
  ${_abs}/strings/internal/charconv_parse.cc
  ${_abs}/strings/internal/memutil.cc
  ${_abs}/strings/match.cc
  ${_abs}/strings/numbers.cc
  ${_abs}/strings/str_cat.cc
  ${_abs}/base/internal/raw_logging.cc
  ${_abs}/base/internal/throw_delegate.cc
  ${_abs}/base/log_severity.cc
  ${_abs}/numeric/int128.cc)
add_library(velora::sfizz_parser ALIAS velora_sfizz_parser)
target_include_directories(velora_sfizz_parser SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sfizz/sfizz
  ${CMAKE_CURRENT_SOURCE_DIR}/abseil-cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/simde
  ${CMAKE_CURRENT_SOURCE_DIR}/ghc)
# Upstream is C++17: its u8"" literals are char there but char8_t in C++20.
# GCC/Clang only; other compilers are not supported targets for this build.
target_compile_options(velora_sfizz_parser PUBLIC
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fno-char8_t>)
target_compile_options(velora_sfizz_parser PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-w>)

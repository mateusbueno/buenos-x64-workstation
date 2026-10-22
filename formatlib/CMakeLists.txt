# libFLAC is the only third-party dependency (BSD, linked as a library; no flac/metaflac binaries).
# Discovery order: CMake package config, pkg-config, then plain find_path/find_library.
# Nothing is downloaded or vendored.
set(VELORA_FLAC_VERSION "")
find_package(FLAC CONFIG QUIET)
if(TARGET FLAC::FLAC)
  set(VELORA_FLAC_TARGET FLAC::FLAC)
  set(VELORA_FLAC_VERSION "${FLAC_VERSION}")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VELORA_FLAC_PC QUIET IMPORTED_TARGET flac)
  endif()
  if(TARGET PkgConfig::VELORA_FLAC_PC)
    set(VELORA_FLAC_TARGET PkgConfig::VELORA_FLAC_PC)
    set(VELORA_FLAC_VERSION "${VELORA_FLAC_PC_VERSION}")
  else()
    find_path(VELORA_FLAC_INCLUDE_DIR FLAC/stream_encoder.h
              HINTS /opt/homebrew/include /usr/local/include)
    find_library(VELORA_FLAC_LIBRARY NAMES FLAC
                 HINTS /opt/homebrew/lib /usr/local/lib)
    if(VELORA_FLAC_INCLUDE_DIR AND VELORA_FLAC_LIBRARY)
      add_library(velora_flac_import UNKNOWN IMPORTED)
      set_target_properties(velora_flac_import PROPERTIES
        IMPORTED_LOCATION "${VELORA_FLAC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${VELORA_FLAC_INCLUDE_DIR}")
      set(VELORA_FLAC_TARGET velora_flac_import)
      # Ask the library itself (FLAC__VERSION_STRING) when no package metadata is available.
      file(WRITE "${CMAKE_BINARY_DIR}/flac_version.cpp"
           "#include <FLAC/format.h>\n#include <cstdio>\nint main(){std::printf(\"%s\",FLAC__VERSION_STRING);}\n")
      try_run(_flac_run _flac_compile
              "${CMAKE_BINARY_DIR}/flac_version_try" "${CMAKE_BINARY_DIR}/flac_version.cpp"
              LINK_LIBRARIES velora_flac_import
              RUN_OUTPUT_VARIABLE VELORA_FLAC_VERSION)
    endif()
  endif()
endif()
if(NOT VELORA_FLAC_TARGET)
  message(FATAL_ERROR "libFLAC development files not found. Install them "
          "(macOS: brew install flac; Debian/Ubuntu: apt-get install libflac-dev).")
endif()
string(STRIP "${VELORA_FLAC_VERSION}" VELORA_FLAC_VERSION)
if(NOT VELORA_FLAC_VERSION)
  message(FATAL_ERROR "libFLAC found but its version could not be determined; >= 1.4 required")
endif()
if(VELORA_FLAC_VERSION VERSION_LESS 1.4)
  message(FATAL_ERROR "libFLAC ${VELORA_FLAC_VERSION} found, >= 1.4 required (target 1.5.0)")
endif()
message(STATUS "formatlib: libFLAC ${VELORA_FLAC_VERSION} (target ${VELORA_FLAC_TARGET})")

add_subdirectory(third_party)

add_library(velora_format STATIC
  src/common.cpp
  src/error.cpp
  src/hash.cpp
  src/codec.cpp
  src/instrument_model.cpp
  src/writer.cpp
  src/reader.cpp
  src/wav_source.cpp
  src/sf2_import.cpp
  src/category.cpp
  src/json_lite.cpp
  src/locale_catalog.cpp
  src/sysex_6op.cpp
  src/va_format.cpp
  src/factory_fm_va.cpp
  src/parametric_bundle.cpp)
add_library(velora::format ALIAS velora_format)
target_include_directories(velora_format PUBLIC include PRIVATE src)
# Dependency rule: formatlib links only libFLAC; it never uses engine/core or engine/host.
target_link_libraries(velora_format PRIVATE ${VELORA_FLAC_TARGET})
target_compile_options(velora_format PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

# SFZ -> instrument model on top of the vendored parser. A separate target on purpose: the vendored
# abseil headers must not share an include path with libFLAC's (a system abseil may live there).
add_library(velora_sfz STATIC src/sfz_import.cpp)
add_library(velora::sfz ALIAS velora_sfz)
target_include_directories(velora_sfz PUBLIC include PRIVATE src)
target_link_libraries(velora_sfz PUBLIC velora_format PRIVATE velora_sfizz_parser)
target_compile_options(velora_sfz PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

# Story 6.2: protected bundles. OpenSSL libcrypto (EVP, >= 3.0, Apache-2.0, dynamic; THIRD_PARTY_NOTICES.md) provides Ed25519 and AES-256-CTR.
# Three layers so shipped code links only what it needs (tools/ci/check_format_no_decrypt.py proves the first):
#   velora_format_verify          signature + hash-list + ciphertext block-hash verification, NO decrypt (desktop, librarian)
#   velora_format_decrypt         + AES-CTR decrypting reader (engine, offline-render, tests only)
#   velora_format_protected_writer  AES-CTR encryption + Ed25519 signing (tools/content-pack, tests only)
# Homebrew keeps OpenSSL keg-only; Apple's LibreSSL has neither the headers nor the EVP Ed25519 API.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(_ossl /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${_ossl}/include/openssl/evp.h")
      set(OPENSSL_ROOT_DIR "${_ossl}")
      break()
    endif()
  endforeach()
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
message(STATUS "formatlib: OpenSSL ${OPENSSL_VERSION} (libcrypto: Ed25519 verify/sign, AES-256-CTR)")

set(VELORA_FORMAT_WARN $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
add_library(velora_format_verify STATIC protected/verify.cpp)
add_library(velora::format_verify ALIAS velora_format_verify)
target_include_directories(velora_format_verify PUBLIC include PRIVATE src)
target_link_libraries(velora_format_verify PUBLIC velora_format PRIVATE OpenSSL::Crypto)
target_compile_options(velora_format_verify PRIVATE ${VELORA_FORMAT_WARN})

add_library(velora_format_decrypt STATIC protected/decrypt.cpp)
add_library(velora::format_decrypt ALIAS velora_format_decrypt)
target_include_directories(velora_format_decrypt PUBLIC include PRIVATE src)
target_link_libraries(velora_format_decrypt PUBLIC velora_format_verify PRIVATE OpenSSL::Crypto)
target_compile_options(velora_format_decrypt PRIVATE ${VELORA_FORMAT_WARN})

add_library(velora_format_protected_writer STATIC protected/protected_writer.cpp)
add_library(velora::format_protected_writer ALIAS velora_format_protected_writer)
target_include_directories(velora_format_protected_writer PUBLIC include PRIVATE src)
target_link_libraries(velora_format_protected_writer PUBLIC velora_format PRIVATE OpenSSL::Crypto)
target_compile_options(velora_format_protected_writer PRIVATE ${VELORA_FORMAT_WARN})

add_executable(format_sysex_test tests/sysex_6op_test.cpp)
target_link_libraries(format_sysex_test PRIVATE velora_format)
target_compile_options(format_sysex_test PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
add_test(NAME format_sysex_test COMMAND format_sysex_test)

foreach(t hash_test bundle_test wav_test)
  add_executable(format_${t} tests/${t}.cpp)
  target_link_libraries(format_${t} PRIVATE velora_format)
  target_include_directories(format_${t} PRIVATE src)
  add_test(NAME format_${t} COMMAND format_${t})
endforeach()

add_executable(format_sfz_test tests/sfz_test.cpp)
target_link_libraries(format_sfz_test PRIVATE velora_sfz)
target_compile_definitions(format_sfz_test PRIVATE FORMATLIB_README="${CMAKE_CURRENT_SOURCE_DIR}/README.md")
add_test(NAME format_sfz_test COMMAND format_sfz_test)

# Story 5.3: SoundFont 2 reader and mapping, fixtures from the test-only SF2 writer (tests/sf2_builder.hpp).
add_executable(format_sf2_test tests/sf2_test.cpp)
target_link_libraries(format_sf2_test PRIVATE velora_format)
target_compile_options(format_sf2_test PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
add_test(NAME format_sf2_test COMMAND format_sf2_test)

# Story 5.5: category model, minimal locale catalog (locale/*.json), proto/category.proto guard.
add_executable(format_category_test tests/category_test.cpp)
target_link_libraries(format_category_test PRIVATE velora_format)
target_compile_options(format_category_test PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
target_compile_definitions(format_category_test PRIVATE
  VELORA_LOCALE_DIR="${PROJECT_SOURCE_DIR}/locale"
  VELORA_CATEGORY_PROTO="${PROJECT_SOURCE_DIR}/proto/category.proto")
add_test(NAME format_category_test COMMAND format_category_test)

# Story 6.2: protected bundle format (round trip, 10,000 seeks, tamper matrix, strict Ed25519, known-answer vectors). Writes the fixture bundle the
# verify-only probe (a binary that links no decrypt code, scanned by check_format_no_decrypt.py) opens.
add_executable(format_protected_test tests/protected_test.cpp)
target_link_libraries(format_protected_test PRIVATE velora_format_decrypt velora_format_protected_writer OpenSSL::Crypto)
target_include_directories(format_protected_test PRIVATE src)
target_compile_options(format_protected_test PRIVATE ${VELORA_FORMAT_WARN})
add_test(NAME format_protected_test COMMAND format_protected_test ${CMAKE_CURRENT_BINARY_DIR}/protected_fixture)
set_tests_properties(format_protected_test PROPERTIES FIXTURES_SETUP protected_fixture TIMEOUT 600)

add_executable(format_verify_probe tests/verify_probe.cpp)
target_link_libraries(format_verify_probe PRIVATE velora_format_verify)
target_compile_options(format_verify_probe PRIVATE ${VELORA_FORMAT_WARN})
add_test(NAME format_verify_probe COMMAND format_verify_probe ${CMAKE_CURRENT_BINARY_DIR}/protected_fixture/fixture.vlrp
         ${CMAKE_CURRENT_BINARY_DIR}/protected_fixture/trusted.hex)
set_tests_properties(format_verify_probe PROPERTIES FIXTURES_REQUIRED protected_fixture)

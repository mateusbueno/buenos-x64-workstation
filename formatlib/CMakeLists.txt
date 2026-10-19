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
  src/locale_catalog.cpp)
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

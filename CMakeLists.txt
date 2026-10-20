cmake_minimum_required(VERSION 3.22)
project(velora VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VELORA_RTSAN "Build RealtimeSanitizer tests (needs clang >= 20 with -fsanitize=realtime)" ON)

enable_testing()

add_subdirectory(proto)
add_subdirectory(mcu/common)
add_subdirectory(engine/ports)
add_subdirectory(engine/core)
add_subdirectory(engine/host)
add_subdirectory(formatlib)
add_subdirectory(librarian)
add_subdirectory(tools/offline-render)
add_subdirectory(tools/library-gen)
add_subdirectory(tools/keys)
add_subdirectory(tools/content-pack)
add_subdirectory(tools/spikes)

# Dependency-purity gate for engine/core (see tools/ci/check_core_deps.py).
if(DEFINED ENV{CI})
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
else()
  find_package(Python3 COMPONENTS Interpreter)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME core_deps_purity
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_core_deps.py
                   --root ${CMAKE_SOURCE_DIR})
  add_test(NAME core_deps_purity_selftest
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_core_deps.py
                   --self-test)
  # Story 6.1 (AR34/NFR23): no private key material in the git index / working tree.
  add_test(NAME no_private_keys
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_no_private_keys.py
                   --root ${CMAKE_SOURCE_DIR})
  add_test(NAME no_private_keys_selftest
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_no_private_keys.py
                   --self-test)
  # Story 6.2 (AR27/NFR13): the desktop-side verification target, the verify-only probe binary and the librarian binaries contain no decrypt path.
  add_test(NAME format_no_decrypt
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --lib $<TARGET_FILE:velora_format_verify> --lib $<TARGET_FILE:velora_format>
                   --binary $<TARGET_FILE:format_verify_probe>
                   --binary $<TARGET_FILE:librarian> --binary $<TARGET_FILE:librarian-service>
                   --lib $<TARGET_FILE:velora_librarian> --lib $<TARGET_FILE:velora_librarian_service> --lib $<TARGET_FILE:velora_sfz>
                   --require-defined ed25519_verify_strict)
  set_tests_properties(format_no_decrypt PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME format_no_decrypt_selftest
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py --self-test --cc ${CMAKE_C_COMPILER})
  add_test(NAME format_no_decrypt_probe
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:format_verify_probe> --require-defined ed25519_verify_strict)
  set_tests_properties(format_no_decrypt_probe PROPERTIES SKIP_RETURN_CODE 77)
  # Not vacuous: the same scanner must flag the cipher calls of the decrypt and writer libraries (exit 0 only when it does).
  add_test(NAME format_no_decrypt_detects_decrypt
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --lib $<TARGET_FILE:velora_format_decrypt> --lib $<TARGET_FILE:velora_format_protected_writer>
                   --expect-flagged EVP_DecryptInit_ex --expect-flagged EVP_EncryptInit_ex --expect-flagged EVP_aes_256_ctr)
  set_tests_properties(format_no_decrypt_detects_decrypt PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME content_pack_not_linked
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_content_pack_not_linked.py --root ${CMAKE_SOURCE_DIR})
  add_test(NAME content_pack_not_linked_selftest
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_content_pack_not_linked.py --self-test)
  # Story 6.3: tools/content-pack is the one allowed signing/encrypting/decrypting target and is never shipped. The same scanner must flag its cipher calls
  # (so it can tell a tool from a shippable binary), while the shipped scans above stay clean with the importer's new build-only mode.
  add_test(NAME content_pack_flagged_by_no_decrypt_scan
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:velora-content-pack>
                   --expect-flagged EVP_aes_256_gcm --expect-flagged EVP_EncryptInit_ex --expect-flagged EVP_DecryptInit_ex)
  set_tests_properties(content_pack_flagged_by_no_decrypt_scan PROPERTIES SKIP_RETURN_CODE 77)
  # The librarian scanner accepts the verify target (only the Ed25519 verification calls) ...
  add_test(NAME format_verify_librarian_scanner
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_librarian_no_crypto.py
                   --binary $<TARGET_FILE:format_verify_probe> --lib $<TARGET_FILE:velora_format_verify>)
  set_tests_properties(format_verify_librarian_scanner PROPERTIES SKIP_RETURN_CODE 77)
  # ... and rejects the decrypt target.
  add_test(NAME format_decrypt_librarian_scanner_rejects
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_librarian_no_crypto.py --lib $<TARGET_FILE:velora_format_decrypt>)
  set_tests_properties(format_decrypt_librarian_scanner_rejects PROPERTIES WILL_FAIL TRUE)
else()
  message(STATUS "Python3 not found: core purity tests not registered")
endif()

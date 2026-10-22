cmake_minimum_required(VERSION 3.22)
project(velora VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VELORA_RTSAN "Build RealtimeSanitizer tests (needs clang >= 20 with -fsanitize=realtime)" ON)
option(VELORA_PRODUCTION "this configuration builds a shippable image: no dev-only code (dev trust variant, dev key provider) may be built into it" OFF)
# Story 6.5: the dev key provider reads a developer's device private key from a file and unwraps content keys with it (engine/host/dev_keys). The board uses the
# OP-TEE provider (Story 6.6) behind the same port. It defaults to ON only for a plain development configuration: a single-config generator with an empty or Debug
# CMAKE_BUILD_TYPE and no VELORA_PRODUCTION. Any other configuration (Release, MinSizeRel, RelWithDebInfo, a custom type, a multi-config generator) defaults to OFF and
# needs an explicit -DVELORA_DEV_KEY_PROVIDER=ON; a Release / MinSizeRel / RelWithDebInfo / VELORA_PRODUCTION one refuses it (engine/host/CMakeLists.txt).
if(NOT VELORA_PRODUCTION AND NOT CMAKE_CONFIGURATION_TYPES AND (CMAKE_BUILD_TYPE STREQUAL "" OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
  set(_velora_dev_keys_default ON)
else()
  set(_velora_dev_keys_default OFF)
endif()
option(VELORA_DEV_KEY_PROVIDER "build the dev key provider (a developer's device key from a file) and the dev flags of engine-host and velora-offline-render that use it" ${_velora_dev_keys_default})

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
add_subdirectory(tools/common/vault_crypto)
add_subdirectory(tools/content-pack)
add_subdirectory(tools/factory-release)
add_subdirectory(tools/registry)
add_subdirectory(tools/spikes)
add_subdirectory(ui)

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
  # Story 2.17: UI strings lint check
  add_test(NAME ui_strings_lint
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_ui_strings.py
                   --root ${CMAKE_SOURCE_DIR})
  add_test(NAME ui_strings_lint_selftest
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_ui_strings.py
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
  # Story 6.4: the librarian installs protected bundles by VERIFYING them. Its real binaries (the shipped CLI and service, and the test CLI that is the same code with
  # the kill hooks) and its libraries hold no cipher symbol, and the scan is not vacuous: they do define the Ed25519 verification and the install path.
  add_test(NAME librarian_binaries_no_cipher
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:librarian> --binary $<TARGET_FILE:librarian-service> --binary $<TARGET_FILE:librarian_test_cli>
                   --lib $<TARGET_FILE:velora_librarian> --lib $<TARGET_FILE:velora_librarian_service> --lib $<TARGET_FILE:velora_format_verify>
                   --require-defined ed25519_verify_strict --require-defined install_protected --require-defined ProtectedBundleView)
  set_tests_properties(librarian_binaries_no_cipher PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME librarian_binary_links_verify_only
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:librarian> --require-defined ed25519_verify_strict)
  set_tests_properties(librarian_binary_links_verify_only PROPERTIES SKIP_RETURN_CODE 77)
  # Negative control: the scanner DOES flag a test binary that links the protected writer (AES), so a clean librarian result is not an empty one.
  add_test(NAME librarian_install_test_flagged_by_no_decrypt_scan
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:librarian_install_test> --expect-flagged EVP_aes_256_ctr --expect-flagged EVP_EncryptInit_ex)
  set_tests_properties(librarian_install_test_flagged_by_no_decrypt_scan PROPERTIES SKIP_RETURN_CODE 77)
  # The dev trust variant (a developer's keys, --trust-list) must not be configurable into a Release / production build.
  add_test(NAME librarian_dev_trust_refused_in_release
           COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_BINARY_DIR}/release_refusal_probe -DCMAKE_BUILD_TYPE=Release)
  set_tests_properties(librarian_dev_trust_refused_in_release PROPERTIES PASS_REGULAR_EXPRESSION "must not be built into a Release")
  # Story 6.5: the dev key provider (a developer's device key read from a file) never reaches a production-like configuration.
  add_test(NAME dev_key_provider_refused_in_production
           COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_BINARY_DIR}/dev_keys_production_probe -DVELORA_PRODUCTION=ON -DVELORA_DEV_KEY_PROVIDER=ON)
  set_tests_properties(dev_key_provider_refused_in_production PROPERTIES PASS_REGULAR_EXPRESSION "developer's device key")
  add_test(NAME dev_key_provider_refused_in_relwithdebinfo
           COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_BINARY_DIR}/dev_keys_rwdi_probe -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVELORA_DEV_KEY_PROVIDER=ON)
  set_tests_properties(dev_key_provider_refused_in_relwithdebinfo PROPERTIES PASS_REGULAR_EXPRESSION "developer's device key")
  add_test(NAME dev_key_provider_default_off_in_release
           COMMAND ${CMAKE_COMMAND} -DMODE=default_off_release -DSRC=${CMAKE_SOURCE_DIR} -DBIN=${CMAKE_BINARY_DIR}/dev_keys_default_probe -P ${CMAKE_SOURCE_DIR}/tools/ci/dev_keys_probe.cmake)
  if(VELORA_DEV_KEY_PROVIDER)
    set(_dev_bins $<TARGET_FILE:velora-offline-render>)
    if(APPLE)
      list(APPEND _dev_bins $<TARGET_FILE:engine-host>)
    endif()
    add_test(NAME dev_key_provider_absent_when_off
             COMMAND ${CMAKE_COMMAND} -DMODE=absent_when_off -DSRC=${CMAKE_SOURCE_DIR} -DBIN=${CMAKE_BINARY_DIR}/dev_keys_off_probe -DPYTHON=${Python3_EXECUTABLE}
                     "-DDEV_BINS=${_dev_bins}" -P ${CMAKE_SOURCE_DIR}/tools/ci/dev_keys_probe.cmake)
    set_tests_properties(dev_key_provider_absent_when_off PROPERTIES TIMEOUT 1800)
  endif()
  add_test(NAME no_dev_keys_selftest
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_no_dev_keys.py --self-test)
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
  # Story 6.9: tools/factory-release (signing, packing, the factory Live Set container) is vendor-side code like the pack tool: the scanner must flag its cipher calls.
  add_test(NAME factory_release_flagged_by_no_decrypt_scan
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:velora-factory-release>
                   --expect-flagged EVP_aes_256_gcm --expect-flagged EVP_EncryptInit_ex --expect-flagged EVP_DecryptInit_ex)
  set_tests_properties(factory_release_flagged_by_no_decrypt_scan PROPERTIES SKIP_RETURN_CODE 77)
  # Story 6.8: the registry (vault KEK, unwrap, wrap) is vendor-server code like the pack tool: the same scanner must flag its cipher calls, which proves a clean
  # shipped scan is not an empty one and that nothing shipped can have picked the registry up.
  add_test(NAME registry_flagged_by_no_decrypt_scan
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/ci/check_format_no_decrypt.py
                   --binary $<TARGET_FILE:velora-registry>
                   --expect-flagged EVP_aes_256_gcm --expect-flagged EVP_EncryptInit_ex --expect-flagged EVP_DecryptInit_ex)
  set_tests_properties(registry_flagged_by_no_decrypt_scan PROPERTIES SKIP_RETURN_CODE 77)
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

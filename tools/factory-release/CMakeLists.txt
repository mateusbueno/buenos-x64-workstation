# Story 6.9: the factory release pipeline. NOT shipped, like tools/content-pack: it links the content-pack library (encrypting and signing code), the protected
# writer (Ed25519 signing of the release index and the factory Live Sets) and the engine's control library (the Live Set proto and the factory Live Set container).
# Nothing in the device images, the desktop build or the librarian may depend on it; tools/ci/check_content_pack_not_linked.py guards velora_factory_release too, and
# the ctest `factory_release_flagged_by_no_decrypt_scan` shows the shipped-binary scanner would flag it.
set(VELORA_FR_WARN $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_library(velora_factory_release STATIC
  src/release_description.cpp
  src/release.cpp)
target_include_directories(velora_factory_release PUBLIC src)
target_link_libraries(velora_factory_release PUBLIC velora_content_pack velora_control velora_format_protected_writer velora_format_verify)
target_compile_options(velora_factory_release PRIVATE ${VELORA_FR_WARN})

add_executable(velora-factory-release src/main.cpp)
target_link_libraries(velora-factory-release PRIVATE velora_factory_release)
target_compile_definitions(velora-factory-release PRIVATE VELORA_CONTENT_PACK_REGISTER="${PROJECT_SOURCE_DIR}/tools/content-pack/vendor-ids"
  VELORA_KEYS_DEV_TRUST_LIST="${PROJECT_SOURCE_DIR}/tools/keys/trust/dev.pub-list")
target_compile_options(velora-factory-release PRIVATE ${VELORA_FR_WARN})

# ---- tests ---------------------------------------------------------------------------------------------------------------------------------------------
# A deterministic synthetic factory tree (tests/synthetic_factory.hpp) through build, rebuild, verify, tamper, the CLI and, with the dev key provider, the host
# librarian install and the engine host components playing every instrument and loading every factory Live Set.
add_executable(factory_release_test tests/factory_release_test.cpp)
target_link_libraries(factory_release_test PRIVATE velora_factory_release velora_librarian_service velora_format_protected_writer)
target_include_directories(factory_release_test PRIVATE ${PROJECT_SOURCE_DIR}/librarian/tests ${PROJECT_SOURCE_DIR}/formatlib/tests)
target_compile_options(factory_release_test PRIVATE ${VELORA_FR_WARN})
target_compile_definitions(factory_release_test PRIVATE FR_EXE="$<TARGET_FILE:velora-factory-release>")
add_dependencies(factory_release_test velora-factory-release)
if(VELORA_DEV_KEY_PROVIDER)
  target_link_libraries(factory_release_test PRIVATE velora_host_protected velora_host_dev_keys)
  target_compile_definitions(factory_release_test PRIVATE FR_HAS_DEV_KEYS=1 LIBRARIAN_EXE="$<TARGET_FILE:librarian>")
  add_dependencies(factory_release_test librarian)
  if(APPLE)  # the real engine-host binary imports the built release on first boot
    target_compile_definitions(factory_release_test PRIVATE ENGINE_HOST_EXE="$<TARGET_FILE:engine-host>")
    add_dependencies(factory_release_test engine-host)
  endif()
endif()
add_test(NAME factory_release_test COMMAND factory_release_test)
# 77 = the install-and-play part could not run (no dev key provider): reported as skipped, never as a silent pass
set_tests_properties(factory_release_test PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)

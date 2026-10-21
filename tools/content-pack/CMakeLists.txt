# Story 6.3: the offline content-pack tool. NOT shipped: it links the encrypting and signing code (velora_format_protected_writer), the decrypting reader
# (to verify what it wrote) and OpenSSL's cipher API. Nothing in the device images, the desktop build or the librarian may depend on it; the ctest
# `content_pack_flagged_by_no_decrypt_scan` proves the no-decrypt scanner would flag it, and the shipped scans (format_no_decrypt, librarian_no_crypto)
# are unchanged.
# Story 6.8: crypto.{hpp,cpp} (vault_entry_v1, wrapped_key_v1) now live in tools/common/vault_crypto, shared with tools/registry.
# Same OpenSSL discovery as formatlib (imported targets are directory scoped).
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(_ossl /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${_ossl}/include/openssl/evp.h")
      set(OPENSSL_ROOT_DIR "${_ossl}")
      break()
    endif()
  endforeach()
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

set(VELORA_CP_WARN $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_library(velora_content_pack STATIC
  src/pack_description.cpp
  src/vendor_register.cpp
  src/key_files.cpp
  src/pack.cpp)
target_include_directories(velora_content_pack PUBLIC src)
target_link_libraries(velora_content_pack PUBLIC velora_librarian velora_format_decrypt velora_format_protected_writer velora_vault_crypto OpenSSL::Crypto)
target_compile_options(velora_content_pack PRIVATE ${VELORA_CP_WARN})

add_executable(velora-content-pack src/main.cpp)
target_link_libraries(velora-content-pack PRIVATE velora_content_pack)
target_compile_definitions(velora-content-pack PRIVATE VELORA_CONTENT_PACK_REGISTER="${CMAKE_CURRENT_SOURCE_DIR}/vendor-ids"
  VELORA_KEYS_DEV_TRUST_LIST="${PROJECT_SOURCE_DIR}/tools/keys/trust/dev.pub-list")
target_compile_options(velora-content-pack PRIVATE ${VELORA_CP_WARN})

# ---- tests ---------------------------------------------------------------------------------------------------------------------------------------------
add_executable(content_pack_test tests/content_pack_test.cpp)
target_link_libraries(content_pack_test PRIVATE velora_content_pack)
target_include_directories(content_pack_test PRIVATE ${PROJECT_SOURCE_DIR}/librarian/tests ${PROJECT_SOURCE_DIR}/formatlib/tests)
target_compile_options(content_pack_test PRIVATE ${VELORA_CP_WARN})
target_compile_definitions(content_pack_test PRIVATE
  CP_EXE="$<TARGET_FILE:velora-content-pack>"
  RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  CP_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  VELORA_CP_COMMITTED_REGISTER="${CMAKE_CURRENT_SOURCE_DIR}/vendor-ids")
add_dependencies(content_pack_test velora-content-pack velora-offline-render)
if(VELORA_DEV_KEY_PROVIDER)  # Story 6.5: the renderer can play the pack's protected bundle directly (dev key provider flags)
  target_compile_definitions(content_pack_test PRIVATE RENDER_HAS_DEV_KEYS=1)
endif()
add_test(NAME content_pack_test COMMAND content_pack_test)
set_tests_properties(content_pack_test PROPERTIES TIMEOUT 600)

# Story 6.8: the vendor-side key-handling primitives (vault_entry_v1, wrapped_key_v1, X25519, AES-GCM, HKDF, secret hygiene), moved out of tools/content-pack
# (Story 6.3) without behaviour change so the content-pack tool and the device registry (tools/registry) share ONE implementation. VENDOR-SIDE ONLY: it links
# OpenSSL's cipher API and must never be linked into a shipped unit target (tools/ci/check_content_pack_not_linked.py enforces it for this library too).
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

add_library(velora_vault_crypto STATIC crypto.cpp)
target_include_directories(velora_vault_crypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velora_vault_crypto PUBLIC velora_format OpenSSL::Crypto)
target_compile_options(velora_vault_crypto PRIVATE $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

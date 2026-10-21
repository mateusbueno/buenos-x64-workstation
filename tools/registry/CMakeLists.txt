# Story 6.8: the vendor device registry and content-key vault. VENDOR-SIDE ONLY: it links OpenSSL's TLS and cipher APIs, SQLite and the shared vault crypto, and
# must never be linked into a unit image, the desktop build or the librarian (tools/ci/check_content_pack_not_linked.py covers velora_registry as it does velora_content_pack).
# Same OpenSSL / SQLite discovery as formatlib and the librarian (imported targets are directory scoped).
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(_ossl /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${_ossl}/include/openssl/evp.h")
      set(OPENSSL_ROOT_DIR "${_ossl}")
      break()
    endif()
  endforeach()
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)
find_package(SQLite3)
if(NOT SQLite3_FOUND)
  message(FATAL_ERROR "registry: SQLite3 (libsqlite3 with headers) is required. macOS ships it in the SDK (or `brew install sqlite`); Debian/Ubuntu: `apt-get install libsqlite3-dev`.")
endif()
if(TARGET SQLite3::SQLite3)
  set(VELORA_REGISTRY_SQLITE SQLite3::SQLite3)
else()
  set(VELORA_REGISTRY_SQLITE SQLite::SQLite3)
endif()
find_package(Threads REQUIRED)

set(VELORA_REG_WARN $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_library(velora_registry STATIC
  src/util.cpp
  src/database.cpp
  src/http.cpp
  src/vault.cpp
  src/service.cpp
  src/backup.cpp
  src/server.cpp)
target_include_directories(velora_registry PUBLIC src)
target_link_libraries(velora_registry PUBLIC velora_vault_crypto velora_format OpenSSL::SSL OpenSSL::Crypto ${VELORA_REGISTRY_SQLITE} Threads::Threads)
target_compile_options(velora_registry PRIVATE ${VELORA_REG_WARN})

add_executable(velora-registry src/main.cpp)
target_link_libraries(velora-registry PRIVATE velora_registry)
target_compile_options(velora-registry PRIVATE ${VELORA_REG_WARN})

# ---- tests ---------------------------------------------------------------------------------------------------------------------------------------------
add_executable(registry_test tests/registry_test.cpp)
target_link_libraries(registry_test PRIVATE velora_registry)
target_compile_options(registry_test PRIVATE ${VELORA_REG_WARN})
target_compile_definitions(registry_test PRIVATE REGISTRY_EXE="$<TARGET_FILE:velora-registry>" REGISTRY_API_MD="${CMAKE_CURRENT_SOURCE_DIR}/API.md")
if(TARGET velora_host_dev_keys)  # the engine's dev key provider is an independent implementation of the device side of wrapped_key_v1 (test only; not linked into the registry)
  target_link_libraries(registry_test PRIVATE velora_host_dev_keys)
  target_compile_definitions(registry_test PRIVATE REGISTRY_HAS_DEV_KEYS=1)
endif()
add_dependencies(registry_test velora-registry)
add_test(NAME registry_test COMMAND registry_test)
set_tests_properties(registry_test PROPERTIES TIMEOUT 600)

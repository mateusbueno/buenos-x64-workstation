find_package(Threads REQUIRED)
add_library(velora_host STATIC
  src/host_info.cpp
  src/time_mapper.cpp
  src/midi_ingest.cpp
  src/render_driver.cpp
  src/posix_fs.cpp
  src/library_resolver.cpp
  src/sample_bundle_loader.cpp
  src/secure_memory.cpp
  src/damage_registry.cpp
  src/trust_list.cpp
  src/mcu_frame.cpp)
add_library(velora::host ALIAS velora_host)
target_include_directories(velora_host PUBLIC include)
# velora_format_verify (Story 6.5): the resolver VERIFIES protected bundles (Ed25519 signature, hash list, index). It holds no cipher: the decrypting code is only in
# velora_host_protected below (tools/ci/check_format_no_decrypt.py scans the librarian, which links this library, and keeps proving it).
target_link_libraries(velora_host PUBLIC velora_core velora_ports velora_format velora_format_verify velora_mcu_common)

# --- control plane (Story 2.1): ws.v1 over a Unix domain socket, parameter registry, snapshots ------------
# protobuf lives only here (engine/host), in proto/ and in tools; engine/core never sees it.
include(${CMAKE_SOURCE_DIR}/proto/params.cmake)
# Config mode first: it carries protobuf's abseil dependencies (Homebrew); the module is the fallback.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
message(STATUS "engine/host: protobuf ${Protobuf_VERSION}")
velora_params_output(host VELORA_PARAMS_HOST_HPP)
velora_params_output(proto VELORA_PARAMS_PROTO)

set(VELORA_PB_DIR "${VELORA_GENERATED_DIR}/pb")
set(VELORA_PB_SOURCES "${VELORA_PB_DIR}/ws_v1.pb.cc" "${VELORA_PB_DIR}/ws_v1_params.pb.cc" "${VELORA_PB_DIR}/live_set.pb.cc" "${VELORA_PB_DIR}/setlist.pb.cc" "${VELORA_PB_DIR}/category.pb.cc" "${VELORA_PB_DIR}/factory_live_set.pb.cc")
set(VELORA_PB_HEADERS "${VELORA_PB_DIR}/ws_v1.pb.h" "${VELORA_PB_DIR}/ws_v1_params.pb.h" "${VELORA_PB_DIR}/live_set.pb.h" "${VELORA_PB_DIR}/setlist.pb.h" "${VELORA_PB_DIR}/category.pb.h" "${VELORA_PB_DIR}/factory_live_set.pb.h")
add_custom_command(
  OUTPUT ${VELORA_PB_SOURCES} ${VELORA_PB_HEADERS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VELORA_PB_DIR}"
  COMMAND $<TARGET_FILE:protobuf::protoc> -I "${CMAKE_SOURCE_DIR}/proto" -I "${VELORA_GENERATED_DIR}/proto"
          --cpp_out "${VELORA_PB_DIR}" "${CMAKE_SOURCE_DIR}/proto/ws_v1.proto" "${CMAKE_SOURCE_DIR}/proto/live_set.proto" "${CMAKE_SOURCE_DIR}/proto/setlist.proto" "${CMAKE_SOURCE_DIR}/proto/category.proto" "${CMAKE_SOURCE_DIR}/proto/factory_live_set.proto" "${VELORA_PARAMS_PROTO}"
  DEPENDS "${CMAKE_SOURCE_DIR}/proto/ws_v1.proto" "${CMAKE_SOURCE_DIR}/proto/live_set.proto" "${CMAKE_SOURCE_DIR}/proto/setlist.proto" "${CMAKE_SOURCE_DIR}/proto/category.proto" "${CMAKE_SOURCE_DIR}/proto/factory_live_set.proto" "${VELORA_PARAMS_PROTO}"
  COMMENT "Generating ws.v1 protobuf code"
  VERBATIM)

add_library(velora_control STATIC
  ${VELORA_PB_SOURCES} ${VELORA_PB_HEADERS} ${VELORA_PARAMS_HOST_HPP}
  src/control_plane.cpp
  src/control_server.cpp
  src/dev_config.cpp
  src/live_set_store.cpp
  src/live_set_loader.cpp
  src/factory_live_set.cpp
  src/scene_controller.cpp
  src/live_set_switcher.cpp
  src/setlist_store.cpp
  src/setlist_controller.cpp
  src/edit_session.cpp)
add_library(velora::control ALIAS velora_control)
target_include_directories(velora_control PUBLIC include "${VELORA_PB_DIR}" "${VELORA_GENERATED_DIR}/host")
target_link_libraries(velora_control PUBLIC velora_core velora_host protobuf::libprotobuf Threads::Threads)
# The generated protobuf sources are not ours to police for warnings.
set_source_files_properties(${VELORA_PB_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# --- protected libraries in the engine (Story 6.5) ----------------------------------------------------------------------------------------------------
# velora_host_protected is the ONLY engine-side target that links the decrypting reader (velora_format_decrypt, OpenSSL): the ProtectedHeadProvider (key through
# the engine/ports key-provider port, hash-check + AES-CTR decrypt on the loading thread) and the EdgeBlockReader (the same on an I/O edge thread). engine/core never
# links it; velora_host and the librarian do not either.
add_library(velora_host_protected STATIC protected/protected_head_provider.cpp)
add_library(velora::host_protected ALIAS velora_host_protected)
target_link_libraries(velora_host_protected PUBLIC velora_control velora_host velora_format_decrypt)
target_compile_options(velora_host_protected PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

# The dev key provider: unwraps wrapped_key_v1 with a developer's device key read from an injected path. Never in a shippable configuration.
if(VELORA_DEV_KEY_PROVIDER AND (VELORA_PRODUCTION OR CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel|RelWithDebInfo)$"))
  message(FATAL_ERROR "engine: the dev key provider (a developer's device key read from a file, and the --dev-device-key flags) must not be built into a Release / MinSizeRel / "
                      "VELORA_PRODUCTION configuration. Set -DVELORA_DEV_KEY_PROVIDER=OFF (the board uses the OP-TEE provider, Story 6.6).")
endif()
# Same OpenSSL discovery as formatlib (imported targets are directory scoped; this directory is configured before formatlib). The tests use it for the vendor side of
# the wrap (fixtures), the dev key provider for the engine side.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(_ossl /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${_ossl}/include/openssl/evp.h")
      set(OPENSSL_ROOT_DIR "${_ossl}")
      break()
    endif()
  endforeach()
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_GLOBAL TRUE)  # the protected tests of tools/ (offline-render) use it too

# Story 6.5 tests that hold no device key and need no dev key provider: they are built and run with VELORA_DEV_KEY_PROVIDER=OFF as well (SecureKeyHandle, the
# DamageRegistry, the trust-list parser, the resolver's verification of bundle.vlrp and of the downgrade by a planted bundle.vlb).
add_executable(protected_keyless_test tests/protected_keyless_test.cpp)
target_link_libraries(protected_keyless_test PRIVATE velora_host velora_format_protected_writer OpenSSL::Crypto)
target_compile_options(protected_keyless_test PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
add_test(NAME protected_keyless_test COMMAND protected_keyless_test)
set_tests_properties(protected_keyless_test PROPERTIES TIMEOUT 300)

if(VELORA_DEV_KEY_PROVIDER)
  add_library(velora_host_dev_keys STATIC dev_keys/dev_key_provider.cpp)
  add_library(velora::host_dev_keys ALIAS velora_host_dev_keys)
  target_link_libraries(velora_host_dev_keys PUBLIC velora_host PRIVATE OpenSSL::Crypto)
  target_compile_options(velora_host_dev_keys PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

  # Story 6.5 tests (they use the dev key provider, the protected writer and OpenSSL directly for the vendor side of the wrap).
  # The engine over protected libraries: key provider vectors and tamper matrix, key memory, resolver verification, typed refusals, the key tied to the generation,
  # damaged blocks found by an edge thread, no plaintext on disk.
  add_executable(protected_engine_test tests/protected_engine_test.cpp)
  target_link_libraries(protected_engine_test PRIVATE velora_host_protected velora_host_dev_keys velora_format_protected_writer OpenSSL::Crypto)
  target_compile_options(protected_engine_test PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)
  add_test(NAME protected_engine_test COMMAND protected_engine_test)
  set_tests_properties(protected_engine_test PROPERTIES TIMEOUT 300)

  # The audio path over a protected instrument beside an I/O edge thread that decrypts and checks the same blocks: the same traffic without the sanitizer (macOS runs
  # the logic) and, below, with RealtimeSanitizer (docker-check, clang-20). The RTSan job also runs the negative control: a decrypt inside a nonblocking function is reported.
  add_executable(protected_rt_traffic_test tests/rtsan_protected_test.cpp)
  target_link_libraries(protected_rt_traffic_test PRIVATE velora_host_protected velora_host_dev_keys velora_format_protected_writer OpenSSL::Crypto Threads::Threads)
  add_test(NAME protected_rt_traffic_test COMMAND protected_rt_traffic_test)
  set_tests_properties(protected_rt_traffic_test PROPERTIES TIMEOUT 600)
  if(VELORA_RTSAN AND VELORA_HAVE_RTSAN)
    add_executable(rtsan_protected_test tests/rtsan_protected_test.cpp)
    target_link_libraries(rtsan_protected_test PRIVATE velora_host_protected velora_host_dev_keys velora_format_protected_writer OpenSSL::Crypto Threads::Threads)
    target_compile_options(rtsan_protected_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_protected_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_protected_test COMMAND rtsan_protected_test)
    set_tests_properties(rtsan_protected_test PROPERTIES TIMEOUT 600)

    add_executable(rtsan_decrypt_unsafe_test tests/rtsan_decrypt_unsafe_test.cpp)
    target_link_libraries(rtsan_decrypt_unsafe_test PRIVATE velora_format_decrypt velora_format_protected_writer OpenSSL::Crypto)
    target_compile_options(rtsan_decrypt_unsafe_test PRIVATE -fsanitize=realtime -g -O0)
    target_link_options(rtsan_decrypt_unsafe_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_decrypt_unsafe_test COMMAND rtsan_decrypt_unsafe_test)
    # Passes only on RTSan's own report of the violation (its real text: "ERROR: RealtimeSanitizer: unsafe-library-call" / "blocking-call" and "Intercepted call to
    # real-time unsafe function"), not on any non-zero exit.
    set_tests_properties(rtsan_decrypt_unsafe_test PROPERTIES PASS_REGULAR_EXPRESSION "ERROR: RealtimeSanitizer: (unsafe-library-call|blocking-call).*Intercepted call to real-time unsafe function")
  endif()
endif()

# Library API client (Story 4.11): a portable UDS client that tells the librarian service which Live Set became active and whether the engine is performing.
# It needs only the generated Library API code (proto/), protobuf and threads: no engine/core, no formatlib.
add_library(velora_library_client STATIC src/library_client.cpp)
add_library(velora::library_client ALIAS velora_library_client)
target_include_directories(velora_library_client PUBLIC include)
target_link_libraries(velora_library_client PUBLIC velora_library_api Threads::Threads)
target_compile_options(velora_library_client PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

# Story 5.5: the generated Category enum equals the formatlib header (ids and names).
add_executable(category_pb_test tests/category_pb_test.cpp)
target_link_libraries(category_pb_test PRIVATE velora_control)
add_test(NAME category_pb_test COMMAND category_pb_test)

set(VELORA_DEV_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/dev/engine-dev.json")

add_executable(control_tests tests/control_test.cpp)
target_link_libraries(control_tests PRIVATE velora_control)
target_compile_definitions(control_tests PRIVATE VELORA_DEV_CONFIG="${VELORA_DEV_CONFIG}")
add_test(NAME control_tests COMMAND control_tests)
set_tests_properties(control_tests PROPERTIES TIMEOUT 120)

# --- Live Set format and store (Story 4.2) ------------------------------------------------------------------
set(VELORA_LIVESET_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/live_sets")
set(VELORA_LIVESET_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
add_executable(live_set_tests tests/live_set_test.cpp)
target_link_libraries(live_set_tests PRIVATE velora_control)
target_compile_definitions(live_set_tests PRIVATE
  VELORA_LIVESET_CORPUS_DIR="${VELORA_LIVESET_CORPUS_DIR}" VELORA_LIVESET_GOLDEN_DIR="${VELORA_LIVESET_GOLDEN_DIR}")
add_test(NAME live_set_tests COMMAND live_set_tests)
set_tests_properties(live_set_tests PROPERTIES TIMEOUT 120)

# Live Set load (Story 4.3): resolve, resident instruments, one generation, reservation, all-or-nothing publication.
add_executable(live_set_loader_tests tests/live_set_loader_test.cpp)
target_link_libraries(live_set_loader_tests PRIVATE velora_control)
target_compile_definitions(live_set_loader_tests PRIVATE VELORA_LIVESET_CORPUS_DIR="${VELORA_LIVESET_CORPUS_DIR}")
add_test(NAME live_set_loader_tests COMMAND live_set_loader_tests)
set_tests_properties(live_set_loader_tests PROPERTIES TIMEOUT 120)

# Factory Live Sets (Story 6.9): the signed container (layout, signature, tamper sweep) and the first-boot install through the single writer (references, idempotence,
# user deletion, user files, release order, typed errors). It signs with the protected writer's Ed25519 (test and tool code only).
add_executable(factory_live_set_tests tests/factory_live_set_test.cpp)
target_link_libraries(factory_live_set_tests PRIVATE velora_control velora_format_protected_writer)
add_test(NAME factory_live_set_tests COMMAND factory_live_set_tests)
set_tests_properties(factory_live_set_tests PROPERTIES TIMEOUT 300)

# Scene recall (Story 4.4): parameter-only snapshot swap, performance values applied once, notes across a recall, stepping, hostile cases, ws.v1.
add_executable(scene_controller_tests tests/scene_controller_test.cpp)
target_link_libraries(scene_controller_tests PRIVATE velora_control)
add_test(NAME scene_controller_tests COMMAND scene_controller_tests)
set_tests_properties(scene_controller_tests PROPERTIES TIMEOUT 180)

# The same recall storms beside a rendering RT thread without the sanitizer (macOS runs the logic); the RTSan build of it is below.
add_executable(scene_rt_traffic_test tests/rtsan_scene_test.cpp)
target_link_libraries(scene_rt_traffic_test PRIVATE velora_control)
add_test(NAME scene_rt_traffic_test COMMAND scene_rt_traffic_test)
set_tests_properties(scene_rt_traffic_test PROPERTIES TIMEOUT 300)

# Seamless Live Set switching (Story 4.5): background load, cancellation, head budget, loading status cadence, notes across the swap, reservation handover, ws.v1.
add_executable(live_set_switcher_tests tests/live_set_switcher_test.cpp)
target_link_libraries(live_set_switcher_tests PRIVATE velora_control)
add_test(NAME live_set_switcher_tests COMMAND live_set_switcher_tests)
set_tests_properties(live_set_switcher_tests PROPERTIES TIMEOUT 300)

# The same switch storms beside a rendering RT thread without the sanitizer (macOS runs the logic); the RTSan build of it is below.
add_executable(switch_rt_traffic_test tests/rtsan_switch_test.cpp)
target_link_libraries(switch_rt_traffic_test PRIVATE velora_control)
add_test(NAME switch_rt_traffic_test COMMAND switch_rt_traffic_test)
set_tests_properties(switch_rt_traffic_test PROPERTIES TIMEOUT 300)

# Setlists, setlist tags, the default ASSIGN map, new Live Sets (Story 4.6): format, store commands, atomicity, single writer, corpus.
set(VELORA_SETLIST_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/setlists")
add_executable(setlist_tests tests/setlist_test.cpp)
target_link_libraries(setlist_tests PRIVATE velora_control)
target_compile_definitions(setlist_tests PRIVATE
  VELORA_SETLIST_CORPUS_DIR="${VELORA_SETLIST_CORPUS_DIR}" VELORA_SETLIST_GOLDEN_DIR="${VELORA_LIVESET_GOLDEN_DIR}")
add_test(NAME setlist_tests COMMAND setlist_tests)
set_tests_properties(setlist_tests PROPERTIES TIMEOUT 180)

# Setlist controller (Story 4.6): ws.v1 commands, loaded setlist, stepping scenarios, concurrent clients.
add_executable(setlist_controller_tests tests/setlist_controller_test.cpp)
target_link_libraries(setlist_controller_tests PRIVATE velora_control)
target_compile_definitions(setlist_controller_tests PRIVATE VELORA_DEV_CONFIG="${VELORA_DEV_CONFIG}")
add_test(NAME setlist_controller_tests COMMAND setlist_controller_tests)
set_tests_properties(setlist_controller_tests PROPERTIES TIMEOUT 180)

add_executable(setlist_corpus_gen tests/setlist_corpus_gen.cpp)
target_link_libraries(setlist_corpus_gen PRIVATE velora_control)
add_custom_target(setlist_corpus_regen
  COMMAND setlist_corpus_gen "${VELORA_SETLIST_CORPUS_DIR}" "${VELORA_LIVESET_GOLDEN_DIR}"
  DEPENDS setlist_corpus_gen
  COMMENT "Regenerating the setlist compatibility corpus"
  VERBATIM)

# Edits in memory and Write scopes (Story 4.9): the control->RT fence, dirty flags and change summary, edits kept across scene and Live Set changes, Write
# scopes, Save as new and its name rules, storage full, no file write while playing (a generated hour on a fake clock), hostile cases.
add_executable(edit_session_tests tests/edit_session_test.cpp)
target_link_libraries(edit_session_tests PRIVATE velora_control)
add_test(NAME edit_session_tests COMMAND edit_session_tests)
set_tests_properties(edit_session_tests PROPERTIES TIMEOUT 300)

# The control->RT fence beside a rendering RT thread without the sanitizer (macOS runs the logic); the RTSan build of it is below.
add_executable(edit_rt_traffic_test tests/rtsan_edit_test.cpp)
target_link_libraries(edit_rt_traffic_test PRIVATE velora_control)
add_test(NAME edit_rt_traffic_test COMMAND edit_rt_traffic_test)
set_tests_properties(edit_rt_traffic_test PROPERTIES TIMEOUT 300)

# ASSIGN maps with a global default (Story 4.10): SetAssignSlot / ClearAssignSlot on the current Live Set (dirty, Write, power cycle) and on the default
# (written at once, existing Live Sets untouched), SetEncoderMode / SelectPart, encoder routing end to end, validation, concurrent clients.
add_executable(assign_map_tests tests/assign_map_test.cpp)
target_link_libraries(assign_map_tests PRIVATE velora_control)
add_test(NAME assign_map_tests COMMAND assign_map_tests)
set_tests_properties(assign_map_tests PROPERTIES TIMEOUT 300)

# Regenerates the compatibility corpus and the golden file from the current code (review the diff, then commit):
#   cmake --build build --target live_set_corpus_regen
add_executable(live_set_corpus_gen tests/live_set_corpus_gen.cpp)
target_link_libraries(live_set_corpus_gen PRIVATE velora_control)
add_custom_target(live_set_corpus_regen
  COMMAND live_set_corpus_gen "${VELORA_LIVESET_CORPUS_DIR}" "${VELORA_LIVESET_GOLDEN_DIR}"
  DEPENDS live_set_corpus_gen
  COMMENT "Regenerating the Live Set compatibility corpus"
  VERBATIM)

# The same 10,000-change / 100-swap RT traffic without the sanitizer, so macOS also runs its logic
# (the RTSan build of it, rtsan_control_test, is below and runs where RealtimeSanitizer exists).
add_executable(control_rt_traffic_test tests/rtsan_control_test.cpp)
target_link_libraries(control_rt_traffic_test PRIVATE velora_control)
add_test(NAME control_rt_traffic_test COMMAND control_rt_traffic_test)
set_tests_properties(control_rt_traffic_test PROPERTIES TIMEOUT 120)

add_executable(host_tests tests/host_test.cpp)
target_link_libraries(host_tests PRIVATE velora_host)
add_test(NAME host_tests COMMAND host_tests)

add_executable(host_unit_tests tests/host_unit_test.cpp)
target_link_libraries(host_unit_tests PRIVATE velora_host)
add_test(NAME host_unit_tests COMMAND host_unit_tests)

add_executable(mcu_wrapper_test tests/mcu_wrapper_test.cpp)
target_link_libraries(mcu_wrapper_test PRIVATE velora_host)
add_test(NAME mcu_wrapper_test COMMAND mcu_wrapper_test)

add_executable(sample_bundle_loader_test tests/sample_bundle_loader_test.cpp)
target_link_libraries(sample_bundle_loader_test PRIVATE velora_host)
add_test(NAME sample_bundle_loader_test COMMAND sample_bundle_loader_test)

add_executable(library_resolver_test tests/library_resolver_test.cpp)
target_link_libraries(library_resolver_test PRIVATE velora_libgen)
add_test(NAME library_resolver_test COMMAND library_resolver_test)

# RealtimeSanitizer variant of the audio path (VELORA_HAVE_RTSAN is probed in engine/core).
if(VELORA_RTSAN AND VELORA_HAVE_RTSAN)
  add_executable(rtsan_host_render_test tests/rtsan_host_render_test.cpp)
  target_link_libraries(rtsan_host_render_test PRIVATE velora_host)
  target_compile_options(rtsan_host_render_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_host_render_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_host_render_test COMMAND rtsan_host_render_test)

  add_executable(rtsan_control_test tests/rtsan_control_test.cpp)
  target_link_libraries(rtsan_control_test PRIVATE velora_control)
  target_compile_options(rtsan_control_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_control_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_control_test COMMAND rtsan_control_test)
  set_tests_properties(rtsan_control_test PROPERTIES TIMEOUT 120)

  add_executable(rtsan_scene_test tests/rtsan_scene_test.cpp)
  target_link_libraries(rtsan_scene_test PRIVATE velora_control)
  target_compile_options(rtsan_scene_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_scene_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_scene_test COMMAND rtsan_scene_test)
  set_tests_properties(rtsan_scene_test PROPERTIES TIMEOUT 300)

  add_executable(rtsan_edit_test tests/rtsan_edit_test.cpp)
  target_link_libraries(rtsan_edit_test PRIVATE velora_control)
  target_compile_options(rtsan_edit_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_edit_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_edit_test COMMAND rtsan_edit_test)
  set_tests_properties(rtsan_edit_test PROPERTIES TIMEOUT 300)

  add_executable(rtsan_switch_test tests/rtsan_switch_test.cpp)
  target_link_libraries(rtsan_switch_test PRIVATE velora_control)
  target_compile_options(rtsan_switch_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_switch_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_switch_test COMMAND rtsan_switch_test)
  set_tests_properties(rtsan_switch_test PROPERTIES TIMEOUT 300)
endif()

# Story 6.5 negative control of the damage path (no key, no provider): REPORTING a damaged block inside a nonblocking function is reported by RTSan.
if(VELORA_RTSAN AND VELORA_HAVE_RTSAN)
  add_executable(rtsan_damage_report_unsafe_test tests/rtsan_damage_report_unsafe_test.cpp)
  target_link_libraries(rtsan_damage_report_unsafe_test PRIVATE velora_host)
  target_compile_options(rtsan_damage_report_unsafe_test PRIVATE -fsanitize=realtime -g -O0)
  target_link_options(rtsan_damage_report_unsafe_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_damage_report_unsafe_test COMMAND rtsan_damage_report_unsafe_test)
  set_tests_properties(rtsan_damage_report_unsafe_test PROPERTIES PASS_REGULAR_EXPRESSION "ERROR: RealtimeSanitizer: (unsafe-library-call|blocking-call).*Intercepted call to real-time unsafe function")
endif()

if(APPLE)
  add_subdirectory(macos)
endif()

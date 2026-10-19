find_package(Threads REQUIRED)
add_library(velora_host STATIC
  src/host_info.cpp
  src/time_mapper.cpp
  src/midi_ingest.cpp
  src/render_driver.cpp
  src/posix_fs.cpp
  src/library_resolver.cpp
  src/sample_bundle_loader.cpp
  src/mcu_frame.cpp)
add_library(velora::host ALIAS velora_host)
target_include_directories(velora_host PUBLIC include)
target_link_libraries(velora_host PUBLIC velora_core velora_ports velora_format velora_mcu_common)

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
set(VELORA_PB_SOURCES "${VELORA_PB_DIR}/ws_v1.pb.cc" "${VELORA_PB_DIR}/ws_v1_params.pb.cc")
set(VELORA_PB_HEADERS "${VELORA_PB_DIR}/ws_v1.pb.h" "${VELORA_PB_DIR}/ws_v1_params.pb.h")
add_custom_command(
  OUTPUT ${VELORA_PB_SOURCES} ${VELORA_PB_HEADERS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VELORA_PB_DIR}"
  COMMAND $<TARGET_FILE:protobuf::protoc> -I "${CMAKE_SOURCE_DIR}/proto" -I "${VELORA_GENERATED_DIR}/proto"
          --cpp_out "${VELORA_PB_DIR}" "${CMAKE_SOURCE_DIR}/proto/ws_v1.proto" "${VELORA_PARAMS_PROTO}"
  DEPENDS "${CMAKE_SOURCE_DIR}/proto/ws_v1.proto" "${VELORA_PARAMS_PROTO}"
  COMMENT "Generating ws.v1 protobuf code"
  VERBATIM)

add_library(velora_control STATIC
  ${VELORA_PB_SOURCES} ${VELORA_PB_HEADERS} ${VELORA_PARAMS_HOST_HPP}
  src/control_plane.cpp
  src/control_server.cpp
  src/dev_config.cpp)
add_library(velora::control ALIAS velora_control)
target_include_directories(velora_control PUBLIC include "${VELORA_PB_DIR}" "${VELORA_GENERATED_DIR}/host")
target_link_libraries(velora_control PUBLIC velora_core velora_host protobuf::libprotobuf Threads::Threads)
# The generated protobuf sources are not ours to police for warnings.
set_source_files_properties(${VELORA_PB_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

set(VELORA_DEV_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/dev/engine-dev.json")

add_executable(control_tests tests/control_test.cpp)
target_link_libraries(control_tests PRIVATE velora_control)
target_compile_definitions(control_tests PRIVATE VELORA_DEV_CONFIG="${VELORA_DEV_CONFIG}")
add_test(NAME control_tests COMMAND control_tests)
set_tests_properties(control_tests PROPERTIES TIMEOUT 120)

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
endif()

if(APPLE)
  add_subdirectory(macos)
endif()

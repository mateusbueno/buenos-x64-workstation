# sysd (Foundation: sysd config service): the host process that owns the system configuration and serves it over ws.v1 on a Unix domain socket.
find_package(Threads REQUIRED)
# It links velora_control (the generated ws.v1 / SysdConfig protobuf code, the atomic-replace and migration helpers). engine/core never sees it.
add_library(velora_sysd_service STATIC
  src/config_store.cpp
  src/config_service.cpp
  src/config_server.cpp)
add_library(velora::sysd_service ALIAS velora_sysd_service)
target_include_directories(velora_sysd_service PUBLIC include)
target_link_libraries(velora_sysd_service PUBLIC velora_control Threads::Threads)
target_compile_options(velora_sysd_service PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_executable(velora-sysd src/main.cpp)
target_link_libraries(velora-sysd PRIVATE velora_sysd_service)

add_executable(sysd_config_tests tests/config_store_test.cpp)
target_link_libraries(sysd_config_tests PRIVATE velora_sysd_service)
add_test(NAME sysd_config_tests COMMAND sysd_config_tests)
set_tests_properties(sysd_config_tests PROPERTIES TIMEOUT 180)

add_executable(sysd_service_tests tests/config_service_test.cpp)
target_link_libraries(sysd_service_tests PRIVATE velora_sysd_service)
add_test(NAME sysd_service_tests COMMAND sysd_service_tests)
set_tests_properties(sysd_service_tests PROPERTIES TIMEOUT 180)

# Sockets, the engine client and the end-to-end smoke (real UDS in a temp dir).
add_executable(sysd_socket_tests tests/socket_test.cpp)
target_link_libraries(sysd_socket_tests PRIVATE velora_sysd_service velora_sysdctl_client)
add_test(NAME sysd_socket_tests COMMAND sysd_socket_tests)
set_tests_properties(sysd_socket_tests PROPERTIES TIMEOUT 180)

# The end-to-end smoke (velora-sysd + velora-sysdctl + engine-host --no-audio --sysd-socket) is engine/host/macos/engine_host_sysd_smoke.cpp (engine-host is Apple-only).

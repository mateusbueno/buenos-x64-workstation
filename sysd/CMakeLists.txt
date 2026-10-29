# sysd (Foundation: sysd config service): the host process that owns the system configuration and serves it over ws.v1 on a Unix domain socket.
find_package(Threads REQUIRED)
# It links velora_control (the generated ws.v1 / SysdConfig protobuf code, the atomic-replace and migration helpers). engine/core never sees it.
add_library(velora_sysd_service STATIC
  src/config_store.cpp
  src/config_service.cpp
  src/config_server.cpp
  src/pairing.cpp
  src/bridge_server.cpp
  src/mcu_boot_checker.cpp
  src/update_installer.cpp)
add_library(velora::sysd_service ALIAS velora_sysd_service)
target_include_directories(velora_sysd_service PUBLIC include)
target_link_libraries(velora_sysd_service PUBLIC velora_control velora_library_api Threads::Threads)
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

# Story 3.11: pedal calibration end to end (a real sysd on a temp directory, the virtual panel's frames, the calibration commands, persistence).
add_executable(sysd_pedal_calibration_test tests/pedal_calibration_test.cpp)
target_link_libraries(sysd_pedal_calibration_test PRIVATE velora_sysd_service velora_sysdctl_client)
add_test(NAME sysd_pedal_calibration_test COMMAND sysd_pedal_calibration_test)
set_tests_properties(sysd_pedal_calibration_test PROPERTIES TIMEOUT 180)

# The end-to-end smoke (velora-sysd + velora-sysdctl + engine-host --no-audio --sysd-socket) is engine/host/macos/engine_host_sysd_smoke.cpp (engine-host is Apple-only).

# Foundation: Velora Studio device connection: the TCP bridge (handshake, pairing, forwarding, limits) against real localhost sockets and fake upstream sockets.
add_executable(sysd_bridge_tests tests/bridge_test.cpp)
target_link_libraries(sysd_bridge_tests PRIVATE velora_sysd_service velora_sysdctl_client)
add_test(NAME sysd_bridge_tests COMMAND sysd_bridge_tests)
set_tests_properties(sysd_bridge_tests PROPERTIES TIMEOUT 180)

# Story 8.4: MCU firmware manifest in the A/B image and boot-time version check / dual-bank reflash.
add_executable(sysd_mcu_boot_checker_test tests/mcu_boot_checker_test.cpp)
target_link_libraries(sysd_mcu_boot_checker_test PRIVATE velora_sysd_service)
add_test(NAME sysd_mcu_boot_checker_test COMMAND sysd_mcu_boot_checker_test)
set_tests_properties(sysd_mcu_boot_checker_test PROPERTIES TIMEOUT 60)

# Story 8.1: sysd as the sole installer from a USB-A stick and shared Studio verification path.
add_executable(sysd_update_installer_test tests/update_installer_test.cpp)
target_link_libraries(sysd_update_installer_test PRIVATE velora_sysd_service)
add_test(NAME sysd_update_installer_test COMMAND sysd_update_installer_test)
set_tests_properties(sysd_update_installer_test PROPERTIES TIMEOUT 60)

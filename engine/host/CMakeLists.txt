add_library(velora_host STATIC src/host_info.cpp)
add_library(velora::host ALIAS velora_host)
target_include_directories(velora_host PUBLIC include)
target_link_libraries(velora_host PUBLIC velora_core velora_ports)

add_executable(host_tests tests/host_test.cpp)
target_link_libraries(host_tests PRIVATE velora_host)
add_test(NAME host_tests COMMAND host_tests)

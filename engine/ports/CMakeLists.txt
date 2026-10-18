add_library(velora_ports INTERFACE)
add_library(velora::ports ALIAS velora_ports)
target_include_directories(velora_ports INTERFACE include)
target_compile_features(velora_ports INTERFACE cxx_std_20)

add_executable(ports_tests tests/ports_test.cpp)
target_link_libraries(ports_tests PRIVATE velora_ports)
add_test(NAME ports_tests COMMAND ports_tests)

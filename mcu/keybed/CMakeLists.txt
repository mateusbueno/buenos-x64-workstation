# mcu/keybed: portable C11 keybed MCU firmware scanner (Story 3.3).
# Dependency rule: no OS, no heap, no C++ here; it must also build for the MCU targets.

set(VELORA_MCU_KEYBED_WARN "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(VELORA_MCU_KEYBED_WARN -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(VELORA_WERROR)
    list(APPEND VELORA_MCU_KEYBED_WARN -Werror)
  endif()
endif()

add_library(velora_mcu_keybed STATIC src/keybed_scanner.c)
add_library(velora::mcu_keybed ALIAS velora_mcu_keybed)
set_target_properties(velora_mcu_keybed PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(velora_mcu_keybed PUBLIC include)
target_link_libraries(velora_mcu_keybed PUBLIC velora_mcu_common)
target_compile_options(velora_mcu_keybed PRIVATE ${VELORA_MCU_KEYBED_WARN} -O2)

add_executable(mcu_keybed_c_test tests/mcu_keybed_c_test.c)
set_target_properties(mcu_keybed_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mcu_keybed_c_test PRIVATE ${VELORA_MCU_KEYBED_WARN})
target_link_libraries(mcu_keybed_c_test PRIVATE velora_mcu_keybed)
add_test(NAME mcu_keybed_c_test COMMAND mcu_keybed_c_test)

add_executable(mcu_keybed_test tests/mcu_keybed_test.cpp)
target_link_libraries(mcu_keybed_test PRIVATE velora_mcu_keybed)
add_test(NAME mcu_keybed_test COMMAND mcu_keybed_test)

# mcu/bootloader: portable C11 dual-bank MCU bootloader and SoC reflash protocol (Story 3.4).
# Dependency rule: no OS, no heap, no C++ here; it must also build for the MCU targets.

set(VELORA_MCU_BOOT_WARN "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(VELORA_MCU_BOOT_WARN -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(VELORA_WERROR)
    list(APPEND VELORA_MCU_BOOT_WARN -Werror)
  endif()
endif()

add_library(velora_mcu_bootloader STATIC src/bootloader.c)
add_library(velora::mcu_bootloader ALIAS velora_mcu_bootloader)
set_target_properties(velora_mcu_bootloader PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(velora_mcu_bootloader PUBLIC include)
target_link_libraries(velora_mcu_bootloader PUBLIC velora_mcu_common)
target_compile_options(velora_mcu_bootloader PRIVATE ${VELORA_MCU_BOOT_WARN} -O2)

add_executable(mcu_bootloader_c_test tests/mcu_bootloader_c_test.c)
set_target_properties(mcu_bootloader_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mcu_bootloader_c_test PRIVATE ${VELORA_MCU_BOOT_WARN})
target_link_libraries(mcu_bootloader_c_test PRIVATE velora_mcu_bootloader)
add_test(NAME mcu_bootloader_c_test COMMAND mcu_bootloader_c_test)

add_executable(mcu_bootloader_test tests/mcu_bootloader_test.cpp)
target_link_libraries(mcu_bootloader_test PRIVATE velora_mcu_bootloader)
add_test(NAME mcu_bootloader_test COMMAND mcu_bootloader_test)

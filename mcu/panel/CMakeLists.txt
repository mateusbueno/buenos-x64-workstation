# mcu/panel: portable C11 panel MCU firmware modules (Stories 3.8 and 3.9).
# Dependency rule: no OS, no heap, no C++ here; it must also build for the MCU targets.

set(VELORA_MCU_PANEL_WARN "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(VELORA_MCU_PANEL_WARN -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(VELORA_WERROR)
    list(APPEND VELORA_MCU_PANEL_WARN -Werror)
  endif()
endif()

add_library(velora_mcu_panel STATIC src/panel_inputs.c src/panel_leds.c)
add_library(velora::mcu_panel ALIAS velora_mcu_panel)
set_target_properties(velora_mcu_panel PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(velora_mcu_panel PUBLIC include)
target_link_libraries(velora_mcu_panel PUBLIC velora_mcu_common)
target_compile_options(velora_mcu_panel PRIVATE ${VELORA_MCU_PANEL_WARN} -O2)

add_executable(mcu_panel_inputs_c_test tests/mcu_panel_inputs_c_test.c)
set_target_properties(mcu_panel_inputs_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mcu_panel_inputs_c_test PRIVATE ${VELORA_MCU_PANEL_WARN})
target_link_libraries(mcu_panel_inputs_c_test PRIVATE velora_mcu_panel)
add_test(NAME mcu_panel_inputs_c_test COMMAND mcu_panel_inputs_c_test)

add_executable(mcu_panel_inputs_test tests/mcu_panel_inputs_test.cpp)
target_link_libraries(mcu_panel_inputs_test PRIVATE velora_mcu_panel)
add_test(NAME mcu_panel_inputs_test COMMAND mcu_panel_inputs_test)

add_executable(mcu_panel_leds_c_test tests/mcu_panel_leds_c_test.c)
set_target_properties(mcu_panel_leds_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mcu_panel_leds_c_test PRIVATE ${VELORA_MCU_PANEL_WARN})
target_link_libraries(mcu_panel_leds_c_test PRIVATE velora_mcu_panel)
add_test(NAME mcu_panel_leds_c_test COMMAND mcu_panel_leds_c_test)

add_executable(mcu_panel_leds_test tests/mcu_panel_leds_test.cpp)
target_link_libraries(mcu_panel_leds_test PRIVATE velora_mcu_panel)
add_test(NAME mcu_panel_leds_test COMMAND mcu_panel_leds_test)

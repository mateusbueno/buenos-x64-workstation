# mcu/common: portable C11 MCU frame protocol library and the generated control/LED ID maps (Story 3.1).
# Dependency rule: no OS, no heap, no C++ here; it must also build for the MCU targets.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Strict warnings for the portable library. -Werror is on by default for our compilers (gcc, clang);
# configure with -DVELORA_WERROR=OFF when trying a new toolchain.
option(VELORA_WERROR "Treat warnings in mcu/common as errors" ON)
set(VELORA_MCU_WARN "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(VELORA_MCU_WARN -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(VELORA_WERROR)
    list(APPEND VELORA_MCU_WARN -Werror)
  endif()
endif()

set(VELORA_MCU_GEN_DIR "${CMAKE_BINARY_DIR}/generated/mcu")
set(VELORA_MCU_GEN_OUTPUTS
    "${VELORA_MCU_GEN_DIR}/mcu/mcu_ids.h"
    "${VELORA_MCU_GEN_DIR}/velora/host/mcu_ids.hpp"
    "${VELORA_MCU_GEN_DIR}/MCU-ID-MAP.md")
add_custom_command(
  OUTPUT ${VELORA_MCU_GEN_OUTPUTS}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_ids.py --out-dir "${VELORA_MCU_GEN_DIR}"
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_ids.py ${CMAKE_CURRENT_SOURCE_DIR}/ids.json
  COMMENT "Generating MCU ID maps"
  VERBATIM)
add_custom_target(velora_mcu_ids_gen DEPENDS ${VELORA_MCU_GEN_OUTPUTS})

add_library(velora_mcu_common STATIC src/mcu_frame.c)
add_library(velora::mcu_common ALIAS velora_mcu_common)
set_target_properties(velora_mcu_common PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(velora_mcu_common PUBLIC include "${VELORA_MCU_GEN_DIR}")
target_compile_options(velora_mcu_common PRIVATE ${VELORA_MCU_WARN} -O2)
add_dependencies(velora_mcu_common velora_mcu_ids_gen)

# Tests (bare main, like the rest). The C smoke test proves the header and library work from plain C11.
add_executable(mcu_frame_c_test tests/mcu_frame_c_test.c)
set_target_properties(mcu_frame_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(mcu_frame_c_test PRIVATE ${VELORA_MCU_WARN})
target_link_libraries(mcu_frame_c_test PRIVATE velora_mcu_common)
add_test(NAME mcu_frame_c_test COMMAND mcu_frame_c_test)

add_executable(mcu_frame_test tests/mcu_frame_test.cpp)
target_link_libraries(mcu_frame_test PRIVATE velora_mcu_common)
add_test(NAME mcu_frame_test COMMAND mcu_frame_test)

# Fuzz: deterministic seed, byte budget from VELORA_FUZZ_BYTES (default 50 MB). Full hour of 3 Mbaud traffic:
#   VELORA_FUZZ_BYTES=1350000000 ctest --test-dir build -R mcu_fuzz --output-on-failure   (labelled "slow;fuzz")
add_executable(mcu_fuzz_test tests/mcu_fuzz_test.cpp)
target_link_libraries(mcu_fuzz_test PRIVATE velora_mcu_common)
target_compile_options(mcu_fuzz_test PRIVATE -O2)  # the default build type has no optimisation; the fuzz budget is large
add_test(NAME mcu_fuzz_test COMMAND mcu_fuzz_test)
set_tests_properties(mcu_fuzz_test PROPERTIES LABELS "slow;fuzz" TIMEOUT 7200)

# ID map gates: ids.json valid (no duplicate IDs / missing fields / reused IDs) and both committed
# MCU-ID-MAP.md copies identical to what the generator emits now.
add_test(NAME mcu_id_map_up_to_date
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_ids.py --check)
add_test(NAME mcu_id_map_selftest
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_ids.py --self-test)

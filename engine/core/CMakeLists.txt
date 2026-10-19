include(${CMAKE_SOURCE_DIR}/proto/params.cmake)
velora_params_output(core VELORA_PARAMS_CORE_HPP)

add_library(velora_core STATIC
  ${VELORA_PARAMS_CORE_HPP}
  src/snapshot.cpp
  src/version.cpp
  src/engine_registry.cpp
  src/test_tone_engine.cpp
  src/sample_engine.cpp
  src/organ_engine.cpp
  src/rotary_speaker.cpp
  src/voice_arbiter.cpp
  src/fx.cpp
  src/mixer.cpp
  src/block_renderer.cpp)
add_library(velora::core ALIAS velora_core)
target_include_directories(velora_core PUBLIC include ${VELORA_GENERATED_DIR}/core)
# Dependency rule: core may link only core and ports targets.
target_link_libraries(velora_core PUBLIC velora_ports)

add_executable(core_tests tests/core_test.cpp)
target_link_libraries(core_tests PRIVATE velora_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(sample_engine_tests tests/sample_engine_test.cpp)
target_link_libraries(sample_engine_tests PRIVATE velora_core)
add_test(NAME sample_engine_tests COMMAND sample_engine_tests)

add_executable(voice_arbiter_tests tests/voice_arbiter_test.cpp)
target_link_libraries(voice_arbiter_tests PRIVATE velora_core)
target_compile_definitions(voice_arbiter_tests PRIVATE VELORA_ARBITER_TEST_HOOKS)
add_test(NAME voice_arbiter_tests COMMAND voice_arbiter_tests)

add_executable(params_tests tests/params_test.cpp)
target_link_libraries(params_tests PRIVATE velora_core)
add_test(NAME params_tests COMMAND params_tests)

add_executable(engine_tests tests/engine_test.cpp)
target_link_libraries(engine_tests PRIVATE velora_core)
add_test(NAME engine_tests COMMAND engine_tests)

add_executable(organ_tests tests/organ_test.cpp)
target_link_libraries(organ_tests PRIVATE velora_core)
target_compile_definitions(organ_tests PRIVATE VELORA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME organ_tests COMMAND organ_tests)

add_executable(organ_percussion_tests tests/organ_percussion_test.cpp)
target_link_libraries(organ_percussion_tests PRIVATE velora_core)
add_test(NAME organ_percussion_tests COMMAND organ_percussion_tests)

add_executable(organ_scanner_tests tests/organ_scanner_test.cpp)
target_link_libraries(organ_scanner_tests PRIVATE velora_core)
add_test(NAME organ_scanner_tests COMMAND organ_scanner_tests)

add_executable(organ_contact_tests tests/organ_contact_test.cpp)
target_link_libraries(organ_contact_tests PRIVATE velora_core)
add_test(NAME organ_contact_tests COMMAND organ_contact_tests)

add_executable(rotary_tests tests/rotary_speaker_test.cpp)
target_link_libraries(rotary_tests PRIVATE velora_core)
add_test(NAME rotary_tests COMMAND rotary_tests)

add_executable(routing_tests tests/routing_test.cpp)
target_link_libraries(routing_tests PRIVATE velora_core)
add_test(NAME routing_tests COMMAND routing_tests)

# The routing traffic (10,000 blocks, chain changes every 3 blocks, determinism) without the sanitizer, so macOS runs its logic too;
# the RTSan build of the same file is below.
add_executable(routing_traffic_test tests/rtsan_routing_test.cpp)
target_link_libraries(routing_traffic_test PRIVATE velora_core)
add_test(NAME routing_traffic_test COMMAND routing_traffic_test)
set_tests_properties(routing_traffic_test PROPERTIES TIMEOUT 120)

# Host micro-benchmark (Story 2.2): cost per voice and per generator; host figures, not representative of the board.
add_executable(organ_bench tests/organ_bench.cpp)
target_link_libraries(organ_bench PRIVATE velora_core)
add_test(NAME organ_bench COMMAND organ_bench)
set_tests_properties(organ_bench PROPERTIES PASS_REGULAR_EXPRESSION "host, not representative")

# --- RealtimeSanitizer gate -------------------------------------------------
if(VELORA_RTSAN)
  include(CheckCXXSourceCompiles)
  set(_saved_flags "${CMAKE_REQUIRED_FLAGS}")
  set(_saved_libs "${CMAKE_REQUIRED_LINK_OPTIONS}")
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=realtime")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=realtime")
  check_cxx_source_compiles("
    void f() [[clang::nonblocking]] {}
    int main() { f(); return 0; }" VELORA_HAVE_RTSAN)
  set(CMAKE_REQUIRED_FLAGS "${_saved_flags}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${_saved_libs}")

  if(VELORA_HAVE_RTSAN)
    message(STATUS "RealtimeSanitizer available: building RTSan tests")
    foreach(t rtsan_safe_test rtsan_unsafe_test)
      add_executable(${t} tests/${t}.cpp)
      target_compile_options(${t} PRIVATE -fsanitize=realtime -g)
      target_link_options(${t} PRIVATE -fsanitize=realtime)
    endforeach()
    add_executable(rtsan_block_loop_test tests/rtsan_block_loop_test.cpp)
    target_link_libraries(rtsan_block_loop_test PRIVATE velora_core)
    target_compile_options(rtsan_block_loop_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_block_loop_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_block_loop_test COMMAND rtsan_block_loop_test)
    add_executable(rtsan_sample_engine_test tests/rtsan_sample_engine_test.cpp)
    target_link_libraries(rtsan_sample_engine_test PRIVATE velora_core)
    target_compile_options(rtsan_sample_engine_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_sample_engine_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_sample_engine_test COMMAND rtsan_sample_engine_test)
    add_executable(rtsan_arbiter_test tests/rtsan_arbiter_test.cpp)
    target_link_libraries(rtsan_arbiter_test PRIVATE velora_core)
    target_compile_definitions(rtsan_arbiter_test PRIVATE VELORA_ARBITER_TEST_HOOKS)
    target_compile_options(rtsan_arbiter_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_arbiter_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_arbiter_test COMMAND rtsan_arbiter_test)
    add_executable(rtsan_organ_test tests/rtsan_organ_test.cpp)
    target_link_libraries(rtsan_organ_test PRIVATE velora_core)
    target_compile_options(rtsan_organ_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_organ_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_organ_test COMMAND rtsan_organ_test)
    add_executable(rtsan_routing_test tests/rtsan_routing_test.cpp)
    target_link_libraries(rtsan_routing_test PRIVATE velora_core)
    target_compile_options(rtsan_routing_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_routing_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_routing_test COMMAND rtsan_routing_test)
    set_tests_properties(rtsan_routing_test PROPERTIES TIMEOUT 120)
    add_test(NAME rtsan_safe_test COMMAND rtsan_safe_test)
    # The deliberately RT-unsafe test must be aborted by RTSan: expected failure.
    add_test(NAME rtsan_unsafe_test COMMAND rtsan_unsafe_test)
    # Pass only when RTSan itself reports the violation, not on any non-zero exit.
    set_tests_properties(rtsan_unsafe_test PROPERTIES PASS_REGULAR_EXPRESSION "RealtimeSanitizer")
    target_compile_options(rtsan_unsafe_test PRIVATE -O0)
  else()
    message(STATUS "RealtimeSanitizer unavailable with ${CMAKE_CXX_COMPILER_ID} "
                   "${CMAKE_CXX_COMPILER_VERSION}: RTSan tests skipped (CI runs them with clang >= 20)")
  endif()
else()
  message(STATUS "VELORA_RTSAN=OFF: RTSan tests skipped")
endif()

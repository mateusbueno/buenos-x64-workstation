include(${CMAKE_SOURCE_DIR}/proto/params.cmake)
velora_params_output(core VELORA_PARAMS_CORE_HPP)

set(VELORA_CORE_SOURCES
  src/snapshot.cpp
  src/version.cpp
  src/engine_registry.cpp
  src/test_tone_engine.cpp
  src/sample_engine.cpp
  src/organ_engine.cpp
  src/rotary_speaker.cpp
  src/voice_arbiter.cpp
  src/fx.cpp
  src/eq.cpp
  src/compressor.cpp
  src/chorus_phaser.cpp
  src/ep_amp.cpp
  src/rotary_effect.cpp
  src/reverb.cpp
  src/delay.cpp
  src/mixer.cpp
  src/transport_clock.cpp
  src/clock_follower.cpp
  src/arpeggiator.cpp
  src/fm_engine.cpp
  src/va_engine.cpp
  src/block_renderer.cpp)
add_library(velora_core STATIC ${VELORA_PARAMS_CORE_HPP} ${VELORA_CORE_SOURCES})
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

# Story 6.5: content integrity on the real-time side (damage map of a protected sample: fade to silence, content_integrity counter, bit-exact when undamaged).
add_executable(content_integrity_tests tests/content_integrity_test.cpp)
target_link_libraries(content_integrity_tests PRIVATE velora_core)
add_test(NAME content_integrity_tests COMMAND content_integrity_tests)

# Story 8.8: the input-activity flags (keys held, sustain pedal) RT publishes for the Boot-into pointer's idle-safe policy.
add_executable(input_activity_tests tests/input_activity_test.cpp)
target_link_libraries(input_activity_tests PRIVATE velora_core)
add_test(NAME input_activity_tests COMMAND input_activity_tests)

# Story 1.21: in-RAM diagnostics counters and histograms.
add_executable(diagnostics_tests tests/diagnostics_test.cpp)
target_link_libraries(diagnostics_tests PRIVATE velora_core)
add_test(NAME diagnostics_tests COMMAND diagnostics_tests)

# Story 5.2: keyswitches, CC crossfades and filters of the sample engine (the sfizz parity is in tools/offline-render).
add_executable(sample_features_tests tests/sample_features_test.cpp)
target_link_libraries(sample_features_tests PRIVATE velora_core)
add_test(NAME sample_features_tests COMMAND sample_features_tests)

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

add_executable(fm_engine_tests tests/fm_engine_test.cpp)
target_link_libraries(fm_engine_tests PRIVATE velora_core)
add_test(NAME fm_engine_tests COMMAND fm_engine_tests)

add_executable(va_engine_tests tests/va_engine_test.cpp)
target_link_libraries(va_engine_tests PRIVATE velora_core)
target_compile_definitions(va_engine_tests PRIVATE VELORA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME va_engine_tests COMMAND va_engine_tests)

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

# ASSIGN-mode encoder routing (Story 4.10): apply_relative_step, the reserved control events, routing per parameter type, mode gating, refusals.
add_executable(assign_tests tests/assign_test.cpp)
target_link_libraries(assign_tests PRIVATE velora_core)
add_test(NAME assign_tests COMMAND assign_tests)

add_executable(part_router_tests tests/part_router_test.cpp)
target_link_libraries(part_router_tests PRIVATE velora_core)
add_test(NAME part_router_tests COMMAND part_router_tests)

add_executable(eq_compressor_tests tests/eq_compressor_test.cpp)
target_link_libraries(eq_compressor_tests PRIVATE velora_core)
add_test(NAME eq_compressor_tests COMMAND eq_compressor_tests)

add_executable(chorus_phaser_tests tests/chorus_phaser_test.cpp)
target_link_libraries(chorus_phaser_tests PRIVATE velora_core)
add_test(NAME chorus_phaser_tests COMMAND chorus_phaser_tests)

add_executable(ep_amp_tests tests/ep_amp_test.cpp)
target_link_libraries(ep_amp_tests PRIVATE velora_core)
add_test(NAME ep_amp_tests COMMAND ep_amp_tests)

add_executable(reverb_tests tests/reverb_test.cpp)
target_link_libraries(reverb_tests PRIVATE velora_core)
add_test(NAME reverb_tests COMMAND reverb_tests)

add_executable(clock_follower_tests tests/clock_follower_test.cpp)
target_link_libraries(clock_follower_tests PRIVATE velora_core)
add_test(NAME clock_follower_tests COMMAND clock_follower_tests)
set_tests_properties(clock_follower_tests PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

add_executable(transport_clock_tests tests/transport_clock_test.cpp)
target_link_libraries(transport_clock_tests PRIVATE velora_core)
add_test(NAME transport_clock_tests COMMAND transport_clock_tests)
set_tests_properties(transport_clock_tests PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

add_executable(arpeggiator_tests tests/arpeggiator_test.cpp)
target_link_libraries(arpeggiator_tests PRIVATE velora_core)
add_test(NAME arpeggiator_tests COMMAND arpeggiator_tests)
set_tests_properties(arpeggiator_tests PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

add_executable(delay_tests tests/delay_test.cpp)
target_link_libraries(delay_tests PRIVATE velora_core)
add_test(NAME delay_tests COMMAND delay_tests)
set_tests_properties(delay_tests PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

# The 10-minute feedback run (Story 2.11): ~86 M frames, a separate, labelled slow test with its own timeout (about a minute in an unoptimised build).
add_executable(delay_longrun_tests tests/delay_longrun_test.cpp)
target_link_libraries(delay_longrun_tests PRIVATE velora_core)
add_test(NAME delay_longrun_tests COMMAND delay_longrun_tests)
set_tests_properties(delay_longrun_tests PROPERTIES TIMEOUT 900 LABELS slow PASS_REGULAR_EXPRESSION "delay_longrun_tests: ok" FAIL_REGULAR_EXPRESSION "FAIL")

add_executable(rotary_insert_tests tests/rotary_insert_test.cpp)
target_link_libraries(rotary_insert_tests PRIVATE velora_core)
add_test(NAME rotary_insert_tests COMMAND rotary_insert_tests)

# The routing traffic (10,000 blocks, chain changes every 3 blocks, determinism) without the sanitizer, so macOS runs its logic too;
# the RTSan build of the same file is below.
add_executable(routing_traffic_test tests/rtsan_routing_test.cpp)
target_link_libraries(routing_traffic_test PRIVATE velora_core)
add_test(NAME routing_traffic_test COMMAND routing_traffic_test)
set_tests_properties(routing_traffic_test PROPERTIES TIMEOUT 120)

# The part router traffic (10,000 blocks of keyboard traffic with the zone model changing every 3 blocks, determinism) without the sanitizer, so
# macOS runs its logic too; the RTSan build of the same file is below.
add_executable(part_router_traffic_test tests/rtsan_part_router_test.cpp)
target_link_libraries(part_router_traffic_test PRIVATE velora_core)
add_test(NAME part_router_traffic_test COMMAND part_router_traffic_test)
set_tests_properties(part_router_traffic_test PROPERTIES TIMEOUT 300)

# Keyswitch changes, controller sweeps and filtered / crossfaded regions beside dense traffic and generation changes (Story 5.2) without the sanitizer, so
# macOS runs its logic too (and checks determinism); the RTSan build is below.
# Story 6.5: voices over protected samples while an edge thread flips damage flags under them (the same without the sanitizer on macOS; the RTSan build is below).
add_executable(integrity_traffic_test tests/rtsan_integrity_test.cpp)
target_link_libraries(integrity_traffic_test PRIVATE velora_core)
add_test(NAME integrity_traffic_test COMMAND integrity_traffic_test)
set_tests_properties(integrity_traffic_test PROPERTIES TIMEOUT 300)
add_executable(sample_features_traffic_test tests/rtsan_sample_features_test.cpp)
target_link_libraries(sample_features_traffic_test PRIVATE velora_core)
add_test(NAME sample_features_traffic_test COMMAND sample_features_traffic_test)
set_tests_properties(sample_features_traffic_test PROPERTIES TIMEOUT 300)

# Encoder storms in ASSIGN mode beside dense traffic and generation changes (Story 4.10) without the sanitizer, so macOS runs its logic too; the RTSan build is below.
add_executable(assign_traffic_test tests/rtsan_assign_test.cpp)
target_link_libraries(assign_traffic_test PRIVATE velora_core)
add_test(NAME assign_traffic_test COMMAND assign_traffic_test)
set_tests_properties(assign_traffic_test PROPERTIES TIMEOUT 300)

# Arpeggiator and EventProcessor traffic across 8 parts beside generation changes (Story 7.2) without the sanitizer, so macOS runs its logic too; the RTSan build is below.
add_executable(arpeggiator_traffic_test tests/rtsan_arpeggiator_test.cpp)
target_link_libraries(arpeggiator_traffic_test PRIVATE velora_core)
add_test(NAME arpeggiator_traffic_test COMMAND arpeggiator_traffic_test)
set_tests_properties(arpeggiator_traffic_test PROPERTIES TIMEOUT 300)

# The cost benchmarks assert the declared cost units, which only means something on optimised code: they link a bench-private copy of the core sources built
# with -O3 -DNDEBUG (and are compiled -O2 themselves), so the assertions run in every configuration, the default unoptimised builds included.
add_library(velora_core_o2 STATIC ${VELORA_PARAMS_CORE_HPP} ${VELORA_CORE_SOURCES})
target_include_directories(velora_core_o2 PUBLIC include ${VELORA_GENERATED_DIR}/core)
target_link_libraries(velora_core_o2 PUBLIC velora_ports)
target_compile_options(velora_core_o2 PRIVATE -O3 -DNDEBUG)

# Host micro-benchmark (Story 2.2): cost per voice and per generator; host figures, not representative of the board.
add_executable(organ_bench tests/organ_bench.cpp)
target_link_libraries(organ_bench PRIVATE velora_core_o2)
target_compile_options(organ_bench PRIVATE -O3 -DNDEBUG)
add_test(NAME organ_bench COMMAND organ_bench)
set_tests_properties(organ_bench PROPERTIES PASS_REGULAR_EXPRESSION "host, not representative" FAIL_REGULAR_EXPRESSION "FAILED")

# Reverb host benchmark (Story 2.10): worst-case reverb cost against the declared cost units; host figures, not representative of the board.
add_executable(reverb_bench tests/reverb_bench.cpp)
target_link_libraries(reverb_bench PRIVATE velora_core_o2)
target_compile_options(reverb_bench PRIVATE -O3 -DNDEBUG)
add_test(NAME reverb_bench COMMAND reverb_bench)
set_tests_properties(reverb_bench PROPERTIES PASS_REGULAR_EXPRESSION "host, not representative" FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 120)

# Delay host benchmark (Story 2.11): worst-case delay cost against the declared cost units; host figures, not representative of the board.
add_executable(delay_bench tests/delay_bench.cpp)
target_link_libraries(delay_bench PRIVATE velora_core_o2)
target_compile_options(delay_bench PRIVATE -O3 -DNDEBUG)
add_test(NAME delay_bench COMMAND delay_bench)
set_tests_properties(delay_bench PROPERTIES PASS_REGULAR_EXPRESSION "host, not representative" FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 120)

# Sample engine voice cost (Story 5.2): filter and crossfade against the declared cost units; prints the figures, asserts nothing about time.
add_executable(sample_bench tests/sample_bench.cpp)
target_link_libraries(sample_bench PRIVATE velora_core_o2)
target_compile_options(sample_bench PRIVATE -O3 -DNDEBUG)
add_test(NAME sample_bench COMMAND sample_bench)
set_tests_properties(sample_bench PROPERTIES PASS_REGULAR_EXPRESSION "host, not representative" FAIL_REGULAR_EXPRESSION "FAILED" TIMEOUT 120)

# Timing tests must not compete with other tests for the cores (the cost ratios would be noise): they run alone.
set_tests_properties(organ_bench reverb_bench delay_bench sample_bench PROPERTIES RUN_SERIAL TRUE)

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
    add_executable(rtsan_integrity_test tests/rtsan_integrity_test.cpp)
    target_link_libraries(rtsan_integrity_test PRIVATE velora_core)
    target_compile_options(rtsan_integrity_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_integrity_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_integrity_test COMMAND rtsan_integrity_test)
    set_tests_properties(rtsan_integrity_test PROPERTIES TIMEOUT 300)
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
    add_executable(rtsan_part_router_test tests/rtsan_part_router_test.cpp)
    target_link_libraries(rtsan_part_router_test PRIVATE velora_core)
    target_compile_options(rtsan_part_router_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_part_router_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_part_router_test COMMAND rtsan_part_router_test)
    set_tests_properties(rtsan_part_router_test PROPERTIES TIMEOUT 300)
    add_executable(rtsan_sample_features_test tests/rtsan_sample_features_test.cpp)
    target_link_libraries(rtsan_sample_features_test PRIVATE velora_core)
    target_compile_options(rtsan_sample_features_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_sample_features_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_sample_features_test COMMAND rtsan_sample_features_test)
    set_tests_properties(rtsan_sample_features_test PROPERTIES TIMEOUT 300)
    add_executable(rtsan_assign_test tests/rtsan_assign_test.cpp)
    target_link_libraries(rtsan_assign_test PRIVATE velora_core)
    target_compile_options(rtsan_assign_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_assign_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_assign_test COMMAND rtsan_assign_test)
    set_tests_properties(rtsan_assign_test PROPERTIES TIMEOUT 300)
    add_executable(rtsan_arpeggiator_test tests/rtsan_arpeggiator_test.cpp)
    target_link_libraries(rtsan_arpeggiator_test PRIVATE velora_core)
    target_compile_options(rtsan_arpeggiator_test PRIVATE -fsanitize=realtime -g)
    target_link_options(rtsan_arpeggiator_test PRIVATE -fsanitize=realtime)
    add_test(NAME rtsan_arpeggiator_test COMMAND rtsan_arpeggiator_test)
    set_tests_properties(rtsan_arpeggiator_test PROPERTIES TIMEOUT 300)
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

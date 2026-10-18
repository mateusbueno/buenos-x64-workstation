add_library(velora_core STATIC
  src/version.cpp
  src/engine_registry.cpp
  src/test_tone_engine.cpp
  src/block_renderer.cpp)
add_library(velora::core ALIAS velora_core)
target_include_directories(velora_core PUBLIC include)
# Dependency rule: core may link only core and ports targets.
target_link_libraries(velora_core PUBLIC velora_ports)

add_executable(core_tests tests/core_test.cpp)
target_link_libraries(core_tests PRIVATE velora_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(engine_tests tests/engine_test.cpp)
target_link_libraries(engine_tests PRIVATE velora_core)
add_test(NAME engine_tests COMMAND engine_tests)

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

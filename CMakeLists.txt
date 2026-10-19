cmake_minimum_required(VERSION 3.22)
project(velora VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VELORA_RTSAN "Build RealtimeSanitizer tests (needs clang >= 20 with -fsanitize=realtime)" ON)

enable_testing()

add_subdirectory(proto)
add_subdirectory(mcu/common)
add_subdirectory(engine/ports)
add_subdirectory(engine/core)
add_subdirectory(engine/host)
add_subdirectory(formatlib)
add_subdirectory(librarian)
add_subdirectory(tools/offline-render)
add_subdirectory(tools/library-gen)
add_subdirectory(tools/spikes)

# Dependency-purity gate for engine/core (see tools/ci/check_core_deps.py).
if(DEFINED ENV{CI})
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
else()
  find_package(Python3 COMPONENTS Interpreter)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME core_deps_purity
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_core_deps.py
                   --root ${CMAKE_SOURCE_DIR})
  add_test(NAME core_deps_purity_selftest
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ci/check_core_deps.py
                   --self-test)
else()
  message(STATUS "Python3 not found: core purity tests not registered")
endif()

# Story 1.16: velora-ssd-qualify, the SSD read-latency qualification tool (page cache bypassed: O_DIRECT on Linux, F_NOCACHE on macOS) that recommends the
# instrument head length. Portable POSIX; the board run is the documented procedure in README.md (not run in CI).
add_library(velora_ssd_qualify STATIC ssd_qualify.cpp)
target_include_directories(velora_ssd_qualify PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(velora_ssd_qualify PUBLIC velora_host Threads::Threads)
target_compile_options(velora_ssd_qualify PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_executable(velora-ssd-qualify main.cpp)
target_link_libraries(velora-ssd-qualify PRIVATE velora_ssd_qualify)

add_executable(ssd_qualify_test tests/ssd_qualify_test.cpp)
target_link_libraries(ssd_qualify_test PRIVATE velora_ssd_qualify)
add_test(NAME ssd_qualify_test COMMAND ssd_qualify_test)
set_tests_properties(ssd_qualify_test PROPERTIES TIMEOUT 60)

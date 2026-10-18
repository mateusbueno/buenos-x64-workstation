add_library(velora_host STATIC
  src/host_info.cpp
  src/time_mapper.cpp
  src/midi_ingest.cpp
  src/render_driver.cpp
  src/posix_fs.cpp
  src/library_resolver.cpp)
add_library(velora::host ALIAS velora_host)
target_include_directories(velora_host PUBLIC include)
target_link_libraries(velora_host PUBLIC velora_core velora_ports velora_format)

add_executable(host_tests tests/host_test.cpp)
target_link_libraries(host_tests PRIVATE velora_host)
add_test(NAME host_tests COMMAND host_tests)

add_executable(host_unit_tests tests/host_unit_test.cpp)
target_link_libraries(host_unit_tests PRIVATE velora_host)
add_test(NAME host_unit_tests COMMAND host_unit_tests)

add_executable(library_resolver_test tests/library_resolver_test.cpp)
target_link_libraries(library_resolver_test PRIVATE velora_libgen)
add_test(NAME library_resolver_test COMMAND library_resolver_test)

# RealtimeSanitizer variant of the audio path (VELORA_HAVE_RTSAN is probed in engine/core).
if(VELORA_RTSAN AND VELORA_HAVE_RTSAN)
  add_executable(rtsan_host_render_test tests/rtsan_host_render_test.cpp)
  target_link_libraries(rtsan_host_render_test PRIVATE velora_host)
  target_compile_options(rtsan_host_render_test PRIVATE -fsanitize=realtime -g)
  target_link_options(rtsan_host_render_test PRIVATE -fsanitize=realtime)
  add_test(NAME rtsan_host_render_test COMMAND rtsan_host_render_test)
endif()

if(APPLE)
  add_subdirectory(macos)
endif()

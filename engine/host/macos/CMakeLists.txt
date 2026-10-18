# Apple-only adapters (CoreAudio output, CoreMIDI input) and the engine-host dev app.
find_library(CORE_AUDIO CoreAudio REQUIRED)
find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
find_library(CORE_MIDI CoreMIDI REQUIRED)
find_library(CORE_FOUNDATION CoreFoundation REQUIRED)

add_library(velora_host_macos STATIC coreaudio_output.cpp coremidi_input.cpp)
target_include_directories(velora_host_macos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velora_host_macos PUBLIC velora_host
  ${CORE_AUDIO} ${AUDIO_TOOLBOX} ${CORE_MIDI} ${CORE_FOUNDATION})

add_executable(engine-host main.cpp)
target_link_libraries(engine-host PRIVATE velora_host_macos)

add_executable(coremidi_virtual_test coremidi_virtual_test.cpp)
target_link_libraries(coremidi_virtual_test PRIVATE velora_host_macos)
add_test(NAME coremidi_virtual_test COMMAND coremidi_virtual_test)
set_tests_properties(coremidi_virtual_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

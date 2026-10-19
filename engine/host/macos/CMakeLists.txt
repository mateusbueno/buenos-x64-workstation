# Apple-only adapters (CoreAudio output, CoreMIDI input) and the engine-host dev app.
find_library(CORE_AUDIO CoreAudio REQUIRED)
find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
find_library(CORE_MIDI CoreMIDI REQUIRED)
find_library(CORE_FOUNDATION CoreFoundation REQUIRED)

add_library(velora_host_macos STATIC coreaudio_output.cpp coreaudio_line_in.cpp coremidi_input.cpp)
target_include_directories(velora_host_macos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velora_host_macos PUBLIC velora_host
  ${CORE_AUDIO} ${AUDIO_TOOLBOX} ${CORE_MIDI} ${CORE_FOUNDATION})

add_executable(engine-host main.cpp)
target_link_libraries(engine-host PRIVATE velora_host_macos velora_control)
target_compile_definitions(engine-host PRIVATE VELORA_DEV_CONFIG_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../dev/engine-dev.json")

add_executable(coremidi_virtual_test coremidi_virtual_test.cpp)
target_link_libraries(coremidi_virtual_test PRIVATE velora_host_macos)
add_test(NAME coremidi_virtual_test COMMAND coremidi_virtual_test)
set_tests_properties(coremidi_virtual_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

# engine-host wiring smoke test (Story 4.4): a subprocess loads the scenes fixture and a ws.v1 client recalls a scene. Runs engine-host --no-audio.
add_executable(engine_host_scene_smoke engine_host_scene_smoke.cpp)
target_link_libraries(engine_host_scene_smoke PRIVATE velora_control)
add_dependencies(engine_host_scene_smoke engine-host)
set(_ehs_dir ${CMAKE_SOURCE_DIR}/tools/offline-render/tests/live_set)
add_test(NAME engine_host_scene_smoke COMMAND engine_host_scene_smoke $<TARGET_FILE:engine-host> ${_ehs_dir}/scenes.liveset ${_ehs_dir}/root)
set_tests_properties(engine_host_scene_smoke PROPERTIES TIMEOUT 60)

# engine-host switching smoke test (Story 4.5): a subprocess with --live-set-dir renders audio while a ws.v1 client jumps to the other Live Set (seamless
# switch, background load, loading status). Runs engine-host --no-audio.
add_executable(engine_host_switch_smoke engine_host_switch_smoke.cpp)
target_link_libraries(engine_host_switch_smoke PRIVATE velora_control)
add_dependencies(engine_host_switch_smoke engine-host)
add_test(NAME engine_host_switch_smoke COMMAND engine_host_switch_smoke $<TARGET_FILE:engine-host> ${_ehs_dir}/switch ${_ehs_dir}/root b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71)
set_tests_properties(engine_host_switch_smoke PROPERTIES TIMEOUT 90)

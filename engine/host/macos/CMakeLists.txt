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
target_link_libraries(engine-host PRIVATE velora_host_macos velora_control velora_library_client)
target_compile_definitions(engine-host PRIVATE VELORA_DEV_CONFIG_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../dev/engine-dev.json")
# Story 6.5: protected libraries through the dev key provider (--trust-list, --dev-device-key). Dev builds only (see engine/host/CMakeLists.txt).
if(VELORA_DEV_KEY_PROVIDER)
  target_link_libraries(engine-host PRIVATE velora_host_protected velora_host_dev_keys)
  target_compile_definitions(engine-host PRIVATE VELORA_ENGINE_DEV_KEYS=1)
endif()

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

# engine-host setlist smoke test (Story 4.6): --data-dir, ws.v1 setlist commands, LIVE SET stepping through the real switcher, persistence across a restart,
# single-writer refusal of a second engine. Runs engine-host --no-audio.
add_executable(engine_host_setlist_smoke engine_host_setlist_smoke.cpp)
target_link_libraries(engine_host_setlist_smoke PRIVATE velora_control)
add_dependencies(engine_host_setlist_smoke engine-host)
add_test(NAME engine_host_setlist_smoke COMMAND engine_host_setlist_smoke $<TARGET_FILE:engine-host> ${_ehs_dir}/switch ${_ehs_dir}/root b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71)
set_tests_properties(engine_host_setlist_smoke PROPERTIES TIMEOUT 120)

# engine-host librarian wiring smoke test (Story 4.11): `--library-socket` reports the sounding Live Set to the real librarian-service, which comes up after the engine.
add_executable(engine_host_library_smoke engine_host_library_smoke.cpp)
target_link_libraries(engine_host_library_smoke PRIVATE velora_control velora_library_api)
add_dependencies(engine_host_library_smoke engine-host librarian-service)
add_test(NAME engine_host_library_smoke COMMAND engine_host_library_smoke $<TARGET_FILE:engine-host> $<TARGET_FILE:librarian-service> ${_ehs_dir}/switch ${_ehs_dir}/root b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71)
set_tests_properties(engine_host_library_smoke PROPERTIES TIMEOUT 120)

# engine-host edit smoke test (Story 4.9): ws.v1 edits, the dirty flag, Write (the fence is acknowledged by the blocks engine-host renders while it waits),
# no autosave across a restart, Save as new. Runs engine-host --no-audio.
add_executable(engine_host_edit_smoke engine_host_edit_smoke.cpp)
target_link_libraries(engine_host_edit_smoke PRIVATE velora_control)
add_dependencies(engine_host_edit_smoke engine-host)
add_test(NAME engine_host_edit_smoke COMMAND engine_host_edit_smoke $<TARGET_FILE:engine-host> ${_ehs_dir}/switch ${_ehs_dir}/root)
set_tests_properties(engine_host_edit_smoke PROPERTIES TIMEOUT 120)

# engine-host ASSIGN smoke test (Story 4.10): SetAssignSlot on the current Live Set (dirty, Write, restart) and on the global default (written at once), typed
# refusals, SetEncoderMode / SelectPart reaching the real-time plane. Runs engine-host --no-audio.
add_executable(engine_host_assign_smoke engine_host_assign_smoke.cpp)
target_link_libraries(engine_host_assign_smoke PRIVATE velora_control)
add_dependencies(engine_host_assign_smoke engine-host)
add_test(NAME engine_host_assign_smoke COMMAND engine_host_assign_smoke $<TARGET_FILE:engine-host> ${_ehs_dir}/switch ${_ehs_dir}/root)
set_tests_properties(engine_host_assign_smoke PROPERTIES TIMEOUT 120)

# engine-host protected-library smoke test (Story 6.5): a subprocess with --trust-list / --dev-device-key plays a PROTECTED library laid out as the librarian installs it,
# refuses another device's key with the typed cause, and reports a block damaged on disk through State.content (found by the I/O edge thread). --no-audio.
if(VELORA_DEV_KEY_PROVIDER)
  add_executable(engine_host_protected_smoke engine_host_protected_smoke.cpp)
  target_link_libraries(engine_host_protected_smoke PRIVATE velora_control velora_format_protected_writer OpenSSL::Crypto)
  add_dependencies(engine_host_protected_smoke engine-host)
  add_test(NAME engine_host_protected_smoke COMMAND engine_host_protected_smoke $<TARGET_FILE:engine-host>)
  set_tests_properties(engine_host_protected_smoke PROPERTIES TIMEOUT 120)

  # Story 6.9: the first-boot import of the signed factory Live Sets by the real engine-host (--factory-live-sets): imported once, idempotent, a deleted one stays deleted, a missing
  # library or an untrusted signer refused with the typed cause and nothing written. --no-audio.
  add_executable(engine_host_factory_smoke engine_host_factory_smoke.cpp)
  target_link_libraries(engine_host_factory_smoke PRIVATE velora_control velora_format_protected_writer OpenSSL::Crypto)
  add_dependencies(engine_host_factory_smoke engine-host)
  add_test(NAME engine_host_factory_smoke COMMAND engine_host_factory_smoke $<TARGET_FILE:engine-host>)
  set_tests_properties(engine_host_factory_smoke PROPERTIES TIMEOUT 180)
endif()

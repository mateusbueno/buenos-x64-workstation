add_library(velora_offline STATIC
  src/smf.cpp
  src/events_file.cpp
  src/wav.cpp
  src/render.cpp
  src/sfizz_random_replay.cpp
  src/render_live_set.cpp
  src/nulltest.cpp
  src/gap_detector.cpp)
target_include_directories(velora_offline PUBLIC src)
# The sample engine renders from a bundle: the loader lives in engine/host (it links formatlib).
# Live Set rendering loads the Live Set through the control side (protobuf), the same loader as engine-host.
target_link_libraries(velora_offline PUBLIC velora_core velora_host velora_control velora_format)

add_executable(velora-gap-detector src/gap_detector_main.cpp)
target_link_libraries(velora-gap-detector PRIVATE velora_offline)

add_executable(velora-offline-render src/render_main.cpp)
target_link_libraries(velora-offline-render PRIVATE velora_offline)
# Story 6.5: protected bundles through the dev key provider (--dev-device-key, --wrapped-key, --trust-list). Dev builds only: the target does not exist in a
# configuration that refuses VELORA_DEV_KEY_PROVIDER (engine/host/CMakeLists.txt), and without it the flags answer an error.
if(VELORA_DEV_KEY_PROVIDER)
  target_link_libraries(velora-offline-render PRIVATE velora_host_protected velora_host_dev_keys)
  target_compile_definitions(velora-offline-render PRIVATE VELORA_OFFLINE_DEV_KEYS=1)

  # Story 6.5: a protected and an unprotected bundle of the same library render the same MIDI suite bit-identically; typed refusals; no plaintext on disk.
  add_executable(protected_render_test tests/protected_render_test.cpp)
  target_link_libraries(protected_render_test PRIVATE velora_offline velora_host_protected velora_host_dev_keys velora_format_protected_writer OpenSSL::Crypto)
  target_compile_definitions(protected_render_test PRIVATE
    RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
    OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  add_dependencies(protected_render_test velora-offline-render)
  add_test(NAME protected_render_test COMMAND protected_render_test)
  set_tests_properties(protected_render_test PROPERTIES TIMEOUT 600)

  # The whole sample-engine suite (every case's SFZ and MIDI) protected vs unprotected, bit-identical.
  add_executable(protected_suite_test tests/protected_suite_test.cpp)
  target_link_libraries(protected_suite_test PRIVATE velora_librarian velora_format_protected_writer OpenSSL::Crypto)
  target_compile_definitions(protected_suite_test PRIVATE
    SUITE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sample_suite"
    RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
    OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  add_dependencies(protected_suite_test velora-offline-render)
  add_test(NAME protected_suite_test COMMAND protected_suite_test)
  set_tests_properties(protected_suite_test PROPERTIES TIMEOUT 900)
endif()

add_executable(velora-null-test src/null_test_main.cpp)
target_link_libraries(velora-null-test PRIVATE velora_offline)

# --- Live Set golden fixtures (Story 4.3): the generator is built so the committed fixtures can be regenerated on purpose ------
add_executable(live-set-fixture-gen tests/live_set/make_fixtures.cpp)
target_link_libraries(live-set-fixture-gen PRIVATE velora_offline)

# --- FM suite fixtures (Story 12.2): generates .syx and .mid test fixtures ---
add_executable(fm-fixture-gen tests/fm_suite/make_fixtures.cpp)
target_link_libraries(fm-fixture-gen PRIVATE velora_offline)

# --- unit tests --------------------------------------------------------------
add_executable(offline_render_tests tests/offline_render_tests.cpp)
target_link_libraries(offline_render_tests PRIVATE velora_offline)
target_compile_definitions(offline_render_tests PRIVATE
  OFFLINE_RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(offline_render_tests velora-offline-render)
add_test(NAME offline_render_tests COMMAND offline_render_tests)

# --- Story 13.10: sfizz random-number replay (seed inverse, draw sequence, layer parity with the engine) ---
add_executable(sfizz_random_replay_test tests/sfizz_random_replay_test.cpp)
target_link_libraries(sfizz_random_replay_test PRIVATE velora_offline)
add_test(NAME sfizz_random_replay_test COMMAND sfizz_random_replay_test)

# --- Live Set renderer: raw controller pass-through to the sample parts (Story 5.2) ---
add_executable(live_set_cc_test tests/live_set_cc_test.cpp)
target_link_libraries(live_set_cc_test PRIVATE velora_offline)
target_compile_definitions(live_set_cc_test PRIVATE OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME live_set_cc_test COMMAND live_set_cc_test)

# --- Story 3.11: half-pedal damping of a sample instrument through the Live Set renderer (name matches "pedal") ---
add_executable(pedal_half_pedal_test tests/pedal_half_pedal_test.cpp)
target_link_libraries(pedal_half_pedal_test PRIVATE velora_offline)
target_compile_definitions(pedal_half_pedal_test PRIVATE OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME pedal_half_pedal_test COMMAND pedal_half_pedal_test)

# --- Story 1.18: parallel voice render, 1 vs 3 worker threads bit-identical in the offline renderer (name matches "parallel") ---
add_executable(parallel_voices_test tests/parallel_voices_test.cpp)
target_link_libraries(parallel_voices_test PRIVATE velora_offline)
add_test(NAME parallel_voices_test COMMAND parallel_voices_test)
set_tests_properties(parallel_voices_test PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

# --- Seamless switching render suite (Story 4.8, NFR14, FR79..FR82, AR2) ---
add_executable(seamless_switching_suite tests/seamless_switching_suite_test.cpp)
target_link_libraries(seamless_switching_suite PRIVATE velora_offline)
target_compile_definitions(seamless_switching_suite PRIVATE
  SEAMLESS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/live_set")
add_test(NAME seamless_switching_suite COMMAND seamless_switching_suite)

# --- routing (Story 2.7): Main/Aux, sends, line-in and master verified by null tests against analytically mixed references ---
add_executable(offline_routing_tests tests/routing_test.cpp)
target_link_libraries(offline_routing_tests PRIVATE velora_offline)
target_compile_definitions(offline_routing_tests PRIVATE
  OFFLINE_RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  ROUTING_TEST_MIDI="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/golden.mid")
add_dependencies(offline_routing_tests velora-offline-render)
add_test(NAME offline_routing_tests COMMAND offline_routing_tests)

# --- golden pipeline ---------------------------------------------------------
set(_golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden)
set(_out ${CMAKE_CURRENT_BINARY_DIR})
set(_render_args --midi ${_golden}/golden.mid --tail-seconds 0.25)

# Stale outputs must not satisfy later tests: remove them first (fixture "wav_clean").
add_test(NAME offline_render_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/run_a.wav ${_out}/run_b.wav ${_out}/detuned.wav)
set_tests_properties(offline_render_clean PROPERTIES FIXTURES_SETUP wav_clean)

# Two renders of the same input are byte-identical.
add_test(NAME offline_render_run_a COMMAND velora-offline-render ${_render_args} --out ${_out}/run_a.wav)
add_test(NAME offline_render_run_b COMMAND velora-offline-render ${_render_args} --out ${_out}/run_b.wav)
add_test(NAME offline_render_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/run_a.wav ${_out}/run_b.wav)
set_tests_properties(offline_render_run_a PROPERTIES FIXTURES_REQUIRED wav_clean FIXTURES_SETUP wav_a)
set_tests_properties(offline_render_run_b PROPERTIES FIXTURES_REQUIRED wav_clean FIXTURES_SETUP wav_b)
set_tests_properties(offline_render_bit_identical PROPERTIES FIXTURES_REQUIRED "wav_a;wav_b")

# A fresh render passes the null test against the committed golden.
add_test(NAME null_test_golden
         COMMAND velora-null-test --case ${_golden}/golden.case --candidate ${_out}/run_a.wav)
set_tests_properties(null_test_golden PROPERTIES FIXTURES_REQUIRED wav_a
                     PASS_REGULAR_EXPRESSION "overall: PASS")

# A deliberately detuned render must fail the same null test on residual (not on any other error).
add_test(NAME offline_render_detuned
         COMMAND velora-offline-render ${_render_args} --detune-cents 20 --out ${_out}/detuned.wav)
set_tests_properties(offline_render_detuned PROPERTIES FIXTURES_REQUIRED wav_clean FIXTURES_SETUP wav_d)
add_test(NAME null_test_detuned_fails
         COMMAND velora-null-test --case ${_golden}/golden.case --candidate ${_out}/detuned.wav)
set_tests_properties(null_test_detuned_fails PROPERTIES FIXTURES_REQUIRED wav_d
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")

# Strict argument parsing (message must come from the parser).
add_test(NAME null_test_rejects_bad_tolerance
         COMMAND velora-null-test --reference ${_golden}/golden.wav --candidate ${_golden}/golden.wav
                 --tolerance-db inf --max-offset-frames 0)
set_tests_properties(null_test_rejects_bad_tolerance PROPERTIES
                     PASS_REGULAR_EXPRESSION "invalid value for --tolerance-db")
add_test(NAME null_test_candidate_quantize_needs_reference_decode
         COMMAND velora-null-test --reference ${_golden}/golden.wav --candidate ${_golden}/golden.wav
                 --tolerance-db -60 --max-offset-frames 0 --candidate-quantize sfizz-s16)
set_tests_properties(null_test_candidate_quantize_needs_reference_decode PROPERTIES
                     PASS_REGULAR_EXPRESSION "invalid value for --candidate_quantize|candidate_quantize needs")
add_test(NAME null_test_rejects_unknown_case_key
         COMMAND velora-null-test --case ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/unknown_key.case
                 --candidate ${_golden}/golden.wav)
set_tests_properties(null_test_rejects_unknown_case_key PROPERTIES
                     PASS_REGULAR_EXPRESSION "unknown key 'bogus_key'")
add_test(NAME offline_render_rejects_bad_tail
         COMMAND velora-offline-render --midi ${_golden}/golden.mid --out ${_out}/never.wav --tail-seconds nan)
set_tests_properties(offline_render_rejects_bad_tail PROPERTIES
                     PASS_REGULAR_EXPRESSION "invalid --tail-seconds")

# Organ engine (Story 2.2): deterministic offline renders with drawbar settings.
add_test(NAME offline_render_organ_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/organ_a.wav ${_out}/organ_b.wav ${_out}/organ_alt.wav ${_out}/organ_zero.wav)
set_tests_properties(offline_render_organ_clean PROPERTIES FIXTURES_SETUP organ_clean)
add_test(NAME offline_render_organ_a
         COMMAND velora-offline-render ${_render_args} --engine organ --drawbars 888000000 --out ${_out}/organ_a.wav)
add_test(NAME offline_render_organ_b
         COMMAND velora-offline-render ${_render_args} --engine organ --drawbars 888000000 --out ${_out}/organ_b.wav)
add_test(NAME offline_render_organ_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/organ_a.wav ${_out}/organ_b.wav)
set_tests_properties(offline_render_organ_a PROPERTIES FIXTURES_REQUIRED organ_clean FIXTURES_SETUP organ_a)
set_tests_properties(offline_render_organ_b PROPERTIES FIXTURES_REQUIRED organ_clean FIXTURES_SETUP organ_b)
set_tests_properties(offline_render_organ_bit_identical PROPERTIES FIXTURES_REQUIRED "organ_a;organ_b")
add_test(NAME offline_render_organ_rejects_bad_drawbars
         COMMAND velora-offline-render ${_render_args} --engine organ --drawbars 9880000 --out ${_out}/never.wav)
set_tests_properties(offline_render_organ_rejects_bad_drawbars PROPERTIES
                     PASS_REGULAR_EXPRESSION "invalid --drawbars")
add_test(NAME offline_render_drawbars_need_organ
         COMMAND velora-offline-render ${_render_args} --drawbars 888000000 --out ${_out}/never.wav)
set_tests_properties(offline_render_drawbars_need_organ PROPERTIES
                     PASS_REGULAR_EXPRESSION "--drawbars needs --engine organ")

add_test(NAME offline_render_organ_alt
         COMMAND velora-offline-render ${_render_args} --engine organ --drawbars 008000000 --out ${_out}/organ_alt.wav)
add_test(NAME offline_render_organ_zero
         COMMAND velora-offline-render ${_render_args} --engine organ --drawbars 000000000 --out ${_out}/organ_zero.wav)
set_tests_properties(offline_render_organ_alt offline_render_organ_zero PROPERTIES FIXTURES_REQUIRED organ_clean FIXTURES_SETUP organ_other)
add_executable(organ_wav_check tests/organ_wav_check.cpp)
target_link_libraries(organ_wav_check PRIVATE velora_offline)
add_test(NAME offline_render_organ_content
         COMMAND organ_wav_check ${_out}/organ_a.wav ${_out}/organ_alt.wav ${_out}/organ_zero.wav)
set_tests_properties(offline_render_organ_content PROPERTIES FIXTURES_REQUIRED "organ_a;organ_other"
                     PASS_REGULAR_EXPRESSION "organ_wav_check: ok")

# Organ percussion and key click (Story 2.3): a committed MIDI file exercises both harmonics, both decays, both volumes,
# staccato and legato playing, and key click; the render is deterministic (run twice, byte-equal) and the fresh render
# null-tests against the committed golden at a very tight tolerance (not byte equality: libm may differ between platforms).
set(_perc ${CMAKE_CURRENT_SOURCE_DIR}/tests/organ_percussion)
set(_perc_args --midi ${_perc}/organ_percussion.mid --engine organ --percussion off --key-click 0.4 --leakage 0 --tail-seconds 0.3)
add_test(NAME organ_perc_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/perc_a.wav ${_out}/perc_b.wav ${_out}/perc_wrong_click.wav ${_out}/perc_wrong_db.wav)
set_tests_properties(organ_perc_clean PROPERTIES FIXTURES_SETUP perc_clean)
add_test(NAME organ_perc_render_a COMMAND velora-offline-render ${_perc_args} --out ${_out}/perc_a.wav)
add_test(NAME organ_perc_render_b COMMAND velora-offline-render ${_perc_args} --out ${_out}/perc_b.wav)
set_tests_properties(organ_perc_render_a PROPERTIES FIXTURES_REQUIRED perc_clean FIXTURES_SETUP perc_a)
set_tests_properties(organ_perc_render_b PROPERTIES FIXTURES_REQUIRED perc_clean FIXTURES_SETUP perc_b)
add_test(NAME organ_perc_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/perc_a.wav ${_out}/perc_b.wav)
set_tests_properties(organ_perc_bit_identical PROPERTIES FIXTURES_REQUIRED "perc_a;perc_b")
add_test(NAME organ_perc_null_golden
         COMMAND velora-null-test --case ${_perc}/organ_percussion.case --candidate ${_out}/perc_a.wav)
set_tests_properties(organ_perc_null_golden PROPERTIES FIXTURES_REQUIRED perc_a PASS_REGULAR_EXPRESSION "overall: PASS")
# The golden is sensitive to what it covers: a different key click level or a different registration fails the null test.
add_test(NAME organ_perc_wrong_click
         COMMAND velora-offline-render --midi ${_perc}/organ_percussion.mid --engine organ --percussion off --key-click 0.8
                 --leakage 0 --tail-seconds 0.3 --out ${_out}/perc_wrong_click.wav)
add_test(NAME organ_perc_wrong_drawbars
         COMMAND velora-offline-render ${_perc_args} --drawbars 008000001 --out ${_out}/perc_wrong_db.wav)
set_tests_properties(organ_perc_wrong_click PROPERTIES FIXTURES_REQUIRED perc_clean FIXTURES_SETUP perc_wc)
set_tests_properties(organ_perc_wrong_drawbars PROPERTIES FIXTURES_REQUIRED perc_clean FIXTURES_SETUP perc_wd)
add_test(NAME organ_perc_wrong_click_fails
         COMMAND velora-null-test --case ${_perc}/organ_percussion.case --candidate ${_out}/perc_wrong_click.wav)
add_test(NAME organ_perc_wrong_drawbars_fails
         COMMAND velora-null-test --case ${_perc}/organ_percussion.case --candidate ${_out}/perc_wrong_db.wav)
set_tests_properties(organ_perc_wrong_click_fails PROPERTIES FIXTURES_REQUIRED perc_wc
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
set_tests_properties(organ_perc_wrong_drawbars_fails PROPERTIES FIXTURES_REQUIRED perc_wd
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# Reverb send (Story 2.10): the golden test-tone MIDI with part 0 sending to the reverb and a 1 s tail, one golden per reverb type. Same scheme as
# the other goldens: render twice (byte-equal), null-test against the committed golden at -100 dB, and require a wrong setting (type, time,
# return level, REV master send level, part send level) to fail it. Regenerate after an intentional change with the command in the README.
set(_rv_args --midi ${_golden}/golden.mid --tail-seconds 1.0 --send 0:0.7)
foreach(_rv room:0.9 hall:1.4 plate:1.1)
  string(REPLACE ":" ";" _rvl "${_rv}")
  list(GET _rvl 0 _ty)
  list(GET _rvl 1 _tm)
  set(_d ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_reverb_${_ty})
  set(_rv_good ${_rv_args} --reverb ${_ty} --reverb-time ${_tm} --reverb-level 0.8 --rev-master 0.9)
  if(_ty STREQUAL "plate")
    set(_other room)
  else()
    set(_other plate)
  endif()
  set(_rv_wrong_type ${_rv_args} --reverb ${_other} --reverb-time ${_tm} --reverb-level 0.8 --rev-master 0.9)
  set(_rv_wrong_time ${_rv_args} --reverb ${_ty} --reverb-time 2.2 --reverb-level 0.8 --rev-master 0.9)
  set(_rv_wrong_level ${_rv_args} --reverb ${_ty} --reverb-time ${_tm} --reverb-level 0.7 --rev-master 0.9)
  set(_rv_wrong_master ${_rv_args} --reverb ${_ty} --reverb-time ${_tm} --reverb-level 0.8 --rev-master 1.0)
  set(_rv_wrong_send --midi ${_golden}/golden.mid --tail-seconds 1.0 --send 0:0.6 --reverb ${_ty} --reverb-time ${_tm} --reverb-level 0.8 --rev-master 0.9)
  add_test(NAME fx_reverb_${_ty}_clean
           COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/fx_reverb_${_ty}_a.wav ${_out}/fx_reverb_${_ty}_b.wav ${_out}/fx_reverb_${_ty}_wt.wav
                   ${_out}/fx_reverb_${_ty}_wi.wav ${_out}/fx_reverb_${_ty}_wl.wav ${_out}/fx_reverb_${_ty}_wm.wav ${_out}/fx_reverb_${_ty}_ws.wav)
  set_tests_properties(fx_reverb_${_ty}_clean PROPERTIES FIXTURES_SETUP fx_reverb_${_ty}_clean)
  add_test(NAME fx_reverb_${_ty}_render_a COMMAND velora-offline-render ${_rv_good} --out ${_out}/fx_reverb_${_ty}_a.wav)
  add_test(NAME fx_reverb_${_ty}_render_b COMMAND velora-offline-render ${_rv_good} --out ${_out}/fx_reverb_${_ty}_b.wav)
  set_tests_properties(fx_reverb_${_ty}_render_a PROPERTIES FIXTURES_REQUIRED fx_reverb_${_ty}_clean FIXTURES_SETUP fx_reverb_${_ty}_a)
  set_tests_properties(fx_reverb_${_ty}_render_b PROPERTIES FIXTURES_REQUIRED fx_reverb_${_ty}_clean FIXTURES_SETUP fx_reverb_${_ty}_b)
  add_test(NAME fx_reverb_${_ty}_bit_identical
           COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/fx_reverb_${_ty}_a.wav ${_out}/fx_reverb_${_ty}_b.wav)
  set_tests_properties(fx_reverb_${_ty}_bit_identical PROPERTIES FIXTURES_REQUIRED "fx_reverb_${_ty}_a;fx_reverb_${_ty}_b")
  add_test(NAME fx_reverb_${_ty}_null_golden
           COMMAND velora-null-test --case ${_d}/fx_reverb_${_ty}.case --candidate ${_out}/fx_reverb_${_ty}_a.wav)
  set_tests_properties(fx_reverb_${_ty}_null_golden PROPERTIES FIXTURES_REQUIRED fx_reverb_${_ty}_a PASS_REGULAR_EXPRESSION "overall: PASS")
  foreach(_w type:wt time:wi level:wl master:wm send:ws)
    string(REPLACE ":" ";" _wl "${_w}")
    list(GET _wl 0 _wn)
    list(GET _wl 1 _wf)
    add_test(NAME fx_reverb_${_ty}_wrong_${_wn} COMMAND velora-offline-render ${_rv_wrong_${_wn}} --out ${_out}/fx_reverb_${_ty}_${_wf}.wav)
    set_tests_properties(fx_reverb_${_ty}_wrong_${_wn} PROPERTIES FIXTURES_REQUIRED fx_reverb_${_ty}_clean FIXTURES_SETUP fx_reverb_${_ty}_${_wf})
    add_test(NAME fx_reverb_${_ty}_wrong_${_wn}_fails
             COMMAND velora-null-test --case ${_d}/fx_reverb_${_ty}.case --candidate ${_out}/fx_reverb_${_ty}_${_wf}.wav)
    set_tests_properties(fx_reverb_${_ty}_wrong_${_wn}_fails PROPERTIES FIXTURES_REQUIRED fx_reverb_${_ty}_${_wf}
                         PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
  endforeach()
endforeach()
# Regenerate the reverb goldens: cmake --build build --target update-reverb-goldens (review the diff before commit).
add_custom_target(update-reverb-goldens
  COMMAND velora-offline-render --midi ${_golden}/golden.mid --tail-seconds 1.0 --send 0:0.7 --reverb room --reverb-time 0.9 --reverb-level 0.8 --rev-master 0.9
          --out ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_reverb_room/fx_reverb_room.wav
  COMMAND velora-offline-render --midi ${_golden}/golden.mid --tail-seconds 1.0 --send 0:0.7 --reverb hall --reverb-time 1.4 --reverb-level 0.8 --rev-master 0.9
          --out ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_reverb_hall/fx_reverb_hall.wav
  COMMAND velora-offline-render --midi ${_golden}/golden.mid --tail-seconds 1.0 --send 0:0.7 --reverb plate --reverb-time 1.1 --reverb-level 0.8 --rev-master 0.9
          --out ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_reverb_plate/fx_reverb_plate.wav)
foreach(_dup "--reverb;room;hall" "--reverb-time;1;2" "--reverb-level;0.5;0.6" "--rev-master;0.5;0.6")
  list(GET _dup 0 _opt)
  list(GET _dup 1 _v1)
  list(GET _dup 2 _v2)
  string(MAKE_C_IDENTIFIER "${_opt}" _id)
  add_test(NAME fx_rejects_duplicate${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_v1} ${_opt} ${_v2} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_duplicate${_id} PROPERTIES PASS_REGULAR_EXPRESSION "duplicate ${_opt}")
endforeach()
foreach(_bad "--reverb;cave" "--reverb-time;11" "--reverb-time;0.1" "--reverb-level;2" "--rev-master;-1")
  list(GET _bad 0 _opt)
  list(GET _bad 1 _val)
  string(MAKE_C_IDENTIFIER "${_opt}_${_val}" _id)
  add_test(NAME fx_rejects_${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_val} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid ${_opt}")
endforeach()

# Delay send (Story 2.11): the golden test-tone MIDI with part 0 sending to the delay and a 1.5 s tail, one golden in milliseconds mode (with damping and
# ping-pong) and one tempo-synced (dotted eighth) with a tempo change during the render (option-driven: --tempo-change SECONDS:BPM sends the tempo_bpm
# performance event, the RT plane's single tempo value). Same scheme as the other goldens: render twice (byte-equal), null-test against the committed golden
# at -100 dB, and require a wrong setting to fail it. Regenerate after an intentional change with the command in the README.
set(_dl_base --midi ${_golden}/golden.mid --tail-seconds 1.5)
set(_dl_ms_good ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_ms_wrong_time ${_dl_base} --send 0:0:0.7 --delay-time 232 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_ms_wrong_feedback ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-feedback 0.45 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_ms_wrong_level ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-feedback 0.5 --delay-level 0.7 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_ms_wrong_damping ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.5 --delay-pingpong 0.5)
set(_dl_ms_wrong_pingpong ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.4)
set(_dl_ms_wrong_send ${_dl_base} --send 0:0:0.6 --delay-time 230 --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_ms_wrong_sync ${_dl_base} --send 0:0:0.7 --delay-time 230 --delay-sync on --delay-feedback 0.5 --delay-level 0.8 --delay-damping 0.4 --delay-pingpong 0.5)
set(_dl_sync_common --send 0:0:0.7 --delay-sync on --delay-feedback 0.55 --delay-level 0.8 --delay-damping 0.3)
set(_dl_sync_good ${_dl_base} ${_dl_sync_common} --delay-note 1/8d --tempo 100 --tempo-change 0.5:140)
set(_dl_sync_wrong_note ${_dl_base} ${_dl_sync_common} --delay-note 1/8t --tempo 100 --tempo-change 0.5:140)
set(_dl_sync_wrong_tempo ${_dl_base} ${_dl_sync_common} --delay-note 1/8d --tempo 110 --tempo-change 0.5:140)
set(_dl_sync_wrong_change ${_dl_base} ${_dl_sync_common} --delay-note 1/8d --tempo 100 --tempo-change 0.5:130)
set(_dl_sync_wrong_when ${_dl_base} ${_dl_sync_common} --delay-note 1/8d --tempo 100 --tempo-change 0.6:140)
set(_dl_sync_wrong_nochange ${_dl_base} ${_dl_sync_common} --delay-note 1/8d --tempo 100)
set(_dl_sync_wrong_syncoff ${_dl_base} --send 0:0:0.7 --delay-note 1/8d --tempo 100 --tempo-change 0.5:140 --delay-feedback 0.55 --delay-level 0.8 --delay-damping 0.3)
foreach(_dl "ms;time:wt;feedback:wf;level:wl;damping:wd;pingpong:wp;send:ws;sync:wy" "sync;note:wn;tempo:wt;change:wc;when:ww;nochange:wx;syncoff:wo")
  list(GET _dl 0 _dn)
  list(REMOVE_AT _dl 0)
  set(_d ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_delay_${_dn})
  set(_pre fx_delay_${_dn})
  add_test(NAME ${_pre}_clean
           COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/${_pre}_a.wav ${_out}/${_pre}_b.wav ${_out}/${_pre}_wt.wav ${_out}/${_pre}_wf.wav ${_out}/${_pre}_wl.wav
                   ${_out}/${_pre}_wd.wav ${_out}/${_pre}_wp.wav ${_out}/${_pre}_ws.wav ${_out}/${_pre}_wy.wav ${_out}/${_pre}_wn.wav ${_out}/${_pre}_wc.wav
                   ${_out}/${_pre}_ww.wav ${_out}/${_pre}_wx.wav ${_out}/${_pre}_wo.wav)
  set_tests_properties(${_pre}_clean PROPERTIES FIXTURES_SETUP ${_pre}_clean)
  add_test(NAME ${_pre}_render_a COMMAND velora-offline-render ${_dl_${_dn}_good} --out ${_out}/${_pre}_a.wav)
  add_test(NAME ${_pre}_render_b COMMAND velora-offline-render ${_dl_${_dn}_good} --out ${_out}/${_pre}_b.wav)
  set_tests_properties(${_pre}_render_a PROPERTIES FIXTURES_REQUIRED ${_pre}_clean FIXTURES_SETUP ${_pre}_a)
  set_tests_properties(${_pre}_render_b PROPERTIES FIXTURES_REQUIRED ${_pre}_clean FIXTURES_SETUP ${_pre}_b)
  add_test(NAME ${_pre}_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/${_pre}_a.wav ${_out}/${_pre}_b.wav)
  set_tests_properties(${_pre}_bit_identical PROPERTIES FIXTURES_REQUIRED "${_pre}_a;${_pre}_b")
  add_test(NAME ${_pre}_null_golden COMMAND velora-null-test --case ${_d}/${_pre}.case --candidate ${_out}/${_pre}_a.wav)
  set_tests_properties(${_pre}_null_golden PROPERTIES FIXTURES_REQUIRED ${_pre}_a PASS_REGULAR_EXPRESSION "overall: PASS")
  foreach(_w ${_dl})
    string(REPLACE ":" ";" _wl "${_w}")
    list(GET _wl 0 _wn)
    list(GET _wl 1 _wf)
    add_test(NAME ${_pre}_wrong_${_wn} COMMAND velora-offline-render ${_dl_${_dn}_wrong_${_wn}} --out ${_out}/${_pre}_${_wf}.wav)
    set_tests_properties(${_pre}_wrong_${_wn} PROPERTIES FIXTURES_REQUIRED ${_pre}_clean FIXTURES_SETUP ${_pre}_${_wf})
    add_test(NAME ${_pre}_wrong_${_wn}_fails COMMAND velora-null-test --case ${_d}/${_pre}.case --candidate ${_out}/${_pre}_${_wf}.wav)
    set_tests_properties(${_pre}_wrong_${_wn}_fails PROPERTIES FIXTURES_REQUIRED ${_pre}_${_wf} PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
  endforeach()
endforeach()
# Regenerate the delay goldens: cmake --build build --target update-delay-goldens (review the diff before commit).
add_custom_target(update-delay-goldens
  COMMAND velora-offline-render ${_dl_ms_good} --out ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_delay_ms/fx_delay_ms.wav
  COMMAND velora-offline-render ${_dl_sync_good} --out ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_delay_sync/fx_delay_sync.wav)
foreach(_dup "--delay-time;100;200" "--delay-sync;on;off" "--delay-note;1/4;1/8" "--delay-feedback;0.1;0.2" "--delay-level;0.5;0.6" "--delay-damping;0.1;0.2"
             "--delay-pingpong;0.1;0.2" "--tempo;100;110")
  list(GET _dup 0 _opt)
  list(GET _dup 1 _v1)
  list(GET _dup 2 _v2)
  string(MAKE_C_IDENTIFIER "${_opt}" _id)
  add_test(NAME fx_rejects_duplicate${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_v1} ${_opt} ${_v2} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_duplicate${_id} PROPERTIES PASS_REGULAR_EXPRESSION "duplicate ${_opt}")
endforeach()
foreach(_bad "--delay-time;0.5" "--delay-time;2001" "--delay-time;nan" "--delay-feedback;0.96" "--delay-feedback;1" "--delay-feedback;-0.1" "--delay-level;2" "--delay-damping;-1"
             "--delay-pingpong;2" "--delay-sync;maybe" "--delay-note;1/2" "--delay-note;eighth" "--tempo;19" "--tempo;301" "--tempo;inf" "--tempo-change;5" "--tempo-change;5:19"
             "--tempo-change;5:301" "--tempo-change;-1:100" "--tempo-change;:100" "--tempo-change;x:100")
  list(GET _bad 0 _opt)
  list(GET _bad 1 _val)
  string(MAKE_C_IDENTIFIER "${_opt}_${_val}" _id)
  add_test(NAME fx_rejects_${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_val} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid ${_opt}")
endforeach()

# Live Set load (Story 4.3): layer_split.liveset (bass / piano split at B2 / C3, a pad layered underneath, an octave doubling, silent parts) is loaded by
# the engine host's LiveSetLoader from the committed libraries in tests/live_set/root and played by layer_split.mid. Same scheme as the other goldens:
# render twice (byte-equal), null-test against the committed golden at -100 dB, and require a wrong zone boundary or octave to fail it.
# Regenerate the fixtures (integer synthesis, identical everywhere) with `cmake --build build --target live-set-fixtures`, the golden with update-live-set-golden.
set(_ls ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_set)
set(_ls_args --live-set ${_ls}/layer_split.liveset --library-root ${_ls}/root --midi ${_ls}/layer_split.mid --tail-seconds 1.0)
add_test(NAME live_set_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/live_set_a.wav ${_out}/live_set_b.wav ${_out}/live_set_ws.wav ${_out}/live_set_wo.wav)
set_tests_properties(live_set_clean PROPERTIES FIXTURES_SETUP live_set_clean)
add_test(NAME live_set_render_a COMMAND velora-offline-render ${_ls_args} --out ${_out}/live_set_a.wav)
add_test(NAME live_set_render_b COMMAND velora-offline-render ${_ls_args} --out ${_out}/live_set_b.wav)
set_tests_properties(live_set_render_a PROPERTIES FIXTURES_REQUIRED live_set_clean FIXTURES_SETUP live_set_a PASS_REGULAR_EXPRESSION "part 5: missing library")
set_tests_properties(live_set_render_b PROPERTIES FIXTURES_REQUIRED live_set_clean FIXTURES_SETUP live_set_b)
add_test(NAME live_set_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/live_set_a.wav ${_out}/live_set_b.wav)
set_tests_properties(live_set_bit_identical PROPERTIES FIXTURES_REQUIRED "live_set_a;live_set_b")
add_test(NAME live_set_null_golden COMMAND velora-null-test --case ${_ls}/layer_split.case --candidate ${_out}/live_set_a.wav)
set_tests_properties(live_set_null_golden PROPERTIES FIXTURES_REQUIRED live_set_a PASS_REGULAR_EXPRESSION "overall: PASS")
foreach(_w split:ws octave:wo)
  string(REPLACE ":" ";" _wl "${_w}")
  list(GET _wl 0 _wn)
  list(GET _wl 1 _wf)
  add_test(NAME live_set_wrong_${_wn}
           COMMAND velora-offline-render --live-set ${_ls}/wrong_${_wn}.liveset --library-root ${_ls}/root --midi ${_ls}/layer_split.mid --tail-seconds 1.0
                   --out ${_out}/live_set_${_wf}.wav)
  set_tests_properties(live_set_wrong_${_wn} PROPERTIES FIXTURES_REQUIRED live_set_clean FIXTURES_SETUP live_set_${_wf})
  add_test(NAME live_set_wrong_${_wn}_fails COMMAND velora-null-test --case ${_ls}/layer_split.case --candidate ${_out}/live_set_${_wf}.wav)
  set_tests_properties(live_set_wrong_${_wn}_fails PROPERTIES FIXTURES_REQUIRED live_set_${_wf}
                       PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
endforeach()
# Option and input errors of the Live Set mode.
add_test(NAME live_set_rejects_missing_library_root
         COMMAND velora-offline-render --live-set ${_ls}/layer_split.liveset --midi ${_ls}/layer_split.mid --out ${_out}/never.wav)
set_tests_properties(live_set_rejects_missing_library_root PROPERTIES PASS_REGULAR_EXPRESSION "usage: velora-offline-render")
add_test(NAME live_set_rejects_engine_options
         COMMAND velora-offline-render ${_ls_args} --engine organ --out ${_out}/never.wav)
set_tests_properties(live_set_rejects_engine_options PROPERTIES PASS_REGULAR_EXPRESSION "--live-set cannot be combined")
add_test(NAME live_set_rejects_missing_file
         COMMAND velora-offline-render --live-set ${_ls}/nope.liveset --library-root ${_ls}/root --midi ${_ls}/layer_split.mid --out ${_out}/never.wav)
set_tests_properties(live_set_rejects_missing_file PROPERTIES PASS_REGULAR_EXPRESSION "live set: read failed")
add_test(NAME live_set_rejects_a_file_that_is_not_a_live_set
         COMMAND velora-offline-render --live-set ${_ls}/layer_split.mid --library-root ${_ls}/root --midi ${_ls}/layer_split.mid --out ${_out}/never.wav)
set_tests_properties(live_set_rejects_a_file_that_is_not_a_live_set PROPERTIES PASS_REGULAR_EXPRESSION "live set: (read failed|invalid live set)")
add_custom_target(live-set-fixtures
  COMMAND live-set-fixture-gen ${_ls}
  COMMAND python3 ${_ls}/make_midi.py ${_ls}/layer_split.mid
  COMMAND python3 ${_ls}/make_scenes_midi.py ${_ls}/scenes.mid
  COMMAND python3 ${_ls}/make_switch_midi.py ${_ls}/switch.mid
  COMMAND python3 ${_ls}/make_assign_midi.py ${_ls}/assign/assign.mid
  DEPENDS live-set-fixture-gen)
add_custom_target(update-live-set-golden
  COMMAND velora-offline-render ${_ls_args} --out ${_ls}/layer_split.wav)

# Scene recall (Story 4.4): scenes.liveset (the same libraries plus an organ, three scenes) rendered with --scene-at 0:0 (verse), 2.0:1 (chorus), 4.5:2 (outro) while
# notes are held, pedal-sustained and releasing across the recalls. Render twice (byte-equal), null-test against the committed golden at -100 dB, and require the
# scenes in the wrong order, and the same render with no recall at all, to fail it. Regenerate the golden with update-scenes-golden (after live-set-fixtures).
set(_sc_args --live-set ${_ls}/scenes.liveset --library-root ${_ls}/root --midi ${_ls}/scenes.mid --tail-seconds 1.5)
set(_sc_good --scene-at 0:0 --scene-at 2.0:1 --scene-at 4.5:2)
add_test(NAME scenes_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/scenes_a.wav ${_out}/scenes_b.wav ${_out}/scenes_wrong_order.wav ${_out}/scenes_none.wav ${_out}/scenes_unsorted.wav)
set_tests_properties(scenes_clean PROPERTIES FIXTURES_SETUP scenes_clean)
add_test(NAME scenes_render_a COMMAND velora-offline-render ${_sc_args} ${_sc_good} --out ${_out}/scenes_a.wav)
add_test(NAME scenes_render_b COMMAND velora-offline-render ${_sc_args} ${_sc_good} --out ${_out}/scenes_b.wav)
set_tests_properties(scenes_render_a PROPERTIES FIXTURES_REQUIRED scenes_clean FIXTURES_SETUP scenes_a PASS_REGULAR_EXPRESSION "3 scene recall")
set_tests_properties(scenes_render_b PROPERTIES FIXTURES_REQUIRED scenes_clean FIXTURES_SETUP scenes_b)
add_test(NAME scenes_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/scenes_a.wav ${_out}/scenes_b.wav)
set_tests_properties(scenes_bit_identical PROPERTIES FIXTURES_REQUIRED "scenes_a;scenes_b")
add_test(NAME scenes_null_golden COMMAND velora-null-test --case ${_ls}/scenes.case --candidate ${_out}/scenes_a.wav)
set_tests_properties(scenes_null_golden PROPERTIES FIXTURES_REQUIRED scenes_a PASS_REGULAR_EXPRESSION "overall: PASS")
# The order of --scene-at on the command line does not matter (recalls are applied in time order): same bytes as the sorted command line.
add_test(NAME scenes_render_unsorted
         COMMAND velora-offline-render ${_sc_args} --scene-at 4.5:2 --scene-at 0:0 --scene-at 2.0:1 --out ${_out}/scenes_unsorted.wav)
set_tests_properties(scenes_render_unsorted PROPERTIES FIXTURES_REQUIRED scenes_clean FIXTURES_SETUP scenes_unsorted)
add_test(NAME scenes_unsorted_is_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/scenes_a.wav ${_out}/scenes_unsorted.wav)
set_tests_properties(scenes_unsorted_is_identical PROPERTIES FIXTURES_REQUIRED "scenes_a;scenes_unsorted")
add_test(NAME scenes_wrong_order
         COMMAND velora-offline-render ${_sc_args} --scene-at 0:0 --scene-at 2.0:2 --scene-at 4.5:1 --out ${_out}/scenes_wrong_order.wav)
set_tests_properties(scenes_wrong_order PROPERTIES FIXTURES_REQUIRED scenes_clean FIXTURES_SETUP scenes_wrong_order)
add_test(NAME scenes_wrong_order_fails COMMAND velora-null-test --case ${_ls}/scenes.case --candidate ${_out}/scenes_wrong_order.wav)
set_tests_properties(scenes_wrong_order_fails PROPERTIES FIXTURES_REQUIRED scenes_wrong_order PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
add_test(NAME scenes_no_recall COMMAND velora-offline-render ${_sc_args} --out ${_out}/scenes_none.wav)
set_tests_properties(scenes_no_recall PROPERTIES FIXTURES_REQUIRED scenes_clean FIXTURES_SETUP scenes_none)
add_test(NAME scenes_no_recall_fails COMMAND velora-null-test --case ${_ls}/scenes.case --candidate ${_out}/scenes_none.wav)
set_tests_properties(scenes_no_recall_fails PROPERTIES FIXTURES_REQUIRED scenes_none PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# Option and input errors of --scene-at.
foreach(_bad "nope" "1" ":1" "x:1" "1:x" "1:8" "-1:0" "1:-1" "4000:0" "nan:0")
  string(MAKE_C_IDENTIFIER "scene_at_${_bad}" _id)
  add_test(NAME scenes_rejects_${_id}
           COMMAND velora-offline-render ${_sc_args} --scene-at ${_bad} --out ${_out}/never.wav)
  set_tests_properties(scenes_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid --scene-at")
endforeach()
add_test(NAME scenes_rejects_missing_scene
         COMMAND velora-offline-render ${_sc_args} --scene-at 1.0:5 --out ${_out}/never.wav)
set_tests_properties(scenes_rejects_missing_scene PROPERTIES PASS_REGULAR_EXPRESSION "scene 5 does not exist \\(the Live Set has 3 scenes\\)")
add_test(NAME scenes_rejects_recall_after_the_end
         COMMAND velora-offline-render ${_sc_args} --scene-at 100:1 --out ${_out}/never.wav)
set_tests_properties(scenes_rejects_recall_after_the_end PROPERTIES PASS_REGULAR_EXPRESSION "after the end of the render")
add_test(NAME scenes_rejects_fractional_index
         COMMAND velora-offline-render ${_sc_args} --scene-at 1.0:1.5 --out ${_out}/never.wav)
set_tests_properties(scenes_rejects_fractional_index PROPERTIES PASS_REGULAR_EXPRESSION "invalid --scene-at '1.0:1.5'.*whole scene number")
# A recall inside the last block (7.0005 s is frame 336024 of 336064) is applied, not rejected.
add_test(NAME scenes_recall_in_the_last_block
         COMMAND velora-offline-render ${_sc_args} --scene-at 0:0 --scene-at 7.0005:1 --out ${_out}/scenes_last_block.wav)
set_tests_properties(scenes_recall_in_the_last_block PROPERTIES PASS_REGULAR_EXPRESSION "2 scene recall")
add_test(NAME scenes_rejects_without_live_set
         COMMAND velora-offline-render ${_render_args} --scene-at 1.0:0 --out ${_out}/never.wav)
set_tests_properties(scenes_rejects_without_live_set PROPERTIES PASS_REGULAR_EXPRESSION "--scene-at needs --live-set")
add_custom_target(update-scenes-golden
  COMMAND velora-offline-render ${_sc_args} ${_sc_good} --out ${_ls}/scenes.wav)

# Seamless Live Set switch (Story 4.5): switch/switch_a.liveset rendered with a switch to switch/switch_b.liveset at 3.0 s while notes are held, releasing and
# pedal-sustained across the swap (the background load is simulated as 100 blocks, so the render is deterministic). Render twice (byte-equal), null-test
# against the committed golden at -100 dB, and require a render with no switch, a switch at the wrong time and a different simulated load time to fail it.
# Regenerate the fixtures with live-set-fixtures, the golden with update-switch-golden.
set(_sw_args --live-set ${_ls}/switch/switch_a.liveset --library-root ${_ls}/root --midi ${_ls}/switch.mid --tail-seconds 1.5)
set(_sw_good --switch-at 3.0:${_ls}/switch/switch_b.liveset --switch-load-blocks 100)
add_test(NAME switch_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/switch_a.wav ${_out}/switch_b.wav ${_out}/switch_none.wav ${_out}/switch_late.wav ${_out}/switch_slow.wav)
set_tests_properties(switch_clean PROPERTIES FIXTURES_SETUP switch_clean)
add_test(NAME switch_render_a COMMAND velora-offline-render ${_sw_args} ${_sw_good} --out ${_out}/switch_a.wav)
add_test(NAME switch_render_b COMMAND velora-offline-render ${_sw_args} ${_sw_good} --out ${_out}/switch_b.wav)
set_tests_properties(switch_render_a PROPERTIES FIXTURES_REQUIRED switch_clean FIXTURES_SETUP switch_a PASS_REGULAR_EXPRESSION "1 live set switch")
set_tests_properties(switch_render_b PROPERTIES FIXTURES_REQUIRED switch_clean FIXTURES_SETUP switch_b)
add_test(NAME switch_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/switch_a.wav ${_out}/switch_b.wav)
set_tests_properties(switch_bit_identical PROPERTIES FIXTURES_REQUIRED "switch_a;switch_b")
add_test(NAME switch_null_golden COMMAND velora-null-test --case ${_ls}/switch.case --candidate ${_out}/switch_a.wav)
set_tests_properties(switch_null_golden PROPERTIES FIXTURES_REQUIRED switch_a PASS_REGULAR_EXPRESSION "overall: PASS")
add_test(NAME switch_no_switch COMMAND velora-offline-render ${_sw_args} --out ${_out}/switch_none.wav)
set_tests_properties(switch_no_switch PROPERTIES FIXTURES_REQUIRED switch_clean FIXTURES_SETUP switch_none)
add_test(NAME switch_no_switch_fails COMMAND velora-null-test --case ${_ls}/switch.case --candidate ${_out}/switch_none.wav)
set_tests_properties(switch_no_switch_fails PROPERTIES FIXTURES_REQUIRED switch_none PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
add_test(NAME switch_wrong_time
         COMMAND velora-offline-render ${_sw_args} --switch-at 3.6:${_ls}/switch/switch_b.liveset --switch-load-blocks 100 --out ${_out}/switch_late.wav)
set_tests_properties(switch_wrong_time PROPERTIES FIXTURES_REQUIRED switch_clean FIXTURES_SETUP switch_late)
add_test(NAME switch_wrong_time_fails COMMAND velora-null-test --case ${_ls}/switch.case --candidate ${_out}/switch_late.wav)
set_tests_properties(switch_wrong_time_fails PROPERTIES FIXTURES_REQUIRED switch_late PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
add_test(NAME switch_wrong_load_time
         COMMAND velora-offline-render ${_sw_args} --switch-at 3.0:${_ls}/switch/switch_b.liveset --switch-load-blocks 700 --out ${_out}/switch_slow.wav)
set_tests_properties(switch_wrong_load_time PROPERTIES FIXTURES_REQUIRED switch_clean FIXTURES_SETUP switch_slow)
add_test(NAME switch_wrong_load_time_fails COMMAND velora-null-test --case ${_ls}/switch.case --candidate ${_out}/switch_slow.wav)
set_tests_properties(switch_wrong_load_time_fails PROPERTIES FIXTURES_REQUIRED switch_slow PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# Option and input errors of --switch-at / --switch-load-blocks.
foreach(_bad "nope" "1" ":x.liveset" "x:x.liveset" "-1:x.liveset" "4000:x.liveset" "nan:x.liveset" "1:")
  string(MAKE_C_IDENTIFIER "switch_at_${_bad}" _id)
  add_test(NAME switch_rejects_${_id}
           COMMAND velora-offline-render ${_sw_args} --switch-at ${_bad} --out ${_out}/never.wav)
  set_tests_properties(switch_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid --switch-at")
endforeach()
foreach(_bad "x" "-1" "1.5" "2000000")
  string(MAKE_C_IDENTIFIER "switch_load_${_bad}" _id)
  add_test(NAME switch_rejects_load_blocks_${_id}
           COMMAND velora-offline-render ${_sw_args} ${_sw_good} --switch-load-blocks ${_bad} --out ${_out}/never.wav)
  set_tests_properties(switch_rejects_load_blocks_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid --switch-load-blocks")
endforeach()
add_test(NAME switch_rejects_missing_file
         COMMAND velora-offline-render ${_sw_args} --switch-at 1.0:${_ls}/switch/nope.liveset --out ${_out}/never.wav)
set_tests_properties(switch_rejects_missing_file PROPERTIES PASS_REGULAR_EXPRESSION "--switch-at: .*nope.liveset")
add_test(NAME switch_rejects_after_the_end
         COMMAND velora-offline-render ${_sw_args} --switch-at 100:${_ls}/switch/switch_b.liveset --out ${_out}/never.wav)
set_tests_properties(switch_rejects_after_the_end PROPERTIES PASS_REGULAR_EXPRESSION "after the end of the render")
add_test(NAME switch_rejects_unfinished_load
         COMMAND velora-offline-render ${_sw_args} --switch-at 6.0:${_ls}/switch/switch_b.liveset --switch-load-blocks 100000 --out ${_out}/never.wav)
set_tests_properties(switch_rejects_unfinished_load PROPERTIES PASS_REGULAR_EXPRESSION "did not complete before the end")
add_test(NAME switch_rejects_superseded_switch
         COMMAND velora-offline-render ${_sw_args} --switch-at 3.0:${_ls}/switch/switch_b.liveset --switch-at 3.01:${_ls}/switch/switch_a.liveset
                 --switch-load-blocks 100 --out ${_out}/never.wav)
set_tests_properties(switch_rejects_superseded_switch PROPERTIES PASS_REGULAR_EXPRESSION "switch superseded before completion")
add_test(NAME switch_rejects_same_block_switches
         COMMAND velora-offline-render ${_sw_args} --switch-at 3.0:${_ls}/switch/switch_b.liveset --switch-at 3.0001:${_ls}/switch/switch_a.liveset
                 --switch-load-blocks 0 --out ${_out}/never.wav)
set_tests_properties(switch_rejects_same_block_switches PROPERTIES PASS_REGULAR_EXPRESSION "switch superseded before completion")
add_test(NAME switch_rejects_without_live_set
         COMMAND velora-offline-render ${_render_args} --switch-at 1.0:${_ls}/switch/switch_b.liveset --out ${_out}/never.wav)
set_tests_properties(switch_rejects_without_live_set PROPERTIES PASS_REGULAR_EXPRESSION "--switch-at and --switch-load-blocks need --live-set")
add_test(NAME switch_rejects_load_blocks_alone
         COMMAND velora-offline-render ${_sw_args} --switch-load-blocks 5 --out ${_out}/never.wav)
set_tests_properties(switch_rejects_load_blocks_alone PROPERTIES PASS_REGULAR_EXPRESSION "--switch-load-blocks needs --switch-at")
add_custom_target(update-switch-golden
  COMMAND velora-offline-render ${_sw_args} ${_sw_good} --out ${_ls}/switch.wav)

# Option parsing.
add_test(NAME organ_perc_rejects_bad_percussion
         COMMAND velora-offline-render ${_perc_args} --percussion third,third --out ${_out}/never.wav)
set_tests_properties(organ_perc_rejects_bad_percussion PROPERTIES PASS_REGULAR_EXPRESSION "invalid --percussion")
add_test(NAME organ_perc_rejects_off_in_list
         COMMAND velora-offline-render ${_perc_args} --percussion off,third --out ${_out}/never.wav)
set_tests_properties(organ_perc_rejects_off_in_list PROPERTIES PASS_REGULAR_EXPRESSION "invalid --percussion")
add_test(NAME organ_perc_rejects_bad_key_click
         COMMAND velora-offline-render ${_perc_args} --key-click 1.5 --out ${_out}/never.wav)
set_tests_properties(organ_perc_rejects_bad_key_click PROPERTIES PASS_REGULAR_EXPRESSION "invalid --key-click")
add_test(NAME organ_perc_options_need_organ
         COMMAND velora-offline-render --midi ${_perc}/organ_percussion.mid --percussion third --out ${_out}/never.wav)
set_tests_properties(organ_perc_options_need_organ PROPERTIES PASS_REGULAR_EXPRESSION "need --engine organ")

# Organ vibrato/chorus scanner and leakage (Story 2.4): a committed MIDI file holds a chord while CC 81/87 cycle off, V1, V2, V3,
# C1, C2, C3, off and CC 83 raises the leakage. Same scheme as the percussion golden: render twice (byte-equal), null-test against
# the committed golden, and require a different scanner setting or leakage to fail it.
set(_scan ${CMAKE_CURRENT_SOURCE_DIR}/tests/organ_scanner)
set(_scan_args --midi ${_scan}/organ_scanner.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
               --vibrato off --leakage 0 --tail-seconds 0.3)
add_test(NAME organ_scan_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/scan_a.wav ${_out}/scan_b.wav ${_out}/scan_wrong_vib.wav ${_out}/scan_wrong_leak.wav)
set_tests_properties(organ_scan_clean PROPERTIES FIXTURES_SETUP scan_clean)
add_test(NAME organ_scan_render_a COMMAND velora-offline-render ${_scan_args} --out ${_out}/scan_a.wav)
add_test(NAME organ_scan_render_b COMMAND velora-offline-render ${_scan_args} --out ${_out}/scan_b.wav)
set_tests_properties(organ_scan_render_a PROPERTIES FIXTURES_REQUIRED scan_clean FIXTURES_SETUP scan_a)
set_tests_properties(organ_scan_render_b PROPERTIES FIXTURES_REQUIRED scan_clean FIXTURES_SETUP scan_b)
add_test(NAME organ_scan_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/scan_a.wav ${_out}/scan_b.wav)
set_tests_properties(organ_scan_bit_identical PROPERTIES FIXTURES_REQUIRED "scan_a;scan_b")
add_test(NAME organ_scan_null_golden
         COMMAND velora-null-test --case ${_scan}/organ_scanner.case --candidate ${_out}/scan_a.wav)
set_tests_properties(organ_scan_null_golden PROPERTIES FIXTURES_REQUIRED scan_a PASS_REGULAR_EXPRESSION "overall: PASS")
# The golden is sensitive to what it covers: starting in V3 (the file's controllers change it later, but the first 0.4 s differ)
# and starting with full leakage both fail the null test.
add_test(NAME organ_scan_wrong_vibrato
         COMMAND velora-offline-render --midi ${_scan}/organ_scanner.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
                 --vibrato v3 --leakage 0 --tail-seconds 0.3 --out ${_out}/scan_wrong_vib.wav)
add_test(NAME organ_scan_wrong_leakage
         COMMAND velora-offline-render --midi ${_scan}/organ_scanner.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
                 --vibrato off --leakage 1 --tail-seconds 0.3 --out ${_out}/scan_wrong_leak.wav)
set_tests_properties(organ_scan_wrong_vibrato PROPERTIES FIXTURES_REQUIRED scan_clean FIXTURES_SETUP scan_wv)
set_tests_properties(organ_scan_wrong_leakage PROPERTIES FIXTURES_REQUIRED scan_clean FIXTURES_SETUP scan_wl)
add_test(NAME organ_scan_wrong_vibrato_fails
         COMMAND velora-null-test --case ${_scan}/organ_scanner.case --candidate ${_out}/scan_wrong_vib.wav)
add_test(NAME organ_scan_wrong_leakage_fails
         COMMAND velora-null-test --case ${_scan}/organ_scanner.case --candidate ${_out}/scan_wrong_leak.wav)
set_tests_properties(organ_scan_wrong_vibrato_fails PROPERTIES FIXTURES_REQUIRED scan_wv
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
set_tests_properties(organ_scan_wrong_leakage_fails PROPERTIES FIXTURES_REQUIRED scan_wl
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# A file with only CC 81 (no CC 87) switches the scanner on at the registry default type (c1), exactly like --vibrato c1 on the
# same chord without the controller: the renderer's frame-0 setup must not pick another type.
add_test(NAME organ_scan_cc81_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/scan_cc81.wav ${_out}/scan_plain_c1.wav)
set_tests_properties(organ_scan_cc81_clean PROPERTIES FIXTURES_SETUP scan_cc81_clean)
add_test(NAME organ_scan_cc81_render
         COMMAND velora-offline-render --midi ${_scan}/organ_cc81.mid --engine organ --key-click 0 --leakage 0 --vibrato off
                 --tail-seconds 0.2 --out ${_out}/scan_cc81.wav)
add_test(NAME organ_scan_plain_c1_render
         COMMAND velora-offline-render --midi ${_scan}/organ_plain.mid --engine organ --key-click 0 --leakage 0 --vibrato c1
                 --tail-seconds 0.2 --out ${_out}/scan_plain_c1.wav)
set_tests_properties(organ_scan_cc81_render PROPERTIES FIXTURES_REQUIRED scan_cc81_clean FIXTURES_SETUP scan_cc81_a)
set_tests_properties(organ_scan_plain_c1_render PROPERTIES FIXTURES_REQUIRED scan_cc81_clean FIXTURES_SETUP scan_cc81_b)
add_test(NAME organ_scan_cc81_matches_c1
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/scan_cc81.wav ${_out}/scan_plain_c1.wav)
set_tests_properties(organ_scan_cc81_matches_c1 PROPERTIES FIXTURES_REQUIRED "scan_cc81_a;scan_cc81_b")
# Option parsing.
add_test(NAME organ_scan_rejects_bad_vibrato
         COMMAND velora-offline-render ${_scan_args} --vibrato v4 --out ${_out}/never.wav)
set_tests_properties(organ_scan_rejects_bad_vibrato PROPERTIES PASS_REGULAR_EXPRESSION "invalid --vibrato")
add_test(NAME organ_scan_rejects_bad_leakage
         COMMAND velora-offline-render ${_scan_args} --leakage 1.5 --out ${_out}/never.wav)
set_tests_properties(organ_scan_rejects_bad_leakage PROPERTIES PASS_REGULAR_EXPRESSION "invalid --leakage")
add_test(NAME organ_scan_options_need_organ
         COMMAND velora-offline-render --midi ${_scan}/organ_scanner.mid --vibrato v1 --out ${_out}/never.wav)
set_tests_properties(organ_scan_options_need_organ PROPERTIES PASS_REGULAR_EXPRESSION "need --engine organ")

# Organ rotary speaker (Story 2.6): a committed MIDI file holds a chord while CC 89 switches the stage on and CC 79 goes fast, stop, slow
# (the horn and the drum ramp with their own time constants). Same scheme as the other organ goldens: render twice (byte-equal),
# null-test against the committed golden, and require a different starting rotary state to fail it.
set(_rot ${CMAKE_CURRENT_SOURCE_DIR}/tests/organ_rotary)
set(_rot_args --midi ${_rot}/organ_rotary.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
              --vibrato off --leakage 0 --rotary off --tail-seconds 0.3)
add_test(NAME organ_rotary_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/rot_a.wav ${_out}/rot_b.wav ${_out}/rot_wrong_fast.wav ${_out}/rot_wrong_stop.wav)
set_tests_properties(organ_rotary_clean PROPERTIES FIXTURES_SETUP rot_clean)
add_test(NAME organ_rotary_render_a COMMAND velora-offline-render ${_rot_args} --out ${_out}/rot_a.wav)
add_test(NAME organ_rotary_render_b COMMAND velora-offline-render ${_rot_args} --out ${_out}/rot_b.wav)
set_tests_properties(organ_rotary_render_a PROPERTIES FIXTURES_REQUIRED rot_clean FIXTURES_SETUP rot_a)
set_tests_properties(organ_rotary_render_b PROPERTIES FIXTURES_REQUIRED rot_clean FIXTURES_SETUP rot_b)
add_test(NAME organ_rotary_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/rot_a.wav ${_out}/rot_b.wav)
set_tests_properties(organ_rotary_bit_identical PROPERTIES FIXTURES_REQUIRED "rot_a;rot_b")
add_test(NAME organ_rotary_null_golden
         COMMAND velora-null-test --case ${_rot}/organ_rotary.case --candidate ${_out}/rot_a.wav)
set_tests_properties(organ_rotary_null_golden PROPERTIES FIXTURES_REQUIRED rot_a PASS_REGULAR_EXPRESSION "overall: PASS")
add_test(NAME organ_rotary_wrong_fast
         COMMAND velora-offline-render --midi ${_rot}/organ_rotary.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
                 --vibrato off --leakage 0 --rotary fast --tail-seconds 0.3 --out ${_out}/rot_wrong_fast.wav)
add_test(NAME organ_rotary_wrong_stop
         COMMAND velora-offline-render --midi ${_rot}/organ_rotary.mid --engine organ --drawbars 888600000 --percussion off --key-click 0.2
                 --vibrato off --leakage 0 --rotary stop --tail-seconds 0.3 --out ${_out}/rot_wrong_stop.wav)
set_tests_properties(organ_rotary_wrong_fast PROPERTIES FIXTURES_REQUIRED rot_clean FIXTURES_SETUP rot_wf)
set_tests_properties(organ_rotary_wrong_stop PROPERTIES FIXTURES_REQUIRED rot_clean FIXTURES_SETUP rot_ws)
add_test(NAME organ_rotary_wrong_fast_fails
         COMMAND velora-null-test --case ${_rot}/organ_rotary.case --candidate ${_out}/rot_wrong_fast.wav)
add_test(NAME organ_rotary_wrong_stop_fails
         COMMAND velora-null-test --case ${_rot}/organ_rotary.case --candidate ${_out}/rot_wrong_stop.wav)
set_tests_properties(organ_rotary_wrong_fast_fails PROPERTIES FIXTURES_REQUIRED rot_wf
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
set_tests_properties(organ_rotary_wrong_stop_fails PROPERTIES FIXTURES_REQUIRED rot_ws
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# The CLI mapping is pinned: --rotary slow|fast|stop on a plain chord renders byte-equal to the same chord with CC 79 (64 / 127 / 0) and
# CC 89 at frame 0 and --rotary off.
add_test(NAME organ_rotary_map_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/map_plain_slow.wav ${_out}/map_cc_slow.wav ${_out}/map_plain_fast.wav
                 ${_out}/map_cc_fast.wav ${_out}/map_plain_stop.wav ${_out}/map_cc_stop.wav)
set_tests_properties(organ_rotary_map_clean PROPERTIES FIXTURES_SETUP rot_map_clean)
foreach(_m slow fast stop)
  add_test(NAME organ_rotary_map_${_m}_cli
           COMMAND velora-offline-render --midi ${_rot}/organ_map_plain.mid --engine organ --rotary ${_m} --tail-seconds 0.2
                   --out ${_out}/map_plain_${_m}.wav)
  add_test(NAME organ_rotary_map_${_m}_cc
           COMMAND velora-offline-render --midi ${_rot}/organ_map_${_m}.mid --engine organ --rotary off --tail-seconds 0.2
                   --out ${_out}/map_cc_${_m}.wav)
  add_test(NAME organ_rotary_map_${_m}_equal
           COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/map_plain_${_m}.wav ${_out}/map_cc_${_m}.wav)
  set_tests_properties(organ_rotary_map_${_m}_cli organ_rotary_map_${_m}_cc PROPERTIES FIXTURES_REQUIRED rot_map_clean
                       FIXTURES_SETUP rot_map_${_m})
  set_tests_properties(organ_rotary_map_${_m}_equal PROPERTIES FIXTURES_REQUIRED rot_map_${_m})
endforeach()
add_test(NAME organ_rotary_rejects_bad_rotary
         COMMAND velora-offline-render ${_rot_args} --rotary spin --out ${_out}/never.wav)
set_tests_properties(organ_rotary_rejects_bad_rotary PROPERTIES PASS_REGULAR_EXPRESSION "invalid --rotary")
add_test(NAME organ_rotary_options_need_organ
         COMMAND velora-offline-render --midi ${_rot}/organ_rotary.mid --rotary slow --out ${_out}/never.wav)
set_tests_properties(organ_rotary_options_need_organ PROPERTIES PASS_REGULAR_EXPRESSION "need --engine organ")

# Organ multi-contact onset (Story 2.5): a committed contact_k event list (--events, test only) holds fast rolled chords (1 ms between
# contacts), a slow smear (20 ms), half presses, skipped contacts, malformed traffic, a Note On/Off-only key and percussion triggered
# by the first contact. Same scheme as the other organ goldens: render twice (byte-equal), null-test against the committed golden,
# and require wrong settings to fail it.
set(_cont ${CMAKE_CURRENT_SOURCE_DIR}/tests/organ_contacts)
set(_cont_args --events ${_cont}/organ_contacts.contacts --engine organ --drawbars 888600000 --percussion off --key-click 0.3
               --vibrato off --leakage 0 --tail-seconds 0.3)
add_test(NAME organ_contacts_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/cont_a.wav ${_out}/cont_b.wav ${_out}/cont_wrong_db.wav ${_out}/cont_wrong_click.wav
                 ${_out}/cont_wrong_count.wav ${_out}/cont_midi_only.wav)
set_tests_properties(organ_contacts_clean PROPERTIES FIXTURES_SETUP cont_clean)
add_test(NAME organ_contacts_render_a COMMAND velora-offline-render ${_cont_args} --out ${_out}/cont_a.wav)
add_test(NAME organ_contacts_render_b COMMAND velora-offline-render ${_cont_args} --out ${_out}/cont_b.wav)
set_tests_properties(organ_contacts_render_a PROPERTIES FIXTURES_REQUIRED cont_clean FIXTURES_SETUP cont_a)
set_tests_properties(organ_contacts_render_b PROPERTIES FIXTURES_REQUIRED cont_clean FIXTURES_SETUP cont_b)
add_test(NAME organ_contacts_bit_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/cont_a.wav ${_out}/cont_b.wav)
set_tests_properties(organ_contacts_bit_identical PROPERTIES FIXTURES_REQUIRED "cont_a;cont_b")
add_test(NAME organ_contacts_null_golden
         COMMAND velora-null-test --case ${_cont}/organ_contacts.case --candidate ${_out}/cont_a.wav)
set_tests_properties(organ_contacts_null_golden PROPERTIES FIXTURES_REQUIRED cont_a PASS_REGULAR_EXPRESSION "overall: PASS")
# Sensitive to what it covers: other drawbars, another key click level and a 2-contact keybed (contact 3 becomes malformed and the
# groups change) all fail the null test.
add_test(NAME organ_contacts_wrong_drawbars
         COMMAND velora-offline-render --events ${_cont}/organ_contacts.contacts --engine organ --drawbars 888600001 --percussion off
                 --key-click 0.3 --vibrato off --leakage 0 --tail-seconds 0.3 --out ${_out}/cont_wrong_db.wav)
add_test(NAME organ_contacts_wrong_click
         COMMAND velora-offline-render --events ${_cont}/organ_contacts.contacts --engine organ --drawbars 888600000 --percussion off
                 --key-click 0.9 --vibrato off --leakage 0 --tail-seconds 0.3 --out ${_out}/cont_wrong_click.wav)
add_test(NAME organ_contacts_wrong_count
         COMMAND velora-offline-render --events ${_cont}/organ_contacts_two.contacts --engine organ --drawbars 888600000 --percussion off
                 --key-click 0.3 --vibrato off --leakage 0 --tail-seconds 0.3 --out ${_out}/cont_wrong_count.wav)
set_tests_properties(organ_contacts_wrong_drawbars PROPERTIES FIXTURES_REQUIRED cont_clean FIXTURES_SETUP cont_wd)
set_tests_properties(organ_contacts_wrong_click PROPERTIES FIXTURES_REQUIRED cont_clean FIXTURES_SETUP cont_wc)
set_tests_properties(organ_contacts_wrong_count PROPERTIES FIXTURES_REQUIRED cont_clean FIXTURES_SETUP cont_wn)
foreach(_w db click count)
  string(REPLACE "db" "drawbars" _wname "${_w}")
  add_test(NAME organ_contacts_wrong_${_wname}_fails
           COMMAND velora-null-test --case ${_cont}/organ_contacts.case --candidate ${_out}/cont_wrong_${_w}.wav)
endforeach()
set_tests_properties(organ_contacts_wrong_drawbars_fails PROPERTIES FIXTURES_REQUIRED cont_wd
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
set_tests_properties(organ_contacts_wrong_click_fails PROPERTIES FIXTURES_REQUIRED cont_wc
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
set_tests_properties(organ_contacts_wrong_count_fails PROPERTIES FIXTURES_REQUIRED cont_wn
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# Option parsing and input errors.
add_test(NAME organ_contacts_events_need_organ
         COMMAND velora-offline-render --events ${_cont}/organ_contacts.contacts --out ${_out}/never.wav)
set_tests_properties(organ_contacts_events_need_organ PROPERTIES PASS_REGULAR_EXPRESSION "--events needs --engine organ")
add_test(NAME organ_contacts_rejects_bad_events
         COMMAND velora-offline-render --events ${_cont}/bad.contacts --engine organ --out ${_out}/never.wav)
set_tests_properties(organ_contacts_rejects_bad_events PROPERTIES PASS_REGULAR_EXPRESSION "bad.contacts: line 3: ")
add_test(NAME organ_contacts_needs_an_input
         COMMAND velora-offline-render --engine organ --out ${_out}/never.wav)
set_tests_properties(organ_contacts_needs_an_input PROPERTIES PASS_REGULAR_EXPRESSION "usage:")

# --midi and --events together: merged by frame, MIDI first on ties. tie.mid ends key 60 at frame 24000, organ_tie.contacts presses it again
# at that frame. With the rule right (Off, then On) the key still sounds afterwards, so the merged render differs from the MIDI-only one; if
# the order were reversed (On is a no-op, then Off) the two would be identical and this null test would pass instead of failing.
add_test(NAME organ_tie_clean COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/tie_both.wav ${_out}/tie_midi.wav)
set_tests_properties(organ_tie_clean PROPERTIES FIXTURES_SETUP tie_clean)
add_test(NAME organ_tie_render_both
         COMMAND velora-offline-render --midi ${_cont}/tie.mid --events ${_cont}/organ_tie.contacts --engine organ --tail-seconds 0.5
                 --out ${_out}/tie_both.wav)
add_test(NAME organ_tie_render_midi
         COMMAND velora-offline-render --midi ${_cont}/tie.mid --engine organ --tail-seconds 0.5 --out ${_out}/tie_midi.wav)
set_tests_properties(organ_tie_render_both organ_tie_render_midi PROPERTIES FIXTURES_REQUIRED tie_clean FIXTURES_SETUP tie_done)
add_test(NAME organ_tie_midi_first
         COMMAND velora-null-test --reference ${_out}/tie_midi.wav --candidate ${_out}/tie_both.wav --tolerance-db -60 --max-offset-frames 0)
set_tests_properties(organ_tie_midi_first PROPERTIES FIXTURES_REQUIRED tie_done
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")

# Regenerate the golden: cmake --build build --target update-golden (review the diff before commit).
add_custom_target(update-golden
  COMMAND velora-offline-render ${_render_args} --out ${_golden}/golden.wav
  DEPENDS velora-offline-render)
# Same for the organ percussion golden: cmake --build build --target update-organ-percussion-golden
add_custom_target(update-organ-percussion-golden
  COMMAND velora-offline-render ${_perc_args} --out ${_perc}/organ_percussion.wav
  DEPENDS velora-offline-render)

add_custom_target(update-organ-contacts-golden
  COMMAND velora-offline-render ${_cont_args} --out ${_cont}/organ_contacts.wav
  DEPENDS velora-offline-render)

add_custom_target(update-organ-rotary-golden
  COMMAND velora-offline-render ${_rot_args} --out ${_rot}/organ_rotary.wav
  DEPENDS velora-offline-render)

add_custom_target(update-organ-scanner-golden
  COMMAND velora-offline-render ${_scan_args} --out ${_scan}/organ_scanner.wav
  DEPENDS velora-offline-render)

# --- sample engine against the sfizz oracle (Story 1.15) ----------------------------------------------
# Goldens are committed sfizz_render outputs, so no sfizz is needed here; see tests/sample_suite/README.md.
set(_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/sample_suite)
add_executable(sample_suite_test tests/sample_suite_test.cpp)
target_link_libraries(sample_suite_test PRIVATE velora_librarian)
target_compile_definitions(sample_suite_test PRIVATE
  SUITE_DIR="${_suite}"
  RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  NULLTEST_EXE="$<TARGET_FILE:velora-null-test>"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(sample_suite_test velora-offline-render velora-null-test)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_suite}/cases.txt)
file(STRINGS ${_suite}/cases.txt _case_lines)
foreach(_line IN LISTS _case_lines)
  if(_line MATCHES "^[ \t]*(#.*)?$")
    continue()
  elseif(_line MATCHES "^[ \t]*([A-Za-z0-9_]+)[ \t]+(hermite|linear)[ \t]+([0-9]+)[ \t]+(-[0-9.]+)[ \t]*$")
    # The exit code carries the result (the driver also fails on any failed check).
    add_test(NAME sample_null_${CMAKE_MATCH_1} COMMAND sample_suite_test ${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
  else()
    message(FATAL_ERROR "tests/sample_suite/cases.txt: malformed line '${_line}' (name hermite|linear quality tolerance_db)")
  endif()
endforeach()
# The harness must be able to fail: a 1 dB gain error and the wrong interpolation are caught.
add_test(NAME sample_null_neg_gain COMMAND sample_suite_test velocity_layers hermite --negative gain)
add_test(NAME sample_null_neg_interp COMMAND sample_suite_test pitch_far hermite --negative interp)
# Story 5.2: the keyswitch, crossfade and filter cases must fail when the same instrument is imported without those opcodes.
foreach(_neg keyswitch_last keyswitch_down_up keyswitch_previous xfade_power xfade_gain filters)
  add_test(NAME sample_null_neg_strip_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip)
endforeach()
# Story 13.4: the velocity and amplitude cases must fail when the same instrument is imported without those opcodes.
foreach(_neg amp_veltrack amp_velcurve amp_gain ampeg_vel2 amp_keytrack)
  add_test(NAME sample_null_neg_strip134_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip134)
endforeach()
# Story 13.5: the CC cases must fail when the same instrument is imported without the CC opcodes.
foreach(_neg cc_gates cc_modulation cc_envelope cc_pseudo)
  add_test(NAME sample_null_neg_strip135_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip135)
endforeach()
# Story 13.6: the group and polyphony cases must fail when the same instrument is imported without those opcodes.
foreach(_neg choke_groups choke_sustain polyphony_limits)
  add_test(NAME sample_null_neg_strip136_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip136)
endforeach()
# Story 13.7: the bend and controller-trigger cases must fail when the same instrument is imported without those opcodes.
foreach(_neg pitch_bend pitch_bend_fine pitch_bend_smooth cc_trigger)
  add_test(NAME sample_null_neg_strip137_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip137)
endforeach()
# Story 13.8: the per-voice filter cases must fail when the same instrument is imported without the new filter opcodes.
foreach(_neg fil_keytrack fil_veltrack cutoff_oncc resonance_oncc filter2)
  add_test(NAME sample_null_neg_strip138_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip138)
endforeach()
# Story 13.9: the EQ, LFO and EG cases must fail when the same instrument is imported without those opcodes.
foreach(_neg eq_bands eq_velocity eq_cc amplfo pitchlfo pitcheg fillfo_fileg)
  add_test(NAME sample_null_neg_strip139_${_neg} COMMAND sample_suite_test ${_neg} hermite --negative strip139)
endforeach()
add_test(NAME sample_render_cli_errors COMMAND sample_suite_test cli-errors)
# Story 1.18: --voice-threads 3 equals --voice-threads 1 bit for bit; 2 and a non-sample engine exit 2.
add_test(NAME sample_render_voice_threads COMMAND sample_suite_test voice-threads)

# Real libraries (Salamander Grand Piano, a VSCO2 instrument) against a locally built sfizz oracle: env-gated.
# Skipped (exit 77) unless VELORA_SFZ_DEMO_DIR is set and sfizz_render exists; see tests/sample_demo_null_test.cpp.
add_executable(sample_demo_null_test tests/sample_demo_null_test.cpp)
target_link_libraries(sample_demo_null_test PRIVATE velora_librarian)
target_compile_definitions(sample_demo_null_test PRIVATE
  SUITE_DIR="${_suite}"
  RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  NULLTEST_EXE="$<TARGET_FILE:velora-null-test>"
  RENDER_ORACLE_SH="${CMAKE_SOURCE_DIR}/tools/oracle/render-oracle.sh"
  ORACLE_DEFAULT_BIN="${CMAKE_SOURCE_DIR}/tools/oracle/.work/build/library/bin/sfizz_render"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(sample_demo_null_test velora-offline-render velora-null-test)
add_test(NAME sample_demo_null_test COMMAND sample_demo_null_test)
set_tests_properties(sample_demo_null_test PROPERTIES SKIP_RETURN_CODE 77)

# --- SF2 import against FluidSynth (Story 5.3) -----------------------------------------------------------
# Goldens are committed FluidSynth renders (tools/oracle/render-fluidsynth.sh), so no FluidSynth is needed here; the fixtures are
# SF2 files generated in code by the test-only writer in formatlib/tests; see tests/sf2_suite/README.md.
set(_sf2suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/sf2_suite)
add_executable(sf2_suite_test tests/sf2_suite_test.cpp)
target_link_libraries(sf2_suite_test PRIVATE velora_librarian)
target_include_directories(sf2_suite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/formatlib/tests)
target_compile_definitions(sf2_suite_test PRIVATE
  SUITE_DIR="${_sf2suite}"
  RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  NULLTEST_EXE="$<TARGET_FILE:velora-null-test>"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(sf2_suite_test velora-offline-render velora-null-test)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sf2suite}/cases.txt)
file(STRINGS ${_sf2suite}/cases.txt _sf2_lines)
set(_sf2_cases "")
foreach(_line IN LISTS _sf2_lines)
  if(_line MATCHES "^[ \t]*(#.*)?$")
    continue()
  elseif(_line MATCHES "^[ \t]*([A-Za-z0-9_]+)[ \t]+(-[0-9.]+)[ \t]+([0-9]+)[ \t]*$")
    # cases.txt and the committed golden/<case>.case must say the same (regenerate_goldens.sh writes the latter from the former).
    if(NOT EXISTS ${_sf2suite}/golden/${CMAKE_MATCH_1}.case OR NOT EXISTS ${_sf2suite}/golden/${CMAKE_MATCH_1}.wav)
      message(FATAL_ERROR "tests/sf2_suite: golden/${CMAKE_MATCH_1}.case or .wav is missing (run regenerate_goldens.sh)")
    endif()
    file(READ ${_sf2suite}/golden/${CMAKE_MATCH_1}.case _sf2_case)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sf2suite}/golden/${CMAKE_MATCH_1}.case)
    string(FIND "${_sf2_case}" "\ntolerance_db=${CMAKE_MATCH_2}\n" _sf2_tol)
    string(FIND "${_sf2_case}" "\nmax_offset_frames=${CMAKE_MATCH_3}\n" _sf2_off)
    if(_sf2_tol EQUAL -1 OR _sf2_off EQUAL -1)
      message(FATAL_ERROR "tests/sf2_suite: golden/${CMAKE_MATCH_1}.case disagrees with cases.txt (tolerance ${CMAKE_MATCH_2}, offset ${CMAKE_MATCH_3}); run regenerate_goldens.sh")
    endif()
    add_test(NAME sf2_null_${CMAKE_MATCH_1} COMMAND sf2_suite_test ${CMAKE_MATCH_1})
    list(APPEND _sf2_cases ${CMAKE_MATCH_1})
  else()
    message(FATAL_ERROR "tests/sf2_suite/cases.txt: malformed line '${_line}' (name tolerance_db max_offset_frames)")
  endif()
endforeach()
# The harness must be able to fail: a 1 dB gain error, the MIDI one block late, and the SF2 imported without
# its envelopes, filters, pans and attenuations are all rejected on residual.
add_test(NAME sf2_null_neg_gain COMMAND sf2_suite_test sf2_keys --negative gain)
add_test(NAME sf2_null_neg_lag COMMAND sf2_suite_test sf2_keys --negative lag)
foreach(_neg sf2_keys sf2_envelope sf2_filter sf2_stereo sf2_kit sf2_single)
  add_test(NAME sf2_null_neg_strip_${_neg} COMMAND sf2_suite_test ${_neg} --negative strip)
endforeach()
add_test(NAME sf2_render_cli COMMAND sf2_suite_test cli-errors)

# EQ and compressor as inserts and in the master section (Story 2.8): the golden test-tone MIDI through an insert effect on part 0 and the
# master effect. One golden per effect, same scheme as the other goldens: render twice (byte-equal), null-test against the committed golden at
# -100 dB, and require different settings (insert or master) to fail it.
#
# Story 2.9 adds the chorus/phaser (a chorus slot then a phaser slot, so both modes are in the golden), the EP amp and the rotary insert. They have
# no master counterpart, so their second "wrong" render (the fx_*_wrong_master tests) is another wrong insert setting.
foreach(_fx eq compressor chorus_phaser ep_amp rotary_insert)
  set(_d ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx_${_fx})
  if(_fx STREQUAL "chorus_phaser")
    set(_fx_args --inserts 0:chorus_phaser@0.0/0.55/0.8/0.5/0.6,chorus_phaser@1.0/0.4/0.8/0.8/0.6)
    set(_fx_wrong_insert --inserts 0:chorus_phaser@0.0/0.55/0.8/0.5/0.6,chorus_phaser@1.0/0.4/0.8/0.7/0.6)
    set(_fx_wrong_master --inserts 0:chorus_phaser@1.0/0.55/0.8/0.5/0.6,chorus_phaser@1.0/0.4/0.8/0.8/0.6)
  elseif(_fx STREQUAL "ep_amp")
    set(_fx_args --inserts 0:ep_amp@0.7/0.6/0.5/0.55/0.5)
    set(_fx_wrong_insert --inserts 0:ep_amp@0.6/0.6/0.5/0.55/0.5)
    set(_fx_wrong_master --inserts 0:ep_amp@0.7/0.6/0.5/0.55/0.0)
  elseif(_fx STREQUAL "rotary_insert")
    set(_fx_args --inserts 0:rotary@0.9/0.5)
    set(_fx_wrong_insert --inserts 0:rotary@0.5/0.5)
    set(_fx_wrong_master --inserts 0:rotary@0.9/0.2)
  elseif(_fx STREQUAL "eq")
    set(_fx_args --inserts 0:eq@0.9/0.2/0.6/0.8 --master-eq -4:5:2500:3)
    set(_fx_wrong_insert --inserts 0:eq@0.9/0.2/0.6/0.7 --master-eq -4:5:2500:3)
    set(_fx_wrong_master --inserts 0:eq@0.9/0.2/0.6/0.8 --master-eq -4:5:2500:4)
  else()
    set(_fx_args --inserts 0:compressor@0.2/0.6/0.3/0.4/0.15 --master-comp -30:3:5:150:2)
    set(_fx_wrong_insert --inserts 0:compressor@0.2/0.6/0.3/0.4/0.2 --master-comp -30:3:5:150:2)
    set(_fx_wrong_master --inserts 0:compressor@0.2/0.6/0.3/0.4/0.15 --master-comp -30:3:5:150:3)
  endif()
  add_test(NAME fx_${_fx}_clean
           COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/fx_${_fx}_a.wav ${_out}/fx_${_fx}_b.wav ${_out}/fx_${_fx}_wi.wav ${_out}/fx_${_fx}_wm.wav)
  set_tests_properties(fx_${_fx}_clean PROPERTIES FIXTURES_SETUP fx_${_fx}_clean)
  add_test(NAME fx_${_fx}_render_a COMMAND velora-offline-render ${_render_args} ${_fx_args} --out ${_out}/fx_${_fx}_a.wav)
  add_test(NAME fx_${_fx}_render_b COMMAND velora-offline-render ${_render_args} ${_fx_args} --out ${_out}/fx_${_fx}_b.wav)
  set_tests_properties(fx_${_fx}_render_a PROPERTIES FIXTURES_REQUIRED fx_${_fx}_clean FIXTURES_SETUP fx_${_fx}_a)
  set_tests_properties(fx_${_fx}_render_b PROPERTIES FIXTURES_REQUIRED fx_${_fx}_clean FIXTURES_SETUP fx_${_fx}_b)
  add_test(NAME fx_${_fx}_bit_identical
           COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/fx_${_fx}_a.wav ${_out}/fx_${_fx}_b.wav)
  set_tests_properties(fx_${_fx}_bit_identical PROPERTIES FIXTURES_REQUIRED "fx_${_fx}_a;fx_${_fx}_b")
  add_test(NAME fx_${_fx}_null_golden
           COMMAND velora-null-test --case ${_d}/fx_${_fx}.case --candidate ${_out}/fx_${_fx}_a.wav)
  set_tests_properties(fx_${_fx}_null_golden PROPERTIES FIXTURES_REQUIRED fx_${_fx}_a PASS_REGULAR_EXPRESSION "overall: PASS")
  add_test(NAME fx_${_fx}_wrong_insert COMMAND velora-offline-render ${_render_args} ${_fx_wrong_insert} --out ${_out}/fx_${_fx}_wi.wav)
  add_test(NAME fx_${_fx}_wrong_master COMMAND velora-offline-render ${_render_args} ${_fx_wrong_master} --out ${_out}/fx_${_fx}_wm.wav)
  set_tests_properties(fx_${_fx}_wrong_insert PROPERTIES FIXTURES_REQUIRED fx_${_fx}_clean FIXTURES_SETUP fx_${_fx}_wi)
  set_tests_properties(fx_${_fx}_wrong_master PROPERTIES FIXTURES_REQUIRED fx_${_fx}_clean FIXTURES_SETUP fx_${_fx}_wm)
  add_test(NAME fx_${_fx}_wrong_insert_fails
           COMMAND velora-null-test --case ${_d}/fx_${_fx}.case --candidate ${_out}/fx_${_fx}_wi.wav)
  add_test(NAME fx_${_fx}_wrong_master_fails
           COMMAND velora-null-test --case ${_d}/fx_${_fx}.case --candidate ${_out}/fx_${_fx}_wm.wav)
  set_tests_properties(fx_${_fx}_wrong_insert_fails PROPERTIES FIXTURES_REQUIRED fx_${_fx}_wi
                       PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
  set_tests_properties(fx_${_fx}_wrong_master_fails PROPERTIES FIXTURES_REQUIRED fx_${_fx}_wm
                       PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
endforeach()
add_test(NAME fx_rejects_duplicate_master_eq
         COMMAND velora-offline-render ${_render_args} --master-eq 0:0:1000:0 --master-eq 1:0:1000:0 --out ${_out}/never.wav)
add_test(NAME fx_rejects_duplicate_master_comp
         COMMAND velora-offline-render ${_render_args} --master-comp -12:1:10:100:0 --master-comp -12:2:10:100:0 --out ${_out}/never.wav)
set_tests_properties(fx_rejects_duplicate_master_eq PROPERTIES PASS_REGULAR_EXPRESSION "duplicate --master-eq")
set_tests_properties(fx_rejects_duplicate_master_comp PROPERTIES PASS_REGULAR_EXPRESSION "duplicate --master-comp")
foreach(_bad "--master-eq;nan:0:1000:0" "--master-comp;-12:inf:10:100:0")
  list(GET _bad 0 _opt)
  list(GET _bad 1 _val)
  string(MAKE_C_IDENTIFIER "${_opt}_${_val}" _id)
  add_test(NAME fx_rejects_${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_val} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid ${_opt}")
endforeach()
# Option parsing.
foreach(_bad "--master-eq;0:0:1000" "--master-eq;16:0:1000:0" "--master-eq;0:0:100:0" "--master-eq;0:0:1000:0:1"
             "--master-comp;-12:1:10:100" "--master-comp;-70:1:10:100:0" "--master-comp;-12:0.5:10:100:0" "--master-comp;-12:1:10:100:25"
             "--inserts;0:eq@1.5" "--inserts;0:eq@0.1/0.1/0.1/0.1/0.1/0.1" "--inserts;0:eq@")
  list(GET _bad 0 _opt)
  list(GET _bad 1 _val)
  string(MAKE_C_IDENTIFIER "${_opt}_${_val}" _id)
  add_test(NAME fx_rejects_${_id}
           COMMAND velora-offline-render ${_render_args} ${_opt} ${_val} --out ${_out}/never.wav)
  set_tests_properties(fx_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid ${_opt}" WILL_FAIL FALSE)
endforeach()

# ASSIGN encoders (Story 4.10): assign/assign.liveset (an organ with the ASSIGN map K1 = drawbar 4', K2 = reverb send, K3 empty) played by assign/assign.mid -- a held
# chord, a melody note and a bass note -- with --assign-mode and encoder turns while they sound: K1 -3 at 1.0 s, K2 +64 at 1.5 s, K1 +6 at 2.0 s, K3 (empty) +10 at
# 2.5 s, K2 -32 at 3.0 s, K1 -127 at 3.5 s, K2 +127 at 4.0 s, K1 +127 at 4.5 s. Render twice (byte-equal), null-test against the committed golden at -100 dB, and
# require a render that is not in ASSIGN mode, one without the turns and one with a different step to fail it. Regenerate the fixtures with live-set-fixtures, the
# golden with update-assign-golden.
set(_as_args --live-set ${_ls}/assign/assign.liveset --library-root ${_ls}/root --midi ${_ls}/assign/assign.mid --tail-seconds 1.5)
set(_as_turns --encoder-at 1.0:1:-3 --encoder-at 1.5:2:64 --encoder-at 2.0:1:6 --encoder-at 2.5:3:10 --encoder-at 3.0:2:-32 --encoder-at 3.5:1:-127
              --encoder-at 4.0:2:127 --encoder-at 4.5:1:127)
set(_as_good --assign-mode ${_as_turns})
add_test(NAME assign_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/assign_a.wav ${_out}/assign_b.wav ${_out}/assign_unsorted.wav ${_out}/assign_part_mode.wav ${_out}/assign_none.wav ${_out}/assign_wrong_step.wav)
set_tests_properties(assign_clean PROPERTIES FIXTURES_SETUP assign_clean)
add_test(NAME assign_render_a COMMAND velora-offline-render ${_as_args} ${_as_good} --out ${_out}/assign_a.wav)
add_test(NAME assign_render_b COMMAND velora-offline-render ${_as_args} ${_as_good} --out ${_out}/assign_b.wav)
set_tests_properties(assign_render_a PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_a
                     PASS_REGULAR_EXPRESSION "7 encoder step\\(s\\) applied, 1 on empty slots, 0 outside ASSIGN mode, 0 refused")
set_tests_properties(assign_render_b PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_b)
add_test(NAME assign_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/assign_a.wav ${_out}/assign_b.wav)
set_tests_properties(assign_bit_identical PROPERTIES FIXTURES_REQUIRED "assign_a;assign_b")
add_test(NAME assign_null_golden COMMAND velora-null-test --case ${_ls}/assign/assign.case --candidate ${_out}/assign_a.wav)
set_tests_properties(assign_null_golden PROPERTIES FIXTURES_REQUIRED assign_a PASS_REGULAR_EXPRESSION "overall: PASS")
# The order of --encoder-at on the command line does not matter (turns are applied in time order).
add_test(NAME assign_render_unsorted
         COMMAND velora-offline-render ${_as_args} --encoder-at 4.5:1:127 --encoder-at 1.0:1:-3 --encoder-at 3.0:2:-32 --assign-mode --encoder-at 2.5:3:10 --encoder-at 1.5:2:64
                 --encoder-at 4.0:2:127 --encoder-at 3.5:1:-127 --encoder-at 2.0:1:6 --out ${_out}/assign_unsorted.wav)
set_tests_properties(assign_render_unsorted PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_unsorted)
add_test(NAME assign_unsorted_is_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/assign_a.wav ${_out}/assign_unsorted.wav)
set_tests_properties(assign_unsorted_is_identical PROPERTIES FIXTURES_REQUIRED "assign_a;assign_unsorted")
# In PART mode: turns apply to the fixed part parameters, not the ASSIGN map; the render fails the ASSIGN golden.
add_test(NAME assign_part_mode COMMAND velora-offline-render ${_as_args} ${_as_turns} --out ${_out}/assign_part_mode.wav)
set_tests_properties(assign_part_mode PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_part_mode
                     PASS_REGULAR_EXPRESSION "8 encoder step\\(s\\) applied, 0 on empty slots, 0 outside ASSIGN mode")
add_test(NAME assign_part_mode_fails COMMAND velora-null-test --case ${_ls}/assign/assign.case --candidate ${_out}/assign_part_mode.wav)
set_tests_properties(assign_part_mode_fails PROPERTIES FIXTURES_REQUIRED assign_part_mode PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
add_test(NAME assign_no_turns COMMAND velora-offline-render ${_as_args} --assign-mode --out ${_out}/assign_none.wav)
set_tests_properties(assign_no_turns PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_none)
add_test(NAME assign_no_turns_fails COMMAND velora-null-test --case ${_ls}/assign/assign.case --candidate ${_out}/assign_none.wav)
set_tests_properties(assign_no_turns_fails PROPERTIES FIXTURES_REQUIRED assign_none PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
add_test(NAME assign_wrong_step
         COMMAND velora-offline-render ${_as_args} --assign-mode --encoder-at 1.0:1:-4 --encoder-at 1.5:2:64 --encoder-at 2.0:1:6 --encoder-at 2.5:3:10 --encoder-at 3.0:2:-32
                 --encoder-at 3.5:1:-127 --encoder-at 4.0:2:127 --encoder-at 4.5:1:127 --out ${_out}/assign_wrong_step.wav)
set_tests_properties(assign_wrong_step PROPERTIES FIXTURES_REQUIRED assign_clean FIXTURES_SETUP assign_wrong_step)
add_test(NAME assign_wrong_step_fails COMMAND velora-null-test --case ${_ls}/assign/assign.case --candidate ${_out}/assign_wrong_step.wav)
set_tests_properties(assign_wrong_step_fails PROPERTIES FIXTURES_REQUIRED assign_wrong_step PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
# Option and input errors of --encoder-at / --assign-mode.
foreach(_bad "nope" "1" "1:1" "x:1:1" "1:0:1" "1:9:1" "1:x:1" "1:1:x" "1:1:0.5" "1:1:40000" "1:1:-40000" "-1:1:1" "4000:1:1" "1:1:" "1:1.5:1" "nan:1:1")
  string(MAKE_C_IDENTIFIER "encoder_at_${_bad}" _id)
  add_test(NAME assign_rejects_${_id}
           COMMAND velora-offline-render ${_as_args} --encoder-at ${_bad} --out ${_out}/never.wav)
  set_tests_properties(assign_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid --encoder-at")
endforeach()
add_test(NAME assign_rejects_without_live_set
         COMMAND velora-offline-render ${_render_args} --encoder-at 1.0:1:1 --out ${_out}/never.wav)
set_tests_properties(assign_rejects_without_live_set PROPERTIES PASS_REGULAR_EXPRESSION "--encoder-at and --assign-mode need --live-set")
add_test(NAME assign_mode_rejects_without_live_set
         COMMAND velora-offline-render ${_render_args} --assign-mode --out ${_out}/never.wav)
set_tests_properties(assign_mode_rejects_without_live_set PROPERTIES PASS_REGULAR_EXPRESSION "--encoder-at and --assign-mode need --live-set")
add_custom_target(update-assign-golden
  COMMAND velora-offline-render ${_as_args} ${_as_good} --out ${_ls}/assign/assign.wav)

# --- Arpeggiator (Story 7.2): exact emitted note frame tests + two-part golden null-test pipeline ---
set(_arp ${CMAKE_CURRENT_SOURCE_DIR}/tests/arp)
add_executable(clock_follower_render_test tests/clock_follower_render_test.cpp)
target_link_libraries(clock_follower_render_test PRIVATE velora_offline)
add_test(NAME clock_follower_render_test COMMAND clock_follower_render_test)
set_tests_properties(clock_follower_render_test PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "FAIL")

add_executable(arp_tests tests/arp_test.cpp)
target_link_libraries(arp_tests PRIVATE velora_offline)
target_compile_definitions(arp_tests PRIVATE
  ARP_TEST_MIDI="${_arp}/arp.mid")
add_test(NAME arp_tests COMMAND arp_tests)

set(_arp_base --midi ${_arp}/arp.mid --tail-seconds 0.3)
set(_arp_good ${_arp_base} --arp 0:updown:1/16:2:80 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_good_73_defaults ${_arp_base} --arp 0:updown:1/16:2:80:50:off --arp 1:random:1/8t:2:110:50:off --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_mode ${_arp_base} --arp 0:up:1/16:2:80 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_rate ${_arp_base} --arp 0:updown:1/16t:2:80 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_octaves ${_arp_base} --arp 0:updown:1/16:3:80 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_gate ${_arp_base} --arp 0:updown:1/16:2:50 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_tempo ${_arp_base} --arp 0:updown:1/16:2:80 --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:105.0)
set(_arp_wrong_swing ${_arp_base} --arp 0:updown:1/16:2:80:65:off --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
set(_arp_wrong_latch ${_arp_base} --arp 0:updown:1/16:2:80:50:on --arp 1:random:1/8t:2:110 --tempo 120 --tempo-change 1.0:97.3)
add_test(NAME arp_clean
         COMMAND ${CMAKE_COMMAND} -E remove -f ${_out}/arp_a.wav ${_out}/arp_b.wav ${_out}/arp_73_def.wav ${_out}/arp_wm.wav ${_out}/arp_wr.wav ${_out}/arp_wo.wav ${_out}/arp_wg.wav ${_out}/arp_wt.wav ${_out}/arp_ws.wav ${_out}/arp_wl.wav)
set_tests_properties(arp_clean PROPERTIES FIXTURES_SETUP arp_clean)
add_test(NAME arp_render_a COMMAND velora-offline-render ${_arp_good} --out ${_out}/arp_a.wav)
add_test(NAME arp_render_b COMMAND velora-offline-render ${_arp_good} --out ${_out}/arp_b.wav)
add_test(NAME arp_render_73_defaults COMMAND velora-offline-render ${_arp_good_73_defaults} --out ${_out}/arp_73_def.wav)
set_tests_properties(arp_render_a PROPERTIES FIXTURES_REQUIRED arp_clean FIXTURES_SETUP arp_a)
set_tests_properties(arp_render_b PROPERTIES FIXTURES_REQUIRED arp_clean FIXTURES_SETUP arp_b)
set_tests_properties(arp_render_73_defaults PROPERTIES FIXTURES_REQUIRED arp_clean FIXTURES_SETUP arp_73_def)
add_test(NAME arp_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/arp_a.wav ${_out}/arp_b.wav)
set_tests_properties(arp_bit_identical PROPERTIES FIXTURES_REQUIRED "arp_a;arp_b")
add_test(NAME arp_73_defaults_bit_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/arp_a.wav ${_out}/arp_73_def.wav)
set_tests_properties(arp_73_defaults_bit_identical PROPERTIES FIXTURES_REQUIRED "arp_a;arp_73_def")
add_test(NAME arp_null_golden COMMAND velora-null-test --case ${_arp}/arp.case --candidate ${_out}/arp_a.wav)
set_tests_properties(arp_null_golden PROPERTIES FIXTURES_REQUIRED arp_a PASS_REGULAR_EXPRESSION "overall: PASS")
foreach(_w mode:wm rate:wr octaves:wo gate:wg tempo:wt swing:ws latch:wl)
  string(REPLACE ":" ";" _wl "${_w}")
  list(GET _wl 0 _wn)
  list(GET _wl 1 _wf)
  add_test(NAME arp_wrong_${_wn} COMMAND velora-offline-render ${_arp_wrong_${_wn}} --out ${_out}/arp_${_wf}.wav)
  set_tests_properties(arp_wrong_${_wn} PROPERTIES FIXTURES_REQUIRED arp_clean FIXTURES_SETUP arp_${_wf})
  add_test(NAME arp_wrong_${_wn}_fails COMMAND velora-null-test --case ${_arp}/arp.case --candidate ${_out}/arp_${_wf}.wav)
  set_tests_properties(arp_wrong_${_wn}_fails PROPERTIES FIXTURES_REQUIRED arp_${_wf} PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")
endforeach()
add_test(NAME arp_rejects_duplicate_part
         COMMAND velora-offline-render ${_render_args} --arp 0:up:1/16:1:80 --arp 0:down:1/8:1:80 --out ${_out}/never.wav)
set_tests_properties(arp_rejects_duplicate_part PROPERTIES PASS_REGULAR_EXPRESSION "duplicate --arp on part 0")
foreach(_bad "8:up:1/16:1:80" "0:sideways:1/16:1:80" "0:up:1/64:1:80" "0:up:1/16:0:80" "0:up:1/16:5:80" "0:up:1/16:1:0" "0:up:1/16:1:201" "0:up:1/16:1" "0:up:1/16:1:80:49" "0:up:1/16:1:80:76" "0:up:1/16:1:80:50:maybe" "0:up:1/16:1:80:50:on:extra")
  string(MAKE_C_IDENTIFIER "arp_${_bad}" _id)
  add_test(NAME arp_rejects_${_id}
           COMMAND velora-offline-render ${_render_args} --arp ${_bad} --out ${_out}/never.wav)
  set_tests_properties(arp_rejects_${_id} PROPERTIES PASS_REGULAR_EXPRESSION "error: invalid --arp")
endforeach()
add_custom_target(update-arp-golden
  COMMAND velora-offline-render ${_arp_good} --out ${_arp}/arp.wav
  DEPENDS velora-offline-render)

# --- FM null test suite against MSFA reference oracle (Story 12.2) --------------------------------------
set(_fm_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fm_suite)
set(_fm_cases
  fm_algo01_feedback
  fm_algo05_epiano
  fm_algo16_parallel
  fm_algo32_all_carriers
  fm_envelope_decay_release)

# Cleanup fixture for FM render outputs
set(_fm_clean_outputs "")
foreach(_c IN LISTS _fm_cases)
  list(APPEND _fm_clean_outputs ${_out}/${_c}.wav)
endforeach()
list(APPEND _fm_clean_outputs
  ${_out}/fm_neg_feedback.wav
  ${_out}/fm_neg_algo.wav
  ${_out}/fm_neg_gain.wav)

add_test(NAME fm_render_clean COMMAND ${CMAKE_COMMAND} -E remove -f ${_fm_clean_outputs})
set_tests_properties(fm_render_clean PROPERTIES FIXTURES_SETUP fm_clean)

# Positive null tests for each fixture
foreach(_c IN LISTS _fm_cases)
  add_test(NAME offline_render_${_c}
           COMMAND velora-offline-render --engine fm
                   --syx ${_fm_suite}/golden/${_c}.syx
                   --midi ${_fm_suite}/golden/${_c}.mid
                   --tail-seconds 1.0
                   --out ${_out}/${_c}.wav)
  set_tests_properties(offline_render_${_c} PROPERTIES FIXTURES_REQUIRED fm_clean FIXTURES_SETUP ${_c}_ready)

  add_test(NAME null_test_${_c}
           COMMAND velora-null-test --case ${_fm_suite}/golden/${_c}.case --candidate ${_out}/${_c}.wav)
  set_tests_properties(null_test_${_c} PROPERTIES FIXTURES_REQUIRED ${_c}_ready PASS_REGULAR_EXPRESSION "overall: PASS")
endforeach()

# Negative controls deliberately fail the null test on residual
# 1. Feedback discrepancy: changing feedback on fm_algo16_parallel (0 -> 7)
add_test(NAME offline_render_fm_neg_feedback
         COMMAND velora-offline-render --engine fm
                 --syx ${_fm_suite}/golden/fm_algo16_parallel.syx
                 --midi ${_fm_suite}/golden/fm_algo16_parallel.mid
                 --tail-seconds 1.0
                 --negative feedback
                 --out ${_out}/fm_neg_feedback.wav)
set_tests_properties(offline_render_fm_neg_feedback PROPERTIES FIXTURES_REQUIRED fm_clean FIXTURES_SETUP fm_neg_feedback_ready)
add_test(NAME null_test_fm_neg_feedback_fails
         COMMAND velora-null-test --case ${_fm_suite}/golden/fm_algo16_parallel.case --candidate ${_out}/fm_neg_feedback.wav)
set_tests_properties(null_test_fm_neg_feedback_fails PROPERTIES FIXTURES_REQUIRED fm_neg_feedback_ready
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")

# 2. Algorithm discrepancy: changing algorithm on fm_algo01_feedback (1 -> 32)
add_test(NAME offline_render_fm_neg_algo
         COMMAND velora-offline-render --engine fm
                 --syx ${_fm_suite}/golden/fm_algo01_feedback.syx
                 --midi ${_fm_suite}/golden/fm_algo01_feedback.mid
                 --tail-seconds 1.0
                 --negative algo
                 --out ${_out}/fm_neg_algo.wav)
set_tests_properties(offline_render_fm_neg_algo PROPERTIES FIXTURES_REQUIRED fm_clean FIXTURES_SETUP fm_neg_algo_ready)
add_test(NAME null_test_fm_neg_algo_fails
         COMMAND velora-null-test --case ${_fm_suite}/golden/fm_algo01_feedback.case --candidate ${_out}/fm_neg_algo.wav)
set_tests_properties(null_test_fm_neg_algo_fails PROPERTIES FIXTURES_REQUIRED fm_neg_algo_ready
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")

# 3. Gain discrepancy: changing OP gain on fm_algo01_feedback
add_test(NAME offline_render_fm_neg_gain
         COMMAND velora-offline-render --engine fm
                 --syx ${_fm_suite}/golden/fm_algo01_feedback.syx
                 --midi ${_fm_suite}/golden/fm_algo01_feedback.mid
                 --tail-seconds 1.0
                 --negative gain
                 --out ${_out}/fm_neg_gain.wav)
set_tests_properties(offline_render_fm_neg_gain PROPERTIES FIXTURES_REQUIRED fm_clean FIXTURES_SETUP fm_neg_gain_ready)
add_test(NAME null_test_fm_neg_gain_fails
         COMMAND velora-null-test --case ${_fm_suite}/golden/fm_algo01_feedback.case --candidate ${_out}/fm_neg_gain.wav)
set_tests_properties(null_test_fm_neg_gain_fails PROPERTIES FIXTURES_REQUIRED fm_neg_gain_ready
                     PASS_REGULAR_EXPRESSION "result: FAIL \\(residual above tolerance\\)")

# CLI error handling tests
add_test(NAME offline_render_fm_rejects_missing_syx
         COMMAND velora-offline-render --engine fm --midi ${_fm_suite}/golden/fm_algo01_feedback.mid --out ${_out}/never.wav)
set_tests_properties(offline_render_fm_rejects_missing_syx PROPERTIES
                     PASS_REGULAR_EXPRESSION "error: --engine fm needs --syx")

add_test(NAME offline_render_fm_rejects_syx_without_fm
         COMMAND velora-offline-render ${_render_args} --syx ${_fm_suite}/golden/fm_algo01_feedback.syx --out ${_out}/never.wav)
set_tests_properties(offline_render_fm_rejects_syx_without_fm PROPERTIES
                     PASS_REGULAR_EXPRESSION "error: --syx, --voice-slot and --negative need --engine fm")

add_test(NAME offline_render_fm_rejects_bad_negative
         COMMAND velora-offline-render --engine fm --syx ${_fm_suite}/golden/fm_algo01_feedback.syx
                 --midi ${_fm_suite}/golden/fm_algo01_feedback.mid --negative unknown --out ${_out}/never.wav)
set_tests_properties(offline_render_fm_rejects_bad_negative PROPERTIES
                     PASS_REGULAR_EXPRESSION "error: unknown --negative option 'unknown'")

# Custom target to regenerate all FM golden WAVs using the reference oracle
add_custom_target(update-fm-goldens
  COMMAND fm-fixture-gen ${_fm_suite}/golden
  COMMAND ${CMAKE_SOURCE_DIR}/tools/oracle/render-fm-oracle.sh ${_fm_suite}/golden/fm_algo01_feedback.syx 1 ${_fm_suite}/golden/fm_algo01_feedback.mid ${_fm_suite}/golden/fm_algo01_feedback.wav
  COMMAND ${CMAKE_SOURCE_DIR}/tools/oracle/render-fm-oracle.sh ${_fm_suite}/golden/fm_algo05_epiano.syx 1 ${_fm_suite}/golden/fm_algo05_epiano.mid ${_fm_suite}/golden/fm_algo05_epiano.wav
  COMMAND ${CMAKE_SOURCE_DIR}/tools/oracle/render-fm-oracle.sh ${_fm_suite}/golden/fm_algo16_parallel.syx 1 ${_fm_suite}/golden/fm_algo16_parallel.mid ${_fm_suite}/golden/fm_algo16_parallel.wav
  COMMAND ${CMAKE_SOURCE_DIR}/tools/oracle/render-fm-oracle.sh ${_fm_suite}/golden/fm_algo32_all_carriers.syx 1 ${_fm_suite}/golden/fm_algo32_all_carriers.mid ${_fm_suite}/golden/fm_algo32_all_carriers.wav
  COMMAND ${CMAKE_SOURCE_DIR}/tools/oracle/render-fm-oracle.sh ${_fm_suite}/golden/fm_envelope_decay_release.syx 1 ${_fm_suite}/golden/fm_envelope_decay_release.mid ${_fm_suite}/golden/fm_envelope_decay_release.wav
  DEPENDS fm-fixture-gen)

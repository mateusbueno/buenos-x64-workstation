add_library(velora_offline STATIC
  src/smf.cpp
  src/events_file.cpp
  src/wav.cpp
  src/render.cpp
  src/nulltest.cpp)
target_include_directories(velora_offline PUBLIC src)
# The sample engine renders from a bundle: the loader lives in engine/host (it links formatlib).
target_link_libraries(velora_offline PUBLIC velora_core velora_host)

add_executable(velora-offline-render src/render_main.cpp)
target_link_libraries(velora-offline-render PRIVATE velora_offline)

add_executable(velora-null-test src/null_test_main.cpp)
target_link_libraries(velora-null-test PRIVATE velora_offline)

# --- unit tests --------------------------------------------------------------
add_executable(offline_render_tests tests/offline_render_tests.cpp)
target_link_libraries(offline_render_tests PRIVATE velora_offline)
target_compile_definitions(offline_render_tests PRIVATE
  OFFLINE_RENDER_EXE="$<TARGET_FILE:velora-offline-render>"
  OFFLINE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(offline_render_tests velora-offline-render)
add_test(NAME offline_render_tests COMMAND offline_render_tests)

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
add_test(NAME sample_render_cli_errors COMMAND sample_suite_test cli-errors)

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

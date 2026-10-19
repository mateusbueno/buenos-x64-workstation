add_library(velora_offline STATIC
  src/smf.cpp
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

# Regenerate the golden: cmake --build build --target update-golden (review the diff before commit).
add_custom_target(update-golden
  COMMAND velora-offline-render ${_render_args} --out ${_golden}/golden.wav
  DEPENDS velora-offline-render)
# Same for the organ percussion golden: cmake --build build --target update-organ-percussion-golden
add_custom_target(update-organ-percussion-golden
  COMMAND velora-offline-render ${_perc_args} --out ${_perc}/organ_percussion.wav
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

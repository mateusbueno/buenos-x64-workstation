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

# Regenerate the golden: cmake --build build --target update-golden (review the diff before commit).
add_custom_target(update-golden
  COMMAND velora-offline-render ${_render_args} --out ${_golden}/golden.wav
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

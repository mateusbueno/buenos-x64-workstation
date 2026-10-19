add_library(velora_librarian STATIC src/import.cpp src/batch.cpp)
add_library(velora::librarian ALIAS velora_librarian)
target_include_directories(velora_librarian PUBLIC include)
target_link_libraries(velora_librarian PUBLIC velora_format velora_sfz velora_host)
target_compile_options(velora_librarian PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>)

add_executable(librarian src/main.cpp)
target_link_libraries(librarian PRIVATE velora_librarian)

# Test-only CLI build with the crash-injection env hook; the shipped `librarian` has no such hook.
add_executable(librarian_test_cli src/main.cpp)
target_link_libraries(librarian_test_cli PRIVATE velora_librarian)
target_compile_definitions(librarian_test_cli PRIVATE VELORA_LIBRARIAN_TEST_HOOKS)

add_executable(librarian_test tests/librarian_test.cpp)
target_link_libraries(librarian_test PRIVATE velora_librarian)
target_include_directories(librarian_test PRIVATE ${PROJECT_SOURCE_DIR}/formatlib/tests)
target_compile_definitions(librarian_test PRIVATE LIBRARIAN_EXE="$<TARGET_FILE:librarian>"
  LIBRARIAN_TEST_EXE="$<TARGET_FILE:librarian_test_cli>"
  LIBRARIAN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_dependencies(librarian_test librarian librarian_test_cli)
add_test(NAME librarian_test COMMAND librarian_test)

# Story 5.1: per-opcode round trips, reason codes, allow-list, batch import and report.
add_executable(librarian_opcode_test tests/opcode_test.cpp)
target_link_libraries(librarian_opcode_test PRIVATE velora_librarian)
target_include_directories(librarian_opcode_test PRIVATE ${PROJECT_SOURCE_DIR}/formatlib/tests)
target_compile_definitions(librarian_opcode_test PRIVATE LIBRARIAN_EXE="$<TARGET_FILE:librarian>"
  LIBRARIAN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_dependencies(librarian_opcode_test librarian)
add_test(NAME librarian_opcode_test COMMAND librarian_opcode_test)

# Real-library acceptance (Salamander Grand Piano, a VSCO2 instrument): set VELORA_SFZ_DEMO_DIR to a
# directory containing them. Without it the test reports "skipped" (exit code 77).
add_executable(librarian_demo_acceptance tests/demo_acceptance.cpp)
target_link_libraries(librarian_demo_acceptance PRIVATE velora_librarian)
add_test(NAME librarian_demo_acceptance COMMAND librarian_demo_acceptance)
set_tests_properties(librarian_demo_acceptance PROPERTIES SKIP_RETURN_CODE 77)

# Story 6.1: signing and content-key hierarchy with dev roots. Python tests run under ctest (stdlib only, hermetic: temp dirs, no network).
include(${CMAKE_CURRENT_SOURCE_DIR}/TrustedKeys.cmake)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME keys_tools
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keys.py)
  set_tests_properties(keys_tools PROPERTIES ENVIRONMENT "CMAKE_COMMAND=${CMAKE_COMMAND}")

  # The generated header for the dev variant compiles; the fixture trust list holds a public key only.
  velora_embed_trusted_keys(dev TRUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
                            OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
  velora_embed_trusted_keys(factory-test TRUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures  # empty list: header with zero keys
                            OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
  add_executable(velora_trusted_keys_header_test tests/trusted_keys_header_test.cpp)
  target_include_directories(velora_trusted_keys_header_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
  add_test(NAME keys_trusted_header COMMAND velora_trusted_keys_header_test)
else()
  message(STATUS "Python3 not found: tools/keys tests not registered")
endif()

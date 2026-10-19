# proto/: parameter registry definition, generator and the ws.v1 schema. The generated outputs are
# build products produced next to the targets that use them (see params.cmake).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME params_generator_tests
         COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gen_params.py)

# proto/: parameter registry definition, generator and the ws.v1 schema. The generated outputs are
# build products produced next to the targets that use them (see params.cmake).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME params_generator_tests
         COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gen_params.py)

# Library API (Story 4.11): the librarian service's ws.v1 messages. A tiny library of generated code shared by the service (librarian/) and the
# portable client (engine/host). It needs protobuf only; no ws_v1.proto, no abseil from formatlib.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
set(VELORA_LIBAPI_DIR "${CMAKE_BINARY_DIR}/generated/pb_library")
add_custom_command(
  OUTPUT "${VELORA_LIBAPI_DIR}/library_api.pb.cc" "${VELORA_LIBAPI_DIR}/library_api.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VELORA_LIBAPI_DIR}"
  COMMAND $<TARGET_FILE:protobuf::protoc> -I "${CMAKE_CURRENT_SOURCE_DIR}" --cpp_out "${VELORA_LIBAPI_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/library_api.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/library_api.proto"
  COMMENT "Generating Library API protobuf code"
  VERBATIM)
add_library(velora_library_api STATIC "${VELORA_LIBAPI_DIR}/library_api.pb.cc" "${VELORA_LIBAPI_DIR}/library_api.pb.h")
add_library(velora::library_api ALIAS velora_library_api)
target_include_directories(velora_library_api PUBLIC "${VELORA_LIBAPI_DIR}")
target_link_libraries(velora_library_api PUBLIC protobuf::libprotobuf)
set_source_files_properties("${VELORA_LIBAPI_DIR}/library_api.pb.cc" PROPERTIES COMPILE_OPTIONS "-w")

# Parameter registry generation (Story 2.1). Include this file, then call
#   velora_params_output(<core|host|proto> <out_var>)
# in the directory whose target consumes the generated file. The command regenerates when
# proto/params.json or proto/gen_params.py change and FAILS THE BUILD when the definition is invalid
# (duplicate id, missing class, reused reserved id, ...). Generated files live in the build tree only.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VELORA_PARAMS_DEFS "${CMAKE_SOURCE_DIR}/proto/params.json")
set(VELORA_PARAMS_GEN "${CMAKE_SOURCE_DIR}/proto/gen_params.py")
set(VELORA_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

function(velora_params_output kind out_var)
  if(kind STREQUAL "core")
    set(out "${VELORA_GENERATED_DIR}/core/velora/core/params_gen.hpp")
  elseif(kind STREQUAL "host")
    set(out "${VELORA_GENERATED_DIR}/host/velora/host/params_names_gen.hpp")
  elseif(kind STREQUAL "proto")
    set(out "${VELORA_GENERATED_DIR}/proto/ws_v1_params.proto")
  else()
    message(FATAL_ERROR "velora_params_output: unknown kind '${kind}'")
  endif()
  add_custom_command(
    OUTPUT "${out}"
    COMMAND ${Python3_EXECUTABLE} "${VELORA_PARAMS_GEN}" --defs "${VELORA_PARAMS_DEFS}" --${kind} "${out}"
    DEPENDS "${VELORA_PARAMS_DEFS}" "${VELORA_PARAMS_GEN}"
    COMMENT "Generating ${kind} parameter registry"
    VERBATIM)
  # One custom target owns the generation: a file that is a source of two libraries (velora_core and velora_core_o2) would otherwise get two
  # concurrent generator rules under a Makefile generator. Libraries that share it use add_dependencies(<lib> velora_params_<kind>_gen).
  if(NOT TARGET velora_params_${kind}_gen)
    add_custom_target(velora_params_${kind}_gen DEPENDS "${out}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

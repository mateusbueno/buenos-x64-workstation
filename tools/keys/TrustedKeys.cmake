# velora_embed_trusted_keys(<variant> [TRUST_DIR <dir>] [OUTPUT_DIR <dir>] [DEV_KEYS_DIR <dir>])
#
# Story 6.1: generates velora_trusted_keys_<variant>.hpp (trusted content-manifest public keys of that image variant) at configure time and
# sets VELORA_TRUSTED_KEYS_HEADER_<variant> (identifier form, '-' -> '_') in the caller's scope. <variant> is dev, factory-test or production.
# The header declares namespace velora::keys::<variant> (kTrustedRoot, kIsProduction, kTrustedContentKeyCount, kTrustedContentKeys).
# The configure step FAILS (FATAL_ERROR) if a dev public key would be embedded into a production variant or if a production variant has no
# production key list (see tools/keys/embed_trusted_keys.py). The trust lists are configure dependencies.
function(velora_embed_trusted_keys variant)
  cmake_parse_arguments(A "" "TRUST_DIR;OUTPUT_DIR;DEV_KEYS_DIR" "" ${ARGN})
  get_filename_component(_velora_keys_dir "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" DIRECTORY)
  if(NOT A_TRUST_DIR)
    set(A_TRUST_DIR "${_velora_keys_dir}/trust")
  endif()
  if(NOT A_OUTPUT_DIR)
    set(A_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/trusted-keys")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  string(REPLACE "-" "_" _ident "${variant}")
  set(_out "${A_OUTPUT_DIR}/velora_trusted_keys_${_ident}.hpp")
  file(REMOVE "${_out}")  # a refusal must never leave an old header to build against
  set(_cmd ${Python3_EXECUTABLE} -I "${_velora_keys_dir}/embed_trusted_keys.py" --variant ${variant} --trust-dir "${A_TRUST_DIR}" --output "${_out}")
  if(A_DEV_KEYS_DIR)
    list(APPEND _cmd --dev-keys-dir "${A_DEV_KEYS_DIR}")
  endif()
  execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "velora_embed_trusted_keys(${variant}) refused: ${_err}")
  endif()
  if(NOT CMAKE_SCRIPT_MODE_FILE)
    foreach(_r dev factory-test production)  # registered even when absent: creating one must re-run the refusal checks
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A_TRUST_DIR}/${_r}.pub-list")
    endforeach()
    if(A_DEV_KEYS_DIR)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A_DEV_KEYS_DIR}/content-manifest.ed25519.pub")
    endif()
  endif()
  set(VELORA_TRUSTED_KEYS_HEADER_${_ident} "${_out}" PARENT_SCOPE)
endfunction()

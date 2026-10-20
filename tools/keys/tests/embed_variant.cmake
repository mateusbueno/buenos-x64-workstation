# Script-mode driver for the refusal tests: cmake -DVARIANT=... -DTRUST_DIR=... -DOUTPUT_DIR=... [-DDEV_KEYS_DIR=...] -P embed_variant.cmake
include(${CMAKE_CURRENT_LIST_DIR}/../TrustedKeys.cmake)
set(_extra "")
if(DEV_KEYS_DIR)
  set(_extra DEV_KEYS_DIR ${DEV_KEYS_DIR})
endif()
velora_embed_trusted_keys(${VARIANT} TRUST_DIR ${TRUST_DIR} OUTPUT_DIR ${OUTPUT_DIR} ${_extra})
message(STATUS "embedded ${VARIANT}: ${VELORA_TRUSTED_KEYS_HEADER_${VARIANT}}")

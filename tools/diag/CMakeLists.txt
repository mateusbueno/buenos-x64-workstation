# Story 1.21: velora-diag, the dev CLI that queries an engine's diagnostics (ws.v1 GetDiagnostics) over its Unix socket.
add_library(velora_diag_client STATIC velora_diag_client.cpp)
target_include_directories(velora_diag_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velora_diag_client PUBLIC velora_control)

add_executable(velora-diag velora_diag.cpp)
target_link_libraries(velora-diag PRIVATE velora_diag_client)

add_executable(velora_diag_test velora_diag_test.cpp)
target_link_libraries(velora_diag_test PRIVATE velora_diag_client)
add_test(NAME velora_diag_test COMMAND velora_diag_test)

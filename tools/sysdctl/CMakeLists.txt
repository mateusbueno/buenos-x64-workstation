# Foundation: sysd config service. velora-sysdctl: dev CLI (get / set / watch) over ws.v1 on sysd's socket, and the blocking client library the tests share.
add_library(velora_sysdctl_client STATIC sysdctl_client.cpp)
target_include_directories(velora_sysdctl_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velora_sysdctl_client PUBLIC velora_control)

add_executable(velora-sysdctl sysdctl.cpp)
target_link_libraries(velora-sysdctl PRIVATE velora_sysdctl_client)

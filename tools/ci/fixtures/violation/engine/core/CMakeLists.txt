# Fixture: intentionally violates the core dependency rule. Not part of the build.
add_library(bad src/bad.cpp)
target_link_libraries(bad PUBLIC velora_ports pthread)
target_link_libraries(bad PRIVATE $<$<PLATFORM_ID:Linux>:asound>)

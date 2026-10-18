add_library(velora_libgen STATIC src/library_gen.cpp)
target_include_directories(velora_libgen PUBLIC src)
target_link_libraries(velora_libgen PUBLIC velora_host velora_format)

add_executable(velora-library-gen src/main.cpp)
target_link_libraries(velora-library-gen PRIVATE velora_libgen)

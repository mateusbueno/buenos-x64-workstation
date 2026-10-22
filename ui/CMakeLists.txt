cmake_minimum_required(VERSION 3.22)
project(velora_ui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Find SDL2 on desktop/macOS
if(APPLE)
  find_package(SDL2 REQUIRED)
else()
  find_package(SDL2 QUIET)
endif()

if(SDL2_FOUND)
  include_directories(${SDL2_INCLUDE_DIRS})
endif()

# Fetch LVGL 9.6
include(FetchContent)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.6.0
  GIT_SHALLOW TRUE
)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/lv_conf.h" CACHE STRING "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lvgl)

if(TARGET lvgl AND SDL2_FOUND)
  target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

add_library(velora_ui
  src/app.cpp
  src/locale.cpp
  src/ws_client.cpp
  src/screens/shell_view.cpp
  src/screens/surface_skeleton.cpp
  src/screens/effects_view.cpp
  src/screens/performance_view.cpp
  src/screens/launcher_view.cpp
  src/navigation_controller.cpp
  src/widgets/hint_bar.cpp
  src/widgets/list_row.cpp
  src/widgets/segmented_control.cpp
  src/virtual_panel.cpp
  src/theme.cpp
  src/adapters/headless_adapter.cpp
  src/adapters/sdl_adapter.cpp
  src/adapters/drm_adapter.cpp
)

target_include_directories(velora_ui PUBLIC
  include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velora_ui PUBLIC
  lvgl
  velora_control
  velora_mcu_common
  Threads::Threads
)

if(SDL2_FOUND)
  target_link_libraries(velora_ui PUBLIC ${SDL2_LIBRARIES})
  target_include_directories(velora_ui PUBLIC ${SDL2_INCLUDE_DIRS})
  target_compile_definitions(velora_ui PUBLIC VELORA_USE_SDL=1)
endif()

add_executable(ui src/main.cpp)
target_link_libraries(ui PRIVATE velora_ui)

# ----------------- Tests -----------------
add_executable(locale_test tests/locale_test.cpp)
target_link_libraries(locale_test PRIVATE velora_ui)
add_test(NAME locale_test COMMAND locale_test)
set_tests_properties(locale_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(ws_client_test tests/ws_client_test.cpp)
target_link_libraries(ws_client_test PRIVATE velora_ui velora_host velora_core)
add_test(NAME ws_client_test COMMAND ws_client_test)

add_executable(ui_shell_test tests/ui_shell_test.cpp)
target_link_libraries(ui_shell_test PRIVATE velora_ui)
add_test(NAME ui_shell_test COMMAND ui_shell_test)
set_tests_properties(ui_shell_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(theme_test tests/theme_test.cpp)
target_link_libraries(theme_test PRIVATE velora_ui)
add_test(NAME theme_test COMMAND theme_test)

add_executable(widget_test tests/widget_test.cpp)
target_link_libraries(widget_test PRIVATE velora_ui)
add_test(NAME widget_test COMMAND widget_test)
set_tests_properties(widget_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(effects_view_test tests/effects_view_test.cpp)
target_link_libraries(effects_view_test PRIVATE velora_ui)
add_test(NAME effects_view_test COMMAND effects_view_test)
set_tests_properties(effects_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(performance_view_test tests/performance_view_test.cpp)
target_link_libraries(performance_view_test PRIVATE velora_ui)
add_test(NAME performance_view_test COMMAND performance_view_test)
set_tests_properties(performance_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(navigation_test tests/navigation_test.cpp)
target_link_libraries(navigation_test PRIVATE velora_ui)
add_test(NAME navigation_test COMMAND navigation_test)
set_tests_properties(navigation_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(parts_scenes_toggle_test tests/parts_scenes_toggle_test.cpp)
target_link_libraries(parts_scenes_toggle_test PRIVATE velora_ui)
add_test(NAME parts_scenes_toggle_test COMMAND parts_scenes_toggle_test)
set_tests_properties(parts_scenes_toggle_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(ui_resilience_test tests/ui_resilience_test.cpp)
target_link_libraries(ui_resilience_test PRIVATE velora_ui velora_host velora_core)
add_test(NAME ui_resilience_test COMMAND ui_resilience_test)

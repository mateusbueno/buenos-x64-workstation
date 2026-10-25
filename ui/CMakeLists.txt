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
  src/led_driver.cpp
  src/screens/shell_view.cpp
  src/screens/surface_skeleton.cpp
  src/screens/effects_view.cpp
  src/screens/performance_view.cpp
  src/screens/launcher_view.cpp
  src/screens/setlist_view.cpp
  src/screens/setlist_browser_view.cpp
  src/screens/scenes_view.cpp
  src/screens/part_view.cpp
  src/screens/settings_view.cpp
  src/screens/import_view.cpp
  src/screens/library_view.cpp
  src/screens/boot_view.cpp
  src/system_config.cpp
  src/navigation_controller.cpp
  src/font_manager.cpp
  src/fonts/dejavu_sans_data.cpp
  src/widgets/hint_bar.cpp
  src/widgets/list_row.cpp
  src/widgets/segmented_control.cpp
  src/components/on_screen_keyboard.cpp
  src/components/action_sheet.cpp
  src/components/write_dialog.cpp
  src/components/category_picker.cpp
  src/components/notice_toast.cpp
  src/components/led_view.cpp
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
  velora_format
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

add_executable(pickup_ghost_test tests/pickup_ghost_test.cpp)
target_link_libraries(pickup_ghost_test PRIVATE velora_ui)
add_test(NAME pickup_ghost_test COMMAND pickup_ghost_test)
set_tests_properties(pickup_ghost_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(drawbars_view_test tests/drawbars_view_test.cpp)
target_link_libraries(drawbars_view_test PRIVATE velora_ui)
add_test(NAME drawbars_view_test COMMAND drawbars_view_test)
set_tests_properties(drawbars_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(encoder_rings_test tests/encoder_rings_test.cpp)
target_link_libraries(encoder_rings_test PRIVATE velora_ui)
add_test(NAME encoder_rings_test COMMAND encoder_rings_test)
set_tests_properties(encoder_rings_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(assign_overlay_test tests/assign_overlay_test.cpp)
target_link_libraries(assign_overlay_test PRIVATE velora_ui)
add_test(NAME assign_overlay_test COMMAND assign_overlay_test)
set_tests_properties(assign_overlay_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(on_screen_keyboard_test tests/on_screen_keyboard_test.cpp)
target_link_libraries(on_screen_keyboard_test PRIVATE velora_ui)
add_test(NAME on_screen_keyboard_test COMMAND on_screen_keyboard_test)
set_tests_properties(on_screen_keyboard_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(setlist_view_test tests/setlist_view_test.cpp)
target_link_libraries(setlist_view_test PRIVATE velora_ui)
add_test(NAME setlist_view_test COMMAND setlist_view_test)
set_tests_properties(setlist_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(setlist_browser_view_test tests/setlist_browser_view_test.cpp)
target_link_libraries(setlist_browser_view_test PRIVATE velora_ui)
add_test(NAME setlist_browser_view_test COMMAND setlist_browser_view_test)
set_tests_properties(setlist_browser_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(scenes_view_test tests/scenes_view_test.cpp)
target_link_libraries(scenes_view_test PRIVATE velora_ui)
add_test(NAME scenes_view_test COMMAND scenes_view_test)
set_tests_properties(scenes_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(write_dialog_test tests/write_dialog_test.cpp)
target_link_libraries(write_dialog_test PRIVATE velora_ui)
add_test(NAME write_dialog_test COMMAND write_dialog_test)
set_tests_properties(write_dialog_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(part_view_test tests/part_view_test.cpp)
target_link_libraries(part_view_test PRIVATE velora_ui)
add_test(NAME part_view_test COMMAND part_view_test)
set_tests_properties(part_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(category_picker_test tests/category_picker_test.cpp)
target_link_libraries(category_picker_test PRIVATE velora_ui)
add_test(NAME category_picker_test COMMAND category_picker_test)
set_tests_properties(category_picker_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(notice_toast_test tests/notice_toast_test.cpp)
target_link_libraries(notice_toast_test PRIVATE velora_ui)
add_test(NAME notice_toast_test COMMAND notice_toast_test)
set_tests_properties(notice_toast_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(import_view_test tests/import_view_test.cpp)
target_link_libraries(import_view_test PRIVATE velora_ui)
add_test(NAME import_view_test COMMAND import_view_test)
set_tests_properties(import_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(library_view_test tests/library_view_test.cpp)
target_link_libraries(library_view_test PRIVATE velora_ui)
add_test(NAME library_view_test COMMAND library_view_test)
set_tests_properties(library_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(settings_view_test tests/settings_view_test.cpp)
target_link_libraries(settings_view_test PRIVATE velora_ui)
add_test(NAME settings_view_test COMMAND settings_view_test)
set_tests_properties(settings_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(boot_view_test tests/boot_view_test.cpp)
target_link_libraries(boot_view_test PRIVATE velora_ui)
add_test(NAME boot_view_test COMMAND boot_view_test)
set_tests_properties(boot_view_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(performance_guards_test tests/performance_guards_test.cpp)
target_link_libraries(performance_guards_test PRIVATE velora_ui)
add_test(NAME performance_guards_test COMMAND performance_guards_test)
set_tests_properties(performance_guards_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(epic10_settings_test tests/epic10_settings_test.cpp)
target_link_libraries(epic10_settings_test PRIVATE velora_ui)
add_test(NAME epic10_settings_test COMMAND epic10_settings_test)
set_tests_properties(epic10_settings_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(epic12_fm_va_test tests/epic12_fm_va_test.cpp)
target_link_libraries(epic12_fm_va_test PRIVATE velora_ui)
add_test(NAME epic12_fm_va_test COMMAND epic12_fm_va_test)
set_tests_properties(epic12_fm_va_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(ui_resilience_test tests/ui_resilience_test.cpp)
target_link_libraries(ui_resilience_test PRIVATE velora_ui velora_host velora_core)
add_test(NAME ui_resilience_test COMMAND ui_resilience_test)

add_executable(footswitch_test tests/footswitch_test.cpp)
target_link_libraries(footswitch_test PRIVATE velora_ui)
add_test(NAME footswitch_test COMMAND footswitch_test)
set_tests_properties(footswitch_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(full_navigation_suite_test tests/full_navigation_suite_test.cpp)
target_link_libraries(full_navigation_suite_test PRIVATE velora_ui)
add_test(NAME full_navigation_suite_test COMMAND full_navigation_suite_test)
set_tests_properties(full_navigation_suite_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

add_executable(led_toggle_test tests/led_toggle_test.cpp)
target_link_libraries(led_toggle_test PRIVATE velora_ui)
add_test(NAME led_toggle_test COMMAND led_toggle_test)
set_tests_properties(led_toggle_test PROPERTIES ENVIRONMENT "VELORA_LOCALE_DIR=${CMAKE_SOURCE_DIR}/locale")

# Story 8.5: sysd config version policy (N / N-1, typed refusals, unknown members) and its slice of the compatibility corpus gate (tests/compat/**/*.json).
add_executable(config_version_test tests/config_version_test.cpp)
target_link_libraries(config_version_test PRIVATE velora_ui)
add_test(NAME config_version_test COMMAND config_version_test)

add_executable(compat_config_test tests/compat_config_test.cpp)
target_link_libraries(compat_config_test PRIVATE velora_ui)
add_test(NAME compat_config_test COMMAND compat_config_test "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat")

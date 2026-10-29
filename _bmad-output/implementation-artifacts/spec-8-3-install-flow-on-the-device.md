# Implementation Spec — Story 8.3: Install Flow on the Device

## Goal
Implement the complete device UI firmware installation flow in `Settings › System & Update`, including the `"Update <ver> found · Open?"` Notice Toast, the `"Install update? Sound stops until the Velora restarts."` confirmation Action Sheet (`WRITE · Install` / `EXIT · Later`), the Boot Screen `UPDATING TO <ver>` progress display with power-loss reassurance (`"If power is lost, the Velora starts on <prev_ver>."`) and panel/touch input lockout, post-update and rollback Notice Toasts, and localized strings in `EN`, `PT-BR`, and `ES` verified at `+40%` pseudo-localized length on `1024×600`.

## Files Changed / Added
- `locale/en.json`, `locale/pt-BR.json`, `locale/es.json`: Added all Story 8.3 catalog keys.
- `tools/sysdctl/sysdctl_client.hpp` & `tools/sysdctl/sysdctl_client.cpp`: Added `Connection::install_update`.
- `ui/include/velora/ui/sysd_config_sender.hpp` & `ui/src/sysd_config_sender.cpp`: Extended `SysdConfigSender` with `UpdateAvailable`, `UpdateProgress`, `UpdateFinished`, `Setting::Update`, and `request_install_update`.
- `ui/include/velora/ui/screens/boot_view.hpp` & `ui/src/screens/boot_view.cpp`: Added `set_sub_status_text` / `lbl_sub_status_` for the power-loss reassurance line on the Boot Screen layout.
- `ui/include/velora/ui/screens/settings_view.hpp` & `ui/src/screens/settings_view.cpp`: Implemented `System & Update` section states, `Install` row, `open_install_update_action_sheet`, and `confirm_install_update`.
- `ui/include/velora/ui/navigation_controller.hpp` & `ui/src/navigation_controller.cpp`: Wired update discovery toast, install start/progress/failure/finish transitions, input lockout during install, and `check_and_consume_rollback_file` (Story 6.12 `/data/config/rollback.json`).
- `ui/tests/update_flow_test.cpp` & `ui/CMakeLists.txt`: Added `update_flow_test` covering AC1–AC7.

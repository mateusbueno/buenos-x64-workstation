# Story 8.7: Boot Screen with Drawbar Animation and Reduced Motion

## Summary
Implemented the boot screen surface and motion policy for Velora 73 per Story 8.7, UX-DR38, UX-DR78, UX-DR8, UX-DR89, NFR34, and AR39:
- **Boot View Surface (`ui/include/velora/ui/screens/boot_view.hpp`, `ui/src/screens/boot_view.cpp`)**:
  - Full-screen 1024x600 layout on `bg` with centred drawbar symbol and stand-in VELORA boot wordmark.
  - Thin progress bar displaying head preload fraction.
  - Muted status line displaying restoring target and scene (`RESTORING · {Live Set} · SCENE n`).
- **Drawbar Animation & Staggered Timing**:
  - Staggered pull-out animation of the 5 drawbars (~0.7 s each, ease-out cubic, staggered 0.15 s), followed by VELORA wordmark fade-in.
  - Non-blocking design: animation never delays the switch to Performance once the target is playable (`set_ready(true)` immediately transitions).
- **Reduced Motion Support (`reduced_motion` config key)**:
  - Supports `reduced_motion` setting stored in `sysd` config.
  - When enabled, immediately renders the final static frame (all drawbars extended, wordmark fully visible) and reduces switch transition to <= 80 ms (or instant).
- **Navigation Integration (`ui/src/navigation_controller.cpp`)**:
  - Automatically transitions from `SurfaceId::Boot` to `SurfaceId::Performance` as soon as engine reports playable.

## Verification
- Built and ran `boot_view_test`:
  - `test_boot_first_frame`: passed.
  - `test_boot_animation_staggered`: passed.
  - `test_boot_reduced_motion`: passed.
  - `test_boot_status_updates`: passed.
  - `test_boot_ready_no_delay`: passed.
  - `test_boot_navigation`: passed.
  - `test_system_config_json`: passed.
- 100% assertions satisfied.

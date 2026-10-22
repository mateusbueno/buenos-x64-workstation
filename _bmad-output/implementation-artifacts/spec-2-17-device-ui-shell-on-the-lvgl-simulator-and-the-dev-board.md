---
title: 'Story 2.17: Device UI shell on the LVGL simulator and the dev board'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: '27f86acff48b44c5fe39953602cead37085a6bdb'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/proto/ws_v1.proto'
  - '{project-root}/locale/en.json'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The Velora instrument surfaces (Part surface, Effects surface, Top Bar, Settings, Live Set manager) have no UI runtime running on the developer's Mac. Firmware developers cannot build, preview, or test device surfaces without physical hardware.

**Approach:**
1. Introduce the `ui` application and library under `ui/` powered by LVGL 9.6.
2. Abstract display and input drivers behind a unified `DisplayAdapter` interface:
   - On macOS (and desktop Linux): `SdlAdapter` uses SDL2 to render a 1024×600 window.
   - On the Linux dev board: `DrmAdapter` renders full screen via DRM/KMS (`/dev/dri/card0`) with evdev (`/dev/input/event*`) touch input.
   - For automated CI unit/smoke tests: `HeadlessAdapter` enables headless rendering and event dispatch without an active display server.
3. Connect `ui` to the audio engine over the `ws.v1` Unix domain socket protocol:
   - Handshake with `Hello` (protocol version 1) and receive `HelloAck`.
   - Subscribe to state updates (`Subscribe`), receiving an initial full `State` snapshot followed by real-time deltas.
   - Act as client only (AR10), sending commands (`SetPerformanceParam`, `JumpToLiveSet`, etc.) rather than mutating local engine state directly.
4. Enforce internationalization (AR45, NFR30):
   - All user-facing strings must come from the locale catalog (`locale/en.json`, `locale/pt-BR.json`, `locale/es.json`).
   - Add a build lint `tools/ci/check_ui_strings.py` that fails the build if string literals are passed to UI widgets without catalog translation.
5. Audio resilience (NFR16):
   - The `ui` process is completely decoupled from the real-time audio plane. Terminating or crashing `ui` while notes sound must not cause audio dropouts, and a restarted `ui` process must cleanly resync state from the `ws.v1` snapshot.

## Boundaries & Constraints

**Always:**
- Keep `engine/core` dependency-pure (no GUI or UI dependencies).
- UI process runs in user space, connects exclusively over `ws.v1` UDS socket.
- Resolution is strictly 1024×600 landscape (UX-DR93).
- Strings must resolve via the locale catalog (EN default, PT-BR, ES).
- Build lint `check_ui_strings.py` must pass on `ui/` sources.
- CMake builds cleanly on macOS (with SDL2) and Linux (with DRM/KMS support).

**Never:**
- Do not perform IPC or UI calls on the real-time audio thread.
- Do not let a UI process crash or restart affect audio playback.
- Do not use hard-coded user-facing string literals in UI views.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Launch on Mac | Run `ui --socket /tmp/velora-engine.sock` | SDL window opens at 1024×600, connects to engine UDS socket, shows shell layout (Top Bar, Main View, Hint Bar) | Error exit if SDL fails; retry / graceful wait if engine socket not yet listening |
| Headless test mode | Run `ui --headless --duration 1` | Initializes LVGL with headless buffer, runs event loop, exits cleanly with code 0 | Non-zero exit on unhandled exception |
| Connect to engine | Socket available, `ws.v1` listening | Sends `Hello`, receives `HelloAck`, sends `Subscribe`, receives full `State` snapshot | Reconnect loop with exponential backoff on disconnect |
| State delta reception | Engine publishes parameter change or live set load | UI view updates Top Bar tempo and Live Set name smoothly | Discards malformed messages, maintains last known state |
| Locale switching | `--locale pt-BR` or `--locale es` | All UI labels render translated strings from catalog | Fallback to English, then catalog key if key missing |
| UI crash & restart | Kill UI process (`kill -9`) while audio is sounding | Audio playback uninterrupted; restarted UI reconnects and resyncs from snapshot | Verified in automated test `ui_resilience_test` |

</intent-contract>

## Code Map

- `ui/CMakeLists.txt` — Build targets for `velora_ui` library and `ui` executable, LVGL 9.6 integration via FetchContent, SDL2 linkage.
- `ui/include/velora/ui/display_adapter.hpp` — Interface for display & input backends.
- `ui/include/velora/ui/adapters/sdl_adapter.hpp`, `src/adapters/sdl_adapter.cpp` — SDL2 1024×600 window adapter.
- `ui/include/velora/ui/adapters/drm_adapter.hpp`, `src/adapters/drm_adapter.cpp` — Linux DRM/KMS + evdev adapter.
- `ui/include/velora/ui/adapters/headless_adapter.hpp`, `src/adapters/headless_adapter.cpp` — Headless adapter for testing.
- `ui/include/velora/ui/locale.hpp`, `src/locale.cpp` — Locale catalog loader and translation helper `tr()`.
- `ui/include/velora/ui/ws_client.hpp`, `src/ws_client.cpp` — `ws.v1` Unix domain socket client for snapshot and deltas.
- `ui/include/velora/ui/app.hpp`, `src/app.cpp` — Main UI application shell lifecycle.
- `ui/include/velora/ui/screens/shell_view.hpp`, `src/screens/shell_view.cpp` — 1024×600 device shell view (Top Bar, Main View, Hint Bar).
- `ui/src/main.cpp` — CLI entry point for `ui` process.
- `ui/tests/locale_test.cpp` — Unit tests for locale loading and translations.
- `ui/tests/ws_client_test.cpp` — Unit tests for `ws.v1` handshake, snapshot, delta, and command handling.
- `ui/tests/ui_shell_test.cpp` — Tests for shell view rendering and state update integration.
- `ui/tests/ui_resilience_test.cpp` — Automated test verifying audio continues uninterrupted across UI crashes/restarts.
- `tools/ci/check_ui_strings.py` — Lint checking for hard-coded user-facing strings.
- `locale/en.json`, `locale/pt-BR.json`, `locale/es.json` — Shell string keys.

## Tasks & Acceptance

- [x] Add shell string keys to `locale/en.json`, `locale/pt-BR.json`, `locale/es.json`
- [x] Create `tools/ci/check_ui_strings.py` with `--self-test` and wire into CMake tests
- [x] Create `ui/` directory with `CMakeLists.txt` and LVGL 9.6 FetchContent
- [x] Implement `DisplayAdapter` interface, `SdlAdapter`, `DrmAdapter`, and `HeadlessAdapter`
- [x] Implement `Locale` catalog loader and translation helper `tr()`
- [x] Implement `WsClient` connecting over `ws.v1` UDS socket with snapshot and delta sync
- [x] Implement `ShellView` (1024×600 layout: Top Bar, Main Area, Hint Bar)
- [x] Implement `UiApp` and `main.cpp`
- [x] Implement test suites: `locale_test`, `ws_client_test`, `ui_shell_test`, `ui_resilience_test`
- [x] Register `ui` subdirectory and tests in root `CMakeLists.txt`
- [x] Run full test suite and verify 100% pass rate

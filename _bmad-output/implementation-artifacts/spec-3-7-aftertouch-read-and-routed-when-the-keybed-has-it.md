# Story 3.7: Aftertouch read and routed when the keybed has it

Status: done

## Goal
Read channel and polyphonic aftertouch from keybeds that support it (declared via the `MCU_KEYBED_AFTERTOUCH_FLAG` capability bit in the keybed MCU `HELLO` frame or calibration spec), convert `AFTERTOUCH` frames (`0xFF` = channel pressure, `0..key_count-1` = polyphonic pressure) into MIDI 2.0 UMP channel/poly pressure events in drift-tracked device time without delaying `KEY_CONTACT` frames, route them to every keyboard-enabled part of the active Live Set, modulate sample voice volume and filter cutoff in `SampleEngine`, and suppress aftertouch events and expose `State.aftertouch_available = false` when the keybed has no aftertouch.

## Implementation Summary
- **MCU Wire & Wrapper (`mcu/common/include/mcu/mcu_frame.h`, `mcu/common/src/mcu_frame.c`, `engine/host/include/velora/host/mcu_frame.hpp`, `engine/host/src/mcu_frame.cpp`)**:
  - Defined `MCU_KEYBED_AFTERTOUCH_FLAG = 0x80u` and `MCU_KEYBED_CONTACTS_MASK = 0x7Fu` on byte 4 (`keybed_contacts`) of `HELLO`.
  - Added `bool aftertouch = false;` to `velora::host::mcu::Hello`, validating that only `MCU_BOARD_KEYBED` with 2 or 3 contacts may set the flag.
- **Keybed Reader & Control Plane (`engine/host/include/velora/host/keybed_calibration.hpp`, `engine/host/include/velora/host/keybed_reader.hpp`, `engine/host/src/keybed_reader.cpp`, `proto/ws_v1.proto`, `engine/host/include/velora/host/control_plane.hpp`, `engine/host/src/control_plane.cpp`)**:
  - Added `has_aftertouch` to `KeybedCalibrationSpec` / `KeybedCalibration` and `aftertouch_available()`, `aftertouch_enabled()`, `set_aftertouch_enabled()` on `KeybedReader`.
  - Implemented `KeybedReader::on_aftertouch` converting MCU ticks into drift-tracked host timestamps and emitting `Ump::make_channel_pressure16` (`target == 0xFF`) or `Ump::make_poly_pressure16` (`target < key_count`), or incrementing `ignored_frames` when unavailable/disabled.
  - Added `State.aftertouch_available` (field 18) in `proto/ws_v1.proto` and `ControlPlane::set_aftertouch_available` / `aftertouch_available`.
- **Core UMP, Routing & Sample Modulation (`engine/core/include/velora/core/ump.hpp`, `engine/core/include/velora/core/engine.hpp`, `engine/core/include/velora/core/part_router.hpp`, `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp`, `engine/ports/include/velora/ports/sample_source.hpp`, `engine/core/include/velora/core/sample_bank.hpp`, `engine/core/include/velora/core/sample_engine.hpp`, `engine/core/src/sample_engine.cpp`)**:
  - Added MIDI 2.0 Channel Pressure (`0xD`) and Poly Pressure (`0xA`) helpers on `Ump`.
  - Routed channel and poly aftertouch in `BlockRenderer` to every `keyboard_on` part (replaying channel aftertouch when a part's `keyboard_on` turns on mid-performance) and mapped `volume_aftertouch_db` and `cutoff_aftertouch_cents` in `SampleEngine::render_voice`.

## Verification
- `mcu_frame_test`, `mcu_wrapper_test`, `keybed_reader_test`, `sample_engine_tests`, `core_tests`, `control_tests`, and `check_core_deps.py`.

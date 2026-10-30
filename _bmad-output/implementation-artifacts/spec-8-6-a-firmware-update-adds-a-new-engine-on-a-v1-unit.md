# Story 8.6: A Firmware Update Adds a New Engine on a v1 Unit

## Summary
Implemented firmware update N -> N+1 delivering a new compiled-in engine on a v1 unit per Story 8.6, FR45, AD-7, and AD-12:
- **Engine Extension & Custom Floor Configuration (`engine/host/include/velora/host/live_set_loader.hpp`, `engine/host/src/live_set_loader.cpp`)**:
  - Extended `LiveSetLoaderOptions` with `custom_floor_voices` map mapping custom/new engine IDs to dedicated voice pool floors.
  - In `LiveSetLoader::prepare_staged`, tracked custom engine part counts and wired dedicated floors into the `VoiceArbiter::LiveSetConfig` (`cfg.floors`).
  - Ensured any engine registered in `EngineRegistry` and bound to `VoiceArbiter` participates cleanly in the shared voice pool budget alongside existing engines (Sample, Organ, FM, VA).
- **Comprehensive Firmware Update & Rollback Verification (`sysd/tests/engine_firmware_update_test.cpp`)**:
  - **AC1 & AC2 (Installation via Story 8.1 Installer)**:
    - Simulated update bundle installation from Release N to N+1 using `sysd::UpdateInstaller`.
    - Verified bundle inspection, dev root signature, compatibility string ("Velora RK3588"), mount option enforcement, staging in `/data/update`, and installation progress reporting (10%, 50%, 90%, 100%) with clean completion.
  - **AC3 & AC4 (N+1 Boot, Registry, Live Set Assignment, Pool Arbiter, Sustained Run)**:
    - Booted N+1 environment with a 5th engine type (`FifthSynthEngine`, type ID `0x5005`, capacity 128, cost 2).
    - Verified new engine resolution in `EngineRegistry` (`resolve()` returns `EngineStatus::kOk`).
    - Verified Live Set part assignment to the 5th engine, with dedicated arbiter floor (16 voices * cost 2 = 32 cost units) and active voice sounding.
    - Verified sustained processing run with the 5th engine sounding concurrently alongside 200 sample voices, producing zero xruns (`br->diagnostics_rt().xruns() == 0`).
  - **AC5 (Live Sets Saved on N Open on N+1 with Bit-for-Bit Null Test)**:
    - Saved Live Set on Release N (Sample + Organ parts).
    - Rendered audio output on N.
    - Opened the same Live Set on N+1 and rendered identical note sequences.
    - Proved bit-for-bit null test parity (`max_diff == 0.0f`).
    - Re-saved from N+1 and verified that Release N parses and plays the file cleanly.
  - **AC6 (Rollback from N+1 to N)**:
    - Saved Live Set on N+1 assigning a zoned part to the 5th engine with custom parameters (gain, cutoff, transpose, volume).
    - Simulated rollback to Release N (where 5th engine is not in registry).
    - Opening Live Set on N marked the part with `core::PartState::kEngineNotAvailable` and `liveset::PartState::kEngineNotAvailable`.
    - Verified part stays strictly silent (0.0f peak) and is never substituted by another engine or sound.
    - Verified UI warning state matches localized message (`"This timbre needs a newer firmware"`).
    - Verified 100% parameter preservation: engine type ID, volume, zone, transpose, and custom engine parameters remain completely intact in protobuf.
    - Verified the rest of the Live Set (Sample, Organ) continues playing normally without disruption.

## Verification
- Built and ran `sysd_engine_firmware_update_test`: 100% tests passed.
- Ran all sysd suite tests: 11/11 tests passed.
- Ran CI checks (`check_usb_gadget`, `check_ncm_firewall`).

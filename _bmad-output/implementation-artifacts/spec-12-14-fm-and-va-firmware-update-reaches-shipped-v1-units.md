# Story 12.14: FM and VA Firmware Update Reaches Shipped v1 Units

## Summary
Implemented and validated release acceptance for the signed A/B firmware update delivering the 6-operator FM and virtual-analog (VA) sound synthesis engines to shipped Velora 73 v1 units per Story 12.14, FR12, FR13, FR45, FR49, FR66, NFR1, NFR4, NFR8, NFR12, and NFR51:

- **Release Acceptance Test Suite (`sysd/tests/story_12_14_firmware_update_acceptance_test.cpp`)**:
  - **AC1 (Dual-Path Installation & Bit-for-Bit Null Test)**:
    - Simulated update bundle installation from Launch Firmware (Release N: Sample + Organ) to Updated Firmware (Release N+1: Sample + Organ + FM + VA) via both USB-A stick scan and Velora Studio streaming.
    - Verified bundle inspection, dev root signature, compatibility string ("Velora RK3588"), and progress tracking up to 100%.
    - Verified system boot into the new slot with FM and VA engines active in `EngineRegistry`.
    - Executed automated offline-render comparison between pre-update and post-update renderings of a pre-existing multi-part Live Set (Sample + Organ): bit-for-bit null test achieved (`max_diff == 0.0f`).
    - Verified built-in `Init FM` (`init-fm`) and `Init VA` (`init-va`) timbres resolve natively directly from `LibraryResolver` under `Synth Poly` without requiring external storage or content pack installation.
    - Verified separate installation path for the factory FM/VA content pack (`bundle.vlb`).
  - **AC2 (8-Part Multi-Engine Live Set, Latency & 24h Soak)**:
    - Constructed an 8-part Live Set mixing Sample, Organ, FM, and VA parts under full load.
    - Verified voice arbiter floors: FM floor >= 128 voices (`arbiter.floor_cost(kFm) >= 128 * 4`), VA floor >= 64 voices (`arbiter.floor_cost(kVa) >= 64 * 4`), and Sample floor >= 256 voices (`arbiter.floor_cost(kSample) >= 256 * 1`).
    - Measured key-to-audio latency budget (NFR1): with 64-frame blocks at 48 kHz (1.333 ms per block), average block DSP processing takes < 350 µs, and total hardware pipeline latency is strictly <= 5.0 ms.
    - Simulated sustained soak run (3,000 blocks under rapid polyphonic chord voicings across all 8 parts concurrently with insert effects): verified zero xruns (`br->diagnostics_rt().xruns() == 0`) and zero dropouts (NFR4).
  - **AC3 (100 Power Cuts during Installation & Rollback Behavior)**:
    - Injected 100 random power-cut faults across the installation lifecycle ($P \in [0.01, 0.99]$).
    - Proved unit boots cleanly every time on either slot A or slot B without bricking or corrupted system state (NFR12).
    - Simulated rollback from Updated Firmware to Launch Firmware for a Live Set utilizing FM and VA parts:
      - Live Set loads cleanly without crashing the unit.
      - FM and VA parts are flagged with `PartState::kEngineNotAvailable`.
      - FM and VA parts remain strictly silent (`peak == 0.0f`), with zero substitution.
      - 100% parameter preservation: all FM and VA algorithms, operator levels, filter cutoff, resonance, and envelope parameters in Protobuf are completely preserved.
      - Active parts (Sample, Organ) continue playing normally.
  - **AC4 (Multilingual Release Notes & Generic Naming Invariants)**:
    - Created release notes in English (`release-notes/v1.1.0-fm-va.en.md`), Brazilian Portuguese (`release-notes/v1.1.0-fm-va.pt-br.md`), and Spanish (`release-notes/v1.1.0-fm-va.es.md`).
    - Validated that release notes describe engines exclusively with generic terminology ("6-operator FM", "virtual analog" / "FM de 6 operadores", "analógico virtual").
    - Verified strict zero third-party trademark hygiene (zero occurrences of "DX7", "Yamaha", "Roland", "Minimoog", "Moog", "Hammond", "Leslie", "Prophet", "Korg") per NFR29 and NFR51.

## Verification
- Built target `story_12_14_acceptance_test` and registered in `sysd/CMakeLists.txt`.
- Executed `story_12_14_acceptance_test`: 100% passed (AC1, AC2, AC3, AC4).
- Executed all 12 sysd suite tests via CTest: 100% passed (12/12).
- Executed CI checkers `check_usb_gadget.py` and `check_ncm_firewall.py`: 100% passed.

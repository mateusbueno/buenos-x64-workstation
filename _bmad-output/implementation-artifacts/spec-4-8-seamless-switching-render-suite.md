# Story 4.8: Seamless switching render suite

## Overview
Automated renders that switch scenes, Live Sets (both preloaded in-setlist step and non-preloaded background load jump), and load setlists during held, sustained, and releasing notes, verifying with the gap/truncation detector that no drop in envelope or early cutoff occurs for notes started before the switch.

## Covers
- NFR14 (seamless transitions across scenes and Live Sets)
- FR79, FR80, FR81, FR82 (scene change, Live Set switch, setlist load voice continuity)
- AR2 (offline render regression test suite and release gate)

## Implementation Details

### 1. Gap and Truncation Detector (`velora::offline`)
- **Header:** `tools/offline-render/src/gap_detector.hpp`
- **Source:** `tools/offline-render/src/gap_detector.cpp`
- **Standalone CLI:** `tools/offline-render/src/gap_detector_main.cpp` (`velora-gap-detector`)
- **Key Algorithms:**
  - Bit-identical verification before the switch boundary (ensures candidate and baseline match prior to transition).
  - Short-time RMS windowing (`window_frames = 240` [5 ms], `hop_frames = 120` [2.5 ms] at 48 kHz).
  - Truncation detection (`truncation_threshold_db = -75.0 dBFS` while baseline sounding at `>= -40 dBFS`).
  - Envelope drop / gap detection (`max_allowed_drop_db` ceiling to catch audible dips, gaps, or voice silences while tolerating expected preset mix volume adjustments).

### 2. Offline Renderer Setlist Support
- Extended `LiveSetRenderOptions` in `tools/offline-render/src/render_live_set.hpp` and `tools/offline-render/src/render_live_set.cpp`:
  - `std::vector<SetlistLoadAt> setlist_loads` (`--load-setlist-at TIME:SETLIST_ID`)
  - `std::vector<SetlistStepAt> setlist_steps` (`--step-at TIME:DIR`)
  - Integration with `host::SetlistStore` and `host::SetlistController`.

### 3. Automated Test Suite (`seamless_switching_suite`)
- **Source:** `tools/offline-render/tests/seamless_switching_suite_test.cpp`
- **Scripted Cases:**
  1. `test_detector_synthetic`: Unit test detector behavior on bit-identical signals, early truncation, and 50 ms dropout gaps.
  2. `test_seamless_scene_change`: Scene recall across held note, releasing note, and pedal-sustained note.
  3. `test_seamless_in_setlist_step_preloaded`: In-setlist Live Set step (preloaded, 0 simulated load blocks) across held, releasing, and pedal-sustained notes.
  4. `test_seamless_non_preloaded_jump`: Jump to non-preloaded Live Set (100 simulated load blocks) across held, releasing, and pedal-sustained notes.
  5. `test_seamless_setlist_load`: Background setlist loading while notes are actively sounding.
  6. `test_seamless_negative_cases`: Proves that intentional 20 ms dropouts, 100 ms voice truncations, or pre-switch mismatches trigger failure.

## Verification
- Executed `seamless_switching_suite`: 100% passed (zero gaps, zero truncations).
- Executed CTest regression suite across all switch, scenes, and assign tests (`ctest -R "seamless|switch|scenes|assign"`): 104/104 tests passed.

# Story 4.36: Footswitch function

## Overview
Implements hands-free section switching via footswitch / switch pedal (`MCU_CTRL_PEDAL_SWITCH`), configurable via Settings › Keyboard & Pedals › Footswitch function row (Next scene, Previous scene, Next Live Set, Rotary speed), stored in `sysd` config.

## Covers
- FR109 (footswitch advances scene by default, configurable to previous scene, next Live Set or rotary speed)
- AR39 (footswitch function persisted in `sysd` config)
- UX-DR65 (Settings › Keyboard & Pedals › Footswitch function row)
- UX-DR86 (footswitch ↔ Hero / current Scene Tile)

## Implementation Details

### 1. Footswitch Function Configuration (`SystemConfig`)
- **Headers & Source:** `ui/include/velora/ui/system_config.hpp`, `ui/src/system_config.cpp`
- **Enum `FootswitchFunction`:**
  - `NextScene = 0` (default: advances scene, same as SCENE ▶)
  - `PreviousScene = 1` (previous scene, same as SCENE ◀)
  - `NextLiveSet = 2` (next Live Set, same as LIVE SET ▶)
  - `RotarySpeed = 3` (toggles slow/fast on organ rotary speaker)
  - Backward-compatible aliases `SceneAdvance = NextScene` and `LiveSetAdvance = NextLiveSet`.
- **JSON Serialization & Deserialization:**
  - Saved to `/data/config/sysd_config.json` under `"footswitch_function"`.
  - Values: `"next_scene"`, `"previous_scene"`, `"next_live_set"`, `"rotary_speed"` (also accepting legacy aliases `"scene_advance"` and `"live_set_advance"`).

### 2. Settings Surface Row
- **Source:** `ui/src/screens/settings_view.cpp`
- Keyboard & Pedals detail card contains a 4-option Segmented Control:
  - Next scene (`settings.key.foot_next_scene`)
  - Prev scene (`settings.key.foot_prev_scene`)
  - Next Live Set (`settings.key.foot_next_liveset`)
  - Rotary speed (`settings.key.foot_rotary`)
- Selection updates `SystemConfig::instance().footswitch_function`, saves to disk immediately, and notifies subscribers.
- Localized in English, Spanish, and Brazilian Portuguese.

### 3. Navigation Controller Footswitch Dispatch
- **Source:** `ui/src/navigation_controller.cpp`, `ui/include/velora/ui/navigation_controller.hpp`
- Intercepts `ev.id == MCU_CTRL_PEDAL_SWITCH` on press transition:
  - `NextScene`: increments `scene_step_count_`, dispatches `MCU_BTN_SCENE_NEXT` to `performance_view_` (recalls next scene, updates scene tiles on screen immediately).
  - `PreviousScene`: decrements `scene_step_count_`, dispatches `MCU_BTN_SCENE_PREV` to `performance_view_`.
  - `NextLiveSet`: increments `live_set_step_count_`, sends `next_live_set` command via WebSocket client (at end of setlist, position remains unchanged and nothing happens).
  - `RotarySpeed`: dispatches `MCU_BTN_ROTARY_SF` to `performance_view_` toggling organ rotary speed between slow and fast, updating organ card and status bar.

### 4. Virtual Panel Key Binding
- **Source:** `ui/src/virtual_panel.cpp`
- Key `'f'` or `'F'` dispatches `MCU_CTRL_PEDAL_SWITCH` press and release events, enabling hands-free or keyboard-driven testing in the Mac simulator.

## Verification
- Created `ui/tests/footswitch_test.cpp` verifying:
  1. Virtual panel `'f'` / `'F'` dispatches `MCU_CTRL_PEDAL_SWITCH`.
  2. Default setting `NextScene` advances scene and updates scene tile.
  3. `PreviousScene` decrements scene and does not underflow at scene 0.
  4. `NextLiveSet` increments `live_set_step_count_` and sends command.
  5. `RotarySpeed` toggles organ rotary speed between 1 (slow) and 2 (fast).
  6. Settings view segmented control selects all 4 options, updating `SystemConfig`.
  7. JSON round-trip serialization and deserialization for all options and legacy aliases.
- Ran CTest suite: all tests passed.

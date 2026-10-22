#include "velora/ui/virtual_panel.hpp"

namespace velora::ui {

// Constants for non-printable keys (SDL compatible)
static constexpr int32_t kKeyEsc = 27;
static constexpr int32_t kKeyTab = 9;
static constexpr int32_t kKeyReturn = 13;
static constexpr int32_t kKeySpace = 32;
static constexpr int32_t kKeyUp = 1073741906;
static constexpr int32_t kKeyDown = 1073741905;
static constexpr int32_t kKeyLeft = 1073741904;
static constexpr int32_t kKeyRight = 1073741903;
static constexpr int32_t kKeyPageUp = 1073741899;
static constexpr int32_t kKeyPageDown = 1073741902;

bool VirtualPanel::handle_key(int32_t keycode, bool pressed) {
  // 1. EXIT: Escape or 'x' / 'X'
  if (keycode == kKeyEsc || keycode == 'x' || keycode == 'X') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_EXIT)
                     : PanelEvent::button_release(MCU_BTN_EXIT));
    return true;
  }

  // 2. MENU: Tab or 'm' / 'M'
  if (keycode == kKeyTab || keycode == 'm' || keycode == 'M') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_MENU)
                     : PanelEvent::button_release(MCU_BTN_MENU));
    return true;
  }

  // 3. WRITE: Return or 'w' / 'W'
  if (keycode == kKeyReturn || keycode == 'w' || keycode == 'W') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_WRITE)
                     : PanelEvent::button_release(MCU_BTN_WRITE));
    return true;
  }

  // 4. VALUE PUSH: Space or 'v' / 'V'
  if (keycode == kKeySpace || keycode == 'v' || keycode == 'V') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_VALUE_PUSH)
                     : PanelEvent::button_release(MCU_BTN_VALUE_PUSH));
    return true;
  }

  // 5. VALUE TURN (rotary encoder): only fires on press
  if (pressed) {
    if (keycode == kKeyUp || keycode == kKeyRight || keycode == ']' || keycode == '+' || keycode == '=') {
      dispatch(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, 1));
      return true;
    }
    if (keycode == kKeyDown || keycode == kKeyLeft || keycode == '[' || keycode == '-') {
      dispatch(PanelEvent::encoder_turn(MCU_CTRL_VALUE_ENCODER, -1));
      return true;
    }
  } else {
    if (keycode == kKeyUp || keycode == kKeyRight || keycode == ']' || keycode == '+' || keycode == '=' ||
        keycode == kKeyDown || keycode == kKeyLeft || keycode == '[' || keycode == '-') {
      return true;
    }
  }

  // 6. LIVE SET PREV / NEXT: PageUp / PageDown or ',' / '.'
  if (keycode == kKeyPageUp || keycode == ',' || keycode == '<') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_LIVE_SET_PREV)
                     : PanelEvent::button_release(MCU_BTN_LIVE_SET_PREV));
    return true;
  }
  if (keycode == kKeyPageDown || keycode == '.' || keycode == '>') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_LIVE_SET_NEXT)
                     : PanelEvent::button_release(MCU_BTN_LIVE_SET_NEXT));
    return true;
  }

  // 7. SCENE PREV / NEXT: '1' / '2' or '(' / ')'
  if (keycode == '1' || keycode == '(' || keycode == '{') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_SCENE_PREV)
                     : PanelEvent::button_release(MCU_BTN_SCENE_PREV));
    return true;
  }
  if (keycode == '2' || keycode == ')' || keycode == '}') {
    dispatch(pressed ? PanelEvent::button_press(MCU_BTN_SCENE_NEXT)
                     : PanelEvent::button_release(MCU_BTN_SCENE_NEXT));
    return true;
  }

  return false;
}

}  // namespace velora::ui

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace velora::ui {

enum class TouchMode : uint8_t {
  Full = 0,
  Safe = 1,
  Off = 2,
};

enum class VelocityCurve : uint8_t {
  Light = 0,
  Normal = 1,
  Heavy = 2,
  Fixed = 3,
};

enum class FootswitchFunction : uint8_t {
  NextScene = 0,
  PreviousScene = 1,
  NextLiveSet = 2,
  RotarySpeed = 3,

  // Backward-compatible aliases
  SceneAdvance = NextScene,
  LiveSetAdvance = NextLiveSet,
};

enum class ClockSourceMode : uint8_t {
  Internal = 0,
  DIN = 1,
  USB = 2,
};

enum class LedBrightnessMode : uint8_t {
  Low = 0,
  Mid = 1,
  High = 2,
  FollowScreen = 3,
};

struct SystemConfig {
  uint32_t schema_version{1};

  // Display & Language
  std::string theme{"stage"};        // "stage" or "studio"
  std::string language{"en"};        // "en", "pt-BR", "es"
  uint32_t screen_brightness{80};    // 5..100, step 5
  LedBrightnessMode led_brightness{LedBrightnessMode::FollowScreen};
  bool large_text{false};
  bool reduced_motion{false};
  TouchMode touch_mode{TouchMode::Safe};
  uint32_t write_hold_ms{500};       // 300..1000 ms, default 500
  uint32_t long_press_ms{500};       // 400, 500, 800 ms, default 500
  bool panel_guard{false};            // false = Off, true = On (300 ms hold on LIVE SET ◀ ▶)
  uint8_t boot_into{0};              // 0: LastSoundUsed, 1: StartOfSetlist, 2: DefaultLiveSet

  // Keyboard & Pedals
  VelocityCurve velocity_curve{VelocityCurve::Normal};
  bool aftertouch_available{false};  // Engine hardware report
  bool aftertouch_enabled{true};     // Off / On
  FootswitchFunction footswitch_function{FootswitchFunction::SceneAdvance};
  uint16_t sustain_min{0};
  uint16_t sustain_max{4095};
  uint16_t expression_min{0};
  uint16_t expression_max{4095};

  // MIDI & Clock
  ClockSourceMode clock_source{ClockSourceMode::Internal};
  uint8_t midi_out_channel{1};       // 1..16
  bool keybed_to_midi_out{true};     // Off / On
  bool midi_thru{false};             // Off / On

  static SystemConfig& instance();

  bool load_from_file(const std::string& path = "");
  bool save_to_file(const std::string& path = "") const;

  std::string to_json() const;
  bool from_json(std::string_view json);

  static std::string default_config_path();
};

}  // namespace velora::ui

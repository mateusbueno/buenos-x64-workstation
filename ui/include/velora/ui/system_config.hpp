#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// The sysd config file (`/data/config/sysd_config.json`) version policy (Story 8.5): N = kConfigSchemaVersion. A file at N or N-1
// (never below 1) loads; a schema_version above N is kTooNew, below N-1 (0 or negative) kTooOld, a non-integer or a document that is
// not a JSON object kParseError: all refused with nothing changed in the config. A file without schema_version is read as N (a deliberate leniency: the writer always emits it). Members this
// build does not know (written by a newer minor) are semantically preserved: re-serialized (whitespace normalised, order kept) and written back by to_json().
// load() into a non-default object merges: known keys absent from the file keep the object's current values (load into a fresh SystemConfig for a clean read). An N-1 file loads as migrated (the
// in-memory config is N; the file is rewritten as N only by the migration runner after the slot is good, or by the next explicit save).
inline constexpr uint32_t kConfigSchemaVersion = 1;
constexpr uint32_t min_readable_config_version(uint32_t current) { return current > 1 ? current - 1 : 1; }

enum class ConfigLoadStatus : uint8_t {
  Ok = 0,
  ParseError,
  TooOld,
  TooNew,
};

struct SystemConfig {
  uint32_t schema_version{kConfigSchemaVersion};
  bool migrated{false};  // set by load(): the file was at N-1
  // Top-level members this build does not know, as (key, compact re-serialized JSON text) in file order; written back by to_json().
  std::vector<std::pair<std::string, std::string>> unknown_fields;

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
  bool from_json(std::string_view json);  // load(json) == Ok
  // Version policy above; nothing is changed unless the result is Ok. `current_version` is N (tests set 2 to exercise N-1).
  ConfigLoadStatus load(std::string_view json, uint32_t current_version = kConfigSchemaVersion);

  static std::string default_config_path();
};

}  // namespace velora::ui

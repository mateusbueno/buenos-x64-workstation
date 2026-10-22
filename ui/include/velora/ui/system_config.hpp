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

struct SystemConfig {
  uint32_t schema_version{1};
  bool reduced_motion{false};
  TouchMode touch_mode{TouchMode::Safe};
  uint32_t write_hold_ms{500};    // 300..1000 ms, default 500
  uint32_t long_press_ms{500};    // 400, 500, 800 ms, default 500
  bool panel_guard{false};         // false = Off, true = On (300 ms hold on LIVE SET ◀ ▶)

  static SystemConfig& instance();

  bool load_from_file(const std::string& path = "");
  bool save_to_file(const std::string& path = "") const;

  std::string to_json() const;
  bool from_json(std::string_view json);

  static std::string default_config_path();
};

}  // namespace velora::ui

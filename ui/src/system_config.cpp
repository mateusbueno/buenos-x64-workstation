#include "velora/ui/system_config.hpp"
#include "velora/format/json_lite.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace velora::ui {

SystemConfig& SystemConfig::instance() {
  static SystemConfig s_instance;
  return s_instance;
}

std::string SystemConfig::default_config_path() {
  const char* env_path = std::getenv("VELORA_CONFIG_PATH");
  if (env_path && *env_path) {
    return std::string(env_path);
  }
  return "/data/config/sysd_config.json";
}

std::string SystemConfig::to_json() const {
  char buf[512];
  const char* touch_str = "safe";
  if (touch_mode == TouchMode::Full) touch_str = "full";
  else if (touch_mode == TouchMode::Off) touch_str = "off";

  std::snprintf(buf, sizeof(buf),
                "{\n"
                "  \"schema_version\": %u,\n"
                "  \"reduced_motion\": %s,\n"
                "  \"touch_mode\": \"%s\",\n"
                "  \"write_hold_ms\": %u,\n"
                "  \"long_press_ms\": %u,\n"
                "  \"panel_guard\": %s\n"
                "}\n",
                schema_version,
                reduced_motion ? "true" : "false",
                touch_str,
                write_hold_ms,
                long_press_ms,
                panel_guard ? "true" : "false");
  return std::string(buf);
}

bool SystemConfig::from_json(std::string_view json) {
  format::JsonValue root;
  std::string err;
  if (!format::parse_json(json, root, &err) || !root.is_object()) {
    return false;
  }

  const auto* ver = root.find("schema_version");
  if (ver && ver->is_number()) {
    schema_version = static_cast<uint32_t>(ver->number);
  }

  const auto* rm = root.find("reduced_motion");
  if (rm && rm->type == format::JsonValue::Type::kBool) {
    reduced_motion = rm->boolean;
  }

  const auto* tm = root.find("touch_mode");
  if (tm && tm->is_string()) {
    if (tm->string == "full") touch_mode = TouchMode::Full;
    else if (tm->string == "safe") touch_mode = TouchMode::Safe;
    else if (tm->string == "off") touch_mode = TouchMode::Off;
  }

  const auto* wh = root.find("write_hold_ms");
  if (wh && wh->is_number()) {
    write_hold_ms = static_cast<uint32_t>(wh->number);
    if (write_hold_ms < 300) write_hold_ms = 300;
    if (write_hold_ms > 1000) write_hold_ms = 1000;
  }

  const auto* lp = root.find("long_press_ms");
  if (lp && lp->is_number()) {
    long_press_ms = static_cast<uint32_t>(lp->number);
    if (long_press_ms != 400 && long_press_ms != 500 && long_press_ms != 800) {
      long_press_ms = 500;
    }
  }

  const auto* pg = root.find("panel_guard");
  if (pg && pg->type == format::JsonValue::Type::kBool) {
    panel_guard = pg->boolean;
  }

  return true;
}

bool SystemConfig::load_from_file(const std::string& path) {
  std::string target_path = path.empty() ? default_config_path() : path;
  std::string content;
  std::string err;
  if (!format::read_text_file(target_path, content, &err)) {
    return false;
  }
  return from_json(content);
}

bool SystemConfig::save_to_file(const std::string& path) const {
  std::string target_path = path.empty() ? default_config_path() : path;
  std::string temp_path = target_path + ".tmp";

  std::ofstream out(temp_path, std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  std::string json_content = to_json();
  out.write(json_content.data(), static_cast<std::streamsize>(json_content.size()));
  out.close();

  if (std::rename(temp_path.c_str(), target_path.c_str()) != 0) {
    // If atomic rename fails (e.g. cross-device), fallback to direct write
    std::ofstream direct(target_path, std::ios::trunc);
    if (!direct.is_open()) return false;
    direct.write(json_content.data(), static_cast<std::streamsize>(json_content.size()));
    direct.close();
    std::remove(temp_path.c_str());
  }
  return true;
}

}  // namespace velora::ui

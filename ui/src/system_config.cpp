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
  char buf[2048];
  const char* touch_str = "safe";
  if (touch_mode == TouchMode::Full) touch_str = "full";
  else if (touch_mode == TouchMode::Off) touch_str = "off";

  const char* vel_str = "normal";
  if (velocity_curve == VelocityCurve::Light) vel_str = "light";
  else if (velocity_curve == VelocityCurve::Heavy) vel_str = "heavy";
  else if (velocity_curve == VelocityCurve::Fixed) vel_str = "fixed";

  const char* foot_str = "scene_advance";
  if (footswitch_function == FootswitchFunction::RotarySpeed) foot_str = "rotary_speed";
  else if (footswitch_function == FootswitchFunction::LiveSetAdvance) foot_str = "live_set_advance";

  const char* clk_str = "internal";
  if (clock_source == ClockSourceMode::DIN) clk_str = "din";
  else if (clock_source == ClockSourceMode::USB) clk_str = "usb";

  const char* led_str = "follow_screen";
  if (led_brightness == LedBrightnessMode::Low) led_str = "low";
  else if (led_brightness == LedBrightnessMode::Mid) led_str = "mid";
  else if (led_brightness == LedBrightnessMode::High) led_str = "high";

  std::snprintf(buf, sizeof(buf),
                "{\n"
                "  \"schema_version\": %u,\n"
                "  \"theme\": \"%s\",\n"
                "  \"language\": \"%s\",\n"
                "  \"screen_brightness\": %u,\n"
                "  \"led_brightness\": \"%s\",\n"
                "  \"large_text\": %s,\n"
                "  \"reduced_motion\": %s,\n"
                "  \"touch_mode\": \"%s\",\n"
                "  \"write_hold_ms\": %u,\n"
                "  \"long_press_ms\": %u,\n"
                "  \"panel_guard\": %s,\n"
                "  \"boot_into\": %u,\n"
                "  \"velocity_curve\": \"%s\",\n"
                "  \"aftertouch_available\": %s,\n"
                "  \"aftertouch_enabled\": %s,\n"
                "  \"footswitch_function\": \"%s\",\n"
                "  \"sustain_min\": %u,\n"
                "  \"sustain_max\": %u,\n"
                "  \"expression_min\": %u,\n"
                "  \"expression_max\": %u,\n"
                "  \"clock_source\": \"%s\",\n"
                "  \"midi_out_channel\": %u,\n"
                "  \"keybed_to_midi_out\": %s,\n"
                "  \"midi_thru\": %s\n"
                "}\n",
                schema_version,
                theme.c_str(),
                language.c_str(),
                screen_brightness,
                led_str,
                large_text ? "true" : "false",
                reduced_motion ? "true" : "false",
                touch_str,
                write_hold_ms,
                long_press_ms,
                panel_guard ? "true" : "false",
                static_cast<unsigned>(boot_into),
                vel_str,
                aftertouch_available ? "true" : "false",
                aftertouch_enabled ? "true" : "false",
                foot_str,
                sustain_min,
                sustain_max,
                expression_min,
                expression_max,
                clk_str,
                midi_out_channel,
                keybed_to_midi_out ? "true" : "false",
                midi_thru ? "true" : "false");
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

  const auto* thm = root.find("theme");
  if (thm && thm->is_string()) theme = thm->string;

  const auto* lang = root.find("language");
  if (lang && lang->is_string()) language = lang->string;

  const auto* sb = root.find("screen_brightness");
  if (sb && sb->is_number()) {
    screen_brightness = static_cast<uint32_t>(sb->number);
    if (screen_brightness < 5) screen_brightness = 5;
    if (screen_brightness > 100) screen_brightness = 100;
  }

  const auto* led = root.find("led_brightness");
  if (led && led->is_string()) {
    if (led->string == "low") led_brightness = LedBrightnessMode::Low;
    else if (led->string == "mid") led_brightness = LedBrightnessMode::Mid;
    else if (led->string == "high") led_brightness = LedBrightnessMode::High;
    else led_brightness = LedBrightnessMode::FollowScreen;
  }

  const auto* lt = root.find("large_text");
  if (lt && lt->type == format::JsonValue::Type::kBool) large_text = lt->boolean;

  const auto* rm = root.find("reduced_motion");
  if (rm && rm->type == format::JsonValue::Type::kBool) reduced_motion = rm->boolean;

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
  if (pg && pg->type == format::JsonValue::Type::kBool) panel_guard = pg->boolean;

  const auto* bi = root.find("boot_into");
  if (bi && bi->is_number()) boot_into = static_cast<uint8_t>(bi->number);

  const auto* vc = root.find("velocity_curve");
  if (vc && vc->is_string()) {
    if (vc->string == "light") velocity_curve = VelocityCurve::Light;
    else if (vc->string == "normal") velocity_curve = VelocityCurve::Normal;
    else if (vc->string == "heavy") velocity_curve = VelocityCurve::Heavy;
    else if (vc->string == "fixed") velocity_curve = VelocityCurve::Fixed;
  }

  const auto* aa = root.find("aftertouch_available");
  if (aa && aa->type == format::JsonValue::Type::kBool) aftertouch_available = aa->boolean;

  const auto* ae = root.find("aftertouch_enabled");
  if (ae && ae->type == format::JsonValue::Type::kBool) aftertouch_enabled = ae->boolean;

  const auto* ff = root.find("footswitch_function");
  if (ff && ff->is_string()) {
    if (ff->string == "scene_advance") footswitch_function = FootswitchFunction::SceneAdvance;
    else if (ff->string == "rotary_speed") footswitch_function = FootswitchFunction::RotarySpeed;
    else if (ff->string == "live_set_advance") footswitch_function = FootswitchFunction::LiveSetAdvance;
  }

  const auto* smin = root.find("sustain_min");
  if (smin && smin->is_number()) sustain_min = static_cast<uint16_t>(smin->number);

  const auto* smax = root.find("sustain_max");
  if (smax && smax->is_number()) sustain_max = static_cast<uint16_t>(smax->number);

  const auto* emin = root.find("expression_min");
  if (emin && emin->is_number()) expression_min = static_cast<uint16_t>(emin->number);

  const auto* emax = root.find("expression_max");
  if (emax && emax->is_number()) expression_max = static_cast<uint16_t>(emax->number);

  const auto* cs = root.find("clock_source");
  if (cs && cs->is_string()) {
    if (cs->string == "internal") clock_source = ClockSourceMode::Internal;
    else if (cs->string == "din") clock_source = ClockSourceMode::DIN;
    else if (cs->string == "usb") clock_source = ClockSourceMode::USB;
  }

  const auto* moc = root.find("midi_out_channel");
  if (moc && moc->is_number()) {
    midi_out_channel = static_cast<uint8_t>(moc->number);
    if (midi_out_channel < 1) midi_out_channel = 1;
    if (midi_out_channel > 16) midi_out_channel = 16;
  }

  const auto* kmo = root.find("keybed_to_midi_out");
  if (kmo && kmo->type == format::JsonValue::Type::kBool) keybed_to_midi_out = kmo->boolean;

  const auto* mt = root.find("midi_thru");
  if (mt && mt->type == format::JsonValue::Type::kBool) midi_thru = mt->boolean;

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
    std::ofstream direct(target_path, std::ios::trunc);
    if (!direct.is_open()) return false;
    direct.write(json_content.data(), static_cast<std::streamsize>(json_content.size()));
    direct.close();
    std::remove(temp_path.c_str());
  }
  return true;
}

}  // namespace velora::ui

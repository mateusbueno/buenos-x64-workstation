#include "velora/ui/system_config.hpp"
#include "velora/format/json_lite.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace velora::ui {

namespace {

void append_json_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

// Compact JSON text of a parsed value, for members this build does not know. They are RE-SERIALIZED, not copied byte for byte: the value is preserved
// semantically (numbers keep up to 17 significant digits, whitespace is normalised, member order is kept).
void append_json_value(std::string& out, const format::JsonValue& v) {
  using T = format::JsonValue::Type;
  switch (v.type) {
    case T::kNull: out += "null"; break;
    case T::kBool: out += v.boolean ? "true" : "false"; break;
    case T::kNumber: {
      char b[40];
      if (v.number > -9e15 && v.number < 9e15 && v.number == static_cast<double>(static_cast<long long>(v.number)))
        std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v.number));
      else
        std::snprintf(b, sizeof(b), "%.17g", v.number);
      out += b;
      break;
    }
    case T::kString: append_json_string(out, v.string); break;
    case T::kArray:
      out += '[';
      for (size_t i = 0; i < v.array.size(); ++i) {
        if (i) out += ',';
        append_json_value(out, v.array[i]);
      }
      out += ']';
      break;
    case T::kObject:
      out += '{';
      for (size_t i = 0; i < v.keys.size(); ++i) {
        if (i) out += ',';
        append_json_string(out, v.keys[i]);
        out += ':';
        append_json_value(out, v.values[i]);
      }
      out += '}';
      break;
  }
}

// Must list every key to_json() emits (config_version_test checks it: a key missing here would be emitted twice, as a field and as an unknown member).
bool known_config_key(const std::string& k) {
  static const char* const kKnown[] = {"schema_version", "theme", "language", "screen_brightness", "led_brightness", "large_text", "reduced_motion",
                                       "touch_mode", "write_hold_ms", "long_press_ms", "panel_guard", "boot_into", "velocity_curve",
                                       "aftertouch_available", "aftertouch_enabled", "footswitch_function", "sustain_min", "sustain_max",
                                       "expression_min", "expression_max", "clock_source", "midi_out_channel", "keybed_to_midi_out", "midi_thru"};
  for (const char* n : kKnown)
    if (k == n) return true;
  return false;
}

}  // namespace

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

  const char* foot_str = "next_scene";
  if (footswitch_function == FootswitchFunction::PreviousScene) foot_str = "previous_scene";
  else if (footswitch_function == FootswitchFunction::NextLiveSet) foot_str = "next_live_set";
  else if (footswitch_function == FootswitchFunction::RotarySpeed) foot_str = "rotary_speed";

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
  std::string out(buf);
  if (!unknown_fields.empty()) {  // members of a newer minor survive read-modify-write: appended before the closing brace
    const size_t close = out.rfind("\n}");
    if (close == std::string::npos) return out;  // cannot happen with the fixed template above; never insert at npos
    std::string extra;
    for (const auto& kv : unknown_fields) {
      extra += ",\n  ";
      append_json_string(extra, kv.first);
      extra += ": " + kv.second;
    }
    out.insert(close, extra);
  }
  return out;
}

bool SystemConfig::from_json(std::string_view json) { return load(json) == ConfigLoadStatus::Ok; }

ConfigLoadStatus SystemConfig::load(std::string_view json, uint32_t current_version) {
  format::JsonValue root;
  std::string err;
  if (!format::parse_json(json, root, &err) || !root.is_object()) {
    return ConfigLoadStatus::ParseError;
  }

  // Version policy first: nothing is touched when the file is refused.
  uint32_t file_version = current_version;
  const auto* ver = root.find("schema_version");
  if (ver) {
    if (!ver->is_number() || !std::isfinite(ver->number) || ver->number != std::floor(ver->number)) return ConfigLoadStatus::ParseError;
    if (ver->number < 1) return ConfigLoadStatus::TooOld;
    if (ver->number > static_cast<double>(current_version)) return ConfigLoadStatus::TooNew;
    file_version = static_cast<uint32_t>(ver->number);
    if (file_version < min_readable_config_version(current_version)) return ConfigLoadStatus::TooOld;
  }
  migrated = file_version < current_version;
  schema_version = current_version;  // an N-1 file is held as N (its members are read by the same code; no field moved yet)

  unknown_fields.clear();
  for (size_t i = 0; i < root.keys.size(); ++i) {
    if (known_config_key(root.keys[i])) continue;
    std::string raw;
    append_json_value(raw, root.values[i]);
    unknown_fields.emplace_back(root.keys[i], std::move(raw));
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
    if (ff->string == "next_scene" || ff->string == "scene_advance") footswitch_function = FootswitchFunction::NextScene;
    else if (ff->string == "previous_scene") footswitch_function = FootswitchFunction::PreviousScene;
    else if (ff->string == "next_live_set" || ff->string == "live_set_advance") footswitch_function = FootswitchFunction::NextLiveSet;
    else if (ff->string == "rotary_speed") footswitch_function = FootswitchFunction::RotarySpeed;
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

  return ConfigLoadStatus::Ok;
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

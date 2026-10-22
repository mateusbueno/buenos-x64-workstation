#include "velora/ui/locale.hpp"

#include <fstream>
#include <sstream>
#include <iostream>

namespace velora::ui {

Locale& Locale::instance() {
  static Locale s_instance;
  return s_instance;
}

static std::string trim(const std::string& str) {
  size_t first = str.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  size_t last = str.find_last_not_of(" \t\r\n");
  return str.substr(first, (last - first + 1));
}

bool Locale::load_file(const std::string& path, std::unordered_map<std::string, std::string>& out) {
  std::ifstream f(path);
  if (!f.is_open()) return false;

  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line == "{" || line == "}") continue;
    if (line.back() == ',') line.pop_back();

    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string key = trim(line.substr(0, colon));
    std::string val = trim(line.substr(colon + 1));

    if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
      key = key.substr(1, key.size() - 2);
    }
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }

    out[key] = val;
  }
  return true;
}

bool Locale::initialize(const std::string& base_dir, const std::string& default_locale) {
  std::lock_guard<std::mutex> lock(mutex_);
  catalogs_.clear();

  if (!base_dir.empty()) {
    base_dir_ = base_dir;
  } else {
    const char* env_dir = std::getenv("VELORA_LOCALE_DIR");
    if (env_dir && *env_dir) {
      base_dir_ = env_dir;
    } else {
      base_dir_ = "locale";
      const char* candidates[] = {
        "locale",
        "../locale",
        "../../locale",
        "../../../locale"
      };
      for (const char* c : candidates) {
        std::ifstream test(std::string(c) + "/en.json");
        if (test.is_open()) {
          base_dir_ = c;
          break;
        }
      }
    }
  }

  // Preload known locales
  for (const auto& code : {"en", "pt-BR", "es"}) {
    std::unordered_map<std::string, std::string> cat;
    std::string p = base_dir_ + "/" + code + ".json";
    if (load_file(p, cat)) {
      catalogs_[code] = std::move(cat);
    }
  }

  current_locale_ = default_locale;
  return catalogs_.find("en") != catalogs_.end();
}

bool Locale::set_locale(const std::string& locale_code) {
  std::lock_guard<std::mutex> lock(mutex_);
  current_locale_ = locale_code;
  if (catalogs_.find(locale_code) == catalogs_.end()) {
    std::unordered_map<std::string, std::string> cat;
    std::string p = base_dir_ + "/" + locale_code + ".json";
    if (load_file(p, cat)) {
      catalogs_[locale_code] = std::move(cat);
      return true;
    }
    return false;
  }
  return true;
}

static std::string pseudo_localize(const std::string& str) {
  if (str.empty()) return str;
  std::string res;
  res.reserve(str.size() * 2 + 16);
  res.push_back('[');
  for (char c : str) {
    switch (c) {
      case 'a': res += "å"; break;
      case 'e': res += "é"; break;
      case 'i': res += "î"; break;
      case 'o': res += "ö"; break;
      case 'u': res += "û"; break;
      case 'c': res += "ç"; break;
      case 'r': res += "ŕ"; break;
      case 's': res += "š"; break;
      case 't': res += "ţ"; break;
      case 'w': res += "ŵ"; break;
      case 'W': res += "Ŵ"; break;
      case 'A': res += "Á"; break;
      case 'C': res += "Ç"; break;
      case 'E': res += "Ê"; break;
      case 'O': res += "Õ"; break;
      case 'N': res += "Ñ"; break;
      default: res.push_back(c); break;
    }
  }
  // +40% expansion with middle/end dots
  size_t pad = (str.size() * 4) / 10;
  if (pad == 0) pad = 1;
  for (size_t i = 0; i < pad; ++i) {
    res += "·";
  }
  res.push_back(']');
  return res;
}

std::string Locale::tr(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_locale_ == "pseudo") {
    auto it_en = catalogs_.find("en");
    if (it_en != catalogs_.end()) {
      auto it_key = it_en->second.find(key);
      if (it_key != it_en->second.end()) {
        return pseudo_localize(it_key->second);
      }
    }
    return pseudo_localize(key);
  }

  // 1. Try active locale
  auto it_cat = catalogs_.find(current_locale_);
  if (it_cat != catalogs_.end()) {
    auto it_key = it_cat->second.find(key);
    if (it_key != it_cat->second.end()) {
      return it_key->second;
    }
  }

  // 2. Fallback to English
  if (current_locale_ != "en") {
    auto it_en = catalogs_.find("en");
    if (it_en != catalogs_.end()) {
      auto it_key = it_en->second.find(key);
      if (it_key != it_en->second.end()) {
        return it_key->second;
      }
    }
  }

  // 3. Fallback to key itself
  return key;
}

void Locale::set_entry(const std::string& locale_code, const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  catalogs_[locale_code][key] = value;
}

void Locale::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  catalogs_.clear();
  current_locale_ = "en";
}

}  // namespace velora::ui

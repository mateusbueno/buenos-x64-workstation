#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace velora::ui {

class Locale {
 public:
  static Locale& instance();

  // Load catalogs from base_dir (defaults to current dir or detected "locale/")
  bool initialize(const std::string& base_dir = "", const std::string& default_locale = "en");

  // Switch active locale ("en", "pt-BR", "es")
  bool set_locale(const std::string& locale_code);
  const std::string& current_locale() const noexcept { return current_locale_; }

  // Lookup translated string by key; falls back to English, then to key itself
  std::string tr(const std::string& key) const;

  // Add translation directly (useful for tests or runtime injection)
  void set_entry(const std::string& locale_code, const std::string& key, const std::string& value);

  // Clear loaded catalogs
  void clear();

 private:
  Locale() = default;

  bool load_file(const std::string& path, std::unordered_map<std::string, std::string>& out);

  mutable std::mutex mutex_;
  std::string base_dir_;
  std::string current_locale_{"en"};
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> catalogs_;
};

inline std::string tr(const std::string& key) {
  return Locale::instance().tr(key);
}

}  // namespace velora::ui

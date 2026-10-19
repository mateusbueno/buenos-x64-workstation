#pragma once
// The minimal locale catalog (Story 5.5; AD-20 "Externalized strings"; Epic 10 / Story 10.7 extends it).
//
// Format: one UTF-8 JSON file per locale, `locale/<locale>.json` (`en`, `pt-BR`, `es`), a single flat object of
// string keys to string values. No nesting, no arrays, no numbers: a non-string value or a duplicate key makes the
// file invalid. Keys are dotted lower-case ids (`category.piano.full`, `category.piano.short`); English is the
// source and default locale. Placeholders (later, Epic 10) will be `{name}` inside the value; the category labels
// have none. Panel legends and user content are never catalog entries (AD-20).
//
// Locale tags are matched case-insensitively with `_` read as `-` ("pt_BR", "pt-br" are "pt-BR"), and a regional
// tag falls back to a loaded locale of the same language ("es-MX", "es-ES" -> "es", "pt" or "pt-PT" -> "pt-BR")
// before the English fallback applies. Lookups return copies: nothing dangles when a locale is reloaded.
// `kNone` has no label (the UX table has none; the Import surface's "Choose category" is its own catalog key,
// Epic 5.12/10): `category_label(locale, Category::kNone)` is empty and callers handle the unassigned state.
//
// Lookup falls back to English when the requested locale lacks a key (or is not loaded) so a missing translation
// can never blank the UI at runtime; the shipped-locale tests (format_category_test) use `find_own` and fail
// instead of relying on the fallback.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "velora/format/category.hpp"

namespace velora::format {

inline constexpr std::string_view kDefaultLocale = "en";
inline constexpr std::string_view kShippedLocales[] = {"en", "pt-BR", "es"};
// A short label (Part Strips) never exceeds this many Unicode code points.
inline constexpr std::size_t kMaxShortLabelLength = 10;

class LocaleCatalog {
 public:
  // Adds (or replaces) a locale from JSON text. On failure nothing changes and `error` says why.
  bool load_text(std::string_view locale, std::string_view json, std::string* error = nullptr);
  bool load_file(std::string_view locale, const std::string& path, std::string* error = nullptr);
  // Loads `<dir>/<locale>.json` for every shipped locale; all or nothing (on any problem the catalog is unchanged).
  bool load_shipped(const std::string& dir, std::string* error = nullptr);

  bool has_locale(std::string_view locale) const;
  std::vector<std::string> locales() const;
  // All keys of a locale in file order (empty if not loaded).
  std::vector<std::string> keys(std::string_view locale) const;

  // The locale's own entry, no fallback.
  std::optional<std::string> find_own(std::string_view locale, std::string_view key) const;
  // The locale's entry, else the English one, else nullopt.
  std::optional<std::string> find(std::string_view locale, std::string_view key) const;
  // `find`, else the key itself (visible, never empty).
  std::string get(std::string_view locale, std::string_view key) const;

  // "category.<slug>.full" / ".short".
  static std::string category_key(Category c, bool short_label);
  std::string category_label(std::string_view locale, Category c, bool short_label = false) const;

 private:
  struct Locale {
    std::string id;
    std::vector<std::pair<std::string, std::string>> entries;
  };
  const Locale* find_locale(std::string_view locale) const;  // exact (normalised), then same-language fallback
  std::vector<Locale> locales_;
};

}  // namespace velora::format

#include "velora/format/locale_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "velora/format/json_lite.hpp"

namespace velora::format {

namespace {

// "pt_br" -> "pt-br" (lower-case, '-' separators): the comparison form.
std::string fold_tag(std::string_view t) {
  std::string s;
  for (char c : t) s.push_back(c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return s;
}
std::string language_of(const std::string& folded) { return folded.substr(0, folded.find('-')); }

// A tag naming a shipped locale in any spelling gets the canonical one ("pt_br" -> "pt-BR").
std::string canonical_id(std::string_view locale) {
  const std::string f = fold_tag(locale);
  for (std::string_view s : kShippedLocales)
    if (fold_tag(s) == f) return std::string(s);
  return std::string(locale);
}

}  // namespace

const LocaleCatalog::Locale* LocaleCatalog::find_locale(std::string_view locale) const {
  const std::string f = fold_tag(locale);
  if (f.empty()) return nullptr;
  for (const auto& l : locales_)
    if (fold_tag(l.id) == f) return &l;
  const std::string lang = language_of(f);
  for (const auto& l : locales_)  // es-MX -> es, pt -> pt-BR
    if (language_of(fold_tag(l.id)) == lang) return &l;
  return nullptr;
}

bool LocaleCatalog::load_text(std::string_view locale, std::string_view json, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = std::string(locale) + ": " + why;
    return false;
  };
  if (locale.empty()) return fail("empty locale id");
  JsonValue root;
  std::string perr;
  if (!parse_json(json, root, &perr)) return fail(perr);
  if (!root.is_object()) return fail("the catalog must be a JSON object");
  Locale l;
  l.id = canonical_id(locale);
  std::set<std::string> seen;
  for (std::size_t i = 0; i < root.keys.size(); ++i) {
    const std::string& k = root.keys[i];
    if (k.empty()) return fail("empty key");
    if (!root.values[i].is_string()) return fail("key '" + k + "' is not a string");
    if (!seen.insert(k).second) return fail("duplicate key '" + k + "'");
    l.entries.emplace_back(k, root.values[i].string);
  }
  for (auto& old : locales_)
    if (fold_tag(old.id) == fold_tag(l.id)) {
      old = std::move(l);
      return true;
    }
  locales_.push_back(std::move(l));
  return true;
}

bool LocaleCatalog::load_file(std::string_view locale, const std::string& path, std::string* error) {
  std::string text, why;
  if (!read_text_file(path, text, &why)) {
    if (error) *error = std::string(locale) + ": " + why;
    return false;
  }
  return load_text(locale, text, error);
}

bool LocaleCatalog::load_shipped(const std::string& dir, std::string* error) {
  LocaleCatalog tmp = *this;
  for (std::string_view loc : kShippedLocales)
    if (!tmp.load_file(loc, dir + "/" + std::string(loc) + ".json", error)) return false;
  *this = std::move(tmp);
  return true;
}

bool LocaleCatalog::has_locale(std::string_view locale) const { return find_locale(locale) != nullptr; }

std::vector<std::string> LocaleCatalog::locales() const {
  std::vector<std::string> v;
  for (const auto& l : locales_) v.push_back(l.id);
  return v;
}

std::vector<std::string> LocaleCatalog::keys(std::string_view locale) const {
  std::vector<std::string> v;
  if (const Locale* l = find_locale(locale))
    for (const auto& e : l->entries) v.push_back(e.first);
  return v;
}

std::optional<std::string> LocaleCatalog::find_own(std::string_view locale, std::string_view key) const {
  if (const Locale* l = find_locale(locale))
    for (const auto& e : l->entries)
      if (e.first == key) return e.second;
  return std::nullopt;
}

std::optional<std::string> LocaleCatalog::find(std::string_view locale, std::string_view key) const {
  if (auto v = find_own(locale, key)) return v;
  return find_own(kDefaultLocale, key);
}

std::string LocaleCatalog::get(std::string_view locale, std::string_view key) const {
  if (auto v = find(locale, key)) return *v;
  return std::string(key);
}

std::string LocaleCatalog::category_key(Category c, bool short_label) {
  return std::string(key(c)) + (short_label ? ".short" : ".full");
}

std::string LocaleCatalog::category_label(std::string_view locale, Category c, bool short_label) const {
  if (!is_assignable(c)) return {};
  return get(locale, category_key(c, short_label));
}

}  // namespace velora::format

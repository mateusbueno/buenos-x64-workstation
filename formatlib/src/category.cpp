#include "velora/format/category.hpp"

#include "velora/format/json_lite.hpp"

#include <sys/stat.h>

#include <cerrno>

namespace velora::format {
namespace {

// Lower-case ASCII letters and digits only: "Mallet & Bells" == "mallet_bells" == "malletbells".
std::string normalise(std::string_view s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out.push_back(static_cast<char>(ch));
  }
  return out;
}

}  // namespace

std::optional<Category> category_from_id(std::uint32_t v) {
  if (v > kCategoryCount) return std::nullopt;
  return static_cast<Category>(v);
}

std::optional<Category> parse_category(std::string_view name) {
  for (unsigned char ch : name)
    if (ch < 0x20 || ch > 0x7e) return std::nullopt;  // never silently strip bytes
  // A leading "category." is the key prefix, not part of the name.
  constexpr std::string_view kPrefix = "category.";
  bool had_prefix = false;
  if (name.size() >= kPrefix.size()) {
    bool eq = true;
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
      char ch = name[i];
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      if (ch != kPrefix[i]) {
        eq = false;
        break;
      }
    }
    if (eq) {
      name.remove_prefix(kPrefix.size());
      had_prefix = true;
    }
  }
  const std::string n = normalise(name);
  if (n.empty()) return std::nullopt;
  if (n == "none") return Category::kNone;
  for (std::size_t i = 1; i < kCategories.size(); ++i) {
    const CategoryInfo& c = kCategories[i];
    if (n == normalise(c.slug)) return c.id;
    if (had_prefix) continue;  // "category.<english name>" is not a spelling we accept
    if (n == normalise(c.english) || n == normalise(c.english_short)) return c.id;
  }
  return std::nullopt;
}

std::optional<Category> parse_assignable_category(std::string_view name) {
  const auto c = parse_category(name);
  if (!c || !is_assignable(*c)) return std::nullopt;
  return c;
}

std::string category_file_text(Category c) {
  if (!is_assignable(c)) return {};
  return std::string(key(c)) + "\n";
}

std::optional<Category> parse_category_file(std::string_view text) {
  auto ws = [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; };
  while (!text.empty() && ws(text.back())) text.remove_suffix(1);
  while (!text.empty() && ws(text.front())) text.remove_prefix(1);
  return parse_assignable_category(text);
}

CategoryFileResult read_category_file(const std::string& path) {
  CategoryFileResult r;
  std::string text, why;
  if (!read_text_file(path, text, &why, 4096)) {
    // Absent = the file does not exist; anything else that cannot be read is unreadable.
    struct stat st;
    r.state = (::lstat(path.c_str(), &st) != 0 && errno == ENOENT) ? CategoryFileState::kAbsent : CategoryFileState::kUnreadable;
    return r;
  }
  if (const auto c = parse_category_file(text)) {
    r.state = CategoryFileState::kValid;
    r.category = *c;
  } else {
    r.state = CategoryFileState::kUnreadable;
  }
  return r;
}

}  // namespace velora::format

#pragma once
// The instrument category model (Story 5.5). Twin of proto/category.proto (same ids and names; the guard test
// `format_category_test` compares them). Hand maintained, no protobuf dependency (formatlib stays light).
//
//  - ids 1..17 are stable and only ever grow by addition; `kNone` (0) is the "unassigned" sentinel for filters and
//    queries and can never be stored on an imported instrument: `is_assignable(kNone)` is false and every writer
//    path (the librarian's category file, `category_file_text`) refuses it.
//  - `key(c)` is the stable string key, `category.<slug>`; the same slug is the stem of the locale-catalog keys
//    `category.<slug>.full` / `category.<slug>.short` (locale/*.json).
//  - English names here are the source strings, copied from the UX EXPERIENCE.md Categories table; the localized
//    labels live in the locale catalog (`LocaleCatalog::category_label`).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace velora::format {

enum class Category : std::uint8_t {
  kNone = 0,
  kPiano = 1,
  kEPiano = 2,
  kOrgan = 3,
  kKeys = 4,
  kMalletBells = 5,
  kStrings = 6,
  kBrass = 7,
  kWoodwind = 8,
  kChoirVocal = 9,
  kGuitar = 10,
  kBass = 11,
  kSynthLead = 12,
  kSynthPad = 13,
  kSynthPoly = 14,
  kDrumsPerc = 15,
  kWorld = 16,
  kFxAmbient = 17,
};

inline constexpr unsigned kCategoryCount = 17;  // assignable categories (ids 1..17)

struct CategoryInfo {
  Category id;
  std::string_view slug;           // "piano", "epiano", "mallet_bells", ...
  std::string_view key;            // "category.piano" ("none" for kNone)
  std::string_view english;        // full English label (EXPERIENCE.md)
  std::string_view english_short;  // short English label
};

// Indexed by numeric id (0 = none).
inline constexpr std::array<CategoryInfo, kCategoryCount + 1> kCategories{{
    {Category::kNone, "none", "none", "None", "None"},
    {Category::kPiano, "piano", "category.piano", "Piano", "Piano"},
    {Category::kEPiano, "epiano", "category.epiano", "E.Piano", "E.Piano"},
    {Category::kOrgan, "organ", "category.organ", "Organ", "Organ"},
    {Category::kKeys, "keys", "category.keys", "Keys", "Keys"},
    {Category::kMalletBells, "mallet_bells", "category.mallet_bells", "Mallet & Bells", "Mallets"},
    {Category::kStrings, "strings", "category.strings", "Strings", "Strings"},
    {Category::kBrass, "brass", "category.brass", "Brass", "Brass"},
    {Category::kWoodwind, "woodwind", "category.woodwind", "Woodwind", "Woodwind"},
    {Category::kChoirVocal, "choir_vocal", "category.choir_vocal", "Choir & Vocal", "Choir"},
    {Category::kGuitar, "guitar", "category.guitar", "Guitar", "Guitar"},
    {Category::kBass, "bass", "category.bass", "Bass", "Bass"},
    {Category::kSynthLead, "synth_lead", "category.synth_lead", "Synth Lead", "Synth Lead"},
    {Category::kSynthPad, "synth_pad", "category.synth_pad", "Synth Pad", "Synth Pad"},
    {Category::kSynthPoly, "synth_poly", "category.synth_poly", "Synth Poly", "Synth Poly"},
    {Category::kDrumsPerc, "drums_perc", "category.drums_perc", "Drums & Perc", "Drums"},
    {Category::kWorld, "world", "category.world", "World", "World"},
    {Category::kFxAmbient, "fx_ambient", "category.fx_ambient", "FX & Ambient", "FX/Amb"},
}};

// True for ids 1..17. `kNone` and any out-of-range value (a cast) are not assignable.
constexpr bool is_assignable(Category c) {
  const unsigned v = static_cast<unsigned>(c);
  return v >= 1 && v <= kCategoryCount;
}
constexpr unsigned id(Category c) { return static_cast<unsigned>(c); }

// Stable string key ("category.piano"); "none" for kNone; empty for a value outside the table.
constexpr std::string_view key(Category c) {
  return static_cast<unsigned>(c) <= kCategoryCount ? kCategories[static_cast<unsigned>(c)].key : std::string_view{};
}
constexpr std::string_view slug(Category c) {
  return static_cast<unsigned>(c) <= kCategoryCount ? kCategories[static_cast<unsigned>(c)].slug : std::string_view{};
}
constexpr std::string_view english_name(Category c) {
  return static_cast<unsigned>(c) <= kCategoryCount ? kCategories[static_cast<unsigned>(c)].english : std::string_view{};
}
constexpr std::string_view english_short_name(Category c) {
  return static_cast<unsigned>(c) <= kCategoryCount ? kCategories[static_cast<unsigned>(c)].english_short
                                                    : std::string_view{};
}

// 0 -> kNone, 1..17 -> the category, anything else -> nullopt.
std::optional<Category> category_from_id(std::uint32_t id);

// Parses a category name: the stable key (`category.piano`), the slug (`piano`), or the English full / short name
// ("E.Piano", "Mallet & Bells", "FX/Amb"), case-insensitive, ignoring spaces and punctuation. "none" gives kNone.
// Unknown names give nullopt, and so does any input with a byte outside printable ASCII ("Pianoé" is not "Piano"). Callers that store a category must also check `is_assignable`
// (`parse_assignable_category` does both).
std::optional<Category> parse_category(std::string_view name);
std::optional<Category> parse_assignable_category(std::string_view name);

// The text of a bundle's `category` file ("category.piano\n"); empty for kNone / out of range (never stored).
// `parse_category_file` is the reader: it accepts the stable key and, for bundles written before Story 5.5,
// the English names that were stored as free text; anything else (or "none") gives nullopt.
std::string category_file_text(Category c);
std::optional<Category> parse_category_file(std::string_view text);

// Reads `<bundle_dir>/category`: kAbsent (no such file: the bundle has no category), kValid, or kUnreadable (the file
// exists but holds "none", unknown or non-text content, is not a regular file, or is over 4 KiB), which callers must
// treat as "needs a category", not as "never had one".
enum class CategoryFileState : std::uint8_t { kAbsent, kValid, kUnreadable };
struct CategoryFileResult {
  CategoryFileState state = CategoryFileState::kAbsent;
  Category category = Category::kNone;  // set when kValid
};
CategoryFileResult read_category_file(const std::string& path);

}  // namespace velora::format

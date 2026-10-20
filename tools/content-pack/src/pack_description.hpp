#pragma once
// Story 6.3: the pack description file (JSON, schema in README.md).
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "errors.hpp"
#include "velora/format/hash.hpp"

namespace velora::contentpack {

enum class Root : int { kDev, kFactoryTest, kProduction };
const char* to_string(Root r);  // "dev", "factory-test", "production"

struct InstrumentOverride {
  std::string key, name, category;  // name / category empty = keep the default
};

struct PackDescription {
  std::string library_id;                          // canonical lower-case UUID text
  velora::format::Uuid library_uuid{};
  std::string name;                                // library display name
  std::map<std::string, std::string> localized_names;  // locale tag -> name (carried into pack.json only)
  std::string source;                              // absolute or relative to the description file's directory (resolved by the loader)
  Root root = Root::kDev;
  std::string category;                            // default category of every instrument
  std::vector<InstrumentOverride> instruments;
};

inline constexpr int kPackSchemaVersion = 1;

// Parses description text. `base_dir` resolves a relative `source`. Strict: unknown fields, wrong types, a duplicate field, a duplicate instrument key, an
// unknown category or a nil/invalid library_id are kBadDescription.
Failure parse_pack_description(const std::string& text, const std::string& base_dir, PackDescription& out);
Failure load_pack_description(const std::string& path, PackDescription& out);

// The register's identity of a library name: ASCII-trimmed, whitespace runs collapsed to one space, ASCII case folded. (Unicode confusables are not folded;
// invisible and bidirectional-control characters are refused outright by has_format_chars.)
std::string normalize_name(std::string_view name);
// True when `utf8` holds a zero-width, soft-hyphen, bidi-control, BOM or other invisible format character (U+00AD, U+061C, U+180E, U+200B-200F,
// U+202A-202E, U+2060-2064, U+2066-206F, U+FEFF) - names with them could pose as another library's name.
bool has_format_chars(std::string_view utf8);

// The dev/test range of library ids that need no register entry (root `dev` only): 00000000-0000-4000-8000-xxxxxxxxxxxx.
bool is_dev_range_id(const velora::format::Uuid& id);

}  // namespace velora::contentpack

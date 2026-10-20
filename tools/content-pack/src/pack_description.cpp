#include "pack_description.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

#include "velora/format/category.hpp"
#include "velora/format/json_lite.hpp"

namespace velora::contentpack {
namespace fmt = velora::format;

const char* to_string(Root r) {
  switch (r) {
    case Root::kDev: return "dev";
    case Root::kFactoryTest: return "factory-test";
    case Root::kProduction: return "production";
  }
  return "?";
}

const char* to_string(Code c) {
  switch (c) {
    case Code::kOk: return "ok";
    case Code::kUsage: return "usage";
    case Code::kBadDescription: return "bad_description";
    case Code::kBadKeyFile: return "bad_key_file";
    case Code::kRootMismatch: return "root_mismatch";
    case Code::kLibraryIdConflict: return "library_id_conflict";
    case Code::kLibraryIdUnregistered: return "library_id_unregistered";
    case Code::kRegisterIo: return "register_io";
    case Code::kImportSkipped: return "import_skipped";
    case Code::kImportFailed: return "import_failed";
    case Code::kBundleFailed: return "bundle_failed";
    case Code::kVaultFailed: return "vault_failed";
    case Code::kOutputFailed: return "output_failed";
    case Code::kVerifyFailed: return "verify_failed";
    case Code::kInternal: return "internal";
  }
  return "?";
}

std::string normalize_name(std::string_view name) {
  std::string out;
  bool pending_space = false;
  for (char c : name) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    pending_space = false;
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

bool has_format_chars(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = c;
    std::size_t n = 1;
    if (c >= 0xf0) { n = 4; cp = c & 0x07; }
    else if (c >= 0xe0) { n = 3; cp = c & 0x0f; }
    else if (c >= 0xc0) { n = 2; cp = c & 0x1f; }
    if (i + n > s.size()) return false;
    for (std::size_t k = 1; k < n; ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += n;
    if (cp == 0xad || cp == 0x61c || cp == 0x180e || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x2064) ||
        (cp >= 0x2066 && cp <= 0x206f) || cp == 0xfeff)
      return true;
  }
  return false;
}

bool is_dev_range_id(const fmt::Uuid& id) {
  static const std::uint8_t kPrefix[10] = {0, 0, 0, 0, 0, 0, 0x40, 0x00, 0x80, 0x00};
  return std::equal(kPrefix, kPrefix + 10, id.begin());
}

namespace {

Failure bad(const std::string& m) { return fail(Code::kBadDescription, m); }

bool clean_text(const std::string& s, std::size_t max) {
  if (s.empty() || s.size() > max || has_format_chars(s)) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

bool locale_tag_ok(const std::string& t) {
  if (t.empty() || t.size() > 35) return false;
  std::size_t part = 0;
  for (std::size_t i = 0; i <= t.size(); ++i) {
    if (i == t.size() || t[i] == '-') {
      if (part == 0 || part > 8) return false;
      part = 0;
    } else if (std::isalnum(static_cast<unsigned char>(t[i]))) {
      ++part;
    } else {
      return false;
    }
  }
  return std::isalpha(static_cast<unsigned char>(t[0])) != 0;
}

// Rejects duplicate members and members outside `allowed`.
Failure check_members(const fmt::JsonValue& o, const std::set<std::string>& allowed, const std::string& where) {
  std::set<std::string> seen;
  for (const std::string& k : o.keys) {
    if (!allowed.count(k)) return bad(where + ": unknown field '" + k + "'");
    if (!seen.insert(k).second) return bad(where + ": duplicate field '" + k + "'");
  }
  return {};
}

}  // namespace

Failure parse_pack_description(const std::string& text, const std::string& base_dir, PackDescription& out) {
  fmt::JsonValue doc;
  std::string err;
  if (!fmt::parse_json(text, doc, &err)) return bad("not valid JSON: " + err);
  if (!doc.is_object()) return bad("the description must be a JSON object");
  if (Failure f = check_members(doc, {"schema_version", "library_id", "name", "localized_names", "source", "root", "category", "instruments"}, "description"))
    return f;

  PackDescription d;
  const fmt::JsonValue* v = doc.find("schema_version");
  if (!v || !v->is_number() || v->number != kPackSchemaVersion) return bad("schema_version must be the number 1");

  v = doc.find("library_id");
  if (!v || !v->is_string()) return bad("library_id (UUID string) is required");
  if (!fmt::uuid_from_string(v->string, d.library_uuid)) return bad("library_id '" + v->string + "' is not a canonical UUID (8-4-4-4-12 hex)");
  if (d.library_uuid == fmt::Uuid{}) return bad("library_id must not be the nil UUID");
  d.library_id = fmt::uuid_to_string(d.library_uuid);

  v = doc.find("name");
  if (!v || !v->is_string() || !clean_text(v->string, 256) || normalize_name(v->string).empty())
    return bad("name is required (1 to 256 characters, no control, invisible or bidirectional-control characters)");
  d.name = v->string;

  if ((v = doc.find("localized_names"))) {
    if (!v->is_object()) return bad("localized_names must be an object of locale tag -> name");
    std::set<std::string> seen;
    for (std::size_t i = 0; i < v->keys.size(); ++i) {
      const std::string& tag = v->keys[i];
      if (!locale_tag_ok(tag)) return bad("localized_names: '" + tag + "' is not a locale tag (e.g. pt-BR)");
      if (!seen.insert(tag).second) return bad("localized_names: duplicate locale '" + tag + "'");
      if (!v->values[i].is_string() || !clean_text(v->values[i].string, 256)) return bad("localized_names." + tag + " must be a name of 1 to 256 characters");
      d.localized_names[tag] = v->values[i].string;
    }
  }

  v = doc.find("source");
  if (!v || !v->is_string() || !clean_text(v->string, 4096)) return bad("source (path of the .sfz or .sf2 file) is required");
  {
    std::filesystem::path p(v->string);
    if (p.is_relative() && !base_dir.empty()) p = std::filesystem::path(base_dir) / p;
    d.source = p.lexically_normal().string();
  }

  v = doc.find("root");
  if (!v || !v->is_string()) return bad("root (dev, factory-test or production) is required");
  if (v->string == "dev") d.root = Root::kDev;
  else if (v->string == "factory-test") d.root = Root::kFactoryTest;
  else if (v->string == "production") d.root = Root::kProduction;
  else return bad("root '" + v->string + "' is not dev, factory-test or production");

  auto category_ok = [&](const std::string& c, const std::string& where) -> Failure {
    const auto parsed = fmt::parse_assignable_category(c);
    if (!parsed) return bad(where + ": '" + c + "' is not an assignable category (a key such as category.piano, its slug or its English name)");
    return {};
  };
  v = doc.find("category");
  if (!v || !v->is_string()) return bad("category (the default category of every instrument) is required");
  if (Failure f = category_ok(v->string, "category")) return f;
  d.category = v->string;

  if ((v = doc.find("instruments"))) {
    if (!v->is_array()) return bad("instruments must be an array");
    std::set<std::string> keys;
    for (std::size_t i = 0; i < v->array.size(); ++i) {
      const fmt::JsonValue& o = v->array[i];
      const std::string where = "instruments[" + std::to_string(i) + "]";
      if (!o.is_object()) return bad(where + " must be an object");
      if (Failure f = check_members(o, {"key", "name", "category"}, where)) return f;
      InstrumentOverride io;
      const fmt::JsonValue* k = o.find("key");
      if (!k || !k->is_string() || !clean_text(k->string, 1024)) return bad(where + ".key is required");
      io.key = k->string;
      if (!keys.insert(io.key).second) return bad(where + ": duplicate instrument key '" + io.key + "'");
      if ((k = o.find("name"))) {
        if (!k->is_string() || !clean_text(k->string, 256)) return bad(where + ".name must be 1 to 256 characters");
        io.name = k->string;
      }
      if ((k = o.find("category"))) {
        if (!k->is_string()) return bad(where + ".category must be a string");
        if (Failure f = category_ok(k->string, where + ".category")) return f;
        io.category = k->string;
      }
      if (io.name.empty() && io.category.empty()) return bad(where + " overrides nothing (give name and/or category)");
      d.instruments.push_back(std::move(io));
    }
  }
  out = std::move(d);
  return {};
}

Failure load_pack_description(const std::string& path, PackDescription& out) {
  std::string text, err;
  if (!fmt::read_text_file(path, text, &err)) return bad(path + ": " + err);
  return parse_pack_description(text, std::filesystem::path(path).parent_path().string(), out);
}

}  // namespace velora::contentpack

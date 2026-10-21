#include "release_description.hpp"

#include <cctype>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <set>

#include "velora/format/category.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/format/json_lite.hpp"

namespace velora::factoryrelease {
namespace fmt = velora::format;
namespace host = velora::host;
namespace cp = velora::contentpack;

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
    case Code::kDuplicateLibrary: return "duplicate_library";
    case Code::kDuplicateProgramId: return "duplicate_program_id";
    case Code::kMissingCategory: return "missing_category";
    case Code::kUnresolvedReference: return "unresolved_reference";
    case Code::kInvalidLiveSet: return "invalid_live_set";
    case Code::kProgramHistory: return "program_history";
    case Code::kInternal: return "internal";
  }
  return "?";
}

namespace {

Failure bad(const std::string& m) { return fail(Code::kBadDescription, m); }

Failure check_members(const fmt::JsonValue& o, const std::set<std::string>& allowed, const std::string& where) {
  std::set<std::string> seen;
  for (const std::string& k : o.keys) {
    if (!allowed.count(k)) return bad(where + ": unknown field '" + k + "'");
    if (!seen.insert(k).second) return bad(where + ": duplicate field '" + k + "'");
  }
  return {};
}

bool is_uint(const fmt::JsonValue* v, double max, std::uint32_t& out) {
  if (!v || !v->is_number() || !std::isfinite(v->number) || v->number < 0 || v->number > max || v->number != std::floor(v->number)) return false;
  out = static_cast<std::uint32_t>(v->number);
  return true;
}
bool is_int(const fmt::JsonValue* v, double lo, double hi, int& out) {
  if (!v || !v->is_number() || !std::isfinite(v->number) || v->number < lo || v->number > hi || v->number != std::floor(v->number)) return false;
  out = static_cast<int>(v->number);
  return true;
}
bool is_float(const fmt::JsonValue* v, float& out) {
  if (!v || !v->is_number() || !std::isfinite(v->number)) return false;
  out = static_cast<float>(v->number);
  return true;
}
bool is_bool(const fmt::JsonValue* v, bool& out) {
  if (!v || v->type != fmt::JsonValue::Type::kBool) return false;
  out = v->boolean;
  return true;
}

Failure parse_params(const fmt::JsonValue* v, const std::string& where, std::vector<EngineParam>& out) {
  if (!v->is_array()) return bad(where + " must be an array of {param_id, value}");
  for (std::size_t i = 0; i < v->array.size(); ++i) {
    const fmt::JsonValue& o = v->array[i];
    const std::string w = where + "[" + std::to_string(i) + "]";
    if (!o.is_object()) return bad(w + " must be an object");
    if (Failure f = check_members(o, {"param_id", "value"}, w)) return f;
    EngineParam p;
    if (!is_uint(o.find("param_id"), 65535, p.param_id) || p.param_id == 0) return bad(w + ".param_id must be a registry id (1..65535)");
    if (!is_float(o.find("value"), p.value)) return bad(w + ".value must be a number");
    out.push_back(p);
  }
  return {};
}

}  // namespace

Failure parse_release_description(const std::string& text, const std::string& base_dir, ReleaseDescription& out) {
  fmt::JsonValue doc;
  std::string err;
  if (!fmt::parse_json(text, doc, &err)) return bad("not valid JSON: " + err);
  if (!doc.is_object()) return bad("the description must be a JSON object");
  if (Failure f = check_members(doc, {"schema_version", "release", "root", "libraries", "live_sets"}, "description")) return f;
  ReleaseDescription d;
  d.base_dir = base_dir;

  const fmt::JsonValue* v = doc.find("schema_version");
  if (!v || !v->is_number() || v->number != kReleaseSchemaVersion) return bad("schema_version must be the number 1");

  v = doc.find("release");
  if (!v || !v->is_object()) return bad("release ({name, version}) is required");
  if (Failure f = check_members(*v, {"name", "version"}, "release")) return f;
  {
    const fmt::JsonValue* n = v->find("name");
    if (!n || !n->is_string() || n->string.empty() || n->string.size() > 128 || cp::has_format_chars(n->string)) return bad("release.name is required (1 to 128 characters)");
    for (unsigned char c : n->string)
      if (c < 0x20 || c == 0x7f) return bad("release.name must not hold control characters");
    d.name = n->string;
    const fmt::JsonValue* ver = v->find("version");
    if (!ver || !ver->is_string() || !host::factory::valid_release_version(ver->string)) return bad("release.version must be MAJOR.MINOR.PATCH (decimal, e.g. 1.0.0)");
    d.version = ver->string;
  }

  v = doc.find("root");
  if (!v || !v->is_string()) return bad("root (dev, factory-test or production) is required");
  if (v->string == "dev") d.root = cp::Root::kDev;
  else if (v->string == "factory-test") d.root = cp::Root::kFactoryTest;
  else if (v->string == "production") d.root = cp::Root::kProduction;
  else return bad("root '" + v->string + "' is not dev, factory-test or production");

  v = doc.find("libraries");
  if (!v || !v->is_array() || v->array.empty()) return bad("libraries must be a non-empty array");
  if (v->array.size() > kMaxLibraries) return bad("too many libraries");
  std::set<std::string> ids, names;
  for (std::size_t i = 0; i < v->array.size(); ++i) {
    const fmt::JsonValue& o = v->array[i];
    const std::string where = "libraries[" + std::to_string(i) + "]";
    if (!o.is_object()) return bad(where + " must be an object");
    if (Failure f = check_members(o, {"library_id", "name", "localized_names", "source", "category", "revision", "instruments"}, where)) return f;
    // The library entry IS a pack description plus `revision`, with the release's root: it goes through the same parser (re-serialised without `revision`).
    LibraryEntry le;
    struct Ser {
      static void str(std::string& s, const std::string& v) {
        s += '"';
        for (unsigned char c : v) {
          if (c == '"' || c == '\\') { s += '\\'; s += static_cast<char>(c); }
          else if (c < 0x20 || c == 0x7f) { static const char* h = "0123456789abcdef"; s += "\\u00"; s += h[c >> 4]; s += h[c & 15]; }
          else s += static_cast<char>(c);
        }
        s += '"';
      }
      static void val(std::string& s, const fmt::JsonValue& j) {
        switch (j.type) {
          case fmt::JsonValue::Type::kNull: s += "null"; break;
          case fmt::JsonValue::Type::kBool: s += j.boolean ? "true" : "false"; break;
          case fmt::JsonValue::Type::kNumber: {
            if (j.number == std::floor(j.number) && std::fabs(j.number) < 9e15) s += std::to_string(static_cast<long long>(j.number));
            else { char b[40]; std::snprintf(b, sizeof b, "%.17g", j.number); s += b; }  // faithful: a fractional value is passed on unchanged (and refused by the pack parser if it is not valid there)
            break;
          }
          case fmt::JsonValue::Type::kString: str(s, j.string); break;
          case fmt::JsonValue::Type::kArray:
            s += '[';
            for (std::size_t n = 0; n < j.array.size(); ++n) { if (n) s += ','; val(s, j.array[n]); }
            s += ']';
            break;
          case fmt::JsonValue::Type::kObject:
            s += '{';
            for (std::size_t n = 0; n < j.keys.size(); ++n) { if (n) s += ','; str(s, j.keys[n]); s += ':'; val(s, j.values[n]); }
            s += '}';
            break;
        }
      }
    };
    std::string pt = "{\"schema_version\": 1, \"root\": \"" + std::string(cp::to_string(d.root)) + "\"";
    for (std::size_t k = 0; k < o.keys.size(); ++k) {
      if (o.keys[k] == "revision") continue;
      pt += ", ";
      Ser::str(pt, o.keys[k]);
      pt += ": ";
      Ser::val(pt, o.values[k]);
    }
    pt += "}";
    if (!o.find("category")) return fail(Code::kMissingCategory, where + ": category (the default category of every instrument) is required");
    if (const cp::Failure pf = cp::parse_pack_description(pt, base_dir, le.pack)) {
      // a category that is not assignable (including "none") is a missing category, any other problem a bad description
      if (pf.message.find("assignable category") != std::string::npos) return fail(Code::kMissingCategory, where + ": " + pf.message);
      return bad(where + ": " + pf.message);
    }
    if (const fmt::JsonValue* r = o.find("revision")) {
      if (!is_uint(r, 999999999, le.revision) || le.revision == 0) return bad(where + ".revision must be a whole number >= 1");
    }
    if (!ids.insert(le.pack.library_id).second) return fail(Code::kDuplicateLibrary, where + ": library_id " + le.pack.library_id + " is listed twice");
    if (!names.insert(cp::normalize_name(le.pack.name)).second) return fail(Code::kDuplicateLibrary, where + ": the library name '" + le.pack.name + "' is listed twice");
    d.libraries.push_back(std::move(le));
  }

  if ((v = doc.find("live_sets"))) {
    if (!v->is_object()) return bad("live_sets must be an object {file, entries}");
    if (Failure f = check_members(*v, {"file", "entries"}, "live_sets")) return f;
    d.live_set_file = host::factory::kDefaultFileName;
    if (const fmt::JsonValue* fl = v->find("file")) {
      static const std::string kExt = ".vfls";
      if (!fl->is_string() || fl->string.size() <= kExt.size() || fl->string.size() > 64 || fl->string.compare(fl->string.size() - kExt.size(), kExt.size(), kExt) != 0)
        return bad("live_sets.file must be a file name ending in .vfls");
      for (unsigned char c : fl->string)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return bad("live_sets.file may hold only letters, digits, '.', '-' and '_'");
      if (fl->string[0] == '.') return bad("live_sets.file must not start with a dot");
      d.live_set_file = fl->string;
    }
    const fmt::JsonValue* en = v->find("entries");
    if (!en || !en->is_array() || en->array.empty()) return bad("live_sets.entries must be a non-empty array");
    if (en->array.size() > host::factory::kMaxEntries) return bad("too many factory Live Sets");
    std::set<std::uint32_t> programs;
    for (std::size_t i = 0; i < en->array.size(); ++i) {
      const fmt::JsonValue& o = en->array[i];
      const std::string where = "live_sets.entries[" + std::to_string(i) + "]";
      if (!o.is_object()) return bad(where + " must be an object");
      if (Failure f = check_members(o, {"program_id", "name", "tempo", "parts", "master"}, where)) return f;
      LiveSetSpec ls;
      if (!is_uint(o.find("program_id"), 4294967295.0, ls.program_id) || ls.program_id == 0) return fail(Code::kDuplicateProgramId, where + ".program_id must be a vendor-assigned number >= 1");
      if (!programs.insert(ls.program_id).second) return fail(Code::kDuplicateProgramId, where + ": program id " + std::to_string(ls.program_id) + " is used twice");
      const fmt::JsonValue* n = o.find("name");
      if (!n || !n->is_string() || n->string.empty() || n->string.size() > 64) return bad(where + ".name is required (1 to 64 bytes)");
      ls.name = n->string;
      if (const fmt::JsonValue* t = o.find("tempo")) {
        if (!is_float(t, ls.tempo)) return bad(where + ".tempo must be a number");
      }
      const fmt::JsonValue* parts = o.find("parts");
      if (!parts || !parts->is_array() || parts->array.empty()) return bad(where + ".parts must be a non-empty array");
      std::set<std::uint32_t> used;
      for (std::size_t k = 0; k < parts->array.size(); ++k) {
        const fmt::JsonValue& po = parts->array[k];
        const std::string w = where + ".parts[" + std::to_string(k) + "]";
        if (!po.is_object()) return bad(w + " must be an object");
        if (Failure f = check_members(po, {"part", "timbre", "engine", "low_key", "high_key", "octave", "transpose", "keyboard_on", "volume", "send_reverb", "send_delay", "output"}, w))
          return f;
        PartSpec ps;
        if (!is_uint(po.find("part"), 15, ps.part)) return bad(w + ".part must be a part index (0..15)");
        if (!used.insert(ps.part).second) return bad(w + ": part " + std::to_string(ps.part) + " is listed twice");
        if (const fmt::JsonValue* t = po.find("timbre")) {
          if (!t->is_object()) return bad(w + ".timbre must be {library_id, instrument_key}");
          if (Failure f = check_members(*t, {"library_id", "instrument_key"}, w + ".timbre")) return f;
          const fmt::JsonValue* lid = t->find("library_id");
          const fmt::JsonValue* key = t->find("instrument_key");
          fmt::Uuid u{};
          if (!lid || !lid->is_string() || !fmt::uuid_from_string(lid->string, u)) return bad(w + ".timbre.library_id must be a canonical UUID");
          if (!key || !key->is_string() || key->string.empty() || key->string.size() > host::liveset::kMaxInstrumentKeyBytes) return bad(w + ".timbre.instrument_key is required (1 to 1024 bytes)");
          ps.has_timbre = true;
          ps.library_id = fmt::uuid_to_string(u);
          ps.instrument_key = key->string;
          if (!ids.count(ps.library_id))
            return fail(Code::kUnresolvedReference, w + ": library " + ps.library_id + " is not part of this release (a factory Live Set may only use the release's own libraries)");
        }
        if (const fmt::JsonValue* e = po.find("engine")) {
          if (!e->is_object()) return bad(w + ".engine must be {type, params}");
          if (Failure f = check_members(*e, {"type", "params"}, w + ".engine")) return f;
          if (const fmt::JsonValue* ty = e->find("type")) {
            if (ty->is_string() && ty->string == "sample") ps.engine_type = 1;
            else if (ty->is_string() && ty->string == "organ") ps.engine_type = 2;
            else if (!is_uint(ty, 255, ps.engine_type) || ps.engine_type == 0) return bad(w + ".engine.type must be \"sample\", \"organ\" or an engine type id");
          }
          if (const fmt::JsonValue* pr = e->find("params"))
            if (Failure f = parse_params(pr, w + ".engine.params", ps.params)) return f;
        }
        if (ps.has_timbre && ps.engine_type != 1) return bad(w + ": only a sample part has a timbre");
        auto opt_uint = [&](const char* key, double hi, bool& has, std::uint32_t& dst) -> Failure {
          if (const fmt::JsonValue* x = po.find(key)) {
            if (!is_uint(x, hi, dst)) return bad(w + "." + key + " is out of range");
            has = true;
          }
          return {};
        };
        auto opt_int = [&](const char* key, int lo, int hi, bool& has, int& dst) -> Failure {
          if (const fmt::JsonValue* x = po.find(key)) {
            if (!is_int(x, lo, hi, dst)) return bad(w + "." + key + " is out of range");
            has = true;
          }
          return {};
        };
        auto opt_float = [&](const char* key, bool& has, float& dst) -> Failure {
          if (const fmt::JsonValue* x = po.find(key)) {
            if (!is_float(x, dst)) return bad(w + "." + key + " must be a number");
            has = true;
          }
          return {};
        };
        if (Failure f = opt_uint("low_key", 127, ps.has_low_key, ps.low_key)) return f;
        if (Failure f = opt_uint("high_key", 127, ps.has_high_key, ps.high_key)) return f;
        if (Failure f = opt_int("octave", -4, 4, ps.has_octave, ps.octave)) return f;
        if (Failure f = opt_int("transpose", -12, 12, ps.has_transpose, ps.transpose)) return f;
        if (Failure f = opt_float("volume", ps.has_volume, ps.volume)) return f;
        if (Failure f = opt_float("send_reverb", ps.has_send_reverb, ps.send_reverb)) return f;
        if (Failure f = opt_float("send_delay", ps.has_send_delay, ps.send_delay)) return f;
        if (const fmt::JsonValue* x = po.find("keyboard_on")) {
          if (!is_bool(x, ps.keyboard_on)) return bad(w + ".keyboard_on must be true or false");
          ps.has_keyboard_on = true;
        }
        if (const fmt::JsonValue* x = po.find("output")) {
          if (!x->is_string() || (x->string != "main" && x->string != "aux")) return bad(w + ".output must be \"main\" or \"aux\"");
          ps.has_output = true;
          ps.output_aux = x->string == "aux";
        }
        ls.parts.push_back(std::move(ps));
      }
      if (const fmt::JsonValue* m = o.find("master"))
        if (Failure f = parse_params(m, where + ".master", ls.master)) return f;
      d.live_sets.push_back(std::move(ls));
    }
  }
  out = std::move(d);
  return {};
}

Failure load_release_description(const std::string& path, ReleaseDescription& out) {
  std::string text, err;
  if (!fmt::read_text_file(path, text, &err)) return bad(path + ": " + err);
  return parse_release_description(text, std::filesystem::path(path).parent_path().string(), out);
}

}  // namespace velora::factoryrelease

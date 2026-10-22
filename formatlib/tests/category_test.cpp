// Story 5.5: category model, proto twin guard, JSON reader, locale catalog (shipped locales).
// No timing assertions. VELORA_LOCALE_DIR and VELORA_CATEGORY_PROTO come from CMake.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "velora/format/category.hpp"
#include "velora/format/json_lite.hpp"
#include "velora/format/locale_catalog.hpp"

using namespace velora::format;

namespace {

// The frozen id -> key list. Ids only ever grow by addition: editing a line below is the signal that a
// stable id changed, which is never allowed; a new category is a new line at the end (and a new proto value).
struct Golden {
  unsigned id;
  const char* slug;
  const char* proto_name;
};
const Golden kGolden[] = {
    {1, "piano", "CATEGORY_PIANO"},         {2, "epiano", "CATEGORY_EPIANO"},
    {3, "organ", "CATEGORY_ORGAN"},         {4, "keys", "CATEGORY_KEYS"},
    {5, "mallet_bells", "CATEGORY_MALLET_BELLS"}, {6, "strings", "CATEGORY_STRINGS"},
    {7, "brass", "CATEGORY_BRASS"},         {8, "woodwind", "CATEGORY_WOODWIND"},
    {9, "choir_vocal", "CATEGORY_CHOIR_VOCAL"}, {10, "guitar", "CATEGORY_GUITAR"},
    {11, "bass", "CATEGORY_BASS"},          {12, "synth_lead", "CATEGORY_SYNTH_LEAD"},
    {13, "synth_pad", "CATEGORY_SYNTH_PAD"}, {14, "synth_poly", "CATEGORY_SYNTH_POLY"},
    {15, "drums_perc", "CATEGORY_DRUMS_PERC"}, {16, "world", "CATEGORY_WORLD"},
    {17, "fx_ambient", "CATEGORY_FX_AMBIENT"},
};

// The labels of the UX EXPERIENCE.md Categories table, verbatim: full and short, EN / PT-BR / ES.
struct Labels {
  const char *en, *en_s, *pt, *pt_s, *es, *es_s;
};
const Labels kLabels[] = {
    {"Piano", "Piano", "Piano", "Piano", "Piano", "Piano"},
    {"E.Piano", "E.Piano", "Piano Elétrico", "P.Elétrico", "Piano Eléctrico", "P.Eléctr."},
    {"Organ", "Organ", "Órgão", "Órgão", "Órgano", "Órgano"},
    {"Keys", "Keys", "Teclas", "Teclas", "Teclas", "Teclas"},
    {"Mallet & Bells", "Mallets", "Percussão Melódica", "Perc.Mel.", "Percusión Melódica", "Perc.Mel."},
    {"Strings", "Strings", "Cordas", "Cordas", "Cuerdas", "Cuerdas"},
    {"Brass", "Brass", "Metais", "Metais", "Metales", "Metales"},
    {"Woodwind", "Woodwind", "Madeiras", "Madeiras", "Maderas", "Maderas"},
    {"Choir & Vocal", "Choir", "Coral & Vozes", "Coral", "Coro & Voces", "Coro"},
    {"Guitar", "Guitar", "Violão & Guitarra", "Violão/Gt", "Guitarra", "Guitarra"},
    {"Bass", "Bass", "Baixo", "Baixo", "Bajo", "Bajo"},
    {"Synth Lead", "Synth Lead", "Synth Lead", "Synth Lead", "Synth Lead", "Synth Lead"},
    {"Synth Pad", "Synth Pad", "Synth Pad", "Synth Pad", "Synth Pad", "Synth Pad"},
    {"Synth Poly", "Synth Poly", "Synth Poly", "Synth Poly", "Synth Poly", "Synth Poly"},
    {"Drums & Perc", "Drums", "Bateria & Percussão", "Bateria", "Batería & Percusión", "Batería"},
    {"World", "World", "Étnicos", "Étnicos", "Étnicos", "Étnicos"},
    {"FX & Ambient", "FX/Amb", "Efeitos & Ambiências", "Efeitos", "Efectos & Ambientes", "Efectos"},
};

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------------------------------------------------------
// The check the story asks for: every category key in every shipped locale, short labels <= 10 code points, and the
// same key set in all locales. Returns the problems (empty = ok). Reused on broken catalogs to prove it fails.
std::vector<std::string> check_catalog(const LocaleCatalog& cat) {
  std::vector<std::string> problems;
  for (std::string_view loc : kShippedLocales) {
    if (!cat.has_locale(loc)) {
      problems.push_back("locale " + std::string(loc) + " is not loaded");
      continue;
    }
    for (unsigned i = 1; i <= kCategoryCount; ++i) {
      const Category c = static_cast<Category>(i);
      for (bool sh : {false, true}) {
        const std::string k = LocaleCatalog::category_key(c, sh);
        const auto v = cat.find_own(loc, k);  // no English fallback: a missing key must fail
        if (!v || v->empty()) {
          problems.push_back(std::string(loc) + ": missing " + k);
        } else if (sh && utf8_length(*v) > kMaxShortLabelLength) {
          problems.push_back(std::string(loc) + ": short label too long: " + k);
        }
      }
    }
  }
  std::set<std::string> base;
  for (const auto& k : cat.keys("en")) base.insert(k);
  for (std::string_view loc : kShippedLocales) {
    std::set<std::string> ks;
    for (const auto& k : cat.keys(loc)) ks.insert(k);
    if (ks != base) problems.push_back(std::string(loc) + ": key set differs from en");
  }
  return problems;
}

std::string json_with(const std::string& locale_tag, const std::string& short_piano, bool drop_epiano_short) {
  (void)locale_tag;
  std::string j = "{";
  bool first = true;
  for (unsigned i = 1; i <= kCategoryCount; ++i) {
    const Category c = static_cast<Category>(i);
    for (bool sh : {false, true}) {
      if (drop_epiano_short && c == Category::kEPiano && sh) continue;
      if (!first) j += ",";
      first = false;
      j += "\"" + LocaleCatalog::category_key(c, sh) + "\":\"";
      j += (c == Category::kPiano && sh) ? short_piano : std::string(sh ? english_short_name(c) : english_name(c));
      j += "\"";
    }
  }
  return j + "}";
}

void test_table() {
  CHECK(kCategoryCount == 17 && kCategories.size() == 18);
  CHECK(!is_assignable(Category::kNone));
  CHECK(!is_assignable(static_cast<Category>(18)) && !is_assignable(static_cast<Category>(255)));
  CHECK(key(Category::kNone) == "none");
  CHECK(key(static_cast<Category>(99)).empty() && slug(static_cast<Category>(99)).empty());
  std::set<std::string> keys, names;
  for (unsigned i = 0; i <= kCategoryCount; ++i) {
    CHECK(id(kCategories[i].id) == i);  // the table is indexed by id
    if (i == 0) continue;
    const Category c = static_cast<Category>(i);
    CHECK(is_assignable(c));
    CHECK(std::string(key(c)) == "category." + std::string(slug(c)));
    CHECK(keys.insert(std::string(key(c))).second);
    CHECK(names.insert(std::string(english_name(c))).second);
    CHECK(category_from_id(i) == c);
  }
  CHECK(category_from_id(0) == Category::kNone && !category_from_id(18) && !category_from_id(0xFFFFFFFFu));
  // Frozen ids.
  CHECK(sizeof(kGolden) / sizeof(kGolden[0]) == kCategoryCount);
  for (const Golden& g : kGolden) {
    CHECK(std::string(slug(static_cast<Category>(g.id))) == g.slug);
    CHECK(std::string(key(static_cast<Category>(g.id))) == std::string("category.") + g.slug);
  }
  // English names are the first column of the labels table (the locale files are checked against them below).
  for (unsigned i = 1; i <= kCategoryCount; ++i) {
    CHECK(std::string(english_name(static_cast<Category>(i))) == kLabels[i - 1].en);
    CHECK(std::string(english_short_name(static_cast<Category>(i))) == kLabels[i - 1].en_s);
  }
}

void test_parse() {
  CHECK(parse_category("Piano") == Category::kPiano);
  CHECK(parse_category("piano") == Category::kPiano);
  CHECK(parse_category("PIANO") == Category::kPiano);
  CHECK(parse_category("category.piano") == Category::kPiano);
  CHECK(parse_category("Category.EPiano") == Category::kEPiano);
  CHECK(parse_category("E.Piano") == Category::kEPiano);
  CHECK(parse_category("e piano") == Category::kEPiano);
  CHECK(parse_category("Mallet & Bells") == Category::kMalletBells);
  CHECK(parse_category("mallet_bells") == Category::kMalletBells);
  CHECK(parse_category("category.mallet_bells") == Category::kMalletBells);
  CHECK(parse_category("Mallets") == Category::kMalletBells);
  CHECK(parse_category("choir & vocal") == Category::kChoirVocal);
  CHECK(parse_category("FX/Amb") == Category::kFxAmbient);
  CHECK(parse_category("fx & ambient") == Category::kFxAmbient);
  CHECK(parse_category("Drums & Perc") == Category::kDrumsPerc);
  // Every English name, short name, slug and key of the table round-trips.
  for (unsigned i = 1; i <= kCategoryCount; ++i) {
    const Category c = static_cast<Category>(i);
    CHECK(parse_category(english_name(c)) == c);
    CHECK(parse_category(english_short_name(c)) == c);
    CHECK(parse_category(slug(c)) == c);
    CHECK(parse_category(key(c)) == c);
  }
  // Unknown names, localized names (English only here) and garbage are refused.
  CHECK(!parse_category("") && !parse_category("   ") && !parse_category("Pianos") && !parse_category("Órgão"));
  CHECK(!parse_category("category.") && !parse_category("category.nope") && !parse_category("category.Piano Elétrico"));
  CHECK(!parse_category("../etc") && !parse_category("a/b"));
  // none parses, but it is never assignable.
  CHECK(parse_category("none") == Category::kNone && parse_category("None") == Category::kNone);
  CHECK(parse_category("category.none") == Category::kNone);
  CHECK(!parse_assignable_category("none") && !parse_assignable_category("nope"));
  CHECK(parse_assignable_category("Keys") == Category::kKeys);
  // The bundle file: the stable key, never none.
  CHECK(category_file_text(Category::kPiano) == "category.piano\n");
  CHECK(category_file_text(Category::kNone).empty() && category_file_text(static_cast<Category>(40)).empty());
  CHECK(parse_category_file("category.organ\n") == Category::kOrgan);
  CHECK(parse_category_file("Piano\n") == Category::kPiano);   // written before Story 5.5 (free text)
  CHECK(parse_category_file("  Keys \r\n") == Category::kKeys);
  CHECK(!parse_category_file("none\n") && !parse_category_file("\n") && !parse_category_file("Whatever\n") &&
        !parse_category_file(""));
}

// Parses proto/category.proto: `NAME = N;` lines and `reserved` statements (comments stripped).
void test_proto_guard() {
  const std::string text = slurp(VELORA_CATEGORY_PROTO);
  CHECK(!text.empty());
  std::string code;
  {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      const auto c = line.find("//");
      code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
  }
  CHECK(code.find("package velora.library.v1;") != std::string::npos);
  CHECK(code.find("enum Category") != std::string::npos);
  std::vector<std::pair<std::string, unsigned>> values;
  std::set<unsigned> reserved_ids;
  std::set<std::string> reserved_names;
  std::istringstream in(code);
  std::string line;
  bool in_enum = false;
  while (std::getline(in, line)) {
    if (line.find("enum Category") != std::string::npos) in_enum = true;
    if (!in_enum) continue;
    auto trim = [](std::string s) {
      const auto b = s.find_first_not_of(" \t");
      const auto e = s.find_last_not_of(" \t\r");
      return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    const std::string t = trim(line);
    if (t.rfind("reserved", 0) == 0) {
      std::string rest = t.substr(8);
      for (char& ch : rest)
        if (ch == ',' || ch == ';') ch = ' ';
      std::istringstream rs(rest);
      std::string tok;
      std::vector<std::string> toks;
      while (rs >> tok) toks.push_back(tok);
      for (std::size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "to") continue;
        if (toks[i][0] == '"') {
          reserved_names.insert(toks[i].substr(1, toks[i].size() - 2));
        } else if (i + 2 < toks.size() && toks[i + 1] == "to") {
          const unsigned lo = std::stoul(toks[i]), hi = toks[i + 2] == "max" ? 1000000u : std::stoul(toks[i + 2]);
          for (unsigned v = lo; v <= std::min(hi, 100000u); ++v) reserved_ids.insert(v);
        } else {
          reserved_ids.insert(std::stoul(toks[i]));
        }
      }
      continue;
    }
    const auto eq = t.find('=');
    const auto semi = t.find(';');
    if (eq == std::string::npos || semi == std::string::npos || t.rfind("CATEGORY_", 0) != 0) continue;
    values.emplace_back(trim(t.substr(0, eq)), static_cast<unsigned>(std::stoul(trim(t.substr(eq + 1, semi - eq - 1)))));
  }
  CHECK(values.size() == kCategoryCount + 1);
  CHECK(!values.empty() && values[0].first == "CATEGORY_NONE" && values[0].second == 0);
  std::set<unsigned> seen_ids;
  std::set<std::string> seen_names;
  for (const auto& v : values) {
    CHECK(seen_ids.insert(v.second).second);    // no id twice
    CHECK(seen_names.insert(v.first).second);   // no name twice
    CHECK(reserved_ids.count(v.second) == 0);   // a reserved id is never defined
    CHECK(reserved_names.count(v.first) == 0);
  }
  // The proto and the header agree, value by value, and both agree with the frozen list.
  for (const Golden& g : kGolden) {
    const auto it = std::find_if(values.begin(), values.end(), [&](const auto& v) { return v.second == g.id; });
    CHECK(it != values.end());
    if (it == values.end()) continue;
    CHECK(it->first == g.proto_name);
    std::string up(g.slug);
    for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    CHECK(it->first == "CATEGORY_" + up);  // the naming rule
  }
}

void test_json() {
  JsonValue v;
  std::string err;
  CHECK(parse_json(R"({"a":"x\n\"\\\/\u00e9\ud83c\udfb9","b":[1,-2.5e1,true,false,null,{}],"c":{"d":"e"}})", v, &err));
  CHECK(v.is_object() && v.find("a") && v.find("a")->string == "x\n\"\\/\xC3\xA9\xF0\x9F\x8E\xB9");
  CHECK(v.find("b") && v.find("b")->array.size() == 6 && v.find("b")->array[1].number == -25.0);
  CHECK(v.find("c") && v.find("c")->find("d") && v.find("c")->find("d")->string == "e");
  CHECK(!v.find("zz"));
  CHECK(parse_json("\xEF\xBB\xBF {\"k\":\"v\"} \n", v));  // BOM and whitespace
  const char* bad[] = {"", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1,]", "{\"a\":1} x", "\"\\q\"", "\"\\ud800\"", "\"\\udc00\"",
                       "\"\\u12\"", "\"tab\there\"", "01", "1.", "-", "nul", "\"\xC3\"", "\"\xC0\x80\"", "{'a':1}"};
  for (const char* b : bad) CHECK(!parse_json(b, v, &err));
  std::string deep(40, '[');
  deep += std::string(40, ']');
  CHECK(!parse_json(deep, v));
  CHECK(parse_json(std::string(20, '[') + std::string(20, ']'), v));
  CHECK(utf8_length("") == 0 && utf8_length("abc") == 3 && utf8_length("P.Elétrico") == 10);
  CHECK(utf8_length("\xC3\x81\xC3\x87\xC3\x8A\xC3\x95\xC3\x91") == 5);  // ÁÇÊÕÑ: 5 code points, 10 bytes
  CHECK(utf8_length("\xF0\x9F\x8E\xB9") == 1);
}

void test_locale_files() {
  LocaleCatalog cat;
  std::string err;
  CHECK(cat.load_shipped(VELORA_LOCALE_DIR, &err));
  if (!err.empty()) std::printf("load error: %s\n", err.c_str());
  for (const std::string& p : check_catalog(cat)) {
    std::printf("PROBLEM %s\n", p.c_str());
    CHECK(false);
  }
  // The shipped labels are exactly the EXPERIENCE.md table.
  for (unsigned i = 1; i <= kCategoryCount; ++i) {
    const Category c = static_cast<Category>(i);
    const Labels& l = kLabels[i - 1];
    CHECK(cat.category_label("en", c) == l.en && cat.category_label("en", c, true) == l.en_s);
    CHECK(cat.category_label("pt-BR", c) == l.pt && cat.category_label("pt-BR", c, true) == l.pt_s);
    CHECK(cat.category_label("es", c) == l.es && cat.category_label("es", c, true) == l.es_s);
  }
  // Category entries plus UI shell keys (Story 2.17; Epic 10 extends further).
  CHECK(cat.keys("en").size() >= 2 * kCategoryCount);
  CHECK(cat.category_label("en", Category::kNone).empty());
  // Short labels are measured in code points, not bytes: "P.Elétrico" is 10 code points and 11 bytes.
  const std::string pe(*cat.find_own("pt-BR", "category.epiano.short"));
  CHECK(utf8_length(pe) == 10 && pe.size() == 11);
}

void test_catalog_checker_fails_on_defects() {
  std::string err;
  auto build = [&](const std::string& en, const std::string& pt, const std::string& es) {
    LocaleCatalog c;
    CHECK(c.load_text("en", en, &err) && c.load_text("pt-BR", pt, &err) && c.load_text("es", es, &err));
    return c;
  };
  const std::string ok = json_with("", "Piano", false);
  CHECK(check_catalog(build(ok, ok, ok)).empty());
  // A key missing in one shipped locale is reported (and the English fallback does not hide it).
  {
    const auto p = check_catalog(build(ok, json_with("", "Piano", true), ok));
    CHECK(!p.empty());
    bool named = false;
    for (const auto& s : p) named = named || s.find("pt-BR: missing category.epiano.short") != std::string::npos;
    CHECK(named);
    bool differs = false;
    for (const auto& s : p) differs = differs || s.find("pt-BR: key set differs") != std::string::npos;
    CHECK(differs);
  }
  // 11 code points is too long; 10 code points of 2-byte letters (20 bytes) is fine.
  CHECK(!check_catalog(build(ok, json_with("", "Abcdefghijk", false), ok)).empty());
  CHECK(check_catalog(build(ok, json_with("", "\xC3\x81\xC3\x87\xC3\x8A\xC3\x95\xC3\x91\xC3\x81\xC3\x87\xC3\x8A\xC3\x95\xC3\x91", false), ok)).empty());
  CHECK(!check_catalog(build(ok, json_with("", "\xC3\x81\xC3\x87\xC3\x8A\xC3\x95\xC3\x91\xC3\x81\xC3\x87\xC3\x8A\xC3\x95\xC3\x91\xC3\x81", false), ok)).empty());
  // A locale that is not loaded at all is a problem.
  {
    LocaleCatalog c;
    CHECK(c.load_text("en", ok, &err) && c.load_text("pt-BR", ok, &err));
    CHECK(!check_catalog(c).empty());
  }
  // An extra key in one locale breaks the equal-key-set rule.
  {
    const std::string extra = ok.substr(0, ok.size() - 1) + ",\"x.only\":\"y\"}";
    CHECK(!check_catalog(build(ok, extra, ok)).empty());
  }
}

void test_catalog_api() {
  LocaleCatalog c;
  std::string err;
  CHECK(!c.load_text("en", "[]", &err) && !err.empty());
  CHECK(!c.load_text("en", "{\"a\":1}", &err));
  CHECK(!c.load_text("en", "{\"a\":\"x\",\"a\":\"y\"}", &err) && err.find("duplicate") != std::string::npos);
  CHECK(!c.load_text("en", "{\"a\":{}}", &err));
  CHECK(!c.load_text("en", "{\"\":\"x\"}", &err));
  CHECK(!c.load_text("", "{}", &err));
  CHECK(!c.load_file("en", "/nonexistent/dir/en.json", &err));
  CHECK(c.locales().empty());  // failures change nothing
  CHECK(c.load_text("en", "{\"hello\":\"Hello\",\"only.en\":\"E\"}", &err));
  CHECK(c.load_text("es", "{\"hello\":\"Hola\"}", &err));
  CHECK(c.get("es", "hello") == "Hola" && c.get("en", "hello") == "Hello");
  CHECK(c.get("es", "only.en") == "E");      // English fallback for a missing key
  CHECK(c.get("fr", "hello") == "Hello");    // English fallback for an unknown locale
  CHECK(c.get("es", "nope") == "nope");      // missing everywhere: the key itself, visibly
  CHECK(!c.find_own("es", "only.en") && c.find_own("es", "hello"));
  CHECK(c.has_locale("es") && !c.has_locale("fr"));
  CHECK(c.load_text("es", "{\"hello\":\"Buenas\"}", &err) && c.get("es", "hello") == "Buenas");  // replace
  CHECK(c.locales().size() == 2);
  CHECK(LocaleCatalog::category_key(Category::kMalletBells, true) == "category.mallet_bells.short");
  CHECK(LocaleCatalog::category_key(Category::kPiano, false) == "category.piano.full");
}

void test_hardening() {
  namespace fs = std::filesystem;
  JsonValue v;
  // Numbers are locale independent: a decimal-comma locale (when installed) must not change 1.5.
  const char* old = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = old ? old : "C";
  for (const char* loc : {"de_DE.UTF-8", "pt_BR.UTF-8", "fr_FR.UTF-8"})
    if (std::setlocale(LC_NUMERIC, loc)) break;
  CHECK(parse_json("[1.5, 2.25e1, -0.5]", v) && v.array[0].number == 1.5 && v.array[1].number == 22.5 && v.array[2].number == -0.5);
  std::setlocale(LC_NUMERIC, saved.c_str());
  CHECK(!parse_json("1e999", v));
  // Size cap.
  CHECK(!parse_json(std::string(kMaxJsonBytes + 1, ' ') + "1", v));
  CHECK(parse_json("[" + std::string(1000, ' ') + "]", v));
  const fs::path dir = fs::temp_directory_path() / ("velora_cat_fmt_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::string text, err;
  {
    std::ofstream(dir / "big.json") << std::string(5000, 'x');
    CHECK(!read_text_file((dir / "big.json").string(), text, &err, 4096) && err.find("too large") != std::string::npos);
    CHECK(read_text_file((dir / "big.json").string(), text, &err, 5000) && text.size() == 5000);
    CHECK(!read_text_file((dir / "missing.json").string(), text, &err));
    CHECK(!read_text_file(dir.string(), text, &err));  // a directory
    CHECK(::mkfifo((dir / "fifo.json").c_str(), 0600) == 0);
    CHECK(!read_text_file((dir / "fifo.json").string(), text, &err) && err.find("not a regular file") != std::string::npos);  // no hang
    LocaleCatalog c;
    CHECK(!c.load_file("en", (dir / "fifo.json").string(), &err));
  }
  // Copies do not dangle when a locale is reloaded; a regional tag finds its language; case and '_' are free.
  {
    LocaleCatalog c;
    CHECK(c.load_text("pt_br", "{\"a\":\"um\"}", &err));
    CHECK(c.locales() == std::vector<std::string>{"pt-BR"});  // canonical id
    auto held = c.find_own("pt-BR", "a");
    CHECK(held && *held == "um");
    for (int i = 0; i < 200; ++i) CHECK(c.load_text("es", "{\"k" + std::to_string(i) + "\":\"v\"}", &err));
    CHECK(c.load_text("pt-BR", "{\"a\":\"dois\"}", &err));
    CHECK(*held == "um" && *c.find_own("pt-BR", "a") == "dois");
    for (const char* t : {"pt", "pt_BR", "pt-br", "PT-BR", "pt-PT"}) CHECK(c.find_own(t, "a") == std::optional<std::string>("dois"));
    for (const char* t : {"es", "es-ES", "es_MX", "ES"}) CHECK(c.find_own(t, "k199") == std::optional<std::string>("v"));
    CHECK(!c.find_own("fr", "a") && !c.has_locale("fr") && c.has_locale("es-MX"));
    CHECK(c.load_text("en", "{\"a\":\"one\"}", &err));
    CHECK(c.get("fr", "a") == "one" && c.get("es-MX", "a") == "one");  // missing in es: English fallback after the language one
  }
  // load_shipped is all or nothing.
  {
    LocaleCatalog c;
    fs::create_directories(dir / "loc");
    std::ofstream(dir / "loc/en.json") << "{\"a\":\"x\"}";
    std::ofstream(dir / "loc/pt-BR.json") << "{\"a\":\"y\"}";  // es.json missing
    CHECK(!c.load_shipped((dir / "loc").string(), &err));
    CHECK(c.locales().empty());
    CHECK(c.load_text("en", "{\"keep\":\"1\"}", &err));
    CHECK(!c.load_shipped((dir / "loc").string(), &err));
    CHECK(c.locales().size() == 1 && c.find_own("en", "keep") && !c.find_own("en", "a"));
  }
  // kNone has no label in the UX table: callers handle the unassigned state; the label is explicitly empty.
  {
    LocaleCatalog c;
    CHECK(c.load_shipped(VELORA_LOCALE_DIR, &err));
    for (const char* loc : {"en", "pt-BR", "es"}) {
      CHECK(c.category_label(loc, Category::kNone).empty() && c.category_label(loc, Category::kNone, true).empty());
      CHECK(!c.find_own(loc, "category.none.full"));
    }
  }
  // Category parsing rejects instead of stripping.
  CHECK(!parse_category("Pian\xC3\xA9") && !parse_category("Piano\xC3\xA9") && !parse_category("Pia\tno") && !parse_category("Piano\n"));
  CHECK(!parse_category("\xFF"));
  CHECK(parse_category_file("\tcategory.keys\t\n") == Category::kKeys);
  // The bundle file: absent vs unreadable vs valid.
  {
    const std::string f = (dir / "category").string();
    CHECK(read_category_file(f).state == CategoryFileState::kAbsent);
    std::ofstream(f) << "category.bass\n";
    CHECK(read_category_file(f).state == CategoryFileState::kValid && read_category_file(f).category == Category::kBass);
    std::ofstream(f, std::ios::trunc) << "Piano\n";
    CHECK(read_category_file(f).state == CategoryFileState::kValid && read_category_file(f).category == Category::kPiano);
    std::ofstream(f, std::ios::trunc) << "garbage\n";
    CHECK(read_category_file(f).state == CategoryFileState::kUnreadable);
    std::ofstream(f, std::ios::trunc) << "none\n";
    CHECK(read_category_file(f).state == CategoryFileState::kUnreadable);
    std::ofstream(f, std::ios::trunc) << std::string(10000, 'a');
    CHECK(read_category_file(f).state == CategoryFileState::kUnreadable);
    std::ofstream(f, std::ios::trunc) << "";
    CHECK(read_category_file(f).state == CategoryFileState::kUnreadable);
    fs::remove(f);
    CHECK(::mkfifo(f.c_str(), 0600) == 0);
    CHECK(read_category_file(f).state == CategoryFileState::kUnreadable);  // no hang
  }
  fs::remove_all(dir);
}

}  // namespace

int main() {
  test_table();
  test_parse();
  test_proto_guard();
  test_json();
  test_locale_files();
  test_catalog_checker_fails_on_defects();
  test_catalog_api();
  test_hardening();
  if (g_fail) {
    std::printf("format_category_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("format_category_test: ok\n");
  return 0;
}

// Story 5.5: category suggestion (keyword table, tokenizer, precedence), the fixture file, the embedded table,
// category storage in the publish and the CLI. No timing assertions. Nothing committed but the TSV.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sf2_builder.hpp"
#include "test_util.hpp"
#include "velora/format/category.hpp"
#include "velora/librarian/batch.hpp"
#include "velora/librarian/category_suggest.hpp"
#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;
using namespace velora::librarian;
namespace fmt = velora::format;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string run(const std::string& cmd, int* status = nullptr) {
  std::string out;
  FILE* p = ::popen(cmd.c_str(), "r");
  if (!p) return out;
  char buf[512];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  const int rc = ::pclose(p);
  if (status) *status = rc;
  return out;
}

struct Env {
  fs::path dir, src, root;
  explicit Env(const char* name) {
    dir = fs::temp_directory_path() / (std::string("velora_category_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    src = dir / "src";
    root = dir / "data";
    fs::create_directories(src);
    fs::create_directories(root);
  }
  ~Env() { std::error_code ec; fs::remove_all(dir, ec); }
  fs::path lib() const { return root / "lib"; }
  ImportOptions opts(const std::string& name) const {
    ImportOptions o;
    o.sfz_path = (src / name).string();
    o.root = root.string();
    return o;
  }
  // A one-sample SFZ named `name` (16-bit mono WAV of 200 frames), plus optional extra header text.
  void put_sfz(const std::string& name, const std::string& header = "") const {
    fs::create_directories((src / name).parent_path());
    std::vector<std::uint8_t> w;
    auto w16 = [&](unsigned v) { w.push_back(v & 255); w.push_back((v >> 8) & 255); };
    auto w32 = [&](unsigned v) { w16(v & 0xFFFF); w16(v >> 16); };
    auto tag = [&](const char* t) { w.insert(w.end(), t, t + 4); };
    const unsigned frames = 200;
    tag("RIFF"); w32(36 + frames * 2); tag("WAVE"); tag("fmt "); w32(16); w16(1); w16(1); w32(48000); w32(96000); w16(2); w16(16);
    tag("data"); w32(frames * 2);
    for (unsigned i = 0; i < frames; ++i) w16((i * 37) & 0x7fff);
    std::ofstream(src / "s.wav", std::ios::binary).write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
    std::ofstream(src / name, std::ios::binary) << header << "<region> sample=s.wav key=60\n";
  }
};

std::optional<fmt::Category> suggest_name(const std::string& n) {
  return suggest_category(suggest_input_for_path(n, {}));
}

void test_tokenizer() {
  using V = std::vector<std::string>;
  CHECK(tokenize_for_category("Rhodes_Mk1") == (V{"rhodes", "mk"}));
  CHECK(tokenize_for_category("EPiano") == (V{"e", "piano"}));
  CHECK(tokenize_for_category("E.Piano") == (V{"e", "piano"}));
  CHECK(tokenize_for_category("upright-bass  jazz") == (V{"upright", "bass", "jazz"}));
  CHECK(tokenize_for_category("ViolinsSustain") == (V{"violins", "sustain"}));
  CHECK(tokenize_for_category("XMLFile") == (V{"xml", "file"}));
  CHECK(tokenize_for_category("FX") == (V{"fx"}));
  CHECK(tokenize_for_category("Synth Strings 1") == (V{"synth", "strings"}));
  CHECK(tokenize_for_category("808 Kit") == (V{"kit"}));
  CHECK(tokenize_for_category("VSCO2") == (V{"vsco"}));
  CHECK(tokenize_for_category("").empty() && tokenize_for_category("__--__").empty() && tokenize_for_category("123").empty());
  // Accents fold (PT-BR / ES), upper and lower case alike; combining marks (NFD names from macOS) are dropped.
  CHECK(tokenize_for_category("Violão Nylon") == (V{"violao", "nylon"}));
  CHECK(tokenize_for_category("ÓRGÃO") == (V{"orgao"}));
  CHECK(tokenize_for_category("Batería") == (V{"bateria"}));
  CHECK(tokenize_for_category("Cuerdas Sint\xC3\xA9ticas") == (V{"cuerdas", "sinteticas"}));
  CHECK(tokenize_for_category("Violao\xCC\x83") == (V{"violao"}));         // o + combining tilde
  CHECK(tokenize_for_category("Viola\xCC\x83o") == (V{"violao"}));
  CHECK(tokenize_for_category("Piano \xE2\x80\x94 Cauda") == (V{"piano", "cauda"}));  // em dash separates
  CHECK(tokenize_for_category("\xFF\xFEpiano") == (V{"piano"}));            // invalid UTF-8 never matches by accident
  // Overlong, surrogate and out-of-range forms never form letters (E0 81 A1 is an overlong 'a').
  CHECK(tokenize_for_category("p\xE0\x81\xA1iano") == (V{"p", "iano"}));
  CHECK(tokenize_for_category("p\xC1\xA1iano") == (V{"p", "iano"}));
  CHECK(tokenize_for_category("p\xED\xA0\x80iano") == (V{"p", "iano"}));
  CHECK(tokenize_for_category("p\xF4\x90\x80\x80iano") == (V{"p", "iano"}));
  // Ligatures spell out, upper and lower case alike; letters outside the table separate words.
  CHECK(tokenize_for_category("Stra\xC3\x9F" "e") == (V{"strasse"}));
  CHECK(tokenize_for_category("C\xC5\x93ur") == (V{"coeur"}));
  CHECK(tokenize_for_category("\xC3\x86on") == (V{"aeon"}));
  CHECK(tokenize_for_category("b\xC3\xA6ss") == (V{"baess"}));
  CHECK(tokenize_for_category("Bj\xC3\xB8rn") == (V{"bjorn"}));
  CHECK(tokenize_for_category("a\xC4\x9F" "b") == (V{"a", "b"}));  // g-breve is not in the table: a separator
}

void test_table() {
  const KeywordTable& t = KeywordTable::embedded();
  CHECK(t.version() >= 1 && t.entries().size() > 200);
  // The embedded copy equals the data file.
  KeywordTable file;
  std::string err;
  CHECK(KeywordTable::load_file(std::string(LIBRARIAN_DATA_DIR) + "/category_keywords.json", file, &err));
  CHECK(file.version() == t.version() && file.entries().size() == t.entries().size());
  CHECK(KeywordTable::embedded_json() == slurp(std::string(LIBRARIAN_DATA_DIR) + "/category_keywords.json"));
  // Every assignable category has keywords, and `none` has its exception phrases.
  std::set<fmt::Category> cats;
  bool has_exception = false;
  for (const auto& e : t.entries()) {
    cats.insert(e.category);
    has_exception = has_exception || e.category == fmt::Category::kNone;
    CHECK(!e.tokens.empty() && e.weight >= 1);
  }
  for (unsigned i = 1; i <= fmt::kCategoryCount; ++i) CHECK(cats.count(static_cast<fmt::Category>(i)) == 1);
  CHECK(has_exception);
  // The three languages are in the table (a few anchors from the story).
  auto has = [&](const char* ph, fmt::Category c) {
    for (const auto& e : t.entries())
      if (e.phrase == ph) return e.category == c;
    return false;
  };
  CHECK(has("rhodes", fmt::Category::kEPiano) && has("wurli", fmt::Category::kEPiano));
  CHECK(has("viola", fmt::Category::kStrings) && has("violino", fmt::Category::kStrings) &&
        has("strings", fmt::Category::kStrings) && has("cordas", fmt::Category::kStrings) && has("cuerdas", fmt::Category::kStrings));
  CHECK(has("upright", fmt::Category::kBass) && has("baixo", fmt::Category::kBass) && has("bajo", fmt::Category::kBass));
  CHECK(has("viola caipira", fmt::Category::kNone));
  // Parse errors.
  KeywordTable x;
  CHECK(KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"piano","weight":10,"phrases":["piano"]}]})", x));
  CHECK(!KeywordTable::parse("{", x));
  // The embedded table loaded; a corrupted embedded text is reported loudly, never as a silently empty table.
  CHECK(KeywordTable::embedded_error().empty() && KeywordTable::embedded().valid());
  {
    const auto bad = KeywordTable::load_embedded("{\"table_version\":1,");
    CHECK(!bad.error.empty() && !bad.table.valid());
    CHECK(!suggest_category(suggest_input_for_path("Piano.sfz", {}), bad.table));
    CHECK(KeywordTable::load_embedded(KeywordTable::embedded_json()).error.empty());
  }
  CHECK(!KeywordTable::parse(R"({"entries":[]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":0,"entries":[{"category":"piano","weight":10,"phrases":["piano"]}]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"nope","weight":10,"phrases":["a"]}]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"piano","weight":0,"phrases":["a"]}]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"piano","weight":10,"phrases":[]}]})", x));
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"piano","weight":10,"phrases":["123"]}]})", x));
  // A phrase under two categories (also after normalisation: case, accents, punctuation) is refused.
  CHECK(!KeywordTable::parse(R"({"table_version":1,"entries":[{"category":"piano","weight":10,"phrases":["Órgão"]},
        {"category":"organ","weight":10,"phrases":["orgao"]}]})", x, &err) && err.find("twice") != std::string::npos);
  CHECK(!KeywordTable::load_file("/nonexistent/table.json", x, &err));
  // A custom table drives the suggestion (the table is data, not code).
  KeywordTable custom;
  CHECK(KeywordTable::parse(R"({"table_version":7,"entries":[{"category":"world","weight":10,"phrases":["piano"]}]})", custom));
  CHECK(custom.version() == 7);
  CHECK(suggest_category(suggest_input_for_path("Piano.sfz", {}), custom) == fmt::Category::kWorld);
  CHECK(suggest_name("Piano.sfz") == fmt::Category::kPiano);
}

void test_fixtures() {
  std::ifstream in(LIBRARIAN_FIXTURES_TSV);
  CHECK(in.good());
  KeywordTable file;
  CHECK(KeywordTable::load_file(std::string(LIBRARIAN_DATA_DIR) + "/category_keywords.json", file));
  std::string line;
  int rows = 0, wrong = 0, none_rows = 0, none_wrong = 0;
  std::set<std::string> seen;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    CHECK(tab != std::string::npos);
    if (tab == std::string::npos) continue;
    const std::string name = line.substr(0, tab), want = line.substr(tab + 1);
    CHECK(seen.insert(name).second);  // no row twice
    const bool want_none = want == "none";
    if (!want_none) CHECK(fmt::parse_assignable_category(want).has_value() && want.rfind("category.", 0) == 0);
    ++rows;
    none_rows += want_none;
    for (const KeywordTable* table : std::initializer_list<const KeywordTable*>{&KeywordTable::embedded(), &file}) {
      const auto got = suggest_category(suggest_input_for_path(name, {}), *table);
      const std::string got_s = got ? std::string(fmt::key(*got)) : "none";
      if (got_s != want) {
        std::printf("FIXTURE MISMATCH %-34s want %-22s got %s\n", name.c_str(), want.c_str(), got_s.c_str());
        ++wrong;
        if (want_none) ++none_wrong;  // precision gate: a category where none was expected
        CHECK(false);
      }
    }
  }
  CHECK(rows >= 60);
  CHECK(none_rows >= 8);
  CHECK(none_wrong == 0);  // never a category on a row expected none
  std::printf("category fixtures: %d rows, %d mismatches\n", rows, wrong);
  // The three examples of the story, spelled as in Flow 2.
  CHECK(suggest_name("Rhodes_Mk1") == fmt::Category::kEPiano);
  CHECK(suggest_name("Upright_Bass_Jazz") == fmt::Category::kBass);
  CHECK(!suggest_name("Viola_Caipira"));
  // Every category is exercised by at least one fixture row.
  std::ifstream in2(LIBRARIAN_FIXTURES_TSV);
  std::set<std::string> covered;
  while (std::getline(in2, line)) {
    if (line.empty() || line[0] == '#') continue;
    covered.insert(line.substr(line.find('\t') + 1));
  }
  for (unsigned i = 1; i <= fmt::kCategoryCount; ++i) CHECK(covered.count(std::string(fmt::key(static_cast<fmt::Category>(i)))) == 1);
}

void test_rules() {
  using C = fmt::Category;
  SuggestInput in;
  // Longest phrase wins and consumes its words.
  CHECK(suggest_name("Bass Clarinet") == C::kWoodwind);
  CHECK(suggest_name("Bass Drum") == C::kDrumsPerc);
  CHECK(suggest_name("Upright Piano") == C::kPiano);
  CHECK(suggest_name("Electric Piano") == C::kEPiano);
  CHECK(suggest_name("Synth Brass") == C::kSynthPoly);
  CHECK(suggest_name("Bass Guitar") == C::kBass);
  // Two keywords of the same category add up; the same phrase twice counts once.
  CHECK(suggest_name("Upright_Bass_Jazz") == C::kBass);
  CHECK(suggest_name("Bass Bass Bass") == C::kBass);
  // Ties and conflicts give none: never guess.
  CHECK(!suggest_name("Piano Pad"));
  CHECK(!suggest_name("Strings Piano"));
  CHECK(!suggest_name("Choir Pad"));
  // The exception phrase beats the keyword it contains, even beside another keyword.
  CHECK(!suggest_name("Viola Caipira"));
  CHECK(!suggest_name("Viola Caipira Cordas"));
  CHECK(!suggest_name("viola_de_cocho"));
  CHECK(suggest_name("Viola Solo") == C::kStrings);
  // Whole tokens only: no substring matches.
  CHECK(suggest_name("Pianoforte") == C::kPiano);
  CHECK(!suggest_name("Sitarra"));                // "sitarra" is not "sitar"
  CHECK(!suggest_name("Airbass2"));               // "airbass" is not "bass"
  // Extensions and paths.
  CHECK(suggest_name("Rhodes_Mk1.sfz") == C::kEPiano);
  CHECK(suggest_name("/Volumes/SSD/libs/Rhodes_Mk1.SF2") == C::kEPiano);
  CHECK(suggest_name("C:\\libs\\Church Organ.sfz") == C::kOrgan);
  // Precedence: file name > metadata > folders. The first source with a match decides, ambiguity is final.
  in = {};
  in.file_name = "Sustain.sfz";
  in.folder_names = {"Salamander Grand Piano"};
  CHECK(suggest_category(in) == C::kPiano);                               // folder only
  in.metadata_strings = {"Warm Pad patch"};
  CHECK(suggest_category(in) == C::kSynthPad);                            // metadata beats the folder
  in.file_name = "Rhodes_Mk1.sfz";
  CHECK(suggest_category(in) == C::kEPiano);                              // file name beats both
  in.file_name = "Piano Pad.sfz";                                         // ambiguous file name: none, no fall-through
  CHECK(!suggest_category(in));
  in.file_name = "Viola_Caipira.sfz";
  in.folder_names = {"Strings"};
  in.metadata_strings = {"Strings"};
  CHECK(!suggest_category(in));                                           // the exception is final
  in.file_name = "Untitled.sfz";
  in.metadata_strings = {"Piano", "Pad"};                                  // metadata tie: none, folders not consulted
  in.folder_names = {"Organ"};
  CHECK(!suggest_category(in));
  in.metadata_strings = {"nothing here"};                                  // no metadata match: folders decide
  CHECK(suggest_category(in) == C::kOrgan);
  in.folder_names = {"Organ", "Strings"};                                  // folder tie: none
  CHECK(!suggest_category(in));
  in = {};
  CHECK(!suggest_category(in));
  // suggest_input_for_path keeps the three nearest folders.
  const SuggestInput p = suggest_input_for_path("/a/b/c/d/e/Name.sfz", {"m"});
  CHECK(p.file_name == "Name.sfz" && p.folder_names == (std::vector<std::string>{"c", "d", "e"}) && p.metadata_strings.size() == 1);
}

void test_metadata_scan() {
  Env e("meta");
  e.put_sfz("Untitled.sfz",
            "// Vintage electric piano, Rhodes style\n"
            "<control> label_cc1=Tremolo depth label_key36=Mute\n"
            "default_path=\n");
  const auto m = sfz_metadata_strings((e.src / "Untitled.sfz").string());
  bool comment = false, label = false, label2 = false;
  for (const auto& s : m) {
    comment = comment || s.find("Rhodes style") != std::string::npos;
    label = label || s == "Tremolo depth";
    label2 = label2 || s == "Mute";
  }
  CHECK(comment && label && label2);
  CHECK(sfz_metadata_strings((e.src / "missing.sfz").string()).empty());
  // The import reports a suggestion from the metadata when the name says nothing.
  const ImportResult r = import_sfz(e.opts("Untitled.sfz"));
  CHECK(r.ok() && r.suggested_category == fmt::Category::kEPiano);
  // Many comment lines are capped.
  std::string big;
  for (int i = 0; i < 500; ++i) big += "// line " + std::to_string(i) + "\n";
  e.put_sfz("Big.sfz", big);
  CHECK(sfz_metadata_strings((e.src / "Big.sfz").string()).size() == 32);  // comments have their own cap
  {  // comments cannot starve labels; labels come first
    std::string h;
    for (int i = 0; i < 200; ++i) h += "// license line " + std::to_string(i) + "\n";
    e.put_sfz("Starve.sfz", h + "<control> label_cc1=Warm Pad\n");
    const auto m2 = sfz_metadata_strings((e.src / "Starve.sfz").string());
    CHECK(!m2.empty() && m2[0] == "Warm Pad" && m2.size() == 33);
  }
  {  // // is a comment only at a line start or after whitespace; /* */ blocks are skipped
    e.put_sfz("Cmt.sfz",
              "/* Rhodes block\n still the block Organ */ <control>\n"
              "<region> sample=dir//strings.wav key=60 // real comment\n"
              "//first\n"
              "x /* inline Brass */ y\n");
    const auto m3 = sfz_metadata_strings((e.src / "Cmt.sfz").string());
    bool real = false, first = false, leak = false;
    for (const auto& x : m3) {
      real = real || x == "real comment";
      first = first || x == "first";
      leak = leak || x.find("strings") != std::string::npos || x.find("Rhodes") != std::string::npos ||
             x.find("Organ") != std::string::npos || x.find("Brass") != std::string::npos;
    }
    CHECK(real && first && !leak && m3.size() == 2);
  }
  {  // a FIFO or a directory gives nothing and never blocks; a huge single line is capped
    const std::string fifo = (e.dir / "pipe.sfz").string();
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(sfz_metadata_strings(fifo).empty());
    CHECK(sfz_metadata_strings(e.src.string()).empty());
    std::ofstream huge(e.src / "Huge.sfz", std::ios::binary);
    huge << "<control> label_cc1=Warm Pad\n// " << std::string(3u << 20, 'a') << "\n";
    huge.close();
    const auto m4 = sfz_metadata_strings((e.src / "Huge.sfz").string());
    CHECK(!m4.empty() && m4[0] == "Warm Pad" && m4.size() <= 2);
    for (const auto& x : m4) CHECK(x.size() <= 200);
  }
}

void test_path_input() {
  Env e("paths");
  fs::create_directories(e.src / "Piano/Sub");
  const fs::path cwd = fs::current_path();
  fs::current_path(e.src);
  const SuggestInput rel = suggest_input_for_path("Piano/Sub/a.sfz", {});
  const SuggestInput dot = suggest_input_for_path("./Piano/../Piano/Sub/a.sfz", {});
  const SuggestInput abs = suggest_input_for_path((fs::canonical(e.src) / "Piano/Sub/a.sfz").string(), {});
  const SuggestInput bare = suggest_input_for_path("a.sfz", {});
  fs::current_path(cwd);
  CHECK(rel.file_name == "a.sfz" && rel.folder_names == abs.folder_names && dot.folder_names == abs.folder_names);
  CHECK(!abs.folder_names.empty() && abs.folder_names.size() <= 3 && abs.folder_names.back() == "Sub");
  CHECK(bare.folder_names.empty() && bare.file_name == "a.sfz");
  // Volume and account names are not folders.
  CHECK(suggest_input_for_path("/Volumes/Piano Drive/Pads/x.sfz", {}).folder_names == (std::vector<std::string>{"Pads"}));
  CHECK(suggest_input_for_path("/media/me/Organ/x/y.sfz", {}).folder_names == (std::vector<std::string>{"x"}));
  CHECK(suggest_input_for_path("/run/media/me/Organ/x.sfz", {}).folder_names.empty());
  CHECK(suggest_input_for_path("/Users/Piano/x.sfz", {}).folder_names.empty());
  CHECK(!suggest_category(suggest_input_for_path("/Volumes/Piano Drive/x.sfz", {})));
  // A damaged SF2 / SFZ still reports the suggestion from its name.
  std::ofstream(e.src / "Rhodes_Mk1.sf2") << "not a soundfont";
  const ImportResult r = import_sf2(e.opts("Rhodes_Mk1.sf2"));
  CHECK(r.status == Status::kSkipped && r.suggested_category == fmt::Category::kEPiano);
}

void test_import_paths() {
  {  // a good SFZ: the suggestion rides in the result, the category only if the caller gives one
    Env e("imp1");
    e.put_sfz("Rhodes_Mk1.sfz");
    ImportOptions o = e.opts("Rhodes_Mk1.sfz");
    const ImportResult r = import_sfz(o);
    CHECK(r.ok() && r.suggested_category == fmt::Category::kEPiano);
    CHECK(!fs::exists(fs::path(r.bundle_dir) / "category"));  // a suggestion is never stored
  }
  {  // the stable key is stored whatever the spelling, and re-imports refresh it
    Env e("imp2");
    e.put_sfz("Viola_Caipira.sfz");
    ImportOptions o = e.opts("Viola_Caipira.sfz");
    o.category = "Mallet & Bells";
    ImportResult r = import_sfz(o);
    CHECK(r.ok() && r.suggested_category == fmt::Category::kNone);  // none suggested, the caller chose
    CHECK(slurp(fs::path(r.bundle_dir) / "category") == "category.mallet_bells\n");
    o.category = "STRINGS";
    r = import_sfz(o);
    CHECK(r.ok() && slurp(fs::path(r.bundle_dir) / "category") == "category.strings\n");
    o.category = "category.fx_ambient";
    r = import_sfz(o);
    CHECK(r.ok() && slurp(fs::path(r.bundle_dir) / "category") == "category.fx_ambient\n");
  }
  {  // none and unknown names are refused before anything is written
    for (const char* bad : {"none", "None", "category.none", "Nope", "category.", "../x", "Piano\n"}) {
      Env e("imp3");
      e.put_sfz("A.sfz");
      ImportOptions o = e.opts("A.sfz");
      o.category = bad;
      const ImportResult r = import_sfz(o);
      CHECK(r.status == Status::kInvalidArgument && !r.reasons.empty());
      CHECK(!fs::exists(e.lib()));
      CHECK(!category_problem(bad).empty());
    }
    CHECK(category_problem("").empty() && category_problem("Piano").empty() && category_problem("category.organ").empty());
  }
  {  // a skipped (damaged) file still carries the suggestion; the batch report has it
    Env e("imp4");
    std::ofstream(e.src / "Wurly_200A.sfz") << "<region> sample=missing.wav key=60\n";
    BatchOptions bo;
    bo.inputs = {(e.src / "Wurly_200A.sfz").string()};
    bo.root = e.root.string();
    const BatchResult b = import_batch(bo);
    CHECK(b.skipped == 1 && b.files[0].result.suggested_category == fmt::Category::kEPiano);
    const std::string jl = report_jsonl(b);
    CHECK(jl.find("\"suggested_category\":\"category.epiano\"") != std::string::npos);
    e.put_sfz("Untitled2.sfz");
    bo.inputs = {(e.src / "Untitled2.sfz").string()};
    CHECK(report_jsonl(import_batch(bo)).find("\"suggested_category\":\"none\"") != std::string::npos);
  }
  {  // SF2: the preset names are the metadata
    Env e("imp5");
    sf2b::Builder b;
    sf2b::add_sample(b, "solo", 4800, 100, 44100, 69, 0);
    sf2b::ZS z;
    z.modes = 1;
    sf2b::Instrument in;
    in.name = "Pipe";
    in.zones = {sf2b::izone(0, z.gens())};
    b.instruments = {in};
    sf2b::add_preset(b, "Church Organ", 0, 19, {sf2b::pzone(0)});
    std::ofstream(e.src / "gm.sf2", std::ios::binary).write(reinterpret_cast<const char*>(b.build().data()),
                                                            std::streamsize(b.build().size()));
    ImportOptions o = e.opts("gm.sf2");
    o.category = "Organ";
    const ImportResult r = import_file(o);
    CHECK(r.ok() && r.sf2 && r.suggested_category == fmt::Category::kOrgan);
    CHECK(slurp(e.lib() / r.library_id / "current" / "category") == "category.organ\n");
    o.category = "none";
    CHECK(import_sf2(o).status == Status::kInvalidArgument);
  }
  {  // bundles written before Story 5.5 hold free text; they stay readable, bytes untouched
    Env e("imp6");
    e.put_sfz("Old.sfz");
    const ImportResult r = import_sfz(e.opts("Old.sfz"));
    CHECK(r.ok());
    std::ofstream(fs::path(r.bundle_dir) / "category") << "Piano\n";
    CHECK(fmt::parse_category_file(slurp(fs::path(r.bundle_dir) / "category")) == fmt::Category::kPiano);
  }
}

void test_cli() {
  Env e("cli");
  const std::string exe = LIBRARIAN_EXE;
  int st = 0;
  const std::string out = run("'" + exe + "' suggest-category 'Rhodes_Mk1' 'Upright_Bass_Jazz' 'Viola_Caipira' 'Violão Nylon.sfz' 2>&1", &st);
  CHECK(st == 0);
  CHECK(out == "Rhodes_Mk1\tcategory.epiano\nUpright_Bass_Jazz\tcategory.bass\nViola_Caipira\tnone\nViolão Nylon.sfz\tcategory.guitar\n");
  // Control characters in an echoed name are escaped, never printed raw.
  CHECK(run("'" + exe + "' suggest-category \"$(printf 'a\\tb\\nPiano')\"", &st) == "a\\x09b\\x0aPiano\tcategory.piano\n" && st == 0);
  // A path counts its folders (below the name).
  CHECK(run("'" + exe + "' suggest-category '/lib/Strings/Sustain.sfz'", &st) == "/lib/Strings/Sustain.sfz\tcategory.strings\n" && st == 0);
  // --table swaps the keyword table.
  std::ofstream(e.dir / "t.json") << R"({"table_version":3,"entries":[{"category":"world","weight":10,"phrases":["piano"]}]})";
  CHECK(run("'" + exe + "' suggest-category --table '" + (e.dir / "t.json").string() + "' Piano", &st) == "Piano\tcategory.world\n" && st == 0);
  run("'" + exe + "' suggest-category --table /nonexistent.json Piano >/dev/null 2>&1", &st);
  CHECK(st != 0);
  run("'" + exe + "' suggest-category >/dev/null 2>&1", &st);
  CHECK(st != 0);
  // import --category: key, slug and English names are accepted; none and unknown names are refused.
  e.put_sfz("A.sfz");
  const std::string base = "'" + exe + "' import '" + (e.src / "A.sfz").string() + "' --root '" + e.root.string() + "'";
  run(base + " --category category.piano >/dev/null 2>&1", &st);
  CHECK(st == 0);
  run(base + " --category 'e.piano' >/dev/null 2>&1", &st);
  CHECK(st == 0);
  run(base + " --category none >/dev/null 2>&1", &st);
  CHECK(st != 0);
  run(base + " --category Nonsense >/dev/null 2>&1", &st);
  CHECK(st != 0);
}

}  // namespace

int main() {
  test_tokenizer();
  test_table();
  test_fixtures();
  test_rules();
  test_metadata_scan();
  test_path_input();
  test_import_paths();
  test_cli();
  if (g_fail) {
    std::printf("librarian_category_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("librarian_category_test: ok\n");
  return 0;
}

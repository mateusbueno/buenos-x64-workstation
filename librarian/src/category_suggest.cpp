#include "velora/librarian/category_suggest.hpp"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "velora/format/json_lite.hpp"
#include <iterator>

namespace velora::librarian {
namespace fmt = velora::format;
namespace fs = std::filesystem;

namespace {

// Latin-1 supplement and Latin Extended-A letters folded to ASCII (the accents of PT-BR and ES, and neighbours).
struct Fold {
  unsigned cp;
  char base;
};
constexpr Fold kFold[] = {
    {0xC0, 'A'}, {0xC1, 'A'}, {0xC2, 'A'}, {0xC3, 'A'}, {0xC4, 'A'}, {0xC5, 'A'}, {0xC7, 'C'}, {0xC8, 'E'},
    {0xC9, 'E'}, {0xCA, 'E'}, {0xCB, 'E'}, {0xCC, 'I'}, {0xCD, 'I'}, {0xCE, 'I'}, {0xCF, 'I'}, {0xD1, 'N'},
    {0xD2, 'O'}, {0xD3, 'O'}, {0xD4, 'O'}, {0xD5, 'O'}, {0xD6, 'O'}, {0xD8, 'O'}, {0xD9, 'U'}, {0xDA, 'U'},
    {0xDB, 'U'}, {0xDC, 'U'}, {0xDD, 'Y'}, {0xE0, 'a'}, {0xE1, 'a'}, {0xE2, 'a'}, {0xE3, 'a'}, {0xE4, 'a'},
    {0xE5, 'a'}, {0xE7, 'c'}, {0xE8, 'e'}, {0xE9, 'e'}, {0xEA, 'e'}, {0xEB, 'e'}, {0xEC, 'i'}, {0xED, 'i'},
    {0xEE, 'i'}, {0xEF, 'i'}, {0xF1, 'n'}, {0xF2, 'o'}, {0xF3, 'o'}, {0xF4, 'o'}, {0xF5, 'o'}, {0xF6, 'o'},
    {0xF8, 'o'}, {0xF9, 'u'}, {0xFA, 'u'}, {0xFB, 'u'}, {0xFC, 'u'}, {0xFD, 'y'}, {0xFF, 'y'},
    {0x100, 'A'}, {0x101, 'a'}, {0x102, 'A'}, {0x103, 'a'}, {0x104, 'A'}, {0x105, 'a'}, {0x106, 'C'}, {0x107, 'c'},
    {0x10C, 'C'}, {0x10D, 'c'}, {0x112, 'E'}, {0x113, 'e'}, {0x118, 'E'}, {0x119, 'e'}, {0x11A, 'E'}, {0x11B, 'e'},
    {0x12A, 'I'}, {0x12B, 'i'}, {0x141, 'L'}, {0x142, 'l'}, {0x143, 'N'}, {0x144, 'n'}, {0x14C, 'O'}, {0x14D, 'o'},
    {0x15A, 'S'}, {0x15B, 's'}, {0x160, 'S'}, {0x161, 's'}, {0x16A, 'U'}, {0x16B, 'u'}, {0x179, 'Z'}, {0x17A, 'z'},
    {0x17B, 'Z'}, {0x17C, 'z'}, {0x17D, 'Z'}, {0x17E, 'z'},
};

// Decodes one code point at s[i]. Invalid UTF-8 (stray continuation bytes, truncated, overlong forms such as
// E0 81 A1, surrogates, > U+10FFFF) decodes as U+FFFD consuming one byte, so it can never form a letter.
unsigned decode(std::string_view s, std::size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    ++i;
    return c;
  }
  std::size_t n = 0;
  unsigned cp = 0, min = 0;
  if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; min = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
  else { ++i; return 0xFFFD; }
  if (i + n > s.size()) { ++i; return 0xFFFD; }
  for (std::size_t k = 1; k < n; ++k) {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (d & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
  i += n;
  return cp;
}

enum class Kind : std::uint8_t { kSep, kLower, kUpper };
struct Ch {
  char c;
  Kind k;
};

}  // namespace

std::vector<std::string> tokenize_for_category(std::string_view text) {
  std::vector<Ch> chars;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned cp = decode(text, i);
    if (cp >= 0x300 && cp <= 0x36F) continue;  // combining marks (NFD file names from macOS): drop, keep the base
    if (cp < 0x80) {
      if (cp >= 'a' && cp <= 'z') chars.push_back({static_cast<char>(cp), Kind::kLower});
      else if (cp >= 'A' && cp <= 'Z') chars.push_back({static_cast<char>(cp - 'A' + 'a'), Kind::kUpper});
      else chars.push_back({' ', Kind::kSep});  // digits, punctuation, spaces
      continue;
    }
    // Ligatures spell out (consistent for upper and lower case); other letters outside the table separate words.
    if (cp == 0xDF) { chars.push_back({'s', Kind::kLower}); chars.push_back({'s', Kind::kLower}); continue; }      // ß
    if (cp == 0xE6) { chars.push_back({'a', Kind::kLower}); chars.push_back({'e', Kind::kLower}); continue; }      // æ
    if (cp == 0xC6) { chars.push_back({'a', Kind::kUpper}); chars.push_back({'e', Kind::kLower}); continue; }      // Æ
    if (cp == 0x153) { chars.push_back({'o', Kind::kLower}); chars.push_back({'e', Kind::kLower}); continue; }     // œ
    if (cp == 0x152) { chars.push_back({'o', Kind::kUpper}); chars.push_back({'e', Kind::kLower}); continue; }     // Œ
    char base = 0;
    for (const Fold& f : kFold)
      if (f.cp == cp) {
        base = f.base;
        break;
      }
    if (base == 0) chars.push_back({' ', Kind::kSep});
    else if (base >= 'A' && base <= 'Z') chars.push_back({static_cast<char>(base - 'A' + 'a'), Kind::kUpper});
    else chars.push_back({base, Kind::kLower});
  }
  std::vector<std::string> tokens;
  std::string cur;
  auto flush = [&]() {
    if (!cur.empty()) tokens.push_back(std::move(cur));
    cur.clear();
  };
  for (std::size_t i = 0; i < chars.size(); ++i) {
    const Ch& ch = chars[i];
    if (ch.k == Kind::kSep) {
      flush();
      continue;
    }
    if (ch.k == Kind::kUpper && !cur.empty()) {
      const Kind prev = chars[i - 1].k;
      const bool next_lower = i + 1 < chars.size() && chars[i + 1].k == Kind::kLower;
      // camelCase: "pianoRoll" -> piano roll; acronym end: "XMLFile" -> xml file; a run of capitals stays together.
      if (prev == Kind::kLower || (prev == Kind::kUpper && next_lower)) flush();
    }
    cur.push_back(ch.c);
  }
  flush();
  return tokens;
}

namespace {

std::string join(const std::vector<std::string>& t) {
  std::string s;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (i) s.push_back(' ');
    s += t[i];
  }
  return s;
}

// A phrase of the table is written lower-case, so its camelCase pass would never split; tokenising is enough.
std::optional<velora::format::Category> category_from_table_name(const std::string& n) {
  if (n == "none") return fmt::Category::kNone;
  for (unsigned i = 1; i <= fmt::kCategoryCount; ++i)
    if (fmt::kCategories[i].slug == n) return fmt::kCategories[i].id;
  return std::nullopt;
}

}  // namespace

bool KeywordTable::parse(std::string_view json, KeywordTable& out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  fmt::JsonValue root;
  std::string perr;
  if (!fmt::parse_json(json, root, &perr)) return fail(perr);
  if (!root.is_object()) return fail("the table must be a JSON object");
  const fmt::JsonValue* ver = root.find("table_version");
  if (!ver || !ver->is_number() || ver->number < 1 || ver->number != static_cast<double>(static_cast<std::uint32_t>(ver->number)))
    return fail("table_version must be a positive integer");
  const fmt::JsonValue* ents = root.find("entries");
  if (!ents || !ents->is_array()) return fail("entries must be an array");
  KeywordTable t;
  t.version_ = static_cast<std::uint32_t>(ver->number);
  std::map<std::string, std::string> owner;  // normalised phrase -> category name
  for (const fmt::JsonValue& e : ents->array) {
    if (!e.is_object()) return fail("an entry is not an object");
    const fmt::JsonValue* cat = e.find("category");
    const fmt::JsonValue* w = e.find("weight");
    const fmt::JsonValue* ph = e.find("phrases");
    if (!cat || !cat->is_string()) return fail("an entry has no category");
    const auto c = category_from_table_name(cat->string);
    if (!c) return fail("unknown category '" + cat->string + "'");
    if (!w || !w->is_number() || w->number < 1 || w->number > 100 || w->number != static_cast<int>(w->number))
      return fail("weight must be an integer 1..100 (category " + cat->string + ")");
    if (!ph || !ph->is_array() || ph->array.empty()) return fail("phrases must be a non-empty array (category " + cat->string + ")");
    for (const fmt::JsonValue& p : ph->array) {
      if (!p.is_string()) return fail("a phrase is not a string");
      KeywordEntry k;
      k.tokens = tokenize_for_category(p.string);
      if (k.tokens.empty()) return fail("phrase '" + p.string + "' has no letters");
      k.phrase = join(k.tokens);
      k.category = *c;
      k.weight = static_cast<int>(w->number);
      if (auto it = owner.find(k.phrase); it != owner.end())
        return fail("phrase '" + k.phrase + "' is listed twice (" + it->second + " and " + cat->string + ")");
      owner.emplace(k.phrase, cat->string);
      t.entries_.push_back(std::move(k));
    }
  }
  if (t.entries_.empty()) return fail("the table has no entries");
  out = std::move(t);
  return true;
}

bool KeywordTable::load_file(const std::string& path, KeywordTable& out, std::string* error) {
  std::string text;
  if (!fmt::read_text_file(path, text, error)) return false;
  return parse(text, out, error);
}

KeywordTableLoaded KeywordTable::load_embedded(std::string_view json) {
  KeywordTableLoaded l;
  if (!parse(json, l.table, &l.error)) {
    l.table = KeywordTable{};
    if (l.error.empty()) l.error = "invalid table";
  }
  return l;
}

namespace {
const KeywordTableLoaded& embedded_loaded() {
  static const KeywordTableLoaded l = [] {
    KeywordTableLoaded x = KeywordTable::load_embedded(KeywordTable::embedded_json());
    if (!x.error.empty()) {  // a build-time invariant; never fail silently
      std::fprintf(stderr, "librarian: the embedded category keyword table is invalid: %s\n", x.error.c_str());
      assert(false && "embedded category keyword table is invalid");
    }
    return x;
  }();
  return l;
}
}  // namespace

const KeywordTable& KeywordTable::embedded() { return embedded_loaded().table; }
const std::string& KeywordTable::embedded_error() { return embedded_loaded().error; }

namespace {

struct Match {
  std::size_t text, start, len;
  const KeywordEntry* entry;
};

enum class Verdict { kNoMatch, kNone, kCategory };
struct Decision {
  Verdict verdict = Verdict::kNoMatch;
  fmt::Category category = fmt::Category::kNone;
};

Decision decide(const std::vector<std::string>& texts, const KeywordTable& table) {
  // phrase -> entries by first token, to keep the scan cheap.
  std::multimap<std::string_view, const KeywordEntry*> by_first;
  for (const KeywordEntry& e : table.entries()) by_first.emplace(e.tokens.front(), &e);
  std::vector<Match> all;
  for (std::size_t ti = 0; ti < texts.size(); ++ti) {
    const std::vector<std::string> tok = tokenize_for_category(texts[ti]);
    for (std::size_t i = 0; i < tok.size(); ++i) {
      auto range = by_first.equal_range(tok[i]);
      for (auto it = range.first; it != range.second; ++it) {
        const KeywordEntry& e = *it->second;
        if (i + e.tokens.size() > tok.size()) continue;
        if (std::equal(e.tokens.begin(), e.tokens.end(), tok.begin() + static_cast<std::ptrdiff_t>(i)))
          all.push_back({ti, i, e.tokens.size(), &e});
      }
    }
  }
  if (all.empty()) return {};
  // Longest phrase first, then heavier, then earlier: non-overlapping greedy cover.
  std::stable_sort(all.begin(), all.end(), [](const Match& a, const Match& b) {
    if (a.len != b.len) return a.len > b.len;
    if (a.entry->weight != b.entry->weight) return a.entry->weight > b.entry->weight;
    if (a.text != b.text) return a.text < b.text;
    return a.start < b.start;
  });
  std::map<std::size_t, std::vector<bool>> used;
  std::map<fmt::Category, int> score;
  std::set<const KeywordEntry*> counted;
  bool exception = false;
  for (const Match& m : all) {
    auto& u = used[m.text];
    if (u.empty()) u.assign(tokenize_for_category(texts[m.text]).size(), false);
    bool overlap = false;
    for (std::size_t k = m.start; k < m.start + m.len; ++k) overlap = overlap || u[k];
    if (overlap) continue;
    for (std::size_t k = m.start; k < m.start + m.len; ++k) u[k] = true;
    if (m.entry->category == fmt::Category::kNone) {
      exception = true;
      continue;
    }
    if (counted.insert(m.entry).second) score[m.entry->category] += m.entry->weight;
  }
  Decision d;
  d.verdict = Verdict::kNone;
  if (exception || score.empty()) return d;
  int best = 0, second = 0;
  fmt::Category best_c = fmt::Category::kNone;
  for (const auto& [c, s] : score) {
    if (s > best) {
      second = best;
      best = s;
      best_c = c;
    } else if (s > second) {
      second = s;
    }
  }
  if (best == second) return d;  // tie: ambiguous
  d.verdict = Verdict::kCategory;
  d.category = best_c;
  return d;
}

std::string base_name(std::string_view name) {
  const std::size_t slash = name.find_last_of("/\\");
  if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
  std::string s(name);
  const std::size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    std::string ext = s.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "sfz" || ext == "sf2" || ext == "sf3" || ext == "wav" || ext == "flac") s.resize(dot);
  }
  return s;
}

}  // namespace

std::optional<fmt::Category> suggest_category(const SuggestInput& in, const KeywordTable& table) {
  const std::vector<std::string> file{base_name(in.file_name)};
  for (const std::vector<std::string>* src : {&file, &in.metadata_strings, &in.folder_names}) {
    if (src->empty()) continue;
    const Decision d = decide(*src, table);
    if (d.verdict == Verdict::kNoMatch) continue;
    if (d.verdict == Verdict::kCategory) return d.category;
    return std::nullopt;
  }
  return std::nullopt;
}

std::vector<std::string> sfz_metadata_strings(const std::string& path) {
  constexpr std::size_t kMaxBytes = 1u << 20, kMaxLine = 4096, kMaxLabels = 64, kMaxComments = 32, kMaxValue = 200;
  std::vector<std::string> labels, comments;
  // O_NONBLOCK so a FIFO cannot block the open; only a regular file is read, at most 1 MiB, in bounded chunks.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return {};
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return {};
  }
  std::string text;
  char buf[65536];
  while (text.size() < kMaxBytes) {
    const ssize_t n = ::read(fd, buf, std::min(sizeof(buf), kMaxBytes - text.size()));
    if (n <= 0) break;
    text.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);

  auto add = [&](std::vector<std::string>& v, std::size_t cap, std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    s.erase(0, b);
    if (s.size() > kMaxValue) s.resize(kMaxValue);
    if (!s.empty() && v.size() < cap) v.push_back(std::move(s));
  };
  bool in_block = false;
  for (std::size_t pos = 0; pos < text.size();) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, std::min(eol - pos, kMaxLine));  // the rest of a huge line is ignored
    pos = eol + 1;
    // /* ... */ blocks (also spanning lines) are not read.
    std::string code;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (in_block) {
        if (line.compare(i, 2, "*/") == 0) { in_block = false; ++i; }
        continue;
      }
      if (line.compare(i, 2, "/*") == 0) { in_block = true; ++i; continue; }
      code.push_back(line[i]);
    }
    // `//` starts a comment only at the line start or after whitespace: never inside an opcode value
    // such as sample=dir//a.wav.
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
      if (code[i] == '/' && code[i + 1] == '/' && (i == 0 || code[i - 1] == ' ' || code[i - 1] == '\t')) {
        add(comments, kMaxComments, code.substr(i + 2));
        code.resize(i);
        break;
      }
    }
    for (std::size_t p = code.find("label_"); p != std::string::npos; p = code.find("label_", p + 1)) {
      if (p > 0 && code[p - 1] != ' ' && code[p - 1] != '\t') continue;
      const std::size_t eq = code.find('=', p);
      if (eq == std::string::npos) break;
      // The value runs to the next opcode (" word=") or the end of the line.
      std::size_t end = code.size();
      for (std::size_t q = eq + 1; q < code.size(); ++q)
        if (code[q] == '=') {
          std::size_t b = q;
          while (b > eq + 1 && code[b - 1] != ' ' && code[b - 1] != '\t') --b;
          end = b > eq + 1 ? b : q;
          break;
        }
      add(labels, kMaxLabels, code.substr(eq + 1, end - eq - 1));
    }
    if (labels.size() >= kMaxLabels && comments.size() >= kMaxComments) break;
  }
  // Labels first (they describe the sound); comments, capped separately, cannot starve them.
  labels.insert(labels.end(), std::make_move_iterator(comments.begin()), std::make_move_iterator(comments.end()));
  return labels;
}

SuggestInput suggest_input_for_path(const std::string& path, std::vector<std::string> metadata) {
  SuggestInput in;
  in.metadata_strings = std::move(metadata);
  std::string p = path;
  // A path with a '/' is made absolute and normalised, so ./x/../Piano/a.sfz, a relative and an absolute spelling
  // give the same folders. A bare name has no folders (the current directory is not part of the instrument).
  if (p.find('/') != std::string::npos) {
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (!ec) p = abs.generic_string();
  }
  std::vector<std::string> parts;
  std::string cur;
  for (char c : p) {
    if (c == '/' || c == '\\') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  if (parts.empty()) return in;
  in.file_name = parts.back();
  parts.pop_back();
  // Volume and account names are not folders of the library: /Volumes/<vol>, /media/<user>/<vol>,
  // /run/media/<user>/<vol>, /mnt/<vol>, /Users/<user>, /home/<user>.
  std::size_t skip = 0;
  if (!parts.empty()) {
    const std::string& r = parts[0];
    if (r == "Volumes" || r == "mnt" || r == "Users" || r == "home") skip = 2;
    else if (r == "media") skip = 3;
    else if (r == "run" && parts.size() > 1 && parts[1] == "media") skip = 4;
  }
  skip = std::min(skip, parts.size());
  parts.erase(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(skip));
  const std::size_t keep = std::min<std::size_t>(parts.size(), 3);
  in.folder_names.assign(parts.end() - static_cast<std::ptrdiff_t>(keep), parts.end());
  return in;
}

}  // namespace velora::librarian

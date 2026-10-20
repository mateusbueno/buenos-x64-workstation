#include "velora/librarian/library_index.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

namespace {

std::string esc(std::string_view s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f || c == '%') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool unesc(std::string_view s, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%') {
      out += s[i];
      continue;
    }
    if (i + 2 >= s.size()) return false;
    const int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
    if (h < 0 || l < 0) return false;
    out += static_cast<char>(h * 16 + l);
    i += 2;
  }
  return true;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> f;
  std::size_t at = 0;
  for (;;) {
    const std::size_t t = line.find('\t', at);
    if (t == std::string_view::npos) {
      f.push_back(line.substr(at));
      return f;
    }
    f.push_back(line.substr(at, t - at));
    at = t + 1;
  }
}

std::uint64_t pcm_bytes(const fmt::SampleInfo& s) {
  return s.frame_count * s.format.channels * (static_cast<std::uint64_t>(s.format.bits_per_sample) / 8);
}

bool looks_like_sf2_key(const std::string& k) {  // "bank:preset", decimal
  const auto colon = k.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= k.size()) return false;
  return std::all_of(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(colon), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
         std::all_of(k.begin() + static_cast<std::ptrdiff_t>(colon) + 1, k.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

std::string catalog_meta_text(const CatalogMeta& m) {
  std::string o = "format_version\t" + std::to_string(m.format_version) + "\n";
  o += "library_name\t" + esc(m.library_name) + "\n";
  o += "source_kind\t" + esc(m.source_kind) + "\n";
  o += "source_path\t" + esc(m.source_path) + "\n";
  for (const auto& i : m.instruments)
    o += "instrument\t" + esc(i.key) + "\t" + esc(i.name) + "\t" + std::string(fmt::key(i.category)) + "\n";
  return o;
}

bool parse_catalog_meta(std::string_view text, CatalogMeta& out) {
  out = CatalogMeta{};
  out.format_version = 0;
  std::set<std::string> keys;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t nl = text.find('\n', at);
    if (nl == std::string_view::npos) return false;  // every record ends with a newline: a cut-off file is refused
    const std::string_view line = text.substr(at, nl - at);
    at = nl + 1;
    if (line.empty()) continue;
    const auto f = split_tabs(line);
    if (f[0] == "format_version") {
      if (f.size() != 2) return false;
      unsigned long v = 0;
      if (f[1].empty() || f[1].size() > 9) return false;
      for (char c : f[1]) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned long>(c - '0');
      }
      if (v < 1 || v > kCatalogMetaFormatVersion) return false;
      out.format_version = static_cast<std::uint32_t>(v);
    } else if (f[0] == "library_name" || f[0] == "source_kind" || f[0] == "source_path") {
      if (f.size() != 2) return false;
      std::string v;
      if (!unesc(f[1], v)) return false;
      (f[0] == "library_name" ? out.library_name : f[0] == "source_kind" ? out.source_kind : out.source_path) = std::move(v);
    } else if (f[0] == "instrument") {
      if (f.size() != 4) return false;
      CatalogMetaInstrument i;
      if (!unesc(f[1], i.key) || !unesc(f[2], i.name) || i.key.empty()) return false;
      const auto c = fmt::parse_category(f[3]);
      if (!c || !fmt::is_assignable(*c)) return false;
      i.category = *c;
      if (!keys.insert(i.key).second) return false;
      out.instruments.push_back(std::move(i));
    }  // unknown record types are ignored (forward compatible)
  }
  return out.format_version != 0;
}

MetaState read_catalog_meta(const std::string& bundle_dir, CatalogMeta& out) {
  const std::string path = bundle_dir + "/" + kCatalogMetaFileName;
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return MetaState::kAbsent;
  if (!S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > kMaxCatalogMetaBytes) return MetaState::kUnreadable;
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in && !in.eof()) return MetaState::kUnreadable;
  return parse_catalog_meta(ss.str(), out) ? MetaState::kValid : MetaState::kUnreadable;
}

DescribeResult describe_bundle(const std::string& bundle_dir) {
  DescribeResult res;
  const fs::path dir(bundle_dir);
  const std::string bundle_name = dir.filename().string();
  const std::string library_name_dir = dir.parent_path().filename().string();
  auto rd = fmt::BundleReader::open_file(bundle_dir + "/" + host::kBundleFileName);
  if (!rd) {
    res.status = DescribeStatus::kUnreadable;
    res.why = std::string("bundle does not open: ") + fmt::to_string(rd.error());
    return res;
  }
  const fmt::BundleReader& r = rd.value();
  if (fmt::uuid_to_string(r.library_id()) != library_name_dir || fmt::to_hex(r.bundle_id()) != bundle_name) {
    res.status = DescribeStatus::kIdentityMismatch;
    res.why = "the bundle's library_id / bundle_id disagree with its path";
    return res;
  }
  CatalogMeta meta;
  const MetaState ms = read_catalog_meta(bundle_dir, meta);
  res.from_meta = ms == MetaState::kValid;
  if (ms == MetaState::kUnreadable) {  // never fall back to the legacy category: it would silently rewrite the categories
    res.status = DescribeStatus::kMetaUnreadable;
    res.why = "catalog.meta is unreadable, malformed, oversized or from a newer format";
    return res;
  }
  for (const auto& ins : r.instruments()) {  // a corrupt manifest must not index outside the sample table (this runs before verify_all)
    if (ins.first_sample > r.samples().size() || ins.sample_count > r.samples().size() - ins.first_sample) {
      res.status = DescribeStatus::kUnreadable;
      res.why = "instrument '" + ins.key + "' names samples outside the bundle";
      return res;
    }
  }
  const fmt::CategoryFileResult legacy = fmt::read_category_file(bundle_dir + "/" + kCategoryFileName);
  LibraryRecord& rec = res.record;
  rec.library_id = library_name_dir;
  rec.bundle_id = bundle_name;
  for (const auto& ins : r.instruments()) {
    InstrumentRecord ir;
    ir.key = ins.key;
    ir.name = ins.name.empty() ? ins.key : ins.name;
    fmt::Category cat = fmt::Category::kNone;
    if (res.from_meta) {
      for (const auto& mi : meta.instruments)
        if (mi.key == ins.key) {
          cat = mi.category;
          if (!mi.name.empty()) ir.name = mi.name;
          break;
        }
    }
    if (!fmt::is_assignable(cat) && legacy.state == fmt::CategoryFileState::kValid) cat = legacy.category;
    if (!fmt::is_assignable(cat)) {
      res.status = DescribeStatus::kUncategorised;
      res.why = "instrument '" + ins.key + "' has no valid category";
      return res;
    }
    ir.category = static_cast<std::uint32_t>(fmt::id(cat));
    ir.sample_count = static_cast<std::uint32_t>(ins.sample_count);
    ir.region_count = static_cast<std::uint32_t>(ins.regions.size());
    for (std::size_t k = 0; k < ins.sample_count; ++k) ir.bytes += pcm_bytes(r.samples()[ins.first_sample + k]);
    rec.instruments.push_back(std::move(ir));
  }
  if (rec.instruments.empty()) {
    res.status = DescribeStatus::kUnreadable;
    res.why = "the bundle holds no instrument";
    return res;
  }
  rec.name = res.from_meta && !meta.library_name.empty() ? meta.library_name
             : rec.instruments.size() == 1                ? rec.instruments[0].name
                                                          : rec.library_id;
  rec.source_kind = res.from_meta && (meta.source_kind == "sfz" || meta.source_kind == "sf2")
                        ? meta.source_kind
                        : (looks_like_sf2_key(rec.instruments[0].key) ? "sf2" : "sfz");
  rec.source_path = res.from_meta ? meta.source_path : std::string();
  res.status = DescribeStatus::kOk;
  return res;
}

}  // namespace velora::librarian

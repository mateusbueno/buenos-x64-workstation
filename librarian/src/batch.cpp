#include "velora/librarian/batch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>

namespace velora::librarian {
namespace fs = std::filesystem;

namespace {

// Length of the valid UTF-8 sequence starting at s[i] (1..4), or 0 when it is invalid
// (bad lead or continuation byte, overlong form, surrogate, above U+10FFFF, truncated).
std::size_t utf8_len(const std::string& s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return 1;
  std::size_t n = 0;
  std::uint32_t cp = 0, min = 0;
  if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; min = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
  else return 0;
  if (i + n > s.size()) return 0;
  for (std::size_t k = 1; k < n; ++k) {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (d & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}

// JSON string; the output is always valid UTF-8: invalid bytes become U+FFFD (\ufffd).
std::string esc(const std::string& s) {
  std::string o = "\"";
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const std::size_t n = utf8_len(s, i);
    if (n == 0) {
      o += "\\ufffd";
      ++i;
      continue;
    }
    if (n > 1) {
      o.append(s, i, n);
      i += n;
      continue;
    }
    ++i;
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o + "\"";
}

bool is_importable(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sfz" || e == ".sf2";
}



// Walks `dir` recursively (symlinked directories are not followed). An unreadable directory is
// recorded in `errors` and the walk goes on with its siblings, so no other file is dropped.
void walk(const fs::path& dir, std::vector<std::string>& found, std::vector<std::string>& errors) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec), end;
  if (ec) {
    errors.push_back(dir.string() + ": " + ec.message());
    return;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code e2;
    const fs::file_status st = it->symlink_status(e2);
    if (e2) continue;
    if (fs::is_directory(st)) walk(it->path(), found, errors);
    else if (fs::is_regular_file(it->status(e2)) && is_importable(it->path())) found.push_back(it->path().string());
  }
  if (ec) errors.push_back(dir.string() + ": " + ec.message());
}

}  // namespace

std::vector<std::string> expand_inputs(const std::vector<std::string>& inputs,
                                       std::vector<std::string>* empty_dirs,
                                       std::vector<std::string>* walk_errors) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  // The same file spelled differently (./a.sfz, a.sfz, a symlink) is one entry; the first spelling wins.
  auto add = [&](const std::string& p) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (seen.insert(ec ? p : canon.string()).second) out.push_back(p);
  };
  for (const auto& in : inputs) {
    std::error_code ec;
    if (!fs::is_directory(in, ec)) {
      add(in);
      continue;
    }
    std::vector<std::string> found;
    std::vector<std::string> errs;
    walk(in, found, errs);
    if (walk_errors) walk_errors->insert(walk_errors->end(), errs.begin(), errs.end());
    std::sort(found.begin(), found.end());
    if (found.empty() && errs.empty() && empty_dirs) empty_dirs->push_back(in);
    for (const auto& f : found) add(f);
  }
  return out;
}

BatchResult import_batch(const BatchOptions& opt) {
  BatchResult br;
  std::vector<std::string> empty_dirs;
  std::vector<std::string> walk_errors;
  const std::vector<std::string> files = expand_inputs(opt.inputs, &empty_dirs, &walk_errors);
  for (const auto& w : walk_errors) {
    FileReport f;
    f.sfz_path = w.substr(0, w.rfind(": "));
    f.result.status = Status::kIo;  // failed: the directory could not be listed completely
    f.result.reasons.push_back("cannot list directory " + w);
    br.files.push_back(std::move(f));
  }
  for (const auto& d : empty_dirs) {
    FileReport f;
    f.sfz_path = d;
    f.result.status = Status::kSkipped;
    f.result.reason_code = ReasonCode::kDamaged;
    f.result.failing_path = d;
    f.result.reasons.push_back(d + ": no .sfz or .sf2 files found");
    br.files.push_back(std::move(f));
  }
  for (const auto& path : files) {
    ImportOptions o;
    o.sfz_path = path;
    o.root = opt.root;
    o.base_dir = opt.base_dir;
    o.category = opt.category;
    o.fault = opt.fault;
    o.checkpoint_timbres = opt.checkpoint_timbres;
    o.checkpoint_bytes = opt.checkpoint_bytes;
    o.source_gone = opt.source_gone;
    FileReport f;
    f.sfz_path = path;
    f.result = import_file(o);  // a skipped or failed file never stops the batch
    const bool drive_gone = f.result.status == Status::kDriveRemoved;
    br.files.push_back(std::move(f));
    if (drive_gone) break;  // Story 5.8: the source drive is gone, the other files cannot be read either
  }
  for (const auto& f : br.files) {
    const std::string s = file_status(f.result);
    if (s == "imported") ++br.imported;
    else if (s == "unchanged") ++br.unchanged;
    else if (s == "skipped") ++br.skipped;
    else ++br.failed;
  }
  return br;
}

const char* file_status(const ImportResult& r) {
  if (r.status == Status::kOk) return r.bundle_created || r.current_flipped ? "imported" : "unchanged";
  if (r.status == Status::kSkipped) return "skipped";
  return "failed";
}

std::string report_file_line(const FileReport& f) {
  const ImportResult& r = f.result;
  std::string o = "{\"type\":\"file\",\"file\":" + esc(f.sfz_path);
  o += std::string(",\"status\":\"") + file_status(r) + "\"";
  o += std::string(",\"reason\":\"") + to_string(r.reason_code) + "\"";
  o += ",\"failing_path\":" + esc(r.failing_path);
  o += ",\"offenders\":[";
  for (std::size_t i = 0; i < r.offenders.size(); ++i) {
    const Offender& x = r.offenders[i];
    if (i) o += ",";
    o += "{\"opcode\":" + esc(x.opcode) + ",\"file\":" + esc(x.file) + ",\"line\":" + std::to_string(x.line) +
         ",\"message\":" + esc(x.message) + "}";
  }
  o += "],\"allowlisted_count\":" + std::to_string(r.allowlisted.size()) + ",\"allowlisted\":[";
  for (std::size_t i = 0; i < r.allowlisted.size(); ++i) {
    const AllowListedUse& x = r.allowlisted[i];
    if (i) o += ",";
    o += "{\"opcode\":" + esc(x.opcode) + ",\"file\":" + esc(x.file) + ",\"line\":" + std::to_string(x.line) + "}";
  }
  o += "],\"warnings\":[";
  for (std::size_t i = 0; i < r.warnings.size(); ++i) {
    const ImportWarning& x = r.warnings[i];
    if (i) o += ",";
    o += "{\"code\":" + esc(x.code) + (!x.preset.empty() ? ",\"preset\":" + esc(x.preset) : std::string()) +
         ",\"opcode\":" + esc(x.opcode) + ",\"file\":" + esc(x.file) + ",\"line\":" + std::to_string(x.line) +
         ",\"message\":" + esc(x.message) + "}";
  }
  o += "],\"warning_count\":" + std::to_string(r.warnings.size()) + ",\"messages\":[";
  for (std::size_t i = 0; i < r.reasons.size(); ++i) {
    if (i) o += ",";
    o += esc(r.reasons[i]);
  }
  o += "],\"library_id\":" + esc(r.library_id) + ",\"bundle_id\":" + esc(r.bundle_id);
  o += ",\"samples\":" + std::to_string(r.sample_count) + ",\"regions\":" + std::to_string(r.region_count);
  o += ",\"detail\":" + esc(r.status == Status::kOk || r.status == Status::kSkipped ? "" : to_string(r.status));
  o += ",\"suggested_category\":" + esc(std::string(velora::format::key(r.suggested_category)));
  if (r.sf2)
    o += ",\"format\":\"sf2\",\"presets\":" + std::to_string(r.preset_count) + ",\"ignored_generators\":" +
         std::to_string(r.ignored_generators) + ",\"ignored_modulators\":" + std::to_string(r.ignored_modulators);
  return o + "}";
}

std::string report_summary_line(const BatchResult& b) {
  return "{\"type\":\"summary\",\"files\":" + std::to_string(b.files.size()) +
         ",\"imported\":" + std::to_string(b.imported) + ",\"unchanged\":" + std::to_string(b.unchanged) +
         ",\"skipped\":" + std::to_string(b.skipped) + ",\"failed\":" + std::to_string(b.failed) + "}";
}

std::string report_jsonl(const BatchResult& b) {
  std::string o;
  for (const auto& f : b.files) o += report_file_line(f) + "\n";
  return o + report_summary_line(b) + "\n";
}

}  // namespace velora::librarian

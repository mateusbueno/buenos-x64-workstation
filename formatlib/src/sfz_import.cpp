#include "velora/format/sfz_import.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "layout.hpp"
#include "parser/Parser.h"
#include "parser/ParserListener.h"

namespace velora::format {
namespace fs = std::filesystem;

const char* to_string(SfzIssueKind k) {
  switch (k) {
    case SfzIssueKind::kUnsupportedOpcode: return "unsupported opcode";
    case SfzIssueKind::kUnsupportedHeader: return "unsupported header";
    case SfzIssueKind::kUnsupportedValue: return "unsupported value";
    case SfzIssueKind::kInvalidValue: return "invalid value";
    case SfzIssueKind::kParse: return "parse error";
    case SfzIssueKind::kMissingSample: return "missing sample";
    case SfzIssueKind::kMissingSampleOpcode: return "region without sample";
  }
  return "?";
}

std::string SfzIssue::to_string() const {
  std::string s = file;
  if (line) s += ":" + std::to_string(line);
  return s + ": " + message;
}

namespace {

struct Op {
  std::string name, value, file;
  std::size_t line = 0;
};
using Level = std::vector<Op>;

const std::set<std::string>& supported_opcodes() {
  static const std::set<std::string> s = {
      "lokey", "hikey", "key", "lovel", "hivel", "pitch_keycenter", "tune", "transpose",
      "volume", "pan", "offset", "seq_length", "seq_position", "lorand", "hirand", "loop_mode",
      "loop_start", "loop_end", "ampeg_delay", "ampeg_start", "ampeg_attack", "ampeg_hold",
      "ampeg_decay", "ampeg_sustain", "ampeg_release", "trigger", "sample"};
  return s;
}

std::string trim(std::string s) {
  const char* ws = " \t\r\n";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool parse_double(const std::string& v, double& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtod(t.c_str(), &end);
  return errno == 0 && *end == 0 && std::isfinite(out);
}

bool parse_int(const std::string& v, std::int64_t& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtoll(t.c_str(), &end, 10);
  return errno == 0 && *end == 0;
}

// MIDI note number or note name (c4 = 60; sharps '#', flats 'b'; octave may be negative).
bool parse_note(const std::string& v, std::int64_t& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  if (parse_int(t, out)) return out >= 0 && out <= 127;
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
  if (l < 'a' || l > 'g') return false;
  int semi = base[l - 'a'];
  std::size_t i = 1;
  if (i < t.size() && t[i] == '#') { ++semi; ++i; }
  else if (i < t.size() && (t[i] == 'b' || t[i] == 'B')) { --semi; ++i; }
  std::int64_t octave = 0;
  if (i >= t.size() || !parse_int(t.substr(i), octave)) return false;
  out = 12 * (octave + 1) + semi;
  return out >= 0 && out <= 127;
}

std::string norm_slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return s;
}

class Importer final : public sfz::ParserListener {
 public:
  explicit Importer(const fs::path& main) : main_(main), dir_(main.parent_path().lexically_normal()) {}

  SfzParseResult result;

  void onParseBegin() override {}
  void onParseEnd() override { flush_region(); }

  void onParseHeader(const sfz::SourceRange& range, const std::string& header) override {
    flush_region();
    const Loc loc = at(range);
    if (header == "global") {
      global_.clear(); master_.clear(); group_.clear(); region_.clear();
      cur_ = Cur::kGlobal;
    } else if (header == "master") {
      master_.clear(); group_.clear(); region_.clear();
      cur_ = Cur::kMaster;
    } else if (header == "group") {
      group_.clear(); region_.clear();
      cur_ = Cur::kGroup;
    } else if (header == "region") {
      region_.clear();
      cur_ = Cur::kRegion;
      region_loc_ = loc;
      ++result.source_region_count;
    } else if (header == "control") {
      cur_ = Cur::kControl;
    } else {
      issue(SfzIssueKind::kUnsupportedHeader, loc, "unsupported header <" + header + ">");
      cur_ = Cur::kIgnored;
    }
  }

  void onParseOpcode(const sfz::SourceRange& range, const sfz::SourceRange&,
                     const std::string& name, const std::string& value) override {
    const Loc loc = at(range);
    Op op{name, value, loc.file, loc.line};
    switch (cur_) {
      case Cur::kNone:
        issue(SfzIssueKind::kParse, loc, "opcode '" + name + "' outside any header");
        return;
      case Cur::kIgnored:
        return;
      case Cur::kControl:
        if (name == "default_path") {
          const std::string dp = norm_slashes(value);
          if (check_path(dp, op)) default_path_ = dp;
        } else {
          issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "' in <control>");
        }
        return;
      default:
        break;
    }
    if (!supported_opcodes().count(name)) {
      issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "'");
      return;
    }
    if (name == "sample") {
      // `sample` is resolved against default_path as it was when the opcode was read.
      const std::string raw = norm_slashes(value);
      if (!default_path_.empty() && fs::path(raw).is_absolute()) {
        issue(SfzIssueKind::kInvalidValue, loc, "absolute sample '" + raw + "' combined with default_path '" + default_path_ + "'");
        return;
      }
      op.value = default_path_ + raw;
      if (!check_path(op.value, op)) return;
    } else {
      Region scratch;  // validate once, here, so each bad value is reported exactly once
      SfzLoopSpec ls;
      if (!apply(scratch, ls, op, true)) return;
    }
    Level& lv = cur_ == Cur::kGlobal ? global_ : cur_ == Cur::kMaster ? master_
              : cur_ == Cur::kGroup ? group_ : region_;
    lv.push_back(std::move(op));
  }

  void onParseError(const sfz::SourceRange& range, const std::string& message) override {
    issue(SfzIssueKind::kParse, at(range), message);
  }
  void onParseWarning(const sfz::SourceRange& range, const std::string& message) override {
    issue(SfzIssueKind::kParse, at(range), "warning: " + message);
  }

 private:
  enum class Cur { kNone, kGlobal, kMaster, kGroup, kRegion, kControl, kIgnored };
  struct Loc {
    std::string file;
    std::size_t line = 0;
  };

  Loc at(const sfz::SourceRange& r) const {
    Loc l;
    if (r.start.filePath) l.file = r.start.filePath->string();
    else l.file = main_.string();
    l.line = r.start ? r.start.lineNumber + 1 : 0;  // the parser counts lines from 0
    return l;
  }

  // Every issue goes through here: identical (file, line, message) triples are reported once.
  void add(SfzIssueKind k, const std::string& file, std::size_t line, std::string msg) {
    if (!seen_.insert(file + ":" + std::to_string(line) + ":" + msg).second) return;
    result.issues.push_back({k, file, line, std::move(msg)});
  }
  void issue(SfzIssueKind k, const Loc& l, std::string msg) { add(k, l.file, l.line, std::move(msg)); }

  // A path (sample or default_path) must stay inside the SFZ's directory.
  bool check_path(const std::string& p, const Op& op) {
    if (p.empty()) return true;
    const fs::path full = (fs::path(p).is_absolute() ? fs::path(p) : dir_ / p).lexically_normal();
    const fs::path rel = full.lexically_relative(dir_);
    if (rel.empty() || *rel.begin() == "..") {
      add(SfzIssueKind::kInvalidValue, op.file, op.line,
          "path '" + p + "' leaves the SFZ directory (no '..' escapes or absolute paths outside it)");
      return false;
    }
    return true;
  }

  void bad(const Op& op, const char* why) {
    if (!report_) return;
    add(SfzIssueKind::kInvalidValue, op.file, op.line,
        "invalid value '" + op.value + "' for opcode '" + op.name + "' (" + why + ")");
  }
  void unsupported_value(const Op& op) {
    if (!report_) return;
    add(SfzIssueKind::kUnsupportedValue, op.file, op.line,
        "unsupported value '" + op.value + "' for opcode '" + op.name + "'");
  }

  bool as_int(const Op& op, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
    if (!parse_int(op.value, out) || out < lo || out > hi) {
      bad(op, "expected integer in range");
      return false;
    }
    return true;
  }
  bool as_float(const Op& op, double lo, double hi, double& out) {
    if (!parse_double(op.value, out) || out < lo || out > hi) {
      bad(op, "expected number in range");
      return false;
    }
    return true;
  }
  bool as_note(const Op& op, std::int64_t& out) {
    if (!parse_note(op.value, out)) {
      bad(op, "expected MIDI note number or name");
      return false;
    }
    return true;
  }

  // Applies one opcode to `r`. Returns false for an invalid value (reported when `report`).
  bool apply(Region& r, SfzLoopSpec& ls, const Op& op, bool report) {
    report_ = report;
    std::int64_t n = 0;
    double d = 0;
    const std::string& k = op.name;
    const std::int64_t kMaxFrames = std::int64_t(1) << 40;
    if (k == "sample") return true;
    if (k == "lokey") { if (!as_note(op, n)) return false; r.key_lo = std::uint8_t(n); }
    else if (k == "hikey") { if (!as_note(op, n)) return false; r.key_hi = std::uint8_t(n); }
    else if (k == "key") { if (!as_note(op, n)) return false; r.key_lo = r.key_hi = r.key_center = std::uint8_t(n); }
    else if (k == "pitch_keycenter") { if (!as_note(op, n)) return false; r.key_center = std::uint8_t(n); }
    else if (k == "lovel") { if (!as_int(op, 0, 127, n)) return false; r.vel_lo = std::uint8_t(n); }
    else if (k == "hivel") { if (!as_int(op, 0, 127, n)) return false; r.vel_hi = std::uint8_t(n); }
    else if (k == "transpose") { if (!as_int(op, -127, 127, n)) return false; r.transpose = std::int32_t(n); }
    else if (k == "tune") { if (!as_float(op, -1200, 1200, d)) return false; r.tune_cents = float(d); }
    else if (k == "volume") { if (!as_float(op, -144, 48, d)) return false; r.volume_db = float(d); }
    else if (k == "pan") { if (!as_float(op, -100, 100, d)) return false; r.pan = float(d); }
    else if (k == "offset") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.offset = std::uint64_t(n); }
    else if (k == "seq_length") { if (!as_int(op, 1, 255, n)) return false; r.seq_length = std::uint32_t(n); }
    else if (k == "seq_position") { if (!as_int(op, 1, 255, n)) return false; r.seq_position = std::uint32_t(n); }
    else if (k == "lorand") { if (!as_float(op, 0, 1, d)) return false; r.lorand = float(d); }
    else if (k == "hirand") { if (!as_float(op, 0, 1, d)) return false; r.hirand = float(d); }
    else if (k == "loop_start") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.loop_start = std::uint64_t(n); ls.start_set = true; }
    else if (k == "loop_end") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.loop_end = std::uint64_t(n); ls.end_set = true; }
    else if (k == "loop_mode") {
      const std::string v = trim(op.value);
      if (v == "no_loop") r.loop_mode = LoopMode::kNoLoop;
      else if (v == "one_shot") r.loop_mode = LoopMode::kOneShot;
      else if (v == "loop_continuous") r.loop_mode = LoopMode::kLoopContinuous;
      else if (v == "loop_sustain") r.loop_mode = LoopMode::kLoopSustain;
      else { unsupported_value(op); return false; }
    } else if (k == "trigger") {
      const std::string v = trim(op.value);
      if (v == "attack") r.trigger = TriggerMode::kAttack;
      else if (v == "release") r.trigger = TriggerMode::kRelease;
      else { unsupported_value(op); return false; }
    }
    else if (k == "ampeg_delay") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.delay = float(d); }
    else if (k == "ampeg_start") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.start = float(d); }
    else if (k == "ampeg_attack") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.attack = float(d); }
    else if (k == "ampeg_hold") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.hold = float(d); }
    else if (k == "ampeg_decay") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.decay = float(d); }
    else if (k == "ampeg_sustain") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.sustain = float(d); }
    else if (k == "ampeg_release") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.release = float(d); }
    return true;
  }

  void flush_region() {
    if (cur_ != Cur::kRegion) return;
    cur_ = Cur::kNone;  // a region is flushed exactly once
    Region r;
    SfzLoopSpec ls;
    const Op* sample_op = nullptr;
    for (const Level* lv : {&global_, &master_, &group_, &region_})
      for (const Op& op : *lv) {
        if (op.name == "sample") sample_op = &op;
        apply(r, ls, op, false);  // values were validated when read
      }
    r.has_loop_points = ls.start_set || ls.end_set;
    if (r.key_lo > r.key_hi || r.vel_lo > r.vel_hi || r.lorand > r.hirand ||
        r.seq_position > r.seq_length) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "region has an inverted or inconsistent range (key, velocity, rand or seq)");
      return;
    }
    if (ls.start_set && ls.end_set && r.loop_start > r.loop_end) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "loop_start > loop_end");
      return;
    }
    if (!sample_op) {
      issue(SfzIssueKind::kMissingSampleOpcode, region_loc_, "region without a sample opcode");
      return;
    }
    const std::string& sample_value = sample_op->value;
    if (sample_value.empty() || sample_value[0] == '*') {
      add(SfzIssueKind::kUnsupportedValue, sample_op->file, sample_op->line,
          "unsupported sample '" + sample_value + "' (generators and empty samples are not supported)");
      return;
    }
    const fs::path p = (fs::path(sample_value).is_absolute() ? fs::path(sample_value) : dir_ / sample_value).lexically_normal();
    std::size_t idx = samples_.size();
    auto it = samples_.find(p.string());
    if (it != samples_.end()) {
      idx = it->second;
    } else {
      std::error_code ec;
      if (!fs::is_regular_file(p, ec)) {
        add(SfzIssueKind::kMissingSample, sample_op->file, sample_op->line, "sample file not found: " + p.string());
        return;
      }
      SfzSample s;
      s.name = p.lexically_relative(dir_).generic_string();
      if (s.name.size() > layout::kMaxStringLen) {
        issue(SfzIssueKind::kInvalidValue, region_loc_, "sample path too long");
        return;
      }
      s.path = p.string();
      s.ref_file = sample_op->file;
      s.ref_line = sample_op->line;
      samples_[p.string()] = idx;
      result.samples.push_back(std::move(s));
    }
    r.sample = static_cast<std::uint32_t>(idx);
    result.regions.push_back(r);
    result.loops.push_back(ls);
    result.region_sources.push_back({region_loc_.file, region_loc_.line});
  }

  fs::path main_, dir_;
  Cur cur_ = Cur::kNone;
  Level global_, master_, group_, region_;
  Loc region_loc_;
  std::string default_path_;
  std::map<std::string, std::size_t> samples_;
  std::set<std::string> seen_;
  bool report_ = false;
};

}  // namespace

bool resolve_loop_points(Region& r, const SfzLoopSpec& spec, std::uint64_t frames, std::string* why) {
  const bool loops = r.loop_mode == LoopMode::kLoopContinuous || r.loop_mode == LoopMode::kLoopSustain;
  if (!spec.start_set && !spec.end_set && !loops) {
    r.has_loop_points = false;
    r.loop_start = r.loop_end = 0;
    return true;
  }
  if (frames == 0) {
    if (why) *why = "the sample is empty, it cannot loop";
    return false;
  }
  if (!spec.start_set) r.loop_start = 0;
  if (!spec.end_set) r.loop_end = frames - 1;
  r.has_loop_points = true;
  if (r.loop_start > r.loop_end) {
    if (why) *why = spec.end_set ? "loop_start > loop_end" : "loop_start is beyond the last frame of the sample";
    return false;
  }
  return true;
}

SfzParseResult parse_sfz_file(const std::string& sfz_path) {
  SfzParseResult none;
  std::error_code ec;
  fs::path abs = fs::absolute(sfz_path, ec);
  if (ec || !fs::is_regular_file(abs, ec)) {
    none.issues.push_back({SfzIssueKind::kParse, sfz_path, 0, "cannot open SFZ file"});
    return none;
  }
  abs = abs.lexically_normal();
  Importer imp(abs);
  try {
    sfz::Parser parser;
    parser.setListener(&imp);
    parser.setMaximumIncludeDepth(16);
    parser.parseFile(abs);
    for (const auto& f : parser.getIncludedFiles()) imp.result.included_files.push_back(f);
  } catch (const std::exception& e) {
    imp.result.issues.push_back({SfzIssueKind::kParse, abs.string(), 0, std::string("parser failure: ") + e.what()});
  }
  for (auto& i : imp.result.issues)
    if (i.kind == SfzIssueKind::kParse && i.message.find("Exceeded maximum include depth") != std::string::npos)
      i.message += " (recursive #include?)";
  std::sort(imp.result.included_files.begin(), imp.result.included_files.end());
  return std::move(imp.result);
}

}  // namespace velora::format

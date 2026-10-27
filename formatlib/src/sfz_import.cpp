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

bool is_unsupported_feature(SfzIssueKind k) {
  return k == SfzIssueKind::kUnsupportedOpcode || k == SfzIssueKind::kUnsupportedHeader ||
         k == SfzIssueKind::kUnsupportedValue;
}

const std::vector<SfzOpcodeEntry>& sfz_opcode_table() {
  using C = OpcodeClass;
  static const std::vector<SfzOpcodeEntry> t = {
      {"sample", C::kSupported, "required in every region"},
      {"default_path", C::kSupported, "<control> only"},
      {"lokey", C::kSupported, "note number or name"},
      {"hikey", C::kSupported, ""},
      {"key", C::kSupported, "lokey = hikey = pitch_keycenter"},
      {"lovel", C::kSupported, ""},
      {"hivel", C::kSupported, ""},
      {"pitch_keycenter", C::kSupported, ""},
      {"tune", C::kSupported, "cents"},
      {"transpose", C::kSupported, "semitones"},
      {"volume", C::kSupported, "dB"},
      {"pan", C::kSupported, "-100..100"},
      {"offset", C::kSupported, "frames"},
      {"seq_length", C::kSupported, "round robin"},
      {"seq_position", C::kSupported, "round robin"},
      {"lorand", C::kSupported, ""},
      {"hirand", C::kSupported, ""},
      {"loop_mode", C::kSupported, "no_loop, one_shot, loop_continuous, loop_sustain"},
      {"loop_start", C::kSupported, "frames"},
      {"loop_end", C::kSupported, "frames, inclusive"},
      {"ampeg_delay", C::kSupported, "s"},
      {"ampeg_start", C::kSupported, "%"},
      {"ampeg_attack", C::kSupported, "s"},
      {"ampeg_hold", C::kSupported, "s"},
      {"ampeg_decay", C::kSupported, "s"},
      {"ampeg_sustain", C::kSupported, "%"},
      {"ampeg_release", C::kSupported, "s"},
      {"trigger", C::kSupported, "attack, release, first, legato, release_key (Story 13.3)"},
      {"end", C::kSupported, "frames, exclusive: last played frame = min(end, length) - 1; 0 disables the region (Story 13.3)"},
      {"sw_lokey", C::kSupported, "keyswitch range low (Story 5.1)"},
      {"sw_hikey", C::kSupported, "keyswitch range high (Story 5.1)"},
      {"sw_last", C::kSupported, "(Story 5.1)"},
      {"sw_down", C::kSupported, "(Story 5.1)"},
      {"sw_up", C::kSupported, "(Story 5.1)"},
      {"sw_previous", C::kSupported, "(Story 5.1)"},
      {"sw_default", C::kSupported, "(Story 5.1)"},
      {"xfin_locc<N>", C::kSupported, "N = controller 0..127, up to 4 per region (Story 5.1)"},
      {"xfin_hicc<N>", C::kSupported, "(Story 5.1)"},
      {"xfout_locc<N>", C::kSupported, "(Story 5.1)"},
      {"xfout_hicc<N>", C::kSupported, "(Story 5.1)"},
      {"xf_cccurve", C::kSupported, "gain, power (Story 5.1)"},
      {"fil_type", C::kSupported, "lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p (Story 5.1)"},
      {"cutoff", C::kSupported, "Hz; the filter exists only when cutoff is given (Story 5.1)"},
      {"resonance", C::kSupported, "dB (Story 5.1)"},
      {"label_cc<N>", C::kAllowListed, "controller name"},
      {"label_key<N>", C::kAllowListed, "key name"},
      {"global_label", C::kAllowListed, ""},
      {"master_label", C::kAllowListed, ""},
      {"group_label", C::kAllowListed, ""},
      {"region_label", C::kAllowListed, ""},
      {"sw_label", C::kAllowListed, "keyswitch name"},
      {"image", C::kAllowListed, "instrument picture"},
      {"hint_*", C::kAllowListed, "editor hints"},
      // Story 5.4 (real VSCO2: 75 of 75 files used it and nothing else outside the subset): a flag that only makes sfizz re-read the
      // ampeg_* values while a voice sounds. The values it could change come from ampeg_*_oncc<N> / ampeg_vel2*, which are unsupported
      // opcodes and reject the file by themselves, so without them the flag cannot change the sound; with them the file is rejected anyway.
      // INERT ONLY WHILE no ampeg_*_oncc<N> / ampeg_vel2* opcode is supported: the moment one is, this entry must leave the allow-list
      // (Story 13.5 revisits it).
      {"ampeg_dynamic", C::kAllowListed, "flag, no effect without ampeg_*_oncc<N> (Story 5.4)"},
  };
  return t;
}

bool opcode_matches(const std::string& pattern, const std::string& name) {
  auto starts = [&](const std::string& prefix) {
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
  };
  if (pattern.size() > 3 && pattern.compare(pattern.size() - 3, 3, "<N>") == 0) {
    const std::string prefix = pattern.substr(0, pattern.size() - 3);
    if (!starts(prefix)) return false;
    const std::size_t digits = name.size() - prefix.size();
    if (digits < 1 || digits > 3) return false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i)
      if (name[i] < '0' || name[i] > '9') return false;
    return true;
  }
  if (!pattern.empty() && pattern.back() == '*') return starts(pattern.substr(0, pattern.size() - 1));
  return name == pattern;
}

OpcodeClass classify_opcode(const std::string& name) {
  for (const auto& e : sfz_opcode_table())
    if (opcode_matches(e.pattern, name)) return e.cls;
  return OpcodeClass::kUnsupported;
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

// State gathered while applying a region's opcodes that does not fit in Region itself.
struct XfPart {
  std::uint8_t lo = 0, hi = 0;
};
struct Acc {
  SfzLoopSpec ls;
  bool cutoff_set = false, type_set = false;
  FilterType type = FilterType::kLpf2p;
  std::map<int, XfPart> in, out;  // by controller number, ascending
  bool never_note = false;        // hikey=-1 (the last hikey wins, so a later valid hikey clears it)
  const Op* type_op = nullptr;  // fil_type / resonance as given (for ignored_opcode warnings)
  const Op* res_op = nullptr;
};

// "xfin_locc64" -> in, lo, cc 64. Names are already classified as xf opcodes.
bool xf_opcode(const std::string& name, bool& in, bool& lo, int& cc) {
  static const char* const names[] = {"xfin_locc", "xfin_hicc", "xfout_locc", "xfout_hicc"};
  for (int i = 0; i < 4; ++i) {
    const std::string p = names[i];
    if (name.size() > p.size() && name.compare(0, p.size(), p) == 0) {
      cc = std::atoi(name.c_str() + p.size());
      in = i < 2;
      lo = i % 2 == 0;
      return true;
    }
  }
  return false;
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
    } else if (header == "curve") {
      cur_ = Cur::kCurve;
      SfzCurve c;
      c.file = loc.file;
      c.line = loc.line;
      if (result.curves.size() < 256) result.curves.push_back(std::move(c));
      else issue(SfzIssueKind::kUnsupportedValue, loc, "more than 256 <curve> blocks", "<curve>");
    } else {
      issue(SfzIssueKind::kUnsupportedHeader, loc, "unsupported header <" + header + ">", "<" + header + ">");
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
      case Cur::kCurve:
        curve_opcode(op);
        return;
      default:
        break;
    }
    if (classify_opcode(name) == OpcodeClass::kAllowListed) {
      // Informational only: accepted, counted, never stored.
      result.allowlisted.push_back({name, loc.file, loc.line});
      return;
    }
    switch (cur_) {
      case Cur::kControl:
        if (name == "default_path") {
          const std::string dp = norm_slashes(value);
          if (check_path(dp, op)) default_path_ = dp;
        } else {
          issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "' in <control>", name);
        }
        return;
      default:
        break;
    }
    if (classify_opcode(name) != OpcodeClass::kSupported || name == "default_path") {
      issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "'", name);
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
      Acc acc;
      if (!apply(scratch, acc, op, true)) return;
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
  enum class Cur { kNone, kGlobal, kMaster, kGroup, kRegion, kControl, kCurve, kIgnored };
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
  bool add(SfzIssueKind k, const std::string& file, std::size_t line, std::string msg,
           std::string opcode = {}) {
    if (!seen_.insert(file + ":" + std::to_string(line) + ":" + msg).second) return false;
    result.issues.push_back({k, file, line, std::move(msg), file, std::move(opcode)});
    return true;
  }
  void issue(SfzIssueKind k, const Loc& l, std::string msg, std::string opcode = {}) {
    add(k, l.file, l.line, std::move(msg), std::move(opcode));
  }

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

  // <curve>: curve_index and vNNN points are stored inert (bounded: 256 curves, 256 points each); anything else is unsupported.
  void curve_opcode(const Op& op) {
    report_ = true;  // called once per opcode
    SfzCurve& c = result.curves.back();
    const std::string& k = op.name;
    std::int64_t n = 0;
    double d = 0;
    if (k == "curve_index") {
      if (parse_int(op.value, n) && n >= 0 && n <= 255) c.index = int(n);
      else ignore(op, "expected curve index 0..255");
    } else if (k.size() >= 2 && k.size() <= 4 && k[0] == 'v' &&
               k.find_first_not_of("0123456789", 1) == std::string::npos) {
      const int pt = std::atoi(k.c_str() + 1);
      if (pt > 255) ignore(op, "curve point index above 255");
      else if (!parse_double(op.value, d) || !std::isfinite(d)) ignore(op, "expected a finite number");
      else if (c.points.size() >= 256) ignore(op, "more than 256 points in one curve");
      else c.points.emplace_back(pt, d);
    } else {
      add(SfzIssueKind::kUnsupportedOpcode, op.file, op.line, "unsupported opcode '" + k + "' in <curve>", k);
    }
  }

  // Records a value replaced by the opcode default (once per opcode occurrence: only on the reporting pass).
  void ignore(const Op& op, const char* why, bool default_used = true) {
    if (!report_) return;
    result.ignored_values.push_back({op.name, op.value, op.file, op.line,
                                     "ignored value '" + op.value + "' for opcode '" + op.name + "' (" + why +
                                         ")" + (default_used ? "; the opcode default is used" : "")});
  }

  void bad(const Op& op, const char* why) {
    if (!report_) return;
    add(SfzIssueKind::kInvalidValue, op.file, op.line,
        "invalid value '" + op.value + "' for opcode '" + op.name + "' (" + why + ")");
  }
  void unsupported_value(const Op& op) {
    if (!report_) return;
    add(SfzIssueKind::kUnsupportedValue, op.file, op.line,
        "unsupported value '" + op.value + "' for opcode '" + op.name + "'", op.name);
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

  // lokey/hikey/key (sfizz): an unparseable or out-of-range note is not an error; the caller keeps the default.
  bool as_note_or_ignore(const Op& op, std::int64_t& out) {
    if (parse_note(op.value, out)) return true;
    ignore(op, "not a MIDI note 0..127");
    return false;
  }

  // Applies one opcode to `r`. Returns false for an invalid value (reported when `report`).
  bool apply(Region& r, Acc& acc, const Op& op, bool report) {
    SfzLoopSpec& ls = acc.ls;
    report_ = report;
    std::int64_t n = 0;
    double d = 0;
    const std::string& k = op.name;
    const std::int64_t kMaxFrames = std::int64_t(1) << 40;
    if (k == "sample") return true;
    if (k == "lokey") {
      r.key_lo = as_note_or_ignore(op, n) ? std::uint8_t(n) : Region{}.key_lo;
    } else if (k == "hikey") {
      std::int64_t raw = 0;
      if (parse_int(op.value, raw) && raw == -1) {  // sfizz: no note triggers this region
        acc.never_note = true;
        ignore(op, "hikey=-1: the region never triggers from a note and is not imported yet", false);
        return true;
      }
      acc.never_note = false;
      r.key_hi = as_note_or_ignore(op, n) ? std::uint8_t(n) : Region{}.key_hi;
    } else if (k == "key") {
      if (as_note_or_ignore(op, n)) { r.key_lo = r.key_hi = r.key_center = std::uint8_t(n); acc.never_note = false; }
      else { const Region d0; r.key_lo = d0.key_lo; r.key_hi = d0.key_hi; r.key_center = d0.key_center; }
    }
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
    else if (k == "end") {  // sfizz: a sample end of 0 (or below) turns the region off; beyond the file it is clamped by the engine
      if (!parse_int(op.value, n) || n > kMaxFrames) { bad(op, "expected integer in range"); return false; }
      r.has_end = true;
      r.end = n > 0 ? std::uint64_t(n) : 0;
    }
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
      else if (v == "first") r.trigger = TriggerMode::kFirst;
      else if (v == "legato") r.trigger = TriggerMode::kLegato;
      else if (v == "release_key") r.trigger = TriggerMode::kReleaseKey;
      else { unsupported_value(op); return false; }
    }
    else if (k == "sw_lokey") { if (!as_note(op, n)) return false; r.sw.lokey = std::int16_t(n); }
    else if (k == "sw_hikey") { if (!as_note(op, n)) return false; r.sw.hikey = std::int16_t(n); }
    else if (k == "sw_last") { if (!as_note(op, n)) return false; r.sw.last = std::int16_t(n); }
    else if (k == "sw_down") { if (!as_note(op, n)) return false; r.sw.down = std::int16_t(n); }
    else if (k == "sw_up") { if (!as_note(op, n)) return false; r.sw.up = std::int16_t(n); }
    else if (k == "sw_previous") { if (!as_note(op, n)) return false; r.sw.previous = std::int16_t(n); }
    else if (k == "sw_default") { if (!as_note(op, n)) return false; r.sw.default_key = std::int16_t(n); }
    else if (k == "xf_cccurve") {
      const std::string v = trim(op.value);
      if (v == "gain") r.xf.curve = XfCurve::kGain;
      else if (v == "power") r.xf.curve = XfCurve::kPower;
      else { unsupported_value(op); return false; }
    } else if (k == "fil_type") {
      const std::string v = trim(op.value);
      FilterType t;
      if (v == "lpf_1p") t = FilterType::kLpf1p;
      else if (v == "lpf_2p") t = FilterType::kLpf2p;
      else if (v == "hpf_1p") t = FilterType::kHpf1p;
      else if (v == "hpf_2p") t = FilterType::kHpf2p;
      else if (v == "bpf_2p") t = FilterType::kBpf2p;
      else { unsupported_value(op); return false; }
      acc.type = t;
      acc.type_set = true;
      acc.type_op = &op;
    }
    else if (k == "cutoff") { if (!as_float(op, 0, 48000, d)) return false; r.filter.cutoff_hz = float(d); acc.cutoff_set = true; }
    else if (k == "resonance") { if (!as_float(op, 0, 40, d)) return false; r.filter.resonance_db = float(d); acc.res_op = &op; }
    else if (bool in = false, lo = false; xf_opcode(k, in, lo, cc_)) {
      {
        const std::size_t base = std::string(in ? "xfin_" : "xfout_").size() + 4;  // prefix + "locc"/"hicc"
        if (k.size() > base + 1 && k[base] == '0') {
          if (report_)
            add(SfzIssueKind::kInvalidValue, op.file, op.line,
                "opcode '" + k + "' has a non-canonical controller number (write '" + k.substr(0, base) +
                    std::to_string(cc_) + "'; leading zeros would spell one controller two ways)");
          return false;
        }
      }
      if (cc_ > 127) { unsupported_value(op); return false; }
      if (!as_int(op, 0, 127, n)) return false;
      XfPart& part = (in ? acc.in : acc.out)[cc_];
      (lo ? part.lo : part.hi) = std::uint8_t(n);
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
    Acc acc;
    const Op* sample_op = nullptr;
    for (const Level* lv : {&global_, &master_, &group_, &region_})
      for (const Op& op : *lv) {
        if (op.name == "sample") sample_op = &op;
        apply(r, acc, op, false);  // values were validated when read
      }
    const SfzLoopSpec ls = acc.ls;
    r.has_loop_points = ls.start_set || ls.end_set;
    // Story 5.1: filter, keyswitch and crossfade state.
    if (acc.cutoff_set) r.filter.type = acc.type_set ? acc.type : FilterType::kLpf2p;
    else {
      r.filter = Filter{};  // fil_type/resonance without cutoff: no filter (documented), but never silent
      for (const Op* o : {acc.type_op, acc.res_op})
        if (o && warned_.insert(o->file + ":" + std::to_string(o->line) + ":" + o->name).second)
          result.warnings.push_back({o->name, o->file, o->line,
                                     "opcode '" + o->name + "' has no effect without 'cutoff'; the region gets no filter"});
    }
    if (r.sw.lokey >= 0 && r.sw.hikey >= 0 && r.sw.lokey > r.sw.hikey) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "sw_lokey > sw_hikey");
      return;
    }
    for (int dir = 0; dir < 2; ++dir) {
      const auto& m = dir == 0 ? acc.in : acc.out;
      const char* nm = dir == 0 ? "xfin" : "xfout";
      if (m.size() > kMaxCcCrossfades) {
        issue(SfzIssueKind::kUnsupportedValue, region_loc_,
              std::string("more than ") + std::to_string(kMaxCcCrossfades) + " " + nm +
                  " controllers in one region",
              std::string(nm) + "_locc<N>");
        return;
      }
      CcRange* out = dir == 0 ? r.xf.in : r.xf.out;
      std::uint8_t& count = dir == 0 ? r.xf.in_count : r.xf.out_count;
      count = 0;
      for (const auto& [cc, part] : m) {
        if (part.lo > part.hi) {
          issue(SfzIssueKind::kInvalidValue, region_loc_,
                std::string(nm) + "_locc" + std::to_string(cc) + " > " + nm + "_hicc" + std::to_string(cc) +
                    " (give both opcodes)");
          return;
        }
        out[count++] = {std::uint8_t(cc), part.lo, part.hi};
      }
    }
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
        if (add(SfzIssueKind::kMissingSample, sample_op->file, sample_op->line, "sample file not found: " + p.string()))
          result.issues.back().path = p.string();  // only the issue just added
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
    result.never_note_triggered.push_back(acc.never_note);
    result.loops.push_back(ls);
    result.region_sources.push_back({region_loc_.file, region_loc_.line});
  }

  fs::path main_, dir_;
  Cur cur_ = Cur::kNone;
  Level global_, master_, group_, region_;
  Loc region_loc_;
  std::string default_path_;
  std::map<std::string, std::size_t> samples_;
  std::set<std::string> seen_, warned_;
  bool report_ = false;
  int cc_ = 0;
};

}  // namespace

std::size_t drop_never_note_triggered(SfzParseResult& r) {
  std::size_t out = 0, dropped = 0;
  for (std::size_t i = 0; i < r.regions.size(); ++i) {
    if (i < r.never_note_triggered.size() && r.never_note_triggered[i]) { ++dropped; continue; }
    if (out != i) {
      r.regions[out] = r.regions[i];
      r.loops[out] = r.loops[i];
      r.region_sources[out] = r.region_sources[i];
    }
    ++out;
  }
  r.regions.resize(out);
  r.loops.resize(out);
  r.region_sources.resize(out);
  r.never_note_triggered.assign(out, false);
  return dropped;
}

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
    none.issues.push_back({SfzIssueKind::kParse, sfz_path, 0, "cannot open SFZ file", sfz_path, {}});
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
    imp.result.issues.push_back({SfzIssueKind::kParse, abs.string(), 0, std::string("parser failure: ") + e.what(), abs.string(), {}});
  }
  for (auto& i : imp.result.issues)
    if (i.kind == SfzIssueKind::kParse && i.message.find("Exceeded maximum include depth") != std::string::npos)
      i.message += " (recursive #include?)";
  std::sort(imp.result.included_files.begin(), imp.result.included_files.end());
  return std::move(imp.result);
}

}  // namespace velora::format

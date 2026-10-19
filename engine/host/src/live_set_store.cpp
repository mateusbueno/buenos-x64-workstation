// Live Set persistence, Story 4.2. Control thread only; see velora/host/live_set_store.hpp.
#include "velora/core/assign_events.hpp"
#include "velora/host/live_set_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "velora/core/engine.hpp"
#include "velora/core/engine_registry.hpp"
#include "velora/core/params_gen.hpp"

namespace velora::host::liveset {
namespace {

namespace params = ::velora::core::params;

constexpr std::uint16_t kTempoParamId = params::kTempoBpm;

// Structural UTF-8 check (no overlongs, surrogates, > U+10FFFF): a string failing it could not be re-read (proto3 strings).
bool utf8_ok(const std::string& s) {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len;
    std::uint32_t cp;
    if (c < 0x80) { ++i; continue; }
    else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

bool key_ok(const std::string& k) {
  if (k.empty() || k.size() > kMaxInstrumentKeyBytes || !utf8_ok(k)) return false;
  if (k[0] == '/' || k.find('\\') != std::string::npos || k.find('\0') != std::string::npos) return false;
  std::size_t start = 0;
  while (start <= k.size()) {
    std::size_t end = k.find('/', start);
    if (end == std::string::npos) end = k.size();
    if (k.compare(start, end - start, "..") == 0) return false;
    start = end + 1;
  }
  return true;
}

// ---- validation ----------------------------------------------------------------------------------------------------
struct Validator {
  ValidationReport rep;

  void err(IssueCode c, int part = -1, int scene = -1, int index = -1) { rep.errors.push_back({c, part, scene, index}); }
  void warn(IssueCode c, int part, int scene, int index) { rep.unknown_params.push_back({c, part, scene, index}); }

  static bool name_ok(const std::string& s) {
    if (s.empty() || s.size() > kMaxNameBytes || !utf8_ok(s)) return false;
    for (unsigned char ch : s)
      if (ch < 0x20 || ch == 0x7F) return false;
    return true;
  }

  static bool value_ok_range(const params::ParamDef& d, float v) { return std::isfinite(v) && v >= d.min && v <= d.max; }

  static bool value_ok(const params::ParamDef& d, float v) {
    if (!std::isfinite(v)) return false;
    if (v < d.min || v > d.max) return false;
    if (d.type != params::Type::kFloat && v != std::floor(v)) return false;
    return true;
  }

  // A list of (param_id, value). `want` is the scope every known id must have. Unknown ids are warnings.
  // Part lists (engine/envelope/eq, scene overrides) hold performance part-scope params: the two structural part-scope
  // params (part_output, timbre_ref) have dedicated fields / are the timbre assignment. Master lists hold master-scope
  // params except tempo (own field) and line_in_level (device input, not Live Set state).
  void entries(const google::protobuf::RepeatedPtrField<lsp::ParamEntry>& list, params::Scope want, int part, int scene) {
    if (list.size() > static_cast<int>(kMaxParamEntries)) {
      err(IssueCode::kTooManyParams, part, scene);
      return;
    }
    std::set<std::uint32_t> seen;
    for (int i = 0; i < list.size(); ++i) {
      const lsp::ParamEntry& e = list[i];
      if (!seen.insert(e.param_id()).second) err(IssueCode::kDuplicateParam, part, scene, i);
      const params::ParamDef* d = params::find(e.param_id());
      if (!d) {
        warn(IssueCode::kUnknownParam, part, scene, static_cast<int>(e.param_id()));
        if (!std::isfinite(e.value())) err(IssueCode::kBadParamValue, part, scene, i);
        continue;
      }
      if (d->scope != want || e.param_id() == kTempoParamId) {
        err(IssueCode::kWrongScope, part, scene, i);
        continue;
      }
      if ((want == params::Scope::kPart && d->cls != params::Class::kPerformance) ||
          (want == params::Scope::kMaster && e.param_id() == params::kLineInLevel)) {
        err(IssueCode::kWrongClass, part, scene, i);
        continue;
      }
      if (!value_ok(*d, e.value())) err(IssueCode::kBadParamValue, part, scene, i);
    }
  }

  void send(float v, std::uint16_t id, int part, int scene) {
    const params::ParamDef* d = params::find(id);
    if (!d || !value_ok(*d, v)) err(IssueCode::kBadSend, part, scene);
  }

  void insert_slot(const ws::v1::InsertSlot& s, int part, int scene, int idx) {
    const params::ParamDef* t = params::find(params::kInsertType);
    const int ty = static_cast<int>(s.type());
    if (ty < 0) err(IssueCode::kBadInsertSlot, part, scene, idx);
    else if (t && ty > static_cast<int>(t->max)) {
      // A type from a newer same-version build: its params mean nothing to us, so they are not range-checked.
      warn(IssueCode::kUnknownInsertType, part, scene, idx);
      return;
    }
    if (s.params_size() > static_cast<int>(params::kInsertParamCount)) {
      err(IssueCode::kBadInsertSlot, part, scene, idx);
      return;
    }
    for (int k = 0; k < s.params_size(); ++k) {
      const params::ParamDef* d = params::find(params::kInsertParam1 + static_cast<std::uint32_t>(k));
      if (!d || !value_ok(*d, s.params(k))) err(IssueCode::kBadInsertSlot, part, scene, idx);
    }
  }

  template <class Slots>
  void inserts(const Slots& slots, int part, int scene) {
    if (slots.size() > static_cast<int>(kMaxInsertSlots)) {
      err(IssueCode::kTooManyInserts, part, scene);
      return;
    }
    for (int i = 0; i < slots.size(); ++i) insert_slot(slots[i], part, scene, i);
  }

  void arp(const lsp::ArpSettings& a, int part, int scene) {
    bool bad = false;
    // Values above the known range come from a newer build: preserved with a warning. 0 / negative are errors.
    if (a.has_pattern()) {
      if (a.pattern() < lsp::ARP_PATTERN_UP) bad = true;
      else if (a.pattern() > lsp::ARP_PATTERN_AS_PLAYED) warn(IssueCode::kUnknownEnumValue, part, scene, static_cast<int>(a.pattern()));
    }
    if (a.has_rate()) {
      if (a.rate() < lsp::ARP_RATE_QUARTER) bad = true;
      else if (a.rate() > lsp::ARP_RATE_THIRTY_SECOND_TRIPLET) warn(IssueCode::kUnknownEnumValue, part, scene, static_cast<int>(a.rate()));
    }
    if (a.has_gate() && !(std::isfinite(a.gate()) && a.gate() >= 1.0f && a.gate() <= 200.0f)) bad = true;
    if (a.has_octave_range() && (a.octave_range() < 1 || a.octave_range() > 4)) bad = true;
    if (a.has_swing() && !(std::isfinite(a.swing()) && a.swing() >= 50.0f && a.swing() <= 75.0f)) bad = true;
    if (bad) err(IssueCode::kBadArp, part, scene);
  }

  // 0..known max ok; above = newer build (warning, preserved); negative = error. Returns false on error.
  bool output_ok(int v, int part, int scene) {
    if (v < 0) return false;
    if (!ws::v1::PartOutput_IsValid(v)) warn(IssueCode::kUnknownEnumValue, part, scene, v);
    return true;
  }
  static bool volume_ok(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }

  void timbre(const lsp::TimbreRef& t, int part) {
    if (!is_canonical_uuid(t.library_id()) || !key_ok(t.instrument_key()))
      err(IssueCode::kBadTimbre, part);
  }

  void part(const lsp::Part& p, int i) {
    if (p.has_timbre()) timbre(p.timbre(), i);
    if (p.low_key() > 127 || p.high_key() > 127 || p.low_key() > p.high_key()) err(IssueCode::kBadKeyRange, i);
    if (p.octave() < kOctaveMin || p.octave() > kOctaveMax) err(IssueCode::kBadOctave, i);
    if (p.transpose() < kTransposeMin || p.transpose() > kTransposeMax) err(IssueCode::kBadTranspose, i);
    if (!volume_ok(p.volume())) err(IssueCode::kBadVolume, i);
    if (!output_ok(static_cast<int>(p.output()), i, -1)) err(IssueCode::kBadOutput, i);
    inserts(p.inserts(), i, -1);
    send(p.send_reverb(), params::kSendReverbLevel, i, -1);
    send(p.send_delay(), params::kSendDelayLevel, i, -1);
    if (!p.has_engine() || p.engine().engine_type_id() == 0) err(IssueCode::kBadEngineType, i);
    if (p.has_engine()) entries(p.engine().params(), params::Scope::kPart, i, -1);
    entries(p.envelope(), params::Scope::kPart, i, -1);
    entries(p.eq(), params::Scope::kPart, i, -1);
    if (p.has_arp()) arp(p.arp(), i, -1);
  }

  void scene_part(const lsp::ScenePart& s, const lsp::Part* base, int pi, int si) {
    // The effective zone (override, else the base part's value) must be a valid pair.
    const std::uint32_t lo = s.has_low_key() ? s.low_key() : (base ? base->low_key() : 0);
    const std::uint32_t hi = s.has_high_key() ? s.high_key() : (base ? base->high_key() : 127);
    if (lo > 127 || hi > 127 || lo > hi) err(IssueCode::kBadKeyRange, pi, si);
    if (s.has_octave() && (s.octave() < kOctaveMin || s.octave() > kOctaveMax)) err(IssueCode::kBadOctave, pi, si);
    if (s.has_transpose() && (s.transpose() < kTransposeMin || s.transpose() > kTransposeMax))
      err(IssueCode::kBadTranspose, pi, si);
    if (s.has_volume() && !volume_ok(s.volume())) err(IssueCode::kBadVolume, pi, si);
    if (s.has_output() && !output_ok(static_cast<int>(s.output()), pi, si)) err(IssueCode::kBadOutput, pi, si);
    if (s.has_inserts()) inserts(s.inserts().slots(), pi, si);
    if (s.has_send_reverb()) send(s.send_reverb(), params::kSendReverbLevel, pi, si);
    if (s.has_send_delay()) send(s.send_delay(), params::kSendDelayLevel, pi, si);
    entries(s.engine_params(), params::Scope::kPart, pi, si);
    entries(s.envelope(), params::Scope::kPart, pi, si);
    entries(s.eq(), params::Scope::kPart, pi, si);
    if (s.has_arp()) arp(s.arp(), pi, si);
  }

  void run(const lsp::LiveSet& ls, std::uint32_t part_count) {
    if (!is_uuid_v4(ls.id())) err(IssueCode::kBadId);
    if (!name_ok(ls.name())) err(IssueCode::kBadName);
    const params::ParamDef* tempo = params::find(kTempoParamId);
    if (!tempo || !value_ok_range(*tempo, ls.tempo())) err(IssueCode::kBadTempo);
    if (static_cast<std::uint32_t>(ls.parts_size()) != part_count) err(IssueCode::kBadPartCount);
    else
      for (int i = 0; i < ls.parts_size(); ++i) part(ls.parts(i), i);
    entries(ls.master(), params::Scope::kMaster, -1, -1);

    if (ls.scenes_size() > static_cast<int>(kMaxScenes)) {
      err(IssueCode::kTooManyScenes);
    } else {
      for (int si = 0; si < ls.scenes_size(); ++si) {
        const lsp::Scene& sc = ls.scenes(si);
        if (!name_ok(sc.name())) err(IssueCode::kBadSceneName, -1, si);
        if (static_cast<std::uint32_t>(sc.parts_size()) > part_count) {
          err(IssueCode::kTooManySceneParts, -1, si);
        } else {
          for (int pi = 0; pi < sc.parts_size(); ++pi)
            scene_part(sc.parts(pi), pi < ls.parts_size() ? &ls.parts(pi) : nullptr, pi, si);
        }
        entries(sc.master(), params::Scope::kMaster, -1, si);
      }
    }

    if (ls.assign_size() != static_cast<int>(kAssignSlots)) {
      err(IssueCode::kBadAssignCount);
    } else {
      for (int k = 0; k < ls.assign_size(); ++k) {
        const std::uint32_t id = ls.assign(k);
        if (id == 0) continue;
        const params::ParamDef* d = params::find(id);
        if (!d) warn(IssueCode::kUnknownParam, -1, -1, static_cast<int>(id));
        else if (d->cls != params::Class::kPerformance) err(IssueCode::kBadAssignParam, -1, -1, k);
        else if (!core::is_assignable(*d)) warn(IssueCode::kUnknownParam, -1, -1, static_cast<int>(id));  // a file from before the flag: still loads, the slot stays in the file and out of the generation
      }
    }
  }
};

// ---- POSIX helpers -------------------------------------------------------------------------------------------------
struct Fd {
  int fd = -1;
  Fd() = default;
  explicit Fd(int f) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  bool ok() const { return fd >= 0; }
  int release() {
    const int f = fd;
    fd = -1;
    return f;
  }
};

bool full_sync(int fd) {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;  // falls back to fsync on file systems without it
#endif
  return ::fsync(fd) == 0;
}

bool sync_dir(const std::string& dir) {
  Fd d(::open(dir.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
  return d.ok() && full_sync(d.fd);
}

bool write_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool no_space_errno(int e) { return e == ENOSPC || e == EDQUOT; }

constexpr const char* kExt = ".liveset";
constexpr const char* kTmpExt = ".liveset.tmp";

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

const char* to_string(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kIo: return "io";
    case Status::kNotFound: return "not_found";
    case Status::kTooLarge: return "too_large";
    case Status::kParseError: return "parse_error";
    case Status::kUnsupportedVersion: return "unsupported_version";
    case Status::kInvalid: return "invalid";
    case Status::kBadId: return "bad_id";
    case Status::kNotControlThread: return "not_control_thread";
    case Status::kWriterBusy: return "writer_busy";
    case Status::kFaultInjected: return "fault_injected";
    case Status::kCommittedNotDurable: return "committed_not_durable";
    case Status::kNoSpace: return "no_space";
    case Status::kCount: break;
  }
  return "?";
}

ValidationReport validate(const lsp::LiveSet& ls, std::uint32_t part_count) {
  Validator v;
  v.run(ls, part_count);
  return std::move(v.rep);
}

// ---- reading -------------------------------------------------------------------------------------------------------
Status migrate(lsp::LiveSet& ls, std::uint32_t from, std::uint32_t to) {
  if (from == to) return Status::kOk;
  if (to >= 1 && from == to - 1) {
    // Stub: no schema change exists between N-1 and N yet; a real migration lands here when one does.
    ls.set_format_version(to);
    return Status::kOk;
  }
  return Status::kUnsupportedVersion;
}

void normalize(lsp::LiveSet& ls) {
  for (lsp::Part& p : *ls.mutable_parts())
    if (p.has_timbre() && p.timbre().library_id().empty() && p.timbre().instrument_key().empty()) p.clear_timbre();
}

Status parse(std::string_view bytes, const ReadOptions& opt, LoadResult& out) {
  out = LoadResult{};
  if (bytes.size() > kMaxFileBytes) return Status::kTooLarge;
  if (!out.live_set.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    out.live_set.Clear();
    return Status::kParseError;
  }
  normalize(out.live_set);
  out.file_version = out.live_set.format_version();
  if (out.file_version < min_readable_version(opt.current_version) || out.file_version > opt.current_version)
    return Status::kUnsupportedVersion;
  if (out.file_version != opt.current_version) {
    if (migrate(out.live_set, out.file_version, opt.current_version) != Status::kOk) return Status::kUnsupportedVersion;
    out.migrated = true;
  }
  std::uint32_t parts = opt.part_count;
  if (parts == 0) {
    const auto n = static_cast<std::uint32_t>(out.live_set.parts_size());
    parts = (n == kV1PartCount || n == kProPartCount) ? n : kV1PartCount;
  }
  out.report = validate(out.live_set, parts);
  return out.report.ok() ? Status::kOk : Status::kInvalid;
}

Status read_file(const std::string& path, const ReadOptions& opt, LoadResult& out) {
  out = LoadResult{};
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!f.ok()) return (errno == ENOENT || errno == ENOTDIR) ? Status::kNotFound : Status::kIo;
  struct stat st{};
  if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) return Status::kIo;
  if (static_cast<std::uint64_t>(st.st_size) > kMaxFileBytes) return Status::kTooLarge;
  std::string buf(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t got = 0;
  while (got < buf.size()) {
    const ssize_t r = ::read(f.fd, buf.data() + got, buf.size() - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::kIo;
    }
    if (r == 0) break;
    got += static_cast<std::size_t>(r);
  }
  buf.resize(got);
  return parse(buf, opt, out);
}

Status serialize(const lsp::LiveSet& ls, std::string& out) {
  out.clear();
  {
    google::protobuf::io::StringOutputStream sos(&out);
    google::protobuf::io::CodedOutputStream cos(&sos);
    cos.SetSerializationDeterministic(true);
    if (!ls.SerializeToCodedStream(&cos)) return Status::kIo;
  }
  if (out.size() > kMaxFileBytes) {
    out.clear();
    return Status::kTooLarge;
  }
  return Status::kOk;
}

// ---- ids -----------------------------------------------------------------------------------------------------------
bool OsRandom::fill(std::uint8_t* dst, std::size_t n) {
  Fd f(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (!f.ok()) return false;
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = ::read(f.fd, dst + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool SeededRandom::fill(std::uint8_t* dst, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    dst[i] = static_cast<std::uint8_t>((s_ * 0x2545F4914F6CDD1Dull) >> 56);
  }
  return true;
}

std::string mint_uuid_v4(RandomSource& rng) {
  std::uint8_t b[16];
  if (!rng.fill(b, sizeof b)) return std::string();
  b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
  char buf[37];
  std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
                b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(buf, 36);
}

bool is_canonical_uuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (std::size_t i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool is_uuid_v4(std::string_view s) {
  if (!is_canonical_uuid(s)) return false;
  const char v = s[14], var = s[19];
  return v == '4' && (var == '8' || var == '9' || var == 'a' || var == 'b');
}

AdoptResult adopt_with_collision_check(lsp::LiveSet& ls, const std::set<std::string>& existing_ids, RandomSource& rng) {
  if (is_uuid_v4(ls.id()) && existing_ids.find(ls.id()) == existing_ids.end()) return AdoptResult::kUnchanged;
  for (int attempt = 0; attempt < kMaxAdoptAttempts; ++attempt) {
    const std::string id = mint_uuid_v4(rng);
    if (id.empty()) return AdoptResult::kFailed;  // random source failed: leave the Live Set as it is
    if (existing_ids.find(id) == existing_ids.end()) {
      ls.set_id(id);
      return AdoptResult::kReplaced;
    }
  }
  return AdoptResult::kFailed;
}

lsp::LiveSet make_default_live_set(const std::string& id, const std::string& name, std::uint32_t part_count) {
  lsp::LiveSet ls;
  ls.set_format_version(kFormatVersion);
  ls.set_id(id);
  ls.set_name(name);
  ls.set_tempo(120.0f);
  for (std::uint32_t i = 0; i < part_count; ++i) {
    lsp::Part* p = ls.add_parts();
    p->set_low_key(0);
    p->set_high_key(127);
    p->set_keyboard_on(true);
    p->set_volume(0.8f);
    p->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kSample));
  }
  for (std::size_t k = 0; k < kAssignSlots; ++k) ls.add_assign(0);
  return ls;
}

// ---- resolution ----------------------------------------------------------------------------------------------------
EngineAvailability availability_of(const core::EngineRegistry& registry) {
  std::set<std::uint32_t> ids;  // snapshot: the function must not touch the registry after this call
  for (std::size_t i = 0; i < registry.size(); ++i)
    if (const core::Engine* e = registry.at(i)) ids.insert(static_cast<std::uint32_t>(e->type_id()));
  return [ids = std::move(ids)](std::uint32_t id) { return ids.count(id) != 0; };
}

std::vector<PartState> resolve_parts(const lsp::LiveSet& ls, const EngineAvailability& engine_available) {
  std::vector<PartState> out;
  out.reserve(static_cast<std::size_t>(ls.parts_size()));
  for (const lsp::Part& p : ls.parts()) {
    const std::uint32_t et = p.engine().engine_type_id();
    if (!engine_available || !engine_available(et)) out.push_back(PartState::kEngineNotAvailable);
    else if (et == static_cast<std::uint32_t>(core::EngineTypeId::kSample) && !p.has_timbre())
      out.push_back(PartState::kTimbreEmpty);
    else out.push_back(PartState::kOk);
  }
  return out;
}

// ---- the store -----------------------------------------------------------------------------------------------------
Status LiveSetStore::open(StoreOptions opt, std::unique_ptr<LiveSetStore>& out) {
  out.reset();
  struct stat st{};
  if (::stat(opt.dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return Status::kNotFound;
  Fd lock(::open((opt.dir + "/.writer.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644));
  if (!lock.ok()) return Status::kIo;
  if (::flock(lock.fd, LOCK_EX | LOCK_NB) != 0) return errno == EWOULDBLOCK ? Status::kWriterBusy : Status::kIo;
  std::unique_ptr<LiveSetStore> s(new LiveSetStore(std::move(opt)));
  s->lock_fd_ = lock.release();
  s->owner_ = std::this_thread::get_id();
  out = std::move(s);
  return Status::kOk;
}

LiveSetStore::~LiveSetStore() {
  if (lock_fd_ >= 0) ::close(lock_fd_);
}

std::string LiveSetStore::path_for(const std::string& id) const { return opt_.dir + "/" + id + kExt; }

Status LiveSetStore::read(const std::string& id, LoadResult& out) const {
  out = LoadResult{};
  if (!is_uuid_v4(id)) return Status::kBadId;
  ReadOptions ro;
  ro.part_count = opt_.part_count;
  ro.current_version = opt_.current_version;
  const Status s = read_file(path_for(id), ro, out);
  // A file copied/renamed to another Live Set's name would shadow that Live Set: the embedded id must match the filename.
  if (s == Status::kOk && out.live_set.id() != id) {
    out.report.errors.push_back({IssueCode::kBadId, -1, -1, -1});
    return Status::kInvalid;
  }
  return s;
}

Status LiveSetStore::write(const lsp::LiveSet& ls, ValidationReport* report) {
  if (std::this_thread::get_id() != owner_) return Status::kNotControlThread;
  if (ls.format_version() != opt_.current_version) return Status::kUnsupportedVersion;
  lsp::LiveSet normalized;
  const lsp::LiveSet* src = &ls;
  for (const lsp::Part& p : ls.parts())
    if (p.has_timbre() && p.timbre().library_id().empty() && p.timbre().instrument_key().empty()) {
      normalized = ls;
      normalize(normalized);
      src = &normalized;
      break;
    }
  ValidationReport rep = validate(*src, opt_.part_count);
  const bool ok = rep.ok();
  if (report) *report = std::move(rep);
  if (!ok) return Status::kInvalid;
  std::string bytes;
  if (const Status s = serialize(*src, bytes); s != Status::kOk) return s;

  mutations_.fetch_add(1, std::memory_order_relaxed);
  return atomic_replace_file(opt_.dir, ls.id() + kExt, ls.id() + kTmpExt, bytes, opt_.on_stage, opt_.inject_dir_sync_failure, opt_.inject_write_errno);
}

Status atomic_replace_file(const std::string& dir, const std::string& name, const std::string& tmp_name, const std::string& bytes,
                           const std::function<HookAction(Stage)>& on_stage, bool inject_dir_sync_failure, int inject_write_errno) {
  const std::string final_path = dir + "/" + name;
  const std::string tmp = dir + "/" + tmp_name;
  ::unlink(tmp.c_str());  // a stale temp of ours (we hold the writer lock)
  const auto hook = [&](Stage st) { return on_stage && on_stage(st) == HookAction::kAbort; };
  {
    Fd f(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (!f.ok()) return no_space_errno(errno) ? Status::kNoSpace : Status::kIo;
    if (inject_write_errno != 0) {  // test hook: the disk fills up while the temp file is written
      errno = inject_write_errno;
      ::unlink(tmp.c_str());
      return no_space_errno(errno) ? Status::kNoSpace : Status::kIo;
    }
    if (!write_all(f.fd, bytes.data(), bytes.size())) {
      const int e = errno;
      ::unlink(tmp.c_str());
      return no_space_errno(e) ? Status::kNoSpace : Status::kIo;
    }
    if (hook(Stage::kTempWritten)) return Status::kFaultInjected;
    if (!full_sync(f.fd)) {
      const int e = errno;
      ::unlink(tmp.c_str());
      return no_space_errno(e) ? Status::kNoSpace : Status::kIo;
    }
  }
  if (hook(Stage::kFsynced)) return Status::kFaultInjected;
  if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
    const int e = errno;
    ::unlink(tmp.c_str());
    return no_space_errno(e) ? Status::kNoSpace : Status::kIo;
  }
  if (hook(Stage::kRenamed)) return Status::kFaultInjected;
  if (inject_dir_sync_failure || !sync_dir(dir)) return Status::kCommittedNotDurable;
  if (hook(Stage::kDirSynced)) return Status::kFaultInjected;
  return Status::kOk;
}

Status read_regular_file(const std::string& path, std::size_t max_bytes, std::string& out) {
  out.clear();
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!f.ok()) return (errno == ENOENT || errno == ENOTDIR) ? Status::kNotFound : Status::kIo;
  struct stat st{};
  if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode)) return Status::kIo;
  if (static_cast<std::uint64_t>(st.st_size) > max_bytes) return Status::kTooLarge;
  std::string buf(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t got = 0;
  while (got < buf.size()) {
    const ssize_t r = ::read(f.fd, buf.data() + got, buf.size() - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::kIo;
    }
    if (r == 0) break;
    got += static_cast<std::size_t>(r);
  }
  buf.resize(got);
  out = std::move(buf);
  return Status::kOk;
}

Status LiveSetStore::remove(const std::string& id, const std::string& path) {
  if (std::this_thread::get_id() != owner_) return Status::kNotControlThread;
  if (!is_uuid_v4(id)) return Status::kBadId;
  std::string target = path_for(id);
  if (!path.empty() && path != target) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || path.substr(0, slash) != opt_.dir || !ends_with(path, kExt)) return Status::kBadId;
    target = path;
  }
  if (opt_.inject_remove_failure) return Status::kIo;
  mutations_.fetch_add(1, std::memory_order_relaxed);
  if (::unlink(target.c_str()) != 0) return errno == ENOENT ? Status::kNotFound : Status::kIo;
  return sync_dir(opt_.dir) ? Status::kOk : Status::kCommittedNotDurable;
}

bool valid_display_name(const std::string& s) { return Validator::name_ok(s); }

Status LiveSetStore::reconcile(std::size_t& removed) {
  removed = 0;
  if (std::this_thread::get_id() != owner_) return Status::kNotControlThread;
  DIR* d = ::opendir(opt_.dir.c_str());
  if (!d) return Status::kIo;
  std::vector<std::string> doomed;
  while (const dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (ends_with(name, kTmpExt)) doomed.push_back(name);
  }
  ::closedir(d);
  Status st = Status::kOk;
  for (const std::string& n : doomed) {
    if (::unlink((opt_.dir + "/" + n).c_str()) == 0) ++removed;
    else st = Status::kIo;  // could not remove a leftover: keep going, report it
  }
  if (removed > 0) sync_dir(opt_.dir);
  return st;
}

}  // namespace velora::host::liveset

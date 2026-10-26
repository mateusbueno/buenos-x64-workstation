#include "velora/sysd/config_store.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "velora/host/sysd_config_client.hpp"  // default_config()

namespace velora::sysd {

namespace {

namespace ls = host::liveset;

bool valid_live_set_id(const std::string& id) {
  if (id.size() > kMaxLiveSetIdBytes) return false;
  for (const char c : id) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool reject(Rejection& why, ws::v1::ErrorCode code, const char* field, const char* reason, const std::string& message) {
  why.code = code;
  why.message = message;
  why.details.clear();
  why.details["field"] = field;
  why.details["reason"] = reason;
  return false;
}

// A single enum field of a SetConfig: UNSPECIFIED and values this release does not know are ERROR_CODE_UNKNOWN_ENUM_VALUE.
template <class E>
bool check_enum(int v, bool (*is_valid)(int), const char* field, Rejection& why) {
  if (v == 0 || !is_valid(v)) {
    const bool ok = reject(why, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, field, "unknown_enum_value", std::string(field) + " is not a known value");
    why.details["value"] = std::to_string(v);
    return ok;
  }
  return true;
}

// A whole pedal calibration as stored / merged.
bool check_pedal(const ws::v1::PedalCalibration& p, const char* field, Rejection& why) {
  if (p.min() > kPedalMax || p.max() > kPedalMax) {
    reject(why, ws::v1::ERROR_CODE_OUT_OF_RANGE, field, "out_of_range", std::string(field) + " min and max are 0..4095");
    why.details["max_allowed"] = std::to_string(kPedalMax);
    return false;
  }
  if (!(p.min() == 0 && p.max() == 0) && p.min() >= p.max())  // 0/0 = "not calibrated"
    return reject(why, ws::v1::ERROR_CODE_INVALID_CONFIG, field, "min_not_below_max", std::string(field) + " needs min < max");
  // Story 3.11: a calibrated range narrower than kMinPedalSpan counts cannot give a usable travel (a pedal that was barely moved): refused.
  if (p.max() > p.min() && p.max() - p.min() < kMinPedalSpan) {
    why.details["min_span"] = std::to_string(kMinPedalSpan);
    return reject(why, ws::v1::ERROR_CODE_INVALID_CONFIG, field, "span_too_small", std::string(field) + " needs max - min of at least 256 counts");
  }
  if (!ws::v1::PedalPolarity_IsValid(static_cast<int>(p.polarity())))
    return reject(why, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, field, "unknown_enum_value", std::string(field) + " polarity is not a known value");
  return true;
}

bool check_id(const std::string& id, Rejection& why) {
  if (!valid_live_set_id(id))
    return reject(why, ws::v1::ERROR_CODE_INVALID_CONFIG, "boot_into.default_live_set_id", "bad_live_set_id",
                  "default_live_set_id is empty or 1..64 characters of [A-Za-z0-9_-]");
  return true;
}

bool check_boot_into(const ws::v1::BootIntoConfig& b, Rejection& why) {
  const int m = static_cast<int>(b.mode());
  if (m == 0 || !ws::v1::BootIntoMode_IsValid(m))
    return reject(why, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "boot_into.mode", "unknown_enum_value", "boot_into.mode is not a known value");
  if (!check_id(b.default_live_set_id(), why)) return false;
  if (b.mode() == ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET && b.default_live_set_id().empty())
    return reject(why, ws::v1::ERROR_CODE_INVALID_CONFIG, "boot_into.default_live_set_id", "missing_live_set_id",
                  "Boot into DEFAULT_LIVE_SET needs a default_live_set_id");
  return true;
}

// The present fields of a patch, each on its own (the merged result is checked afterwards by valid_config()).
bool check_pedal_patch(const ws::v1::PedalPatch& p, const char* field, Rejection& why) {
  if ((p.has_min() && p.min() > kPedalMax) || (p.has_max() && p.max() > kPedalMax)) {
    reject(why, ws::v1::ERROR_CODE_OUT_OF_RANGE, field, "out_of_range", std::string(field) + " min and max are 0..4095");
    why.details["max_allowed"] = std::to_string(kPedalMax);
    return false;
  }
  if (p.has_polarity() && !ws::v1::PedalPolarity_IsValid(static_cast<int>(p.polarity())))
    return reject(why, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, field, "unknown_enum_value", std::string(field) + " polarity is not a known value");
  return true;
}

bool check_patch_fields(const ws::v1::ConfigPatch& p, Rejection& why) {
  if (p.has_clock_source() && !check_enum<ws::v1::ClockSource>(p.clock_source(), &ws::v1::ClockSource_IsValid, "clock_source", why)) return false;
  if (p.has_velocity_curve() && !check_enum<ws::v1::VelocityCurve>(p.velocity_curve(), &ws::v1::VelocityCurve_IsValid, "velocity_curve", why)) return false;
  if (p.has_sustain() && !check_pedal_patch(p.sustain(), "sustain", why)) return false;
  if (p.has_expression() && !check_pedal_patch(p.expression(), "expression", why)) return false;
  if (p.has_boot_into()) {
    const ws::v1::BootIntoPatch& b = p.boot_into();
    if (b.has_mode() && (b.mode() == 0 || !ws::v1::BootIntoMode_IsValid(static_cast<int>(b.mode()))))
      return reject(why, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "boot_into.mode", "unknown_enum_value", "boot_into.mode is not a known value");
    if (b.has_default_live_set_id() && !check_id(b.default_live_set_id(), why)) return false;
  }
  return true;
}

void merge_pedal(ws::v1::PedalCalibration* dst, const ws::v1::PedalPatch& src) {  // field-wise: absent fields and unknown fields of dst survive
  if (src.has_min()) dst->set_min(src.min());
  if (src.has_max()) dst->set_max(src.max());
  if (src.has_polarity()) dst->set_polarity(src.polarity());
}

void merge_patch(ws::v1::Config& c, const ws::v1::ConfigPatch& p) {
  if (p.has_clock_source()) c.set_clock_source(p.clock_source());
  if (p.has_velocity_curve()) c.set_velocity_curve(p.velocity_curve());
  if (p.has_sustain()) merge_pedal(c.mutable_sustain(), p.sustain());
  if (p.has_expression()) merge_pedal(c.mutable_expression(), p.expression());
  if (p.has_boot_into()) {
    if (p.boot_into().has_mode()) c.mutable_boot_into()->set_mode(p.boot_into().mode());
    if (p.boot_into().has_default_live_set_id()) c.mutable_boot_into()->set_default_live_set_id(p.boot_into().default_live_set_id());
  }
}

bool patch_empty(const ws::v1::ConfigPatch& p) {
  const bool pedal = (p.has_sustain() && (p.sustain().has_min() || p.sustain().has_max() || p.sustain().has_polarity())) ||
                     (p.has_expression() && (p.expression().has_min() || p.expression().has_max() || p.expression().has_polarity()));
  const bool boot = p.has_boot_into() && (p.boot_into().has_mode() || p.boot_into().has_default_live_set_id());
  return !p.has_clock_source() && !p.has_velocity_curve() && !pedal && !boot;
}

bool same(const google::protobuf::MessageLite& a, const google::protobuf::MessageLite& b) { return a.SerializeAsString() == b.SerializeAsString(); }

std::string dir_file(const std::string& dir, const char* name) { return dir + "/" + name; }

// Keeps the file at `path` under a name that does not exist yet (`path + suffix`, then `.1`, `.2`, ...): an aside file is never overwritten.
// hard link first, so `path` stays in place until the caller replaces it; where links are not supported, a rename (which moves it) is the fallback.
// `remove_original` unlinks `path` afterwards (a file that is bad and must not be read again). Returns the name used, empty on failure.
std::string keep_aside(const std::string& path, const std::string& suffix, bool remove_original) {
  for (int n = 0; n < 1000; ++n) {
    const std::string name = path + suffix + (n ? "." + std::to_string(n) : std::string());
    if (::link(path.c_str(), name.c_str()) == 0) {
      if (remove_original) ::unlink(path.c_str());
      return name;
    }
    if (errno == EEXIST) continue;
    if (errno == ENOENT) return {};
    struct stat st{};
    if (::lstat(name.c_str(), &st) == 0) continue;  // exists: never overwrite
    if (::rename(path.c_str(), name.c_str()) == 0) return name;  // links unsupported: move it instead
    return {};
  }
  return {};
}

}  // namespace

const char* to_string(LoadReason r) {
  switch (r) {
    case LoadReason::kLoaded: return "loaded";
    case LoadReason::kMissing: return "missing";
    case LoadReason::kCorrupt: return "corrupt";
    case LoadReason::kInvalidContent: return "invalid_content";
    case LoadReason::kTooOld: return "too_old";
    case LoadReason::kTooNew: return "too_new";
    case LoadReason::kTooLarge: return "too_large";
    case LoadReason::kUnreadable: return "unreadable";
  }
  return "unknown";
}

ws::v1::Config normalized(ws::v1::Config c) {
  if (c.clock_source() == ws::v1::CLOCK_SOURCE_UNSPECIFIED) c.set_clock_source(ws::v1::CLOCK_SOURCE_INTERNAL);
  if (c.velocity_curve() == ws::v1::VELOCITY_CURVE_UNSPECIFIED) c.set_velocity_curve(ws::v1::VELOCITY_CURVE_NORMAL);
  if (c.boot_into().mode() == ws::v1::BOOT_INTO_MODE_UNSPECIFIED) c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_LAST_SOUND);
  c.mutable_sustain();
  c.mutable_expression();
  return c;
}

bool valid_config(const ws::v1::Config& c, Rejection* why) {
  Rejection local;
  Rejection& w = why ? *why : local;
  const int ck = static_cast<int>(c.clock_source()), vc = static_cast<int>(c.velocity_curve());
  if (!ws::v1::ClockSource_IsValid(ck)) return reject(w, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "clock_source", "unknown_enum_value", "unknown clock_source");
  if (!ws::v1::VelocityCurve_IsValid(vc)) return reject(w, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "velocity_curve", "unknown_enum_value", "unknown velocity_curve");
  if (!check_pedal(c.sustain(), "sustain", w) || !check_pedal(c.expression(), "expression", w)) return false;
  return check_boot_into(c.boot_into(), w);
}

bool validate_patch(const ws::v1::Config& current, const ws::v1::ConfigPatch& patch, Rejection& why) {
  if (patch_empty(patch)) {
    why = Rejection{};
    why.code = ws::v1::ERROR_CODE_INVALID_COMMAND;
    why.message = "SetConfig carries no field";
    why.details["reason"] = "empty_patch";
    return false;
  }
  if (!check_patch_fields(patch, why)) return false;
  ws::v1::Config merged = current;  // cross-field rules (Boot into mode vs id) hold on the merged result too
  merge_patch(merged, patch);
  return valid_config(merged, &why);
}

ConfigStore::ConfigStore(StoreOptions opt) : opt_(std::move(opt)) {
  const std::string lock_path = dir_file(opt_.dir, kLockFileName);
  lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ >= 0 && ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  if (lock_fd_ < 0) {
    log("sysd: cannot take the writer lock of " + opt_.dir + ": " + std::strerror(errno));
    config_ = host::default_config();
    file_.set_schema_version(opt_.current_version);
    *file_.mutable_config() = config_;
    file_version_ = opt_.current_version;
    return;
  }
  load();
}

ConfigStore::~ConfigStore() {
  if (lock_fd_ >= 0) ::close(lock_fd_);  // releases the flock
}

void ConfigStore::load() {
  const std::string path = dir_file(opt_.dir, kFileName);
  auto use_defaults = [&](LoadReason r, const std::string& detail) {
    reason_ = r;
    file_.Clear();
    file_.set_schema_version(opt_.current_version);
    *file_.mutable_config() = host::default_config();
    config_ = file_.config();
    file_version_ = opt_.current_version;
    if (r != LoadReason::kMissing) log(std::string("sysd: config file ") + path + " not used (" + to_string(r) + (detail.empty() ? "" : ": " + detail) + "); serving defaults");
  };
  std::string bytes;
  const ls::Status rs = ls::read_regular_file(path, kMaxFileBytes, bytes);
  if (rs == ls::Status::kNotFound) return use_defaults(LoadReason::kMissing, "");
  if (rs == ls::Status::kTooLarge) {
    aside_suffix_ = ".too_large";
    return use_defaults(LoadReason::kTooLarge, "");
  }
  if (rs != ls::Status::kOk) {
    aside_suffix_ = ".unreadable";
    return use_defaults(LoadReason::kUnreadable, ls::to_string(rs));
  }
  velora::sysd::v1::SysdConfig f;
  auto set_aside = [&](const char* suffix) {  // the bad file is kept under a fresh name, never overwriting an earlier aside, never deleted
    if (keep_aside(path, suffix, true).empty()) log("sysd: cannot move " + path + " aside: " + std::strerror(errno));
  };
  if (!f.ParseFromString(bytes)) {
    set_aside(".corrupt");
    return use_defaults(LoadReason::kCorrupt, "bytes do not parse");
  }
  const std::uint32_t v = f.schema_version(), n = opt_.current_version;
  if (v == 0 || v < min_readable_version(n)) {
    aside_suffix_ = ".too_old";
    return use_defaults(LoadReason::kTooOld, "schema_version " + std::to_string(v) + ", this release reads " + std::to_string(min_readable_version(n)) + ".." + std::to_string(n));
  }
  if (v > n) {
    aside_suffix_ = ".too_new";
    return use_defaults(LoadReason::kTooNew, "schema_version " + std::to_string(v) + ", this release reads " + std::to_string(min_readable_version(n)) + ".." + std::to_string(n));
  }
  Rejection why;
  ws::v1::Config norm = normalized(f.config());
  // Story 3.11: a file written before the minimum span existed may hold a pedal range of 1..255 counts. That is not a reason to void the whole file: the pedal is
  // normalised to uncalibrated (0 / 0, polarity kept) and the rest of the config loads. The span rule still applies to every patch (the merged result).
  for (const char* which : {"sustain", "expression"}) {
    ws::v1::PedalCalibration* p = which[0] == 's' ? norm.mutable_sustain() : norm.mutable_expression();
    if (p->max() > p->min() && p->max() <= kPedalMax && p->max() - p->min() < kMinPedalSpan) {
      log(std::string("sysd: config file: ") + which + " calibration " + std::to_string(p->min()) + ".." + std::to_string(p->max()) + " is narrower than " +
          std::to_string(kMinPedalSpan) + " counts; treated as uncalibrated");
      p->set_min(0);
      p->set_max(0);
    }
  }
  if (!valid_config(norm, &why)) {
    set_aside(".corrupt");
    return use_defaults(LoadReason::kInvalidContent, why.details["field"] + " " + why.details["reason"]);
  }
  *f.mutable_config() = norm;
  file_ = std::move(f);
  config_ = file_.config();
  file_version_ = v;
  reason_ = LoadReason::kLoaded;
  if (v != n) log("sysd: config file is at schema_version " + std::to_string(v) + " (N-1): read-only until the slot is good, then migrated");
}

ApplyResult ConfigStore::apply(const ws::v1::ConfigPatch& patch) {
  ApplyResult res;
  if (lock_fd_ < 0) {
    res.rejection.code = ws::v1::ERROR_CODE_STORAGE_FAILED;
    res.rejection.message = "this sysd does not own the config directory";
    res.rejection.details["reason"] = "writer_busy";
    return res;
  }
  if (!validate_patch(config_, patch, res.rejection)) return res;
  velora::sysd::v1::SysdConfig work = file_;
  merge_patch(*work.mutable_config(), patch);
  if (same(work.config(), file_.config())) {
    res.status = ApplyStatus::kUnchanged;
    return res;
  }
  work.set_revision(file_.revision() + 1);
  std::uint32_t stamp = opt_.current_version;
  if (file_version_ != opt_.current_version && !(opt_.slot_good && opt_.slot_good())) stamp = file_version_;  // N-1 stays N-1 until migrated
  work.set_schema_version(stamp);
  const std::string out = work.SerializeAsString();
  if (out.size() > kMaxFileBytes) {
    res.rejection.code = ws::v1::ERROR_CODE_STORAGE_FAILED;
    res.rejection.message = "the config would exceed the maximum file size";
    res.rejection.details["reason"] = "too_large";
    return res;
  }
  if (!aside_suffix_.empty() && !aside_kept_) {  // an unreadable-version file is about to be replaced: keep a copy first (a link: the primary stays in place)
    if (keep_aside(dir_file(opt_.dir, kFileName), aside_suffix_, false).empty() && errno != ENOENT) {
      res.rejection.code = ws::v1::ERROR_CODE_STORAGE_FAILED;
      res.rejection.message = "cannot keep the unreadable config file aside";
      res.rejection.details["reason"] = "set_aside_failed";
      return res;
    }
    aside_kept_ = true;  // a retry after a failed replace does not make a second copy
  }
  const ls::Status s = ls::atomic_replace_file(opt_.dir, kFileName, kTmpFileName, out, opt_.on_stage, opt_.inject_dir_sync_failure, opt_.inject_write_errno);
  if (s != ls::Status::kOk && s != ls::Status::kCommittedNotDurable) {
    res.rejection.code = s == ls::Status::kNoSpace ? ws::v1::ERROR_CODE_STORAGE_FULL : ws::v1::ERROR_CODE_STORAGE_FAILED;
    res.rejection.message = "the config could not be written; nothing changed";
    res.rejection.details["reason"] = ls::to_string(s);
    return res;
  }
  aside_suffix_.clear();  // only now: a failed replace or a crash before it leaves the primary file untouched
  aside_kept_ = false;
  file_ = std::move(work);
  config_ = file_.config();
  file_version_ = stamp;
  reason_ = LoadReason::kLoaded;
  res.status = s == ls::Status::kOk ? ApplyStatus::kApplied : ApplyStatus::kNotDurable;
  return res;
}

host::migration::Outcome ConfigStore::migrate() {
  host::migration::Options mo;
  mo.dir = opt_.dir;
  mo.current_version = opt_.current_version;
  mo.slot_good = opt_.slot_good;
  mo.on_stage = opt_.on_stage;
  mo.inject_dir_sync_failure = opt_.inject_dir_sync_failure;
  mo.inject_write_errno = opt_.inject_write_errno;
  const host::migration::Report rep = host::migration::run(mo, {migration_unit(opt_.current_version)});
  const host::migration::UnitResult& u = rep.units.front();
  if (u.outcome == host::migration::Outcome::kMigrated || u.outcome == host::migration::Outcome::kMigratedNotDurable) {
    file_.set_schema_version(opt_.current_version);
    file_version_ = opt_.current_version;
    log("sysd: config file migrated to schema_version " + std::to_string(opt_.current_version));
  }
  return u.outcome;
}

host::migration::Unit migration_unit(std::uint32_t current_version) {
  host::migration::Unit u;
  u.name = "sysd_config";
  u.file = kFileName;
  u.from_version = current_version > 1 ? current_version - 1 : 0;
  u.to_version = current_version;
  u.max_bytes = kMaxFileBytes;
  u.version_of = [](std::string_view bytes, std::uint32_t& version) {
    velora::sysd::v1::SysdConfig f;
    if (!f.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) || f.schema_version() == 0) return false;
    version = f.schema_version();
    return true;
  };
  u.migrate = [current_version](std::string_view in, std::string& out) {
    velora::sysd::v1::SysdConfig f;
    if (!f.ParseFromArray(in.data(), static_cast<int>(in.size()))) return false;
    f.set_schema_version(current_version);  // additive schema: the stamp is the whole change; unknown fields are kept by protobuf
    out = f.SerializeAsString();
    return true;
  };
  return u;
}

}  // namespace velora::sysd

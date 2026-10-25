#include "velora/host/boot_into_store.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velora::host::bootinto {

namespace {

constexpr const char* kMagic = "velora-boot-into";

std::uint32_t crc32(const std::string& s) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (const unsigned char ch : s) c = table[(c ^ ch) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

bool parse_u32(const std::string& s, std::uint32_t& out, int base) {
  if (s.empty() || s.size() > 10) return false;
  std::uint64_t v = 0;
  for (const char ch : s) {
    int d;
    if (ch >= '0' && ch <= '9') d = ch - '0';
    else if (base == 16 && ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
    else return false;
    if (d >= base) return false;
    v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    if (v > 0xFFFFFFFFull) return false;
  }
  out = static_cast<std::uint32_t>(v);
  return true;
}

// Reads "key=" at the start of the next line; returns the value (up to '\n') and advances `pos`.
bool take_line(const std::string& b, std::size_t& pos, std::string& line) {
  const std::size_t nl = b.find('\n', pos);
  if (nl == std::string::npos) return false;
  line = b.substr(pos, nl - pos);
  pos = nl + 1;
  return true;
}
bool take_field(const std::string& b, std::size_t& pos, const char* key, std::string& value) {
  std::string line;
  if (!take_line(b, pos, line)) return false;
  const std::size_t kl = std::strlen(key);
  if (line.size() < kl + 1 || line.compare(0, kl, key) != 0 || line[kl] != '=') return false;
  value = line.substr(kl + 1);
  return true;
}

}  // namespace

bool valid_id(const std::string& id) {
  if (id.size() > kMaxIdBytes) return false;
  for (const char c : id) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool valid_pointer(const Pointer& p) {
  return p.schema_version == kSchemaVersion && valid_id(p.setlist_id) && valid_id(p.live_set_id) && !p.live_set_id.empty();
}

std::string serialize(const Pointer& p) {
  std::string body = std::string(kMagic) + "\nschema_version=" + std::to_string(p.schema_version) + "\nsetlist_id=" + p.setlist_id +
                     "\nlive_set_id=" + p.live_set_id + "\nscene_index=" + std::to_string(p.scene_index) + "\n";
  char crc[16];
  std::snprintf(crc, sizeof crc, "%08x", crc32(body));
  body += "crc32=";
  body += crc;
  body += "\n";
  return body;
}

bool parse(const std::string& bytes, Pointer& out) {
  if (bytes.empty() || bytes.size() > kMaxFileBytes) return false;
  std::size_t pos = 0;
  std::string line, v_schema, v_setlist, v_live, v_scene, v_crc;
  if (!take_line(bytes, pos, line) || line != kMagic) return false;
  if (!take_field(bytes, pos, "schema_version", v_schema)) return false;
  std::uint32_t schema = 0;
  if (!parse_u32(v_schema, schema, 10) || schema != kSchemaVersion) return false;  // unknown schema => no pointer
  if (!take_field(bytes, pos, "setlist_id", v_setlist)) return false;
  if (!take_field(bytes, pos, "live_set_id", v_live)) return false;
  if (!take_field(bytes, pos, "scene_index", v_scene)) return false;
  const std::size_t body_end = pos;
  if (!take_field(bytes, pos, "crc32", v_crc)) return false;
  if (pos != bytes.size()) return false;  // trailing bytes
  std::uint32_t crc = 0, scene = 0;
  if (v_crc.size() != 8 || !parse_u32(v_crc, crc, 16)) return false;
  if (crc != crc32(bytes.substr(0, body_end))) return false;
  if (!parse_u32(v_scene, scene, 10)) return false;
  Pointer p;
  p.setlist_id = v_setlist;
  p.live_set_id = v_live;
  p.scene_index = scene;
  p.schema_version = schema;
  if (!valid_pointer(p)) return false;
  out = std::move(p);
  return true;
}

LoadStatus load(const std::string& dir, Pointer& out) {
  std::string bytes;
  if (liveset::read_regular_file(dir + "/" + kFileName, kMaxFileBytes, bytes) != liveset::Status::kOk) return LoadStatus::kNoPointer;
  Pointer p;
  if (!parse(bytes, p)) return LoadStatus::kNoPointer;
  out = std::move(p);
  return LoadStatus::kOk;
}

BootIntoStore::BootIntoStore(StoreOptions opt) : opt_(std::move(opt)), owner_(std::this_thread::get_id()) {
  Pointer p;
  if (load(opt_.dir, p) == LoadStatus::kOk) {
    stored_ = std::move(p);
    has_stored_ = true;
  }
}

liveset::Status BootIntoStore::write(const Pointer& p, bool* wrote) {
  if (wrote) *wrote = false;
  if (std::this_thread::get_id() != owner_) return liveset::Status::kNotControlThread;
  if (!valid_pointer(p)) return liveset::Status::kInvalid;
  if (has_stored_ && stored_ == p) return liveset::Status::kOk;  // identical: nothing is rewritten
  const std::string bytes = serialize(p);
  if (bytes.size() > kMaxFileBytes) return liveset::Status::kTooLarge;
  ++writes_;
  const liveset::Status s = liveset::atomic_replace_file(opt_.dir, kFileName, kTmpFileName, bytes, opt_.on_stage, opt_.inject_dir_sync_failure, opt_.inject_write_errno);
  if (s != liveset::Status::kOk) {
    has_stored_ = false;  // the new file may be in place (renamed, directory fsync failed): never trust the cached copy for the identical-skip again
    return s;
  }
  stored_ = p;
  has_stored_ = true;
  if (wrote) *wrote = true;
  return liveset::Status::kOk;
}

BootIntoTracker::BootIntoTracker(BootIntoStore& store, TrackerProbes probes) : store_(store), probes_(std::move(probes)) {
  quiet_since_ns_ = probes_.now_ns ? probes_.now_ns() : 0;
}

void BootIntoTracker::mark_dirty(const Pointer& p) {
  if (!valid_pointer(p)) return;
  if (dirty_ && pending_ == p) return;
  pending_ = p;
  if (!dirty_) change_ns_ = probes_.now_ns ? probes_.now_ns() : 0;  // the 30 s clock runs from the first change; more changes do not restart it
  dirty_ = true;
}

void BootIntoTracker::observe(const Pointer& p) {
  if (!valid_pointer(p)) return;
  if (have_observed_ && observed_ == p) return;
  observed_ = p;
  have_observed_ = true;
  mark_dirty(p);
}

namespace {
std::uint64_t elapsed(std::uint64_t now, std::uint64_t since) { return now > since ? now - since : 0; }  // a clock that steps back counts as no time
}

bool BootIntoTracker::update(const Position& pos) {
  if (!bound_) {
    store_.rebind_control_thread();  // the first tick runs on the control thread
    bound_ = true;
  }
  if (!pos.live_set_id.empty() && !pos.loading && pos.scene >= 0) {
    Pointer p;
    p.live_set_id = pos.live_set_id;
    p.scene_index = static_cast<std::uint32_t>(pos.scene);
    p.setlist_id = pos.setlist_id;
    observe(p);
  }
  return tick();
}

bool BootIntoTracker::tick() {
  const std::uint64_t now = probes_.now_ns ? probes_.now_ns() : 0;
  const std::uint32_t notes = probes_.held_notes ? probes_.held_notes() : 0;
  const bool pedal = probes_.pedal_down ? probes_.pedal_down() : false;
  if (notes > 0 || pedal) quiet_since_ns_ = now;  // activity seen: the idle clock restarts
  if (!dirty_) return false;
  if (probes_.import_verifying && probes_.import_verifying()) return false;
  if (notes > 0) return false;  // never while a key is held, whatever the 30 s rule says
  const bool idle = !pedal && elapsed(now, quiet_since_ns_) >= kIdleNs;
  const bool waited = elapsed(now, change_ns_) >= kMaxWaitNs;
  if (!idle && !waited) return false;
  if (retry_at_ns_ != 0 && now < retry_at_ns_ && retry_at_ns_ - now <= kRetryNs) return false;  // backing off after a failure
  bool wrote = false;
  const liveset::Status s = store_.write(pending_, &wrote);
  if (s == liveset::Status::kOk) {
    dirty_ = false;  // written, or identical to the stored pointer
    retry_at_ns_ = 0;
    return wrote;
  }
  retry_at_ns_ = now + kRetryNs;  // stays dirty, retried after the backoff
  return false;
}

}  // namespace velora::host::bootinto

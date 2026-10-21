#include "service.hpp"

#include <algorithm>
#include <chrono>
#include <set>

#include "velora/format/json_lite.hpp"

namespace velora::registry {
namespace fmt = velora::format;
namespace {

std::int64_t steady_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The route template of a path, never the path itself (a refused call's path is attacker-controlled text).
std::string route_label(const std::string& path) {
  if (path == "/v1/devices") return path;
  const std::string prefix = "/v1/devices/";
  if (path.rfind(prefix, 0) != 0) return "(unknown)";
  const std::size_t slash = path.find('/', prefix.size());
  if (slash == std::string::npos) return prefix + "{id}";
  const std::string rest = path.substr(slash);
  if (rest == "/revoke" || rest == "/wrapped-keys") return prefix + "{id}" + rest;
  return "(unknown)";
}

std::string device_json(const Device& d) {
  std::string j = "{";
  j += "\"device_id\":" + json_str(d.device_id);
  j += ",\"device_public_key\":" + json_str(hex_encode(d.public_key.data(), 32));
  j += ",\"som_id\":" + json_str(d.som_id);
  j += ",\"serial_number\":" + json_str(d.serial_number);
  j += ",\"tier\":" + json_str(d.tier);
  j += ",\"provisioning_date\":" + json_str(d.provisioning_date);
  j += ",\"station_id\":" + json_str(d.station_id);
  j += ",\"status\":" + json_str(d.status);
  j += ",\"replaces\":" + (d.replaces_row ? json_str(d.replaces_device_id) : std::string("null"));
  j += ",\"registered_at\":" + json_str(rfc3339(d.registered_at));
  j += ",\"revoked_at\":" + (d.revoked_at ? json_str(rfc3339(d.revoked_at)) : std::string("null"));
  j += ",\"revoked_by\":" + (d.revoked_at ? json_str(d.revoked_by) : std::string("null"));
  j += ",\"revoke_reason\":" + (d.revoked_at ? json_str(d.revoke_reason) : std::string("null"));
  j += "}";
  return j;
}

HttpResponse ok_json(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t p = 0;
  while (true) {
    const std::size_t e = s.find(sep, p);
    out.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  return out;
}

// Parses a JSON object body with a fixed set of allowed members; duplicate and unknown members are refused.
bool parse_object(const std::string& body, const std::set<std::string>& allowed, fmt::JsonValue& v, std::string& why) {
  std::string e;
  if (!fmt::parse_json(body, v, &e)) {
    why = "the body is not valid JSON";
    return false;
  }
  if (!v.is_object()) {
    why = "the body must be a JSON object";
    return false;
  }
  std::set<std::string> seen;
  for (const std::string& k : v.keys) {
    if (!allowed.count(k)) {
      why = "unknown member";
      return false;
    }
    if (!seen.insert(k).second) {
      why = "duplicate member";
      return false;
    }
  }
  return true;
}

}  // namespace

std::string Principal::actor() const {
  switch (role) {
    case Role::kStation: return "station:" + id;
    case Role::kAdmin: return "admin:" + id;
    case Role::kNone: break;
  }
  return "anonymous:" + (remote.empty() ? std::string("-") : remote);
}

RateLimiter::RateLimiter(int max, int window_seconds, std::function<std::int64_t()> clock_s)
    : max_(max), window_(window_seconds), clock_(clock_s ? std::move(clock_s) : std::function<std::int64_t()>(steady_seconds)) {}

int RateLimiter::check(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::int64_t now = clock_();
  if (slots_.size() > 4096) {  // bounded memory: drop expired windows
    for (auto it = slots_.begin(); it != slots_.end();) it = (now - it->second.window_start >= window_) ? slots_.erase(it) : std::next(it);
    if (slots_.size() > 8192 && !slots_.count(key)) return 2;  // flood of distinct keys: refuse the new ones
  }
  Slot& s = slots_[key];
  if (now - s.window_start >= window_) {
    s.window_start = now;
    s.count = 0;
  }
  ++s.count;
  if (s.count <= max_) return 0;
  return s.count == max_ + 1 ? 1 : 2;
}

Service::Service(Database& db, KeyVault& vault, RateLimitConfig rl, LogFn log)
    : db_(db), vault_(vault), limiter_(rl.max_requests, rl.window_seconds), register_limiter_(std::make_unique<RateLimiter>(600, 3600)), log_(std::move(log)) {}

void Service::set_station_libraries(std::map<std::string, std::set<std::string>> allow) {
  restrict_libraries_ = true;
  station_libraries_ = std::move(allow);
}
void Service::set_registration_cap(int per_hour) { register_limiter_ = std::make_unique<RateLimiter>(per_hour, 3600); }

bool load_station_libraries(const std::string& path, std::map<std::string, std::set<std::string>>& out, std::string* err) {
  std::string text;
  if (!fmt::read_text_file(path, text, err)) return false;
  std::size_t pos = 0;
  int line_no = 0;
  while (pos <= text.size()) {
    std::size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    ++line_no;
    if (const std::size_t h = line.find('#'); h != std::string::npos) line.resize(h);
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
      std::size_t j = i;
      while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
      if (j > i) words.push_back(line.substr(i, j - i));
      i = j;
    }
    if (words.empty()) continue;
    auto bad = [&](const std::string& m) {
      if (err) *err = path + ":" + std::to_string(line_no) + ": " + m;
      return false;
    };
    if (!valid_token(words[0], 64)) return bad("bad station id");
    if (words.size() < 2) return bad("a station needs at least one library id or *");
    if (out.count(words[0])) return bad("station " + words[0] + " is listed twice");
    std::set<std::string>& allowed = out[words[0]];
    for (std::size_t k = 1; k < words.size(); ++k) {
      if (words[k] != "*" && !valid_library_id(words[k])) return bad("not a canonical library id: " + words[k]);
      allowed.insert(words[k]);
    }
  }
  return true;
}

void Service::audit_refusal_throttled(const std::string& actor, std::string_view action, std::string_view code, std::string_view detail) {
  const std::int64_t now = steady_seconds();
  int summarize = 0;
  bool audit_this = false;
  {
    std::lock_guard<std::mutex> lk(suppress_mu_);
    if (suppress_.size() > 4096)
      for (auto it = suppress_.begin(); it != suppress_.end();) it = (now - it->second.window_start >= 60 && it->second.count == 0) ? suppress_.erase(it) : std::next(it);
    Suppress& s = suppress_[actor + "|" + std::string(code)];
    if (s.window_start == 0 || now - s.window_start >= 60) {
      summarize = s.count;
      s.window_start = now;
      s.count = 0;
      audit_this = true;
    } else {
      ++s.count;
    }
  }
  if (summarize > 0) audit_refusal(actor, "refusals_suppressed", code, "count=" + std::to_string(summarize) + " further refusals in the previous 60 s window");
  if (audit_this) audit_refusal(actor, action, code, detail);
  else if (log_) log_("refusal " + std::string(code) + " from " + actor + " counted, not audited (first of the window already is)");
}

HttpResponse Service::error_response(int status, std::string_view code, std::string_view message, std::string_view extra) {
  HttpResponse r;
  r.status = status;
  r.body = "{\"error\":{\"code\":" + json_str(code) + ",\"message\":" + json_str(message) + (extra.empty() ? "" : "," + std::string(extra)) + "}}";
  return r;
}

void Service::audit_refusal(const std::string& actor, std::string_view action, std::string_view code, std::string_view detail) {
  AuditEvent e;
  e.actor = actor;
  e.action = std::string(action);
  e.outcome = "refused";
  e.code = std::string(code);
  e.detail = std::string(detail);
  if (!db_.audit(e) && log_) log_("error: could not write the audit row of a refusal (" + std::string(code) + ")");
}

HttpResponse Service::refuse(const Principal& who, std::string_view action, const std::string& device_id, const std::string& library_id, HttpResponse r) {
  AuditEvent e;
  e.actor = who.actor();
  e.action = std::string(action);
  e.device_id = device_id;
  e.library_id = library_id;
  e.outcome = r.status >= 500 ? "error" : "refused";
  // The typed code is the member after "code":" in our own error body.
  const std::size_t p = r.body.find("\"code\":\"");
  if (p != std::string::npos) e.code = r.body.substr(p + 8, r.body.find('"', p + 8) - (p + 8));
  e.detail = "http " + std::to_string(r.status);
  if (!db_.audit(e)) {
    if (log_) log_("error: could not write the audit row of a refusal");
    return error_response(500, "audit_failed", "the audit log could not be written");
  }
  return r;
}

HttpResponse Service::handle(const HttpRequest& req, const Principal& who) {
  if (who.role == Role::kNone) {
    // Defence in depth: the TLS layer already refuses anonymous clients.
    return refuse(who, "request", "", "", error_response(401, "unauthenticated", "a client certificate issued under the factory root is required"));
  }
  if (const int rl = limiter_.check(who.actor()); rl != 0) {
    HttpResponse r = error_response(429, "rate_limited", "too many requests; retry later");
    r.headers.emplace_back("Retry-After", std::to_string(limiter_.window_seconds()));
    // The first refusal of a window is audited; the rest only counted in the log, so a flood can not fill the audit log.
    if (rl == 1) return refuse(who, "request", "", "", std::move(r));
    if (log_) log_("rate limited " + who.actor());
    return r;
  }
  std::string device, library;
  HttpResponse r = route(req, who, device, library);
  if (r.status >= 400) return refuse(who, req.method + " " + route_label(req.path), device, library, std::move(r));
  return r;
}

HttpResponse Service::route(const HttpRequest& req, const Principal& who, std::string& audit_device, std::string& audit_library) {
  // /v1/devices
  // /v1/devices/{id}
  // /v1/devices/{id}/revoke
  // /v1/devices/{id}/wrapped-keys
  const std::string& p = req.path;
  if (p == "/v1/devices") {
    if (req.method != "POST") return error_response(405, "method_not_allowed", "use POST to register a device");
    if (who.role != Role::kStation) return error_response(403, "forbidden", "only a station may register devices");
    return register_device(req, who);
  }
  const std::string prefix = "/v1/devices/";
  if (p.rfind(prefix, 0) != 0) return error_response(404, "not_found", "no such endpoint");
  const std::vector<std::string> parts = split(p.substr(prefix.size()), '/');
  if (parts.empty() || parts.size() > 2 || parts[0].empty()) return error_response(404, "not_found", "no such endpoint");
  const std::string& id = parts[0];
  if (!valid_id_token(id, 64)) return error_response(400, "invalid_device_id", "the device id in the path is malformed");
  audit_device = id;
  if (parts.size() == 1) {
    if (req.method != "GET") return error_response(405, "method_not_allowed", "use GET to read a device");
    return get_device(id);
  }
  if (parts[1] == "revoke") {
    if (req.method != "POST") return error_response(405, "method_not_allowed", "use POST to revoke a device");
    if (who.role != Role::kAdmin) return error_response(403, "forbidden", "only an administrator may revoke devices");
    return revoke_device(id, req, who);
  }
  if (parts[1] == "wrapped-keys") {
    if (who.role != Role::kStation) return error_response(403, "forbidden", "only a station may request wrapped keys");
    std::vector<std::string> libs;
    if (req.method == "GET") {
      // ?library_id=<uuid>[&library_id=<uuid>...] or library_id=<uuid>,<uuid>
      for (const std::string& kv : split(req.query, '&')) {
        if (kv.rfind("library_id=", 0) != 0) {
          if (kv.empty()) continue;
          return error_response(400, "invalid_request", "unknown query parameter");
        }
        for (const std::string& l : split(kv.substr(11), ',')) libs.push_back(l);
      }
    } else if (req.method == "POST") {
      fmt::JsonValue v;
      std::string why;
      if (!parse_object(req.body, {"library_ids"}, v, why)) return error_response(400, "invalid_request", why);
      const fmt::JsonValue* a = v.find("library_ids");
      if (!a || !a->is_array()) return error_response(400, "invalid_request", "library_ids must be an array of library id strings");
      for (const fmt::JsonValue& e : a->array) {
        if (!e.is_string()) return error_response(400, "invalid_request", "library_ids must be an array of library id strings");
        libs.push_back(e.string);
      }
    } else {
      return error_response(405, "method_not_allowed", "use GET or POST to request wrapped keys");
    }
    if (libs.empty()) return error_response(400, "invalid_request", "at least one library id is required");
    if (libs.size() > kMaxLibrariesPerRequest) return error_response(400, "too_many_libraries", "at most 64 libraries per request");
    for (const std::string& l : libs)
      if (!valid_library_id(l)) return error_response(400, "invalid_library_id", "a library id must be a canonical lower-case UUID");
    {
      std::set<std::string> uniq(libs.begin(), libs.end());
      if (uniq.size() != libs.size()) return error_response(400, "invalid_request", "duplicate library id");
    }
    audit_library = libs.size() == 1 ? libs[0] : std::string();
    return wrapped_keys(id, libs, who);
  }
  return error_response(404, "not_found", "no such endpoint");
}

HttpResponse Service::get_device(const std::string& id) {
  const std::optional<Device> d = db_.find_device(id);
  if (!d) return error_response(404, "unknown_device", "no such device", "\"device_id\":" + json_str(id));
  return ok_json(200, device_json(*d));
}

HttpResponse Service::register_device(const HttpRequest& req, const Principal& who) {
  fmt::JsonValue v;
  std::string why;
  if (!parse_object(req.body, {"device_id", "device_public_key", "som_id", "serial_number", "tier", "provisioning_date", "replaces"}, v, why))
    return error_response(400, "invalid_request", why);
  auto str = [&](const char* k, std::string& out) {
    const fmt::JsonValue* m = v.find(k);
    if (!m || !m->is_string()) return false;
    out = m->string;
    return true;
  };
  NewDevice d;
  std::string pub;
  auto bad = [&](const char* field, const char* msg) { return error_response(400, "invalid_field", msg, "\"field\":" + json_str(field)); };
  if (!str("device_id", d.device_id) || !valid_id_token(d.device_id, 64)) return bad("device_id", "device_id is required: 1-64 characters of a-z 0-9 . _ : -, starting with a letter or digit");
  if (!str("device_public_key", pub) || !hex_decode32(pub, d.public_key)) return bad("device_public_key", "device_public_key is required: 64 hex digits (X25519)");
  {
    std::string why;
    if (!acceptable_x25519_public(d.public_key, &why)) return bad("device_public_key", "not a usable X25519 public key (bit 255 set, non-canonical or small-order)");
  }
  if (!str("som_id", d.som_id) || !valid_token(d.som_id, 64)) return bad("som_id", "som_id is required: 1-64 characters of A-Z a-z 0-9 . _ : / -");
  if (!str("serial_number", d.serial_number) || !valid_token(d.serial_number, 64)) return bad("serial_number", "serial_number is required: 1-64 characters of A-Z a-z 0-9 . _ : / -");
  if (!str("tier", d.tier) || !valid_token(d.tier, 32)) return bad("tier", "tier is required: 1-32 characters of A-Z a-z 0-9 . _ : / -");
  if (!str("provisioning_date", d.provisioning_date) || !valid_date(d.provisioning_date)) return bad("provisioning_date", "provisioning_date is required: YYYY-MM-DD");
  if (v.find("replaces")) {
    if (!str("replaces", d.replaces) || !valid_id_token(d.replaces, 64)) return bad("replaces", "replaces must be the device_id of a revoked device");
  }
  if (register_limiter_->check(who.actor()) != 0) return error_response(429, "registration_rate_limited", "too many registrations from this station; retry later");
  d.station_id = who.id;  // from the certificate, never from the body
  Device out;
  switch (db_.register_device(d, who.actor(), out)) {
    case DbStatus::kOk: return ok_json(201, device_json(out));
    case DbStatus::kDuplicateDeviceId: return error_response(409, "duplicate_device_id", "an active device with this device_id is already registered");
    case DbStatus::kDuplicateSerial: return error_response(409, "duplicate_serial", "an active device with this serial number is already registered");
    case DbStatus::kDuplicatePublicKey: return error_response(409, "duplicate_public_key", "this device public key is already registered");
    case DbStatus::kInvalidReplaces:
      return error_response(409, "invalid_replaces", "replaces must name a revoked device that has not been superseded yet and has the same serial number or SoM id");
    case DbStatus::kAuditFailed: return error_response(500, "audit_failed", "the audit log could not be written; nothing was registered");
    default: break;
  }
  return error_response(500, "internal_error", "the device could not be registered");
}

HttpResponse Service::revoke_device(const std::string& id, const HttpRequest& req, const Principal& who) {
  std::string reason;
  if (!req.body.empty()) {
    fmt::JsonValue v;
    std::string why;
    if (!parse_object(req.body, {"reason"}, v, why)) return error_response(400, "invalid_request", why);
    if (const fmt::JsonValue* r = v.find("reason")) {
      if (!r->is_string() || r->string.size() > 200) return error_response(400, "invalid_field", "reason must be a string of at most 200 characters", "\"field\":\"reason\"");
      for (const char c : r->string)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return error_response(400, "invalid_field", "reason must not contain control characters", "\"field\":\"reason\"");
      reason = r->string;
    }
  }
  Device out;
  switch (db_.revoke_device(id, who.actor(), reason, out)) {
    case DbStatus::kOk: return ok_json(200, device_json(out));
    case DbStatus::kNotFound: return error_response(404, "unknown_device", "no such device", "\"device_id\":" + json_str(id));
    case DbStatus::kAlreadyRevoked: return error_response(409, "already_revoked", "the device is already revoked");
    case DbStatus::kAuditFailed: return error_response(500, "audit_failed", "the audit log could not be written; nothing was revoked");
    default: break;
  }
  return error_response(500, "internal_error", "the device could not be revoked");
}

HttpResponse Service::wrapped_keys(const std::string& id, const std::vector<std::string>& libs, const Principal& who) {
  const std::optional<Device> d = db_.find_device(id);
  if (!d) return error_response(404, "unknown_device", "no such device", "\"device_id\":" + json_str(id));
  if (d->status != "active") return error_response(403, "device_revoked", "the device is revoked and receives no keys", "\"device_id\":" + json_str(id));
  if (restrict_libraries_) {
    const auto it = station_libraries_.find(who.id);
    for (const std::string& l : libs)
      if (it == station_libraries_.end() || (!it->second.count("*") && !it->second.count(l)))
        return error_response(403, "library_not_allowed", "this station may not request this library", "\"library_id\":" + json_str(l));
  }
  // All or nothing: every library is wrapped before anything is returned or audited as issued.
  std::vector<WrappedBlob> blobs(libs.size());
  for (std::size_t i = 0; i < libs.size(); ++i) {
    switch (vault_.wrap(libs[i], d->public_key, blobs[i])) {
      case WrapStatus::kOk: break;
      case WrapStatus::kUnknownLibrary: return error_response(404, "unknown_library", "no such library in the vault", "\"library_id\":" + json_str(libs[i]));
      case WrapStatus::kVaultUnreadable: return error_response(500, "vault_unavailable", "the vault entry could not be read", "\"library_id\":" + json_str(libs[i]));
      case WrapStatus::kVaultEntryInvalid: return error_response(500, "vault_entry_invalid", "the vault entry does not open with the configured KEK", "\"library_id\":" + json_str(libs[i]));
      case WrapStatus::kWrapFailed: return error_response(500, "key_wrap_failed", "the key could not be wrapped for this device", "\"library_id\":" + json_str(libs[i]));
    }
  }
  if (before_issue_hook) before_issue_hook();
  // Audit first (device, library, station, time; never the key), in a transaction that re-checks the device is STILL active: a revoke that landed after the lookup above
  // wins, and then no key leaves. If the audit rows can not be written no key leaves either.
  std::vector<AuditEvent> ev;
  for (const std::string& l : libs) {
    AuditEvent e;
    e.actor = who.actor();
    e.action = "wrapped_key_issued";
    e.device_id = id;
    e.library_id = l;
    e.outcome = "ok";
    ev.push_back(std::move(e));
  }
  switch (db_.audit_issuance(d->row_id, ev)) {
    case DbStatus::kOk: break;
    case DbStatus::kNotActive: return error_response(403, "device_revoked", "the device is revoked and receives no keys", "\"device_id\":" + json_str(id));
    default: return error_response(500, "audit_failed", "the audit log could not be written; no key was issued");
  }
  std::string body = "{\"device_id\":" + json_str(id) + ",\"keys\":[";
  for (std::size_t i = 0; i < libs.size(); ++i) {
    if (i) body += ",";
    body += "{\"library_id\":" + json_str(libs[i]) + ",\"wrapped_key\":" + json_str(hex_encode(blobs[i].data(), blobs[i].size())) + "}";
  }
  body += "]}";
  return ok_json(200, std::move(body));
}

}  // namespace velora::registry

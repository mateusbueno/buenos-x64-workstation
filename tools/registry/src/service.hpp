#pragma once
// Story 6.8: the registry API: routing, authorisation by role, validation, typed JSON errors, audit of every write and every refusal, rate limiting.
// Transport-independent (the TLS server builds the Principal from the client certificate), so tests and the fuzz run call handle() directly.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "database.hpp"
#include "http.hpp"
#include "vault.hpp"

namespace velora::registry {

enum class Role { kNone, kStation, kAdmin };

struct Principal {
  Role role = Role::kNone;
  std::string id;      // certificate CN: the station / admin id (token charset), empty when unauthenticated
  std::string remote;  // peer IP text
  std::string actor() const;
};

using LogFn = std::function<void(const std::string&)>;

struct RateLimitConfig {
  int max_requests = 120;   // per principal per window
  int window_seconds = 60;
};

// Fixed-window counter per key. Thread safe. `clock_s` is injectable for tests.
class RateLimiter {
 public:
  RateLimiter(int max, int window_seconds, std::function<std::int64_t()> clock_s = nullptr);
  // 0 = allowed, 1 = refused (the first refusal of this window), 2 = refused again.
  int check(const std::string& key);
  int window_seconds() const { return window_; }

 private:
  int max_, window_;
  std::function<std::int64_t()> clock_;
  std::mutex mu_;
  struct Slot { std::int64_t window_start = 0; int count = 0; };
  std::map<std::string, Slot> slots_;
};

// `--station-libraries FILE`: one line per station, `<station CN> <library_id>... | *`, `#` comments. With a file configured a station not listed receives no libraries;
// without one every station may request every library. false + `err` on a malformed file.
bool load_station_libraries(const std::string& path, std::map<std::string, std::set<std::string>>& out, std::string* err);

class Service {
 public:
  Service(Database& db, KeyVault& vault, RateLimitConfig rl, LogFn log);
  HttpResponse handle(const HttpRequest& req, const Principal& who);
  // A call that never reached handle(): malformed / oversized request, TLS handshake failure, saturated queue. Audited as a refusal.
  void audit_refusal(const std::string& actor, std::string_view action, std::string_view code, std::string_view detail);
  // Same, for the paths before the router (malformed / oversized / incomplete requests, wrong-role certificates), where the caller is cheap to repeat: the FIRST refusal
  // per (actor, code) per 60 s window is audited, later ones only counted; when the next window opens one `refusals_suppressed` row records how many were counted.
  void audit_refusal_throttled(const std::string& actor, std::string_view action, std::string_view code, std::string_view detail);
  void set_station_libraries(std::map<std::string, std::set<std::string>> allow);  // enables the per-station library allow-list
  void set_registration_cap(int per_hour);                                         // registrations per station per hour (default 600)
  std::function<void()> before_issue_hook;                                          // test hook: runs after the vault wrap, before the issuance is recorded
  static HttpResponse error_response(int status, std::string_view code, std::string_view message, std::string_view extra_json_members = "");
  static constexpr std::size_t kMaxLibrariesPerRequest = 64;

 private:
  HttpResponse route(const HttpRequest& req, const Principal& who, std::string& audit_device, std::string& audit_library);
  HttpResponse register_device(const HttpRequest& req, const Principal& who);
  HttpResponse revoke_device(const std::string& device_id, const HttpRequest& req, const Principal& who);
  HttpResponse get_device(const std::string& device_id);
  HttpResponse wrapped_keys(const std::string& device_id, const std::vector<std::string>& libs, const Principal& who);
  HttpResponse refuse(const Principal& who, std::string_view action, const std::string& device_id, const std::string& library_id, HttpResponse r);

  Database& db_;
  KeyVault& vault_;
  RateLimiter limiter_;
  std::unique_ptr<RateLimiter> register_limiter_;
  bool restrict_libraries_ = false;
  std::map<std::string, std::set<std::string>> station_libraries_;
  struct Suppress { std::int64_t window_start = 0; int count = 0; };
  std::mutex suppress_mu_;
  std::map<std::string, Suppress> suppress_;
  LogFn log_;
};

}  // namespace velora::registry

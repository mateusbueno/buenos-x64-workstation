#pragma once
// Story 6.8: the registry's TLS 1.3 mutual-TLS HTTP server. Clients must present a certificate that chains to the configured client CA (the factory-test root);
// the role comes from the certificate subject (OU "velora-station" or "velora-admin", CN = the station / admin id). A fixed pool of worker threads serves one request per
// connection with socket timeouts and an overall request deadline; a full queue refuses new connections.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "http.hpp"
#include "velora/format/json_lite.hpp"
#include "service.hpp"

typedef struct ssl_ctx_st SSL_CTX;

namespace velora::registry {

inline constexpr const char* kStationOu = "velora-station";
inline constexpr const char* kAdminOu = "velora-admin";

struct ServerConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;           // 0 = an ephemeral port (read it back with Server::port())
  std::string cert_file;            // server certificate chain (PEM)
  std::string key_file;             // server private key (PEM, mode 0600)
  std::string client_ca_file;       // PEM bundle: the root(s) station and admin certificates must chain to
  unsigned workers = 8;
  unsigned max_pending = 64;        // accepted connections waiting for a worker; more are closed at once
  int io_timeout_ms = 5000;         // per socket read / write / handshake step
  int request_deadline_ms = 10000;  // whole request from the end of the handshake to the last byte received
  HttpLimits http;
  int handshake_failure_audit_limit = 30;  // audited handshake failures per peer IP per minute (later ones are counted; the first suppressed one is logged)
  unsigned max_conn_per_ip = 16;    // concurrent connections (queued or served) per peer IP; more are closed at once
  std::string revoked_certs_file;   // optional deny-list: one certificate serial number (hex) per line, `#` comments; re-read when its mtime changes
  std::string expect_ca_sha256;     // optional: hex SHA-256 of the DER of the client CA; start() fails unless EVERY certificate of client_ca_file has it
};

class Server {
 public:
  Server(ServerConfig cfg, Service& service, LogFn log);
  ~Server();
  bool start(std::string* err);
  void stop();
  std::uint16_t port() const { return port_; }

 private:
  void accept_loop();
  bool is_revoked(const std::string& serial_hex);
  bool load_revoked(std::string* err);
  void worker_loop();
  void serve_connection(int fd, const std::string& peer, std::chrono::steady_clock::time_point accepted);

  ServerConfig cfg_;
  Service& service_;
  LogFn log_;
  SSL_CTX* ctx_ = nullptr;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cv_;
  struct Pending { int fd; std::string peer; std::chrono::steady_clock::time_point accepted; };
  std::deque<Pending> queue_;
  RateLimiter handshake_audit_limiter_;
  std::map<std::string, unsigned> ip_count_;
  std::mutex rev_mu_;
  std::set<std::string> revoked_;
  long long rev_mtime_ = -1;
};

}  // namespace velora::registry

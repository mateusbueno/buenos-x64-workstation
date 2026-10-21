#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <functional>
#include <chrono>
#include <cstring>
#include <memory>

namespace velora::registry {
namespace {

using Clock = std::chrono::steady_clock;

struct SslDel {
  void operator()(SSL* s) const { SSL_free(s); }
};

std::string ssl_error_text() {
  const unsigned long e = ERR_get_error();
  ERR_clear_error();
  if (!e) return "no detail";
  char buf[160];
  ERR_error_string_n(e, buf, sizeof buf);
  return buf;
}

std::string subject_part(X509* cert, int nid) {
  X509_NAME* n = X509_get_subject_name(cert);
  char buf[130];
  const int len = X509_NAME_get_text_by_NID(n, nid, buf, sizeof buf);
  return len > 0 ? std::string(buf, static_cast<std::size_t>(len)) : std::string();
}

struct ConnCtx {
  std::string claimed_cn;
  std::function<bool(const std::string&)> revoked;
};

std::string serial_hex(X509* c) {
  ASN1_INTEGER* si = X509_get_serialNumber(c);
  BIGNUM* bn = ASN1_INTEGER_to_BN(si, nullptr);
  if (!bn) return "";
  char* hx = BN_bn2hex(bn);
  std::string out = hx ? hx : "";
  OPENSSL_free(hx);
  BN_free(bn);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const std::size_t nz = out.find_first_not_of('0');
  return nz == std::string::npos ? "0" : out.substr(nz);
}

// Verify callback: remembers the subject CN of the presented (still unauthenticated) leaf so a refused handshake can be audited with the name the client claimed (OpenSSL
// frees the peer certificate when verification fails, so this is the only place to read it), and fails the handshake for a certificate on the deny-list.
int g_claim_index = -1;
int verify_cb(int ok, X509_STORE_CTX* sctx) {
  if (X509_STORE_CTX_get_error_depth(sctx) == 0) {
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(sctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    X509* cur = X509_STORE_CTX_get_current_cert(sctx);
    auto* cc = ssl ? static_cast<ConnCtx*>(SSL_get_ex_data(ssl, g_claim_index)) : nullptr;
    if (cc && cur) {
      cc->claimed_cn = subject_part(cur, NID_commonName);
      if (ok && cc->revoked && cc->revoked(serial_hex(cur))) {
        X509_STORE_CTX_set_error(sctx, X509_V_ERR_CERT_REVOKED);
        return 0;
      }
    }
  }
  return ok;
}

struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

// One SSL operation driven to completion on a non-blocking socket against an ABSOLUTE deadline: a client that dribbles bytes, or stalls a handshake, can not hold a
// worker past it. `io_ms` also bounds a single wait with no progress.
struct Io {
  SSL* ssl;
  int fd;
  Clock::time_point deadline;
  int io_ms;
  bool timed_out = false;
  int err = SSL_ERROR_NONE;
};

template <class F>
int io_run(Io& io, F op) {
  for (;;) {
    ERR_clear_error();
    const int r = op();
    if (r > 0) return r;
    io.err = SSL_get_error(io.ssl, r);
    short ev;
    if (io.err == SSL_ERROR_WANT_READ) ev = POLLIN;
    else if (io.err == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
    else return r;
    const auto now = Clock::now();
    if (now >= io.deadline) {
      io.timed_out = true;
      return -1;
    }
    const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(io.deadline - now).count()) + 1;
    const int wait = std::min(remaining, io.io_ms);
    pollfd p{io.fd, ev, 0};
    const int pr = ::poll(&p, 1, wait);
    if (pr < 0 && errno != EINTR) return -1;
    if (pr == 0 && (wait == io.io_ms || Clock::now() >= io.deadline)) {
      io.timed_out = true;
      return -1;
    }
  }
}

long long mtime_ns(const struct stat& st) {
#ifdef __APPLE__
  return static_cast<long long>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  return static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

std::string norm_serial(std::string s) {
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const std::size_t nz = s.find_first_not_of('0');
  return nz == std::string::npos ? "0" : s.substr(nz);
}

}  // namespace

bool Server::load_revoked(std::string* err) {
  struct stat st;
  if (::stat(cfg_.revoked_certs_file.c_str(), &st) != 0) {
    if (err) *err = cfg_.revoked_certs_file + ": " + std::strerror(errno);
    return false;
  }
  std::string text;
  if (!velora::format::read_text_file(cfg_.revoked_certs_file, text, err)) return false;
  std::set<std::string> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    if (const std::size_t h = line.find('#'); h != std::string::npos) line.resize(h);
    std::string tok;
    for (char c : line)
      if (!std::isspace(static_cast<unsigned char>(c))) tok += c;
    if (tok.empty()) continue;
    if (tok.find_first_not_of("0123456789abcdefABCDEFxX") != std::string::npos) {
      if (err) *err = cfg_.revoked_certs_file + ": not a hex serial number: " + tok;
      return false;
    }
    out.insert(norm_serial(tok));
  }
  std::lock_guard<std::mutex> lk(rev_mu_);
  revoked_ = std::move(out);
  rev_mtime_ = mtime_ns(st);
  return true;
}

bool Server::is_revoked(const std::string& serial) {
  if (cfg_.revoked_certs_file.empty()) return false;
  struct stat st;
  if (::stat(cfg_.revoked_certs_file.c_str(), &st) == 0) {
    const long long mt = mtime_ns(st);
    bool changed;
    {
      std::lock_guard<std::mutex> lk(rev_mu_);
      changed = mt != rev_mtime_;
    }
    std::string err;
    if (changed && !load_revoked(&err) && log_) log_("revoked-certs file not reloaded (keeping the previous list): " + err);
  }
  std::lock_guard<std::mutex> lk(rev_mu_);
  return revoked_.count(serial) != 0;
}

Server::Server(ServerConfig cfg, Service& service, LogFn log)
    : cfg_(std::move(cfg)), service_(service), log_(std::move(log)), handshake_audit_limiter_(cfg_.handshake_failure_audit_limit, 60) {}

Server::~Server() { stop(); }

bool Server::start(std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = m;
    if (ctx_) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    return false;
  };
  ::signal(SIGPIPE, SIG_IGN);
  std::string why;
  if (!check_private_mode(cfg_.key_file, &why)) return bad("server key: " + why);
  if (!cfg_.revoked_certs_file.empty() && !load_revoked(&why)) return bad("revoked certificates: " + why);
  // The CA the clients must chain to can be pinned: every certificate in the bundle must be the expected one.
  {
    FILE* f = std::fopen(cfg_.client_ca_file.c_str(), "r");
    if (!f) return bad("client CA " + cfg_.client_ca_file + ": " + std::strerror(errno));
    int n = 0;
    bool all_match = true;
    while (X509* c = PEM_read_X509(f, nullptr, nullptr, nullptr)) {
      unsigned char md[EVP_MAX_MD_SIZE];
      unsigned int mdl = 0;
      X509_digest(c, EVP_sha256(), md, &mdl);
      if (!cfg_.expect_ca_sha256.empty() && hex_encode(md, mdl) != cfg_.expect_ca_sha256) all_match = false;
      X509_free(c);
      ++n;
    }
    std::fclose(f);
    ERR_clear_error();
    if (n == 0) return bad("client CA " + cfg_.client_ca_file + " holds no certificate");
    if (!cfg_.expect_ca_sha256.empty() && !all_match)
      return bad("client CA " + cfg_.client_ca_file + " does not match --expect-ca-sha256: every certificate in it must have the expected SHA-256 fingerprint");
  }
  ctx_ = SSL_CTX_new(TLS_server_method());
  if (!ctx_) return bad("cannot create the TLS context");
  SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
  SSL_CTX_set_options(ctx_, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
  SSL_CTX_set_num_tickets(ctx_, 0);
  SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_OFF);
  if (SSL_CTX_use_certificate_chain_file(ctx_, cfg_.cert_file.c_str()) != 1) return bad("server certificate " + cfg_.cert_file + ": " + ssl_error_text());
  if (SSL_CTX_use_PrivateKey_file(ctx_, cfg_.key_file.c_str(), SSL_FILETYPE_PEM) != 1) return bad("server key " + cfg_.key_file + ": " + ssl_error_text());
  if (SSL_CTX_check_private_key(ctx_) != 1) return bad("the server key does not match the server certificate");
  if (SSL_CTX_load_verify_locations(ctx_, cfg_.client_ca_file.c_str(), nullptr) != 1) return bad("client CA " + cfg_.client_ca_file + ": " + ssl_error_text());
  STACK_OF(X509_NAME)* names = SSL_load_client_CA_file(cfg_.client_ca_file.c_str());
  if (!names) return bad("client CA " + cfg_.client_ca_file + " holds no certificate");
  SSL_CTX_set_client_CA_list(ctx_, names);
  if (g_claim_index < 0) g_claim_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
  SSL_CTX_set_verify_depth(ctx_, 3);

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return bad(std::string("socket: ") + std::strerror(errno));
  ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(cfg_.port);
  if (inet_pton(AF_INET, cfg_.bind_address.c_str(), &a.sin_addr) != 1) return bad("bind address " + cfg_.bind_address + " is not an IPv4 address");
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return bad(std::string("bind: ") + std::strerror(errno));
  if (::listen(listen_fd_, 128) != 0) return bad(std::string("listen: ") + std::strerror(errno));
  socklen_t alen = sizeof a;
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &alen);
  port_ = ntohs(a.sin_port);
  stop_ = false;
  for (unsigned i = 0; i < cfg_.workers; ++i) workers_.emplace_back([this] { worker_loop(); });
  acceptor_ = std::thread([this] { accept_loop(); });
  return true;
}

void Server::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);  // set under the mutex the workers wait on, or a worker can miss the wake-up and sleep forever
    stop_ = true;
  }
  cv_.notify_all();
  if (acceptor_.joinable()) acceptor_.join();
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Pending& p : queue_) ::close(p.fd);
    queue_.clear();
    ip_count_.clear();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (ctx_) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

void Server::accept_loop() {
  while (!stop_) {
    pollfd p{listen_fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, 100);
    if (r <= 0) continue;
    sockaddr_in a{};
    socklen_t al = sizeof a;
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&a), &al);
    if (fd < 0) continue;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    std::unique_lock<std::mutex> lk(mu_);
    unsigned& n = ip_count_[ip];
    if (queue_.size() >= cfg_.max_pending || n >= cfg_.max_conn_per_ip) {
      const bool per_ip = n >= cfg_.max_conn_per_ip;
      if (n == 0) ip_count_.erase(ip);
      lk.unlock();
      ::close(fd);  // saturated or too many from this address: refuse at once rather than queue without bound
      if (log_) log_(std::string("connection refused (") + (per_ip ? "per-address limit" : "queue full") + "): " + ip);
      continue;
    }
    ++n;
    queue_.push_back(Pending{fd, ip, Clock::now()});
    lk.unlock();
    cv_.notify_one();
  }
}

void Server::worker_loop() {
  while (true) {
    Pending p;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      p = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      serve_connection(p.fd, p.peer, p.accepted);
    } catch (const std::exception& e) {
      if (log_) log_(std::string("connection handler failed: ") + e.what());
    } catch (...) {
      if (log_) log_("connection handler failed with an unknown exception");
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto it = ip_count_.find(p.peer);
    if (it != ip_count_.end() && --it->second == 0) ip_count_.erase(it);
  }
}

void Server::serve_connection(int fd, const std::string& peer, std::chrono::steady_clock::time_point started) {
  FdGuard guard{fd};
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  std::unique_ptr<SSL, SslDel> ssl(SSL_new(ctx_));
  if (!ssl) return;
  SSL_set_fd(ssl.get(), fd);
  ConnCtx cc;
  cc.revoked = [this](const std::string& serial) { return is_revoked(serial); };
  SSL_set_ex_data(ssl.get(), g_claim_index, &cc);
  // ONE absolute deadline for the whole connection, handshake included.
  Io io{ssl.get(), fd, started + std::chrono::milliseconds(cfg_.request_deadline_ms), cfg_.io_timeout_ms};
  if (io_run(io, [&] { return SSL_accept(ssl.get()); }) != 1) {
    // No usable client identity: no certificate, a certificate under another CA, an expired or revoked one, a TLS version below 1.3, garbage, or a stalled handshake.
    const std::string why = io.timed_out ? "handshake timed out" : ssl_error_text();
    const long vr = SSL_get_verify_result(ssl.get());
    const std::string claimed = valid_token(cc.claimed_cn, 64) ? cc.claimed_cn : "-";  // unauthenticated text, audited as such
    const std::string detail = "claimed_cn=" + claimed + " verify=" + X509_verify_cert_error_string(vr) + " err=" + why.substr(0, 160);
    const int lim = handshake_audit_limiter_.check("hs:" + peer);
    if (lim == 0) {
      service_.audit_refusal("anonymous:" + peer, "tls_handshake_failed", "handshake_failed", detail);
      if (log_) log_("tls handshake failed from " + peer + ": " + detail);
    } else if (lim == 1 && log_) {
      log_("tls handshake failures from " + peer + " exceed the audit limit: further ones are counted, not audited or logged individually");
    }
    return;
  }
  Principal who;
  who.remote = peer;
  if (X509* c = SSL_get1_peer_certificate(ssl.get())) {
    const std::string ou = subject_part(c, NID_organizationalUnitName), cn = subject_part(c, NID_commonName);
    X509_free(c);
    if (valid_token(cn, 64)) {
      who.id = cn;
      who.role = ou == kStationOu ? Role::kStation : ou == kAdminOu ? Role::kAdmin : Role::kNone;
    }
  }
  auto respond = [&](const HttpResponse& r) {
    const std::string out = serialize_response(r);
    Io wio{ssl.get(), fd, Clock::now() + std::chrono::milliseconds(cfg_.io_timeout_ms), cfg_.io_timeout_ms};
    std::size_t off = 0;
    while (off < out.size()) {
      const int w = io_run(wio, [&] { return SSL_write(ssl.get(), out.data() + off, static_cast<int>(out.size() - off)); });
      if (w <= 0) break;
      off += static_cast<std::size_t>(w);
    }
    SSL_shutdown(ssl.get());
  };
  if (who.role == Role::kNone) {
    // A valid certificate of the right CA that is neither a station nor an admin identity.
    service_.audit_refusal_throttled("anonymous:" + peer, "request", "forbidden", "certificate role not recognised");
    respond(Service::error_response(403, "forbidden", "the certificate subject is not a station or administrator identity"));
    return;
  }

  std::string received;
  HttpRequest req;
  ParseResult pr;
  char buf[4096];
  while (true) {
    pr = parse_http_request(received, cfg_.http, req);
    if (pr.state != ParseState::kNeedMore) break;
    const int n = io_run(io, [&] { return SSL_read(ssl.get(), buf, sizeof buf); });
    if (n <= 0) {
      if (io.timed_out) {
        pr.state = ParseState::kError;
        pr.status = 408;
        pr.code = "request_timeout";
        pr.message = "the request was not completed in time";
        break;
      }
      // The peer closed or broke the connection before the request was complete: nothing to answer.
      service_.audit_refusal_throttled(who.actor(), "request", "incomplete_request", "the client closed or broke the connection before completing the request");
      return;
    }
    received.append(buf, static_cast<std::size_t>(n));
  }
  HttpResponse resp;
  if (pr.state == ParseState::kError) {
    resp = Service::error_response(pr.status, pr.code, pr.message);
    service_.audit_refusal_throttled(who.actor(), "request", pr.code, "http " + std::to_string(pr.status));
  } else {
    resp = service_.handle(req, who);
  }
  respond(resp);
  if (log_) log_(who.actor() + " " + req.method + " " + req.path.substr(0, 96) + " -> " + std::to_string(resp.status));
}

}  // namespace velora::registry

#pragma once
// Shared helpers of the Story 4.11 librarian tests: CHECK, a short temp directory (UDS paths are limited to ~100 bytes), and a blocking Library API
// test client. Waits are bounded polls for an event, never timing assertions.
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "library_api.pb.h"

inline int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace ltest {

namespace fs = std::filesystem;

struct TempDir {
  std::string path;
  TempDir() {
    char tpl[] = "/tmp/vl.XXXXXX";
    const char* d = ::mkdtemp(tpl);
    path = d ? d : "";
  }
  ~TempDir() {
    std::error_code ec;
    if (!path.empty()) fs::remove_all(path, ec);
  }
  std::string sub(const std::string& n) const { return path + "/" + n; }
};

inline std::string slurp(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Waits (bounded polling) until `pred` holds; false if it never does within `max_ms`. A wait for an event, not a timing check.
inline bool wait_until(const std::function<bool()>& pred, int max_ms = 10000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

inline std::string frame_of(const std::string& body) {
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  return std::string(h, 4) + body;
}

class Client {
 public:
  ~Client() { close(); }
  bool connect(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }
  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  int fd() const { return fd_; }
  void shutdown_write() { ::shutdown(fd_, SHUT_WR); }  // half-close: the peer still answers what was sent
  bool send_raw(const std::string& bytes) {
    std::size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, 0);
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return true;
  }
  bool send(const ws::v1::LibraryClientMessage& m) {
    std::string b;
    (void)m.SerializeToString(&b);
    return send_raw(frame_of(b));
  }
  // Reads one server message; nullopt on timeout / closed connection.
  std::optional<ws::v1::LibraryServerMessage> recv(int timeout_ms = 5000) {
    std::string hd, body;
    if (!read_exact(hd, 4, timeout_ms)) return std::nullopt;
    const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
    if (!read_exact(body, len, timeout_ms)) return std::nullopt;
    ws::v1::LibraryServerMessage m;
    if (!m.ParseFromString(body)) return std::nullopt;
    return m;
  }
  // Sends `m` (request_id assigned) and returns its reply; messages with request_id 0 that arrive first are kept in `pushes`.
  std::optional<ws::v1::LibraryServerMessage> call(ws::v1::LibraryClientMessage m, int timeout_ms = 5000) {
    m.set_request_id(++rid_);
    if (!send(m)) return std::nullopt;
    for (;;) {
      auto r = recv(timeout_ms);
      if (!r) return std::nullopt;
      if (r->request_id() == rid_) return r;
      pushes.push_back(*r);
    }
  }
  bool handshake(const std::string& name = "test", std::uint32_t version = 1) {
    ws::v1::LibraryClientMessage m;
    m.mutable_hello()->set_version(version);
    m.mutable_hello()->set_client_name(name);
    m.mutable_hello()->add_features("import.checkpoints.v1");  // Story 5.8: a client that knows the DRIVE_REMOVED state
    auto r = call(m);
    return r && r->has_hello_ack();
  }
  bool at_eof(int timeout_ms = 2000) {  // true when the peer closed the connection
    pollfd pf{fd_, POLLIN, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) return false;
    char c;
    return ::recv(fd_, &c, 1, 0) == 0;
  }
  std::deque<ws::v1::LibraryServerMessage> pushes;

  // Convenience commands.
  std::optional<ws::v1::LibraryServerMessage> add_fav(const std::string& id) {
    ws::v1::LibraryClientMessage m;
    m.mutable_add_favorite()->set_live_set_id(id);
    return call(m);
  }
  std::optional<ws::v1::LibraryServerMessage> remove_fav(const std::string& id) {
    ws::v1::LibraryClientMessage m;
    m.mutable_remove_favorite()->set_live_set_id(id);
    return call(m);
  }
  std::optional<ws::v1::LibraryServerMessage> report(const std::string& id) {
    ws::v1::LibraryClientMessage m;
    m.mutable_report_active()->set_live_set_id(id);
    return call(m);
  }
  std::optional<ws::v1::LibraryServerMessage> playing(bool p) {
    ws::v1::LibraryClientMessage m;
    m.mutable_set_playing()->set_playing(p);
    return call(m);
  }
  std::optional<ws::v1::LibraryServerMessage> get() {
    ws::v1::LibraryClientMessage m;
    m.mutable_get_favorites_and_recent();
    return call(m);
  }
  std::optional<ws::v1::LibraryServerMessage> flush() {
    ws::v1::LibraryClientMessage m;
    m.mutable_flush_recent();
    return call(m);
  }

 private:
  bool read_exact(std::string& out, std::size_t n, int timeout_ms) {
    out.clear();
    while (out.size() < n) {
      pollfd pf{fd_, POLLIN, 0};
      if (::poll(&pf, 1, timeout_ms) <= 0) return false;
      char buf[4096];
      const ssize_t k = ::recv(fd_, buf, std::min(sizeof buf, n - out.size()), 0);
      if (k <= 0) return false;
      out.append(buf, static_cast<std::size_t>(k));
    }
    return true;
  }
  int fd_ = -1;
  std::uint64_t rid_ = 0;
};

inline std::vector<std::string> strs(const google::protobuf::RepeatedPtrField<std::string>& f) { return {f.begin(), f.end()}; }

// A deterministic UUID-shaped id for tests.
inline std::string uid(int n) {
  char b[40];
  std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012d", n);
  return b;
}

}  // namespace ltest

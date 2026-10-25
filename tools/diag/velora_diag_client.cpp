#include "velora_diag_client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace velora::diag {
namespace {

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

bool write_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool read_exact(int fd, std::string& out, std::size_t n, int timeout_ms) {
  out.clear();
  while (out.size() < n) {
    pollfd pf{fd, POLLIN, 0};
    int pr;
    do pr = ::poll(&pf, 1, timeout_ms);
    while (pr < 0 && errno == EINTR);
    if (pr <= 0) return false;
    char buf[4096];
    const ssize_t k = ::recv(fd, buf, std::min(sizeof buf, n - out.size()), 0);
    if (k <= 0) return false;
    out.append(buf, static_cast<std::size_t>(k));
  }
  return true;
}

bool call(int fd, const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply, int timeout_ms) {
  std::string b;
  m.SerializeToString(&b);
  const std::uint32_t n = static_cast<std::uint32_t>(b.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  if (!write_all(fd, std::string(h, 4) + b)) return false;
  for (;;) {  // skip pushed messages (request_id 0)
    std::string hd, body;
    if (!read_exact(fd, hd, 4, timeout_ms)) return false;
    const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
    if (len > (16u << 20) || !read_exact(fd, body, len, timeout_ms) || !reply.ParseFromString(body)) return false;
    if (reply.request_id() == m.request_id()) return true;
  }
}

void histogram(std::string& s, const char* name, const ws::v1::Histogram& h) {
  char buf[128];
  std::snprintf(buf, sizeof buf, "%s (us):\n", name);
  s += buf;
  for (int i = 0; i < h.counts_size(); ++i) {
    const unsigned lo = i == 0 ? 0u : h.edges_us(i - 1);
    if (i < h.edges_us_size()) std::snprintf(buf, sizeof buf, "  [%6u, %6u)  %llu\n", lo, h.edges_us(i), (unsigned long long)h.counts(i));
    else std::snprintf(buf, sizeof buf, "  [%6u,    inf)  %llu\n", lo, (unsigned long long)h.counts(i));
    s += buf;
  }
}

}  // namespace

bool query(const std::string& path, ws::v1::DiagnosticsReply& out, std::string& err, int timeout_ms) {
  Fd c;
  c.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
  if (c.fd >= 0) {
    const int one = 1;
    ::setsockopt(c.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  }
#endif
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) {
    err = "socket path too long: " + path;
    return false;
  }
  std::memcpy(a.sun_path, path.c_str(), path.size());
  if (c.fd < 0 || ::connect(c.fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    err = "cannot connect to " + path + ": " + std::strerror(errno);
    return false;
  }
  ws::v1::ClientMessage m;
  ws::v1::ServerMessage rep;
  m.set_request_id(1);
  m.mutable_hello()->set_version(1);
  m.mutable_hello()->set_client_name("velora-diag");
  if (!call(c.fd, m, rep, timeout_ms) || !rep.has_hello_ack()) {
    err = "handshake with " + path + " failed";
    return false;
  }
  m.Clear();
  m.set_request_id(2);
  m.mutable_get_diagnostics();
  if (!call(c.fd, m, rep, timeout_ms) || !rep.has_diagnostics_reply()) {
    err = rep.has_error() ? "engine refused GetDiagnostics: " + rep.error().message() : "no diagnostics reply";
    return false;
  }
  out = rep.diagnostics_reply();
  return true;
}

std::string format(const ws::v1::DiagnosticsReply& r) {
  std::string s;
  char buf[160];
  std::snprintf(buf, sizeof buf, "uptime_ms: %llu\nxrun: %llu\nvoice_steal: %llu\nvoice_exhausted: %llu\nstream_underrun: %llu\n",
                (unsigned long long)r.uptime_ms(), (unsigned long long)r.xrun(), (unsigned long long)r.voice_steal(),
                (unsigned long long)r.voice_exhausted(), (unsigned long long)r.stream_underrun());
  s += buf;
  histogram(s, "render_time", r.render_time());
  histogram(s, "event_to_onset_latency", r.latency());
  return s;
}

}  // namespace velora::diag

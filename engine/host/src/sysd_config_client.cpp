#include "velora/host/sysd_config_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace velora::host {

bool clock_source_from_config(ws::v1::ClockSource s, core::ClockSource& out) noexcept {
  switch (s) {
    case ws::v1::CLOCK_SOURCE_INTERNAL: out = core::ClockSource::kInternal; return true;
    case ws::v1::CLOCK_SOURCE_DIN: out = core::ClockSource::kDin; return true;
    case ws::v1::CLOCK_SOURCE_USB: out = core::ClockSource::kUsb; return true;
    default: return false;
  }
}

bool apply_clock_source(const ws::v1::Config& c, MidiIngest& ingest, core::BlockRenderer& renderer) noexcept {
  core::ClockSource cs;
  if (!clock_source_from_config(c.clock_source(), cs)) return false;
  if (ingest.clock_source() == cs && renderer.clock_source() == cs) return true;  // nothing to change (no follower reset for a repeated value)
  ingest.set_clock_source(cs);
  renderer.set_clock_source(cs);
  return true;
}

bool velocity_curve_from_config(ws::v1::VelocityCurve c, VelocityCurve& out) noexcept {
  switch (c) {
    case ws::v1::VELOCITY_CURVE_NORMAL: out = VelocityCurve::kNormal; return true;
    case ws::v1::VELOCITY_CURVE_LIGHT: out = VelocityCurve::kLight; return true;
    case ws::v1::VELOCITY_CURVE_HEAVY: out = VelocityCurve::kHeavy; return true;
    case ws::v1::VELOCITY_CURVE_FIXED: out = VelocityCurve::kFixed; return true;
    default: return false;
  }
}

bool apply_velocity_curve(const ws::v1::Config& c, VelocityCurveHolder& holder) {
  VelocityCurve vc;
  if (!velocity_curve_from_config(c.velocity_curve(), vc)) return false;
  if (holder.curve() != vc) holder.set(vc);
  return true;
}

namespace {

constexpr std::uint32_t kMaxFrame = 65536;

bool set_nonblocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool send_all(int fd, const std::string& data, int timeout_ms) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, kSendFlags);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd pf{fd, POLLOUT, 0};
      if (::poll(&pf, 1, timeout_ms) <= 0) return false;
      continue;
    }
    return false;
  }
  return true;
}

std::string frame(const ws::v1::ClientMessage& m) {
  std::string body;
  (void)m.SerializeToString(&body);
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  return std::string(h, 4) + body;
}

// Non-blocking from the start: a full backlog or a stuck peer never hangs the worker (stop() wakes the poll). -1 = not connected (the caller backs off).
int connect_unix(const std::string& path, const std::atomic<bool>& stop, int wake_fd) {
  sockaddr_un addr{};
  if (path.empty() || path.size() >= sizeof addr.sun_path) return -1;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  if (!set_nonblocking(fd)) {
    ::close(fd);
    return -1;
  }
  int rc;
  do rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  while (rc != 0 && errno == EINTR);
  if (rc != 0 && errno == EINPROGRESS) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    bool done = false;
    while (!done && !stop.load()) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) break;
      pollfd pf[2] = {{fd, POLLOUT, 0}, {wake_fd, POLLIN, 0}};
      ::poll(pf, 2, static_cast<int>(left));
      if (pf[1].revents & POLLIN) {
        char b[64];
        while (::read(wake_fd, b, sizeof b) > 0) {
        }
      }
      done = (pf[0].revents & (POLLOUT | POLLERR | POLLHUP)) != 0;
    }
    int err = 1;
    socklen_t len = sizeof err;
    rc = done && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0 ? 0 : -1;
  }
  if (rc != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void drain(int fd) {
  char buf[64];
  while (::read(fd, buf, sizeof buf) > 0) {
  }
}

}  // namespace

ws::v1::Config default_config() {
  ws::v1::Config c;
  c.set_clock_source(ws::v1::CLOCK_SOURCE_INTERNAL);
  c.set_velocity_curve(ws::v1::VELOCITY_CURVE_NORMAL);
  c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_LAST_SOUND);
  c.mutable_sustain();     // uncalibrated: min = max = 0, polarity unspecified
  c.mutable_expression();
  return c;
}

SysdConfigClient::SysdConfigClient(SysdConfigClientOptions opts) : opts_(std::move(opts)), last_(default_config()) {
  if (opts_.backoff_min_ms < 1) opts_.backoff_min_ms = 1;
  if (opts_.backoff_max_ms < opts_.backoff_min_ms) opts_.backoff_max_ms = opts_.backoff_min_ms;
  if (::pipe(wake_) != 0 || !set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) {
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
  }
}

SysdConfigClient::~SysdConfigClient() {
  stop();
  if (wake_[0] >= 0) ::close(wake_[0]);
  if (wake_[1] >= 0) ::close(wake_[1]);
}

void SysdConfigClient::start() {
  if (running_.exchange(true) || wake_[0] < 0) return;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void SysdConfigClient::stop() {
  if (!running_.exchange(false)) return;
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  if (thread_.joinable()) thread_.join();
  connected_.store(false);
}

ws::v1::Config SysdConfigClient::config() const {
  std::lock_guard<std::mutex> lk(mu_);
  return last_;
}

std::uint64_t SysdConfigClient::revision() const {
  std::lock_guard<std::mutex> lk(mu_);
  return have_last_ ? last_revision_ : 0;
}

bool SysdConfigClient::poll(const Callback& cb) {
  ws::v1::Config c;
  std::uint64_t rev = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!pending_) return false;
    c = std::move(pending_config_);
    rev = pending_revision_;
    pending_ = false;
  }
  delivered_.fetch_add(1, std::memory_order_relaxed);
  if (cb) cb(c, rev);
  return true;
}

void SysdConfigClient::on_config(const ws::v1::ConfigReply& r, Conn& conn) {
  const std::uint64_t rev = r.revision();
  configs_seen_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(mu_);
  const bool had = conn.have;
  bool accept;
  if (had) {
    accept = newer_in_connection(conn.last, rev);
  } else {  // first Config of a connection: new unless it is exactly what we already hold
    accept = !have_last_ || rev != last_revision_ || last_.SerializeAsString() != r.config().SerializeAsString();
  }
  conn.last = had ? std::max(conn.last, rev) : rev;
  conn.have = true;
  if (!accept) {
    ignored_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  last_ = r.config();
  last_revision_ = rev;
  have_last_ = true;
  pending_ = true;
  pending_config_ = r.config();
  pending_revision_ = rev;
  received_.fetch_add(1, std::memory_order_relaxed);
}

void SysdConfigClient::run() {
  int backoff = opts_.backoff_min_ms;
  while (!stop_.load()) {
    const int fd = connect_unix(opts_.socket_path, stop_, wake_[0]);
    if (fd >= 0) {
      const bool got_config = session(fd);
      ::close(fd);
      connected_.store(false);
      if (got_config) backoff = opts_.backoff_min_ms;  // only a session that delivered a valid Config resets the backoff: a server that accepts and says nothing keeps backing off
      if (stop_.load()) break;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff);
    while (!stop_.load()) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) break;
      pollfd pf{wake_[0], POLLIN, 0};
      ::poll(&pf, 1, static_cast<int>(left));
      drain(wake_[0]);
    }
    backoff = std::min(backoff * 2, opts_.backoff_max_ms);
  }
}

// One connection. True when at least one valid Config was received in it.
bool SysdConfigClient::session(int fd) {
  std::string rx;
  // Reads one frame into `m`, waiting up to timeout_ms (-1 = until data, stop or loss). 1 = got one, 0 = timeout, -1 = lost / stop / bad frame.
  auto read_frame = [&](ws::v1::ServerMessage& m, int timeout_ms) -> int {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
      if (rx.size() >= 4) {
        const std::uint32_t len = (std::uint8_t(rx[0]) << 24) | (std::uint8_t(rx[1]) << 16) | (std::uint8_t(rx[2]) << 8) | std::uint8_t(rx[3]);
        if (len > kMaxFrame) return -1;
        if (rx.size() >= 4 + len) {
          const bool ok = m.ParseFromArray(rx.data() + 4, static_cast<int>(len));
          rx.erase(0, 4 + len);
          return ok ? 1 : -1;
        }
      }
      if (stop_.load()) return -1;
      int wait = 200;
      if (timeout_ms >= 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return 0;
        wait = static_cast<int>(std::min<long long>(left, 200));
      }
      pollfd pf[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
      ::poll(pf, 2, wait);
      drain(wake_[0]);
      if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n == 0) return -1;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
        if (n > 0) rx.append(buf, static_cast<std::size_t>(n));
      }
    }
  };

  ws::v1::ClientMessage hello;
  hello.set_request_id(1);
  hello.mutable_hello()->set_version(1);
  hello.mutable_hello()->add_features("config.v1");
  hello.mutable_hello()->set_client_name(opts_.client_name);
  if (!send_all(fd, frame(hello), 2000)) return false;
  ws::v1::ServerMessage m;
  if (read_frame(m, 2000) != 1 || !m.has_hello_ack()) return false;
  ws::v1::ClientMessage sub;
  sub.set_request_id(2);
  sub.mutable_subscribe_config()->set_enable(true);
  if (!send_all(fd, frame(sub), 2000)) return false;
  connected_.store(true);
  connects_.fetch_add(1, std::memory_order_relaxed);
  Conn conn;
  for (;;) {
    ws::v1::ServerMessage in;
    const int r = read_frame(in, -1);
    if (r < 0) return conn.have;
    if (in.has_config_reply()) on_config(in.config_reply(), conn);
    else if (in.has_error() && in.request_id() == 2) return conn.have;  // sysd refused the subscription: drop and retry later
  }
}

}  // namespace velora::host

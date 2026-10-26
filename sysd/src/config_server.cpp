#include "velora/sysd/config_server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace velora::sysd {

namespace {

bool set_nonblocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::uint32_t be32(const unsigned char* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

ConfigServer::ConfigServer(ConfigService& service, std::string socket_path, ConfigServerLimits limits)
    : svc_(service), path_(std::move(socket_path)), limits_(limits) {}

ConfigServer::~ConfigServer() { stop(); }

bool ConfigServer::start(std::string& err) {
  sockaddr_un addr{};
  if (path_.empty() || path_.size() >= sizeof(addr.sun_path)) {
    err = "socket path is empty or too long: " + path_;
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
  struct stat st{};
  if (::lstat(path_.c_str(), &st) == 0 && !S_ISSOCK(st.st_mode)) {
    err = path_ + " exists and is not a socket";
    return false;
  }
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  auto bind_once = [&] {
    const mode_t old = ::umask(0177);
    const int rc = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    const int e = errno;
    ::umask(old);
    errno = e;
    return rc;
  };
  auto fail = [&](const std::string& what) {
    err = what;
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
    return false;
  };
  if (bind_once() != 0) {
    if (errno != EADDRINUSE) return fail(std::string("bind ") + path_ + ": " + std::strerror(errno));
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const bool connected = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    const int ce = errno;
    if (probe >= 0) ::close(probe);
    if (connected || !(ce == ECONNREFUSED || ce == ENOENT)) return fail("another sysd is already listening on " + path_);
    ::unlink(path_.c_str());  // stale socket of a crashed run
    if (bind_once() != 0) return fail(std::string("bind ") + path_ + ": " + std::strerror(errno));
  }
  ::chmod(path_.c_str(), 0600);
  if (::listen(listen_fd_, 16) != 0 || !set_nonblocking(listen_fd_)) {
    ::unlink(path_.c_str());
    return fail(std::string("listen: ") + std::strerror(errno));
  }
  if (::pipe(wake_) != 0) {
    wake_[0] = wake_[1] = -1;
    ::unlink(path_.c_str());
    return fail(std::string("pipe: ") + std::strerror(errno));
  }
  if (!set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) {
    ::unlink(path_.c_str());
    return fail("wake pipe");
  }
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
  return true;
}

void ConfigServer::stop() {
  if (!thread_.joinable()) return;
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  thread_.join();
  for (Client& c : clients_) {
    svc_.close(c.id);
    ::close(c.fd);
  }
  clients_.clear();
  ::close(listen_fd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
  listen_fd_ = wake_[0] = wake_[1] = -1;
  ::unlink(path_.c_str());
}

ConfigServer::Client* ConfigServer::find(ConnId id) {
  for (Client& c : clients_)
    if (c.id == id) return &c;
  return nullptr;
}

void ConfigServer::run() {
  std::vector<pollfd> fds;
  while (!stop_.load()) {
    fds.clear();
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const Client& c : clients_) fds.push_back({c.fd, static_cast<short>((c.closing ? 0 : POLLIN) | (c.tx.empty() ? 0 : POLLOUT)), 0});
    ::poll(fds.data(), fds.size(), 200);
    if (fds[0].revents & POLLIN) {
      char buf[16];
      while (::read(wake_[0], buf, sizeof buf) > 0) {
      }
    }
    if (fds[1].revents & POLLIN) accept_clients();
    const std::size_t polled = fds.size() - 2;
    for (std::size_t i = 0; i < polled && i < clients_.size(); ++i) {
      Client& c = clients_[i];
      const short ev = fds[i + 2].revents;
      if (ev & (POLLERR | POLLNVAL)) c.dead = true;
      if (!c.dead && (ev & (POLLIN | POLLHUP))) read_client(c);
    }
    for (Client& c : clients_) {  // replies and pushes queued for any client go out in this same iteration
      if (!c.dead && !c.tx.empty()) flush(c);
      if (!c.dead && c.closing && c.tx.empty()) c.dead = true;
      if (!c.handshaken && !c.dead && now_ms() - c.accepted_ms > limits_.handshake_timeout_ms) c.dead = true;
    }
    for (auto it = clients_.begin(); it != clients_.end();) {
      if (it->dead) {
        svc_.close(it->id);
        ::close(it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void ConfigServer::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) ::usleep(50'000);
      return;
    }
    if (clients_.size() >= limits_.max_clients || !set_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    Client c;
    c.fd = fd;
    c.id = next_id_++;
    c.accepted_ms = now_ms();
    svc_.open(c.id);
    clients_.push_back(std::move(c));
    accepted_.fetch_add(1, std::memory_order_relaxed);
  }
}

void ConfigServer::read_client(Client& c) {
  char buf[8192];
  bool eof = false;  // the peer closed its write side: frames already buffered are still served (a fire-and-forget client), then the replies are flushed and it closes
  for (;;) {
    const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    if (n > 0) {
      if (!c.closing && !c.dead) c.rx.append(buf, static_cast<std::size_t>(n));
      if (c.rx.size() > kMaxFrame + 4 + sizeof buf) break;
      continue;
    }
    if (n == 0) {
      eof = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
    break;
  }
  while (!c.closing && !c.dead && c.rx.size() >= 4) {
    const std::uint32_t len = be32(reinterpret_cast<const unsigned char*>(c.rx.data()));
    if (len > kMaxFrame) {  // cannot resynchronise: report and close
      ws::v1::ServerMessage e;
      e.mutable_error()->set_code(ws::v1::ERROR_CODE_FRAME_TOO_LARGE);
      e.mutable_error()->set_message("frame exceeds 65536 bytes");
      (*e.mutable_error()->mutable_details())["max_frame"] = std::to_string(kMaxFrame);
      Outbound o;
      o.to = c.id;
      o.msg = std::move(e);
      o.close_after = true;
      deliver({o});
      c.rx.clear();
      break;
    }
    if (c.rx.size() < 4 + len) break;
    const std::string payload = c.rx.substr(4, len);
    c.rx.erase(0, 4 + len);
    dispatch(c, payload);
  }
  if (eof && !c.dead) c.closing = true;  // flush what is queued, then close
}

void ConfigServer::dispatch(Client& c, const std::string& payload) {
  std::vector<Outbound> out;
  svc_.handle(c.id, payload, out);
  deliver(out);
  if (Client* self = find(c.id)) {
    // A reply that is not an error ends the handshake phase (the service tracks the real state; this only stops the timeout).
    for (const Outbound& o : out)
      if (o.to == c.id && o.msg.has_hello_ack()) self->handshaken = true;
  }
}

void ConfigServer::deliver(const std::vector<Outbound>& out) {
  for (const Outbound& o : out) {
    Client* c = find(o.to);
    if (!c || c->dead) continue;
    std::string body;
    (void)o.msg.SerializeToString(&body);
    if (body.size() > kMaxFrame) continue;  // never emit a frame the peer must reject (a Config is a few hundred bytes)
    if (c->tx.size() + body.size() + 4 > kMaxQueuedBytes) {  // slow consumer: drop it, never block
      c->dead = true;
      continue;
    }
    const std::uint32_t len = static_cast<std::uint32_t>(body.size());
    const char hdr[4] = {char(len >> 24), char(len >> 16), char(len >> 8), char(len)};
    c->tx.append(hdr, 4);
    c->tx.append(body);
    if (o.close_after) c->closing = true;
  }
}

void ConfigServer::flush(Client& c) {
  while (!c.tx.empty()) {
    const ssize_t n = ::send(c.fd, c.tx.data(), c.tx.size(), kSendFlags);
    if (n > 0) {
      c.tx.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    c.dead = true;
    return;
  }
}

}  // namespace velora::sysd

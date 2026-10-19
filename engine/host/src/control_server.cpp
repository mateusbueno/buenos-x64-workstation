#include "velora/host/control_server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace velora::host {

namespace {

bool set_nonblocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::uint32_t be32(const unsigned char* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

ws::v1::ServerMessage make_error(std::uint64_t request_id, std::uint64_t revision, ws::v1::ErrorCode code,
                                 const std::string& message) {
  ws::v1::ServerMessage m;
  m.set_request_id(request_id);
  m.set_revision(revision);
  m.mutable_error()->set_code(code);
  m.mutable_error()->set_message(message);
  return m;
}

}  // namespace

ControlServer::ControlServer(ControlPlane& plane, std::string socket_path, ControlServerLimits limits)
    : plane_(plane), path_(std::move(socket_path)), limits_(limits) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(std::string& err) {
  sockaddr_un addr{};
  if (path_.empty() || path_.size() >= sizeof(addr.sun_path)) {
    err = "socket path is empty or too long: " + path_;
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::lstat(path_.c_str(), &st) == 0 && !S_ISSOCK(st.st_mode)) {  // never touch a file that is not a socket
    err = path_ + " exists and is not a socket";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  auto bind_once = [&] {
    const mode_t old = ::umask(0177);  // the socket is created 0600 from the start
    const int rc = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    const int e = errno;
    ::umask(old);
    errno = e;
    return rc;
  };
  auto fail_bind = [&] {
    err = std::string("bind ") + path_ + ": " + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  };
  if (bind_once() != 0) {
    if (errno != EADDRINUSE) return fail_bind();
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const bool connected = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    const int cerrno = errno;
    if (probe >= 0) ::close(probe);
    // Only a refused connection proves the file is stale; EAGAIN/EACCES/... mean we cannot tell: leave it alone.
    const bool stale = !connected && (cerrno == ECONNREFUSED || cerrno == ENOENT);
    if (!stale) {
      err = "another engine is already listening on " + path_;
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    ::unlink(path_.c_str());  // stale socket file from a crashed run
    if (bind_once() != 0) return fail_bind();
  }
  ::chmod(path_.c_str(), 0600);
  auto fail_setup = [&](const char* what) {
    err = std::string(what) + ": " + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
    ::unlink(path_.c_str());
    return false;
  };
  if (::listen(listen_fd_, 16) != 0 || !set_nonblocking(listen_fd_)) return fail_setup("listen");
  if (::pipe(wake_) != 0) {
    wake_[0] = wake_[1] = -1;
    return fail_setup("pipe");
  }
  if (!set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) return fail_setup("wake pipe");  // also FD_CLOEXEC
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
  return true;
}

void ControlServer::stop() {
  if (!thread_.joinable()) return;
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  thread_.join();
  for (Client& c : clients_) ::close(c.fd);
  clients_.clear();
  ::close(listen_fd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
  listen_fd_ = wake_[0] = wake_[1] = -1;
  ::unlink(path_.c_str());
}

void ControlServer::run() {
  std::vector<pollfd> fds;
  while (!stop_.load()) {
    fds.clear();
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const Client& c : clients_)
      fds.push_back({c.fd, static_cast<short>((c.closing ? 0 : POLLIN) | (c.tx.empty() ? 0 : POLLOUT)), 0});
    ::poll(fds.data(), fds.size(), 20);
    if (fds[0].revents & POLLIN) {
      char buf[16];
      while (::read(wake_[0], buf, sizeof buf) > 0) {
      }
    }
    if (fds[1].revents & POLLIN) accept_clients();
    // clients_ may have grown in accept_clients(); only the ones polled have an entry in fds.
    for (std::size_t i = 0; i + 2 < fds.size() && i < clients_.size(); ++i) {
      Client& c = clients_[i];
      const short ev = fds[i + 2].revents;
      if (ev & (POLLERR | POLLNVAL)) c.dead = true;
      if (!c.dead && (ev & (POLLIN | POLLHUP))) read_client(c);
      if (!c.dead && !c.tx.empty()) flush(c);
      if (!c.dead && c.closing && c.tx.empty()) c.dead = true;
    }
    // Replies produced above may be queued for other clients (broadcasts): push them out now.
    for (Client& c : clients_)
      if (!c.dead && !c.tx.empty()) flush(c);
    const auto now = std::chrono::steady_clock::now();
    for (Client& c : clients_)
      if (!c.handshaken && !c.dead && now - c.accepted > std::chrono::milliseconds(limits_.handshake_timeout_ms)) c.dead = true;
    for (auto it = clients_.begin(); it != clients_.end();) {
      if (it->dead) {
        ::close(it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
    if (tick_ && tick_()) broadcast_state();
    plane_.reclaim();
  }
}

void ControlServer::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
        ::usleep(50'000);  // out of descriptors: the listener stays readable, do not spin
      return;
    }
    if (clients_.size() >= limits_.max_clients || !set_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
    no_sigpipe(fd);
    Client c;
    c.fd = fd;
    clients_.push_back(std::move(c));
    accepted_.fetch_add(1, std::memory_order_relaxed);
  }
}

void ControlServer::read_client(Client& c) {
  char buf[8192];
  for (;;) {
    const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    if (n > 0) {
      if (!c.closing && !c.dead) c.rx.append(buf, static_cast<std::size_t>(n));
      if (c.rx.size() > kMaxFrame + 4 + sizeof buf) break;  // handled below, then keep reading
      continue;
    }
    if (n == 0) {
      c.dead = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
    break;
  }
  while (!c.closing && !c.dead && c.rx.size() >= 4) {
    const std::uint32_t len = be32(reinterpret_cast<const unsigned char*>(c.rx.data()));
    if (len > kMaxFrame) {  // cannot resynchronise: report and close
      auto e = make_error(0, plane_.revision(), ws::v1::ERROR_CODE_FRAME_TOO_LARGE, "frame exceeds 65536 bytes");
      (*e.mutable_error()->mutable_details())["max_frame"] = std::to_string(kMaxFrame);
      send(c, e);
      c.closing = true;
      c.rx.clear();
      break;
    }
    if (c.rx.size() < 4 + len) break;
    const std::string payload = c.rx.substr(4, len);
    c.rx.erase(0, 4 + len);
    frames_in_.fetch_add(1, std::memory_order_relaxed);
    process_frame(c, payload);
  }
}

void ControlServer::process_frame(Client& c, const std::string& payload) {
  ws::v1::ClientMessage msg;
  if (!msg.ParseFromString(payload)) {
    auto e = make_error(0, plane_.revision(), ws::v1::ERROR_CODE_MALFORMED_MESSAGE, "frame is not a valid ClientMessage");
    send(c, e);
    if (!c.handshaken) c.closing = true;
    return;
  }
  if (!c.handshaken) {
    if (msg.body_case() != ws::v1::ClientMessage::kHello) {
      auto e = make_error(msg.request_id(), plane_.revision(), ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED,
                          "the first message on a connection must be Hello");
      send(c, e);
      c.closing = true;
      return;
    }
    if (msg.hello().version() != ControlPlane::kProtocolVersion) {
      auto e = make_error(msg.request_id(), plane_.revision(), ws::v1::ERROR_CODE_UNSUPPORTED_VERSION,
                          "unsupported protocol version");
      (*e.mutable_error()->mutable_details())["supported"] = std::to_string(ControlPlane::kProtocolVersion);
      (*e.mutable_error()->mutable_details())["requested"] = std::to_string(msg.hello().version());
      send(c, e);
      c.closing = true;
      return;
    }
    c.handshaken = true;
    ws::v1::ServerMessage ack;
    ack.set_request_id(msg.request_id());
    ack.set_revision(plane_.revision());
    ws::v1::HelloAck* h = ack.mutable_hello_ack();
    h->set_version(ControlPlane::kProtocolVersion);
    for (const char* f : {"params.v1", "snapshots.v1", "subscribe.v1"}) h->add_features(f);
    h->set_revision(plane_.revision());
    send(c, ack);
    return;
  }
  bool changed = false;
  ws::v1::ServerMessage reply = plane_.handle(msg, changed);
  if (msg.body_case() == ws::v1::ClientMessage::kSubscribe && !reply.has_error())
    c.subscribed = msg.subscribe().enable();
  send(c, reply);
  if (msg.body_case() == ws::v1::ClientMessage::kSubscribe && c.subscribed) {  // current state right away
    ws::v1::ServerMessage push;
    push.set_revision(plane_.revision());
    plane_.fill_state(push.mutable_state());
    send(c, push);
  }
  if (changed) broadcast_state();
}

void ControlServer::broadcast_state() {
  ws::v1::ServerMessage push;
  push.set_request_id(0);
  push.set_revision(plane_.revision());
  plane_.fill_state(push.mutable_state());
  for (Client& c : clients_)
    if (c.subscribed && !c.dead) send(c, push);
}

void ControlServer::send(Client& c, const ws::v1::ServerMessage& msg) {
  std::string body;
  msg.SerializeToString(&body);
  if (body.size() > kMaxFrame) {  // never emit a frame the peer must reject
    ws::v1::ServerMessage e = make_error(msg.request_id(), msg.revision(), ws::v1::ERROR_CODE_INTERNAL,
                                         "reply exceeds the maximum frame size");
    body.clear();
    e.SerializeToString(&body);
  }
  const std::uint32_t len = static_cast<std::uint32_t>(body.size());
  const char hdr[4] = {char(len >> 24), char(len >> 16), char(len >> 8), char(len)};
  if (c.tx.size() + body.size() + 4 > kMaxQueuedBytes) {  // slow consumer: drop it, never block
    c.dead = true;
    dropped_slow_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  c.tx.append(hdr, 4);
  c.tx.append(body);
}

void ControlServer::flush(Client& c) {
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

}  // namespace velora::host

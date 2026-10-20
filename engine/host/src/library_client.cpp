#include "velora/host/library_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "library_api.pb.h"

namespace velora::host {

namespace {

constexpr std::uint32_t kMaxClientFrame = 262144;

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

std::string frame(const ws::v1::LibraryClientMessage& m) {
  std::string body;
  (void)m.SerializeToString(&body);
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  return std::string(h, 4) + body;
}

int connect_unix(const std::string& path) {
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
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || !set_nonblocking(fd)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

LibraryClient::LibraryClient(LibraryClientOptions opts) : opts_(std::move(opts)) {
  if (opts_.queue_capacity == 0) opts_.queue_capacity = 1;
  if (opts_.backoff_min_ms < 1) opts_.backoff_min_ms = 1;
  if (opts_.backoff_max_ms < opts_.backoff_min_ms) opts_.backoff_max_ms = opts_.backoff_min_ms;
  if (::pipe(wake_) != 0 || !set_nonblocking(wake_[0]) || !set_nonblocking(wake_[1])) {
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
  }
}

LibraryClient::~LibraryClient() {
  stop();
  if (wake_[0] >= 0) ::close(wake_[0]);
  if (wake_[1] >= 0) ::close(wake_[1]);
}

void LibraryClient::start() {
  if (running_.exchange(true) || wake_[0] < 0) return;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void LibraryClient::stop(int flush_ms) {
  if (!running_.load()) return;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(flush_ms);
  while (connected_.load() && queued() > 0 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (!running_.exchange(false)) return;
  stop_.store(true);
  const char b = 1;
  [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);
  if (thread_.joinable()) thread_.join();
  connected_.store(false);
}

std::size_t LibraryClient::queued() const {
  std::lock_guard<std::mutex> lk(mu_);
  return queue_.size();
}

void LibraryClient::push(Item item) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (item.is_playing) last_playing_ = item.playing;
    else last_active_ = item.id;
    item.seq = ++next_item_seq_;
    if (queue_.size() >= opts_.queue_capacity) {
      const std::uint64_t busy = sending_seq_;  // the item being written is neither evicted nor counted as dropped
      auto it = std::find_if(queue_.begin(), queue_.end(), [busy](const Item& i) { return !i.is_playing && i.seq != busy; });  // reports go first
      if (it == queue_.end()) it = std::find_if(queue_.begin(), queue_.end(), [busy](const Item& i) { return i.seq != busy; });
      if (it != queue_.end()) {
        queue_.erase(it);
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }  // else the only item is the one being sent: the queue exceeds its capacity by one until it is done
    }
    queue_.push_back(std::move(item));
  }
  if (wake_[1] >= 0) {
    const char b = 1;
    [[maybe_unused]] const ssize_t w = ::write(wake_[1], &b, 1);  // non-blocking; a full pipe already means "wake up"
  }
}

void LibraryClient::report_active(const std::string& live_set_id) {
  Item i;
  i.id = live_set_id;
  push(std::move(i));
}

void LibraryClient::set_playing(bool playing) {
  Item i;
  i.is_playing = true;
  i.playing = playing;
  push(std::move(i));
}

void LibraryClient::run() {
  int backoff = opts_.backoff_min_ms;
  while (!stop_.load()) {
    const int fd = connect_unix(opts_.socket_path);
    if (fd >= 0) {
      const bool had_session = session(fd);
      ::close(fd);
      connected_.store(false);
      if (had_session) backoff = opts_.backoff_min_ms;  // a connection that got through the handshake resets the backoff
      if (stop_.load()) break;
    }
    // Wait out the backoff (interruptible by stop(); pushes also wake the pipe, which we drain and ignore until the deadline).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff);
    while (!stop_.load()) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) break;
      pollfd pf{wake_[0], POLLIN, 0};
      ::poll(&pf, 1, static_cast<int>(left));
      char buf[64];
      while (::read(wake_[0], buf, sizeof buf) > 0) {
      }
    }
    backoff = std::min(backoff * 2, opts_.backoff_max_ms);
  }
}

// One connection. Returns true when the handshake succeeded (the connection later ended), false when it never got that far.
bool LibraryClient::session(int fd) {
  std::uint64_t rid = 0;
  {
    ws::v1::LibraryClientMessage hello;
    hello.set_request_id(++rid);
    hello.mutable_hello()->set_version(1);
    hello.mutable_hello()->add_features("library.v1");
    hello.mutable_hello()->set_client_name(opts_.client_name);
    if (!send_all(fd, frame(hello), 2000)) return false;
  }
  std::string rx;
  bool acked = false;
  const auto hs_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (!acked) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(hs_deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0 || stop_.load()) return false;
    pollfd pf[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    ::poll(pf, 2, static_cast<int>(left));
    char buf[4096];
    while (::read(wake_[0], buf, sizeof buf) > 0) {
    }
    if (pf[0].revents & (POLLIN | POLLHUP)) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n == 0) return false;
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
      if (n > 0) rx.append(buf, static_cast<std::size_t>(n));
    }
    if (rx.size() >= 4) {
      const std::uint32_t len = (std::uint8_t(rx[0]) << 24) | (std::uint8_t(rx[1]) << 16) | (std::uint8_t(rx[2]) << 8) | std::uint8_t(rx[3]);
      if (len > kMaxClientFrame) return false;
      if (rx.size() >= 4 + len) {
        ws::v1::LibraryServerMessage m;
        if (!m.ParseFromArray(rx.data() + 4, static_cast<int>(len)) || !m.has_hello_ack() || m.hello_ack().version() != 1) return false;
        rx.erase(0, 4 + len);
        acked = true;
      }
    }
  }
  connected_.store(true);
  connects_.fetch_add(1, std::memory_order_relaxed);
  // A (re)started service begins with playing=false: say what the engine is doing now.
  {
    std::optional<bool> p;
    {
      std::lock_guard<std::mutex> lk(mu_);
      p = last_playing_;
    }
    if (p) {
      ws::v1::LibraryClientMessage m;
      m.set_request_id(++rid);
      m.mutable_set_playing()->set_playing(*p);
      if (!send_all(fd, frame(m), 2000)) return true;
      sent_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  {  // a restarted service lost the Live Set that sounds unless a newer report is already waiting
    std::optional<std::string> a;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const bool newer = std::any_of(queue_.begin(), queue_.end(), [](const Item& i) { return !i.is_playing; });
      if (!newer) a = last_active_;
    }
    if (a) {
      ws::v1::LibraryClientMessage m;
      m.set_request_id(++rid);
      m.mutable_report_active()->set_live_set_id(*a);
      if (!send_all(fd, frame(m), 2000)) return true;
      sent_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  while (!stop_.load()) {
    Item item;
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!queue_.empty()) {
        item = queue_.front();  // popped only after it has been sent
        sending_seq_ = item.seq;
        have = true;
      }
    }
    if (have) {
      ws::v1::LibraryClientMessage m;
      m.set_request_id(++rid);
      if (item.is_playing)
        m.mutable_set_playing()->set_playing(item.playing);
      else
        m.mutable_report_active()->set_live_set_id(item.id);
      const bool sent_ok = send_all(fd, frame(m), 2000);
      std::lock_guard<std::mutex> lk(mu_);
      sending_seq_ = 0;
      if (!sent_ok) return true;  // stays queued; the next connection sends it
      sent_.fetch_add(1, std::memory_order_relaxed);
      if (!queue_.empty() && queue_.front().seq == item.seq) queue_.pop_front();  // drop-oldest may have removed it meanwhile
      continue;
    }
    pollfd pf[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    ::poll(pf, 2, 200);
    char buf[4096];
    while (::read(wake_[0], buf, sizeof buf) > 0) {
    }
    if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {  // discard replies; detect a closed connection
      for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n > 0) continue;
        if (n == 0) return true;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return true;
      }
    }
  }
  return true;
}

}  // namespace velora::host

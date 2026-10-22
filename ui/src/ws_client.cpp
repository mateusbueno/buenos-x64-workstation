#include "velora/ui/ws_client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <chrono>

namespace velora::ui {

static constexpr uint32_t kMaxFrame = 65536;

WsClient::WsClient(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

WsClient::~WsClient() {
  stop();
}

bool WsClient::start() {
  if (running_.load()) return true;
  running_.store(true);
  worker_thread_ = std::thread(&WsClient::run_worker, this);
  return true;
}

void WsClient::stop() {
  if (!running_.exchange(false)) return;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (active_fd_ >= 0) {
      ::shutdown(active_fd_, SHUT_RDWR);
      ::close(active_fd_);
      active_fd_ = -1;
    }
  }

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  connected_.store(false);
}

ws::v1::State WsClient::get_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return latest_state_;
}

float WsClient::get_tempo_bpm() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (const auto& pv : latest_state_.performance_init()) {
    if (pv.param_id() == ws::v1::PARAM_TEMPO_BPM) {
      return pv.value();
    }
  }
  return 120.0f;
}

std::string WsClient::get_live_set_name() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!latest_state_.live_set_name().empty()) {
    return latest_state_.live_set_name();
  }
  return "Default";
}

void WsClient::set_state_callback(StateCallback cb) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_cb_ = std::move(cb);
}

void WsClient::set_error_callback(ErrorCallback cb) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  error_cb_ = std::move(cb);
}

bool WsClient::send_command(const ws::v1::ClientMessage& msg) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  if (active_fd_ < 0 || !connected_.load()) return false;
  return send_client_message(active_fd_, msg);
}

static bool write_all(int fd, const void* data, size_t size) {
  const char* ptr = static_cast<const char*>(data);
  size_t off = 0;
  while (off < size) {
    ssize_t n = ::send(fd, ptr + off, size - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

static bool read_exact(int fd, void* data, size_t size, int timeout_ms, const std::atomic<bool>& running) {
  char* ptr = static_cast<char*>(data);
  size_t off = 0;
  while (off < size && running.load()) {
    pollfd pfd{fd, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r <= 0) return false;
    ssize_t n = ::recv(fd, ptr + off, size - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return off == size;
}

bool WsClient::send_client_message(int fd, const ws::v1::ClientMessage& msg) {
  std::string payload;
  if (!msg.SerializeToString(&payload)) return false;
  if (payload.size() > kMaxFrame) return false;

  uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
  if (!write_all(fd, &len, sizeof(len))) return false;
  return write_all(fd, payload.data(), payload.size());
}

bool WsClient::read_server_message(int fd, ws::v1::ServerMessage& msg, int timeout_ms) {
  uint32_t net_len = 0;
  if (!read_exact(fd, &net_len, sizeof(net_len), timeout_ms, running_)) return false;
  uint32_t len = ntohl(net_len);
  if (len > kMaxFrame) return false;

  std::vector<char> buf(len);
  if (!read_exact(fd, buf.data(), len, timeout_ms, running_)) return false;

  return msg.ParseFromArray(buf.data(), static_cast<int>(len));
}

bool WsClient::connect_and_handshake(int& out_fd) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }

  // 1. Send Hello
  ws::v1::ClientMessage hello_msg;
  hello_msg.set_request_id(1);
  auto* hello = hello_msg.mutable_hello();
  hello->set_version(ws::v1::PROTOCOL_VERSION_1);
  hello->set_client_name("ui");

  if (!send_client_message(fd, hello_msg)) {
    ::close(fd);
    return false;
  }

  // 2. Receive HelloAck
  ws::v1::ServerMessage ack_msg;
  if (!read_server_message(fd, ack_msg, 3000) || !ack_msg.has_hello_ack()) {
    ::close(fd);
    return false;
  }
  last_revision_.store(ack_msg.revision(), std::memory_order_release);

  // 3. Send Subscribe
  ws::v1::ClientMessage sub_msg;
  sub_msg.set_request_id(2);
  sub_msg.mutable_subscribe()->set_enable(true);
  if (!send_client_message(fd, sub_msg)) {
    ::close(fd);
    return false;
  }

  out_fd = fd;
  return true;
}

void WsClient::run_worker() {
  while (running_.load()) {
    int fd = -1;
    if (!connect_and_handshake(fd)) {
      connected_.store(false);
      // Wait and retry
      for (int i = 0; i < 10 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      active_fd_ = fd;
      connected_.store(true);
    }

    // Process incoming server messages
    while (running_.load()) {
      ws::v1::ServerMessage smsg;
      if (!read_server_message(fd, smsg, 1000)) {
        // Timeout or disconnect
        if (!running_.load()) break;
        // Check if socket is still alive
        pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 0);
        if (pr < 0 || (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
          // Disconnected
          break;
        }
        continue;
      }

      if (smsg.has_state()) {
        StateCallback cb;
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          latest_state_ = smsg.state();
          has_snapshot_.store(true, std::memory_order_release);
          last_revision_.store(smsg.revision(), std::memory_order_release);
          cb = state_cb_;
        }
        if (cb) {
          cb(smsg.state());
        }
      } else if (smsg.has_error()) {
        ErrorCallback ecb;
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          ecb = error_cb_;
        }
        if (ecb) {
          ecb(smsg.error());
        }
      }
    }

    // Clean up closed socket
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      if (active_fd_ >= 0) {
        ::close(active_fd_);
        active_fd_ = -1;
      }
      connected_.store(false);
    }
  }
}

}  // namespace velora::ui

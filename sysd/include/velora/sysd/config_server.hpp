#pragma once
// ws.v1 over a Unix domain socket for the ConfigService (same framing as the engine's control server: 4-byte big-endian length, then a protobuf message,
// largest frame 65536 bytes). One thread accepts, reads, hands each frame to the ConfigService, writes the replies and pushes. A client that has not
// completed Hello within handshake_timeout_ms, one that falls kMaxQueuedBytes behind, an oversize frame and connections beyond max_clients are closed.
// The socket is created 0600 (the sysd user's; deployment decides the group) and a stale socket file of a crashed run is replaced; a live one is refused.
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "velora/sysd/config_service.hpp"

namespace velora::sysd {

struct ConfigServerLimits {
  std::size_t max_clients = 64;
  int handshake_timeout_ms = 5000;
};

class ConfigServer {
 public:
  static constexpr std::uint32_t kMaxFrame = 65536;
  static constexpr std::size_t kMaxQueuedBytes = 4u << 20;

  ConfigServer(ConfigService& service, std::string socket_path, ConfigServerLimits limits = {});
  ~ConfigServer();
  ConfigServer(const ConfigServer&) = delete;
  ConfigServer& operator=(const ConfigServer&) = delete;

  bool start(std::string& err);
  void stop();  // joins the thread and removes the socket file
  const std::string& path() const noexcept { return path_; }
  std::uint64_t accepted() const noexcept { return accepted_.load(std::memory_order_relaxed); }
  // Outputs of a ConfigService call that belong to connections of ANOTHER server (the TCP bridge shares the service; ids >= kBridgeConnBase) go to this sink.
  // Set before start(). And the reverse: post() (any thread) delivers outputs produced elsewhere to this server's clients.
  void set_foreign_sink(std::function<void(std::vector<Outbound>)> sink) { foreign_ = std::move(sink); }
  void post(std::vector<Outbound> out);

 private:
  struct Client {
    int fd = -1;
    ConnId id = 0;
    std::string rx, tx;
    bool handshaken = false, closing = false, dead = false;
    std::int64_t accepted_ms = 0;
  };
  void run();
  void accept_clients();
  void read_client(Client& c);
  void dispatch(Client& c, const std::string& payload);
  void deliver(const std::vector<Outbound>& out);
  void flush(Client& c);
  Client* find(ConnId id);

  ConfigService& svc_;
  std::string path_;
  ConfigServerLimits limits_;
  int listen_fd_ = -1;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::vector<Client> clients_;
  ConnId next_id_ = 1;
  std::atomic<std::uint64_t> accepted_{0};
  std::function<void(std::vector<Outbound>)> foreign_;
  std::mutex inbox_mu_;
  std::vector<Outbound> inbox_;
  bool post_closed_ = false;
};

}  // namespace velora::sysd

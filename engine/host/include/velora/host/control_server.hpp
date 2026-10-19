#pragma once
// ws.v1 server on a Unix domain socket (Story 2.1). One thread (the control thread) accepts clients,
// reads length-prefixed protobuf frames (4-byte big-endian length, then ClientMessage; largest frame
// 65536 bytes), runs the version/feature handshake, hands commands to the ControlPlane one at a time (so
// concurrent clients are serialized and revisions strictly increase), writes ServerMessage replies and
// pushes the new State to subscribers after each published generation. It also reclaims generations.
//
// Closing rules: a frame above the limit, an unparsable first frame, a first message that is not Hello, an
// unsupported version, a client that has not handshaken within the deadline, one that falls kMaxQueuedBytes behind
// and connections beyond max_clients close the connection (after the error reply where one applies). After the
// handshake an unparsable frame or an invalid command gets an error reply and the connection stays.
// Thread-safety: everything runs on the server thread; only the counters are readable from other threads.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "velora/host/control_plane.hpp"

namespace velora::host {

struct ControlServerLimits {
  std::size_t max_clients = 64;       // connections beyond this are closed right after accept
  int handshake_timeout_ms = 5000;    // a client that has not completed Hello by then is dropped
};

class ControlServer {
 public:
  static constexpr std::uint32_t kMaxFrame = 65536;
  static constexpr std::size_t kMaxQueuedBytes = 4u << 20;  // a client that falls this far behind is dropped

  ControlServer(ControlPlane& plane, std::string socket_path, ControlServerLimits limits = {});
  ~ControlServer();
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Binds the socket (removing a stale one, refusing a live one), mode 0600, and starts the thread.
  bool start(std::string& err);
  void stop();  // joins the thread and removes the socket file
  const std::string& path() const noexcept { return path_; }

  // The control tick (Story 4.5): `f` runs on the server thread on every iteration of its poll loop (at least every 20 ms), after the clients
  // were served and before generations are reclaimed. When it returns true the current State is pushed to the subscribers (the Live Set
  // switcher uses it to publish the loading status and to commit a finished background load). Set it before start().
  void set_tick(std::function<bool()> f) { tick_ = std::move(f); }
  // Story 4.6: `f` runs once on the server thread before it serves anything. The server thread is THE control thread, so the single-writer
  // stores (LiveSetStore, SetlistStore), which record the thread that opened them, hand their ownership over here (rebind_control_thread()).
  // Set it before start().
  void set_thread_start(std::function<void()> f) { thread_start_ = std::move(f); }

  std::uint64_t accepted() const noexcept { return accepted_.load(std::memory_order_relaxed); }
  std::uint64_t dropped_slow() const noexcept { return dropped_slow_.load(std::memory_order_relaxed); }
  std::uint64_t frames_in() const noexcept { return frames_in_.load(std::memory_order_relaxed); }

 private:
  struct Client {
    int fd = -1;
    std::string rx, tx;
    bool handshaken = false, subscribed = false, closing = false, dead = false;
    std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();
  };

  void run();
  void accept_clients();
  void read_client(Client& c);
  void process_frame(Client& c, const std::string& payload);
  void send(Client& c, const ws::v1::ServerMessage& msg);
  void flush(Client& c);
  void broadcast_state();

  ControlPlane& plane_;
  std::string path_;
  ControlServerLimits limits_;
  int listen_fd_ = -1;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::vector<Client> clients_;
  std::function<bool()> tick_;
  std::function<void()> thread_start_;
  std::atomic<std::uint64_t> accepted_{0}, frames_in_{0}, dropped_slow_{0};
};

}  // namespace velora::host

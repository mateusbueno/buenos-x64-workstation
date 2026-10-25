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
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

  // Story 3.10: a panel BUTTON frame for the subscribed ui clients (ServerMessage.panel_event, request_id 0). Callable from any thread (the panel input
  // thread): it queues the event and wakes the server thread, which pushes it. False when the queue is full (the event is dropped and counted).
  // Never relayed through sysd: this is the only path a button takes to the ui.
  bool push_panel_event(std::uint32_t button_id, bool pressed, std::uint64_t device_ns);
  // Bound of the pending PanelEvent queue (default 1024); presses beyond it are dropped, a release replaces the oldest pending press. Test seam.
  void set_panel_queue_max(std::size_t n) {
    std::lock_guard<std::mutex> lk(panel_mu_);
    panel_max_ = n ? n : 1;
  }
  // The pending (not yet pushed) PanelEvents as (button id, pressed), oldest first. Test seam.
  std::vector<std::pair<std::uint32_t, bool>> panel_pending() const {
    std::lock_guard<std::mutex> lk(panel_mu_);
    std::vector<std::pair<std::uint32_t, bool>> v;
    for (const PanelItem& p : panel_q_) v.emplace_back(p.id, p.pressed);
    return v;
  }
  std::uint64_t panel_events_sent() const noexcept { return panel_sent_.load(std::memory_order_relaxed); }
  std::uint64_t panel_events_dropped() const noexcept { return panel_dropped_.load(std::memory_order_relaxed); }

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
  void drain_panel_events();

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
  struct PanelItem { std::uint32_t id; bool pressed; std::uint64_t ns; };
  std::size_t panel_max_ = 1024;  // guarded by panel_mu_
  mutable std::mutex panel_mu_;
  int panel_wake_ = -1;  // wake_[1] while running; guarded by panel_mu_
  std::deque<PanelItem> panel_q_;
  std::atomic<std::uint64_t> panel_sent_{0}, panel_dropped_{0};
  std::atomic<std::uint64_t> accepted_{0}, frames_in_{0}, dropped_slow_{0};
};

}  // namespace velora::host

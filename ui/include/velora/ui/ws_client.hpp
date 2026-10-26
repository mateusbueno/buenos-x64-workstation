#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <condition_variable>
#include <cstdint>

#include "velora/ui/panel_event.hpp"
#include "ws_v1.pb.h"

namespace velora::ui {

class WsClient {
 public:
  using StateCallback = std::function<void(const ws::v1::State&)>;

  WsClient(std::string socket_path = "/tmp/velora-engine.sock");
  ~WsClient();

  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  // Start background connection thread
  bool start();
  // Stop and close connection
  void stop();

  // Connection status
  bool is_connected() const noexcept { return connected_.load(std::memory_order_acquire); }
  bool has_snapshot() const noexcept { return has_snapshot_.load(std::memory_order_acquire); }
  uint64_t last_revision() const noexcept { return last_revision_.load(std::memory_order_acquire); }

  // Snapshot data accessors
  ws::v1::State get_state() const;
  float get_tempo_bpm() const;
  std::string get_live_set_name() const;

  // Register callback invoked on the UI/client thread when State changes
  void set_state_callback(StateCallback cb);

  // Register callback invoked when the server reports a command Error
  using ErrorCallback = std::function<void(const ws::v1::Error&)>;
  void set_error_callback(ErrorCallback cb);

  // Story 3.10: register a listener for the panel button events the engine pushes (ServerMessage.panel_event, after Subscribe). Invoked on the client
  // thread with a Button PanelEvent whose timestamp_us is the engine's device time of the press / release.
  void set_panel_event_listener(PanelEventListener cb);

  // Story 8.9: a SetlistsReply (the answer to GetSetlists, e.g. with all_live_sets for the Boot into picker), invoked on the client thread.
  using SetlistsCallback = std::function<void(const ws::v1::SetlistsReply&)>;
  void set_setlists_callback(SetlistsCallback cb);

  // Send a command to the engine
  bool send_command(const ws::v1::ClientMessage& msg);

  // Set socket path (before start)
  void set_socket_path(std::string path) { socket_path_ = std::move(path); }
  const std::string& socket_path() const noexcept { return socket_path_; }

 private:
  void run_worker();
  bool connect_and_handshake(int& fd);
  bool read_server_message(int fd, ws::v1::ServerMessage& msg, int timeout_ms);
  bool send_client_message(int fd, const ws::v1::ClientMessage& msg);

  std::string socket_path_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> has_snapshot_{false};
  std::atomic<uint64_t> last_revision_{0};
  std::atomic<uint64_t> next_req_id_{10};

  mutable std::mutex state_mutex_;
  ws::v1::State latest_state_;
  StateCallback state_cb_;
  ErrorCallback error_cb_;
  PanelEventListener panel_cb_;
  SetlistsCallback setlists_cb_;

  std::thread worker_thread_;
  int active_fd_{-1};
  std::mutex send_mutex_;
};

}  // namespace velora::ui

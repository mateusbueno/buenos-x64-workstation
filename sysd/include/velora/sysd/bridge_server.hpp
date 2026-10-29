#pragma once
// The sysd TCP bridge (Foundation: Velora Studio device connection; Story 9.3 Mac/localhost logic, Stories 9.6 / 9.7 pairing-lite and relay).
//
// Velora Studio speaks the SAME ws.v1 schema as the on-device clients, framed on ONE TCP port (4-byte big-endian length + protobuf, largest frame 65536
// bytes, the framing of the Unix sockets). The bridge is the single door to the unit's services:
//
//   TCP client --Hello--> HelloAck {device info, NEGOTIATED features, paired}
//   unpaired: only Hello and PairRequest are answered; anything else (even a body the table does not know) is ERROR_CODE_NOT_PAIRED. The allow-list is the
//             `unpaired_allowed` column of the message table (message_rules()). A Hello pairs only with a valid pairing token (see pairing.hpp), never by id alone.
//   paired:   each message is routed by the TOTAL message table (message_rules(): no default; unlisted = INVALID_COMMAND; hardware / panel commands are denied):
//               engine  -> the engine's control UDS (a connection of its own per client: the engine keeps its per-client Subscribe state, and its
//                          State pushes are relayed back as they come), library.v1 -> the librarian UDS (LibraryRequest carries LibraryClientMessage
//                          bytes, allow-listed by library_rules()), config.v1 -> the sysd ConfigService in-process (shared with the Unix socket server).
//   The bridge never interprets Live Set content. request_id is the client's: the bridge maps (session, request_id) to a fresh upstream id and the reply
//   goes back with the original. A message of a feature that was not negotiated is ERROR_CODE_UNSUPPORTED_FEATURE; an upstream that cannot be
//   reached is ERROR_CODE_UPSTREAM_UNAVAILABLE (and when an upstream connection is lost mid-session the client connection is closed after that error, so
//   the client reconnects and re-subscribes).
//
// Limits (per connection / server): frame cap 65536 (violation = error + close, counted), max_clients (default 8), handshake timeout, idle timeout, a message
// rate cap (BUSY above it, close far above), a slow client (output queue above max_queued_bytes) is dropped. Nothing here is TLS: the physical USB link is the
// trust boundary (architecture). NCM addressing and the nftables single-port policy are board work; the bind address is an option.
// One thread owns every socket. Other threads only call post() / counters.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "velora/sysd/config_service.hpp"
#include "velora/sysd/pairing.hpp"

namespace velora::sysd {

struct BridgeOptions {
  std::string bind_host = "127.0.0.1";
  int port = 0;  // 0: an ephemeral port (tests); see BridgeServer::port()
  std::string engine_socket;     // empty: engine commands answer UPSTREAM_UNAVAILABLE
  std::string librarian_socket;  // empty: library messages answer UPSTREAM_UNAVAILABLE
  std::string model = "Velora";
  std::string firmware_version = "0.1.0-dev";
  std::string update_staging_dir = "/data/update/.staging";
  std::function<void(const std::string& version)> on_firmware_ready;
  bool dev_bypass_pairing = false;  // DEV ONLY: every client is paired, model "DEV HOST"
  std::size_t max_clients = 8;
  std::size_t max_unpaired = 3;  // handshaken but unpaired sessions at once (more are refused with BUSY at Hello)
  int handshake_timeout_ms = 5000;
  int idle_timeout_ms = 120000;  // no frame from a paired client for this long: closed (Studio sends a keepalive)
  int unpaired_idle_timeout_ms = 45000;  // the same for an unpaired one
  int pending_timeout_ms = 30000;  // a request with no upstream reply for this long is answered UPSTREAM_UNAVAILABLE and forgotten
  int max_msgs_per_second = 500;
  std::size_t max_queued_bytes = 4u << 20;
  std::size_t max_pending_per_upstream = 256;
  std::size_t max_upstream_tx_bytes = 1u << 20;  // bytes queued toward one upstream (a stalled engine must not grow memory)
  std::function<std::int64_t()> now_ms;  // steady clock in ms; default std::chrono::steady_clock
  std::function<void(const std::string&)> log;
};

class BridgeServer {
 public:
  static constexpr std::uint32_t kMaxFrame = 65536;
  static constexpr std::uint32_t kMaxLibraryFrame = 262144;
  // The feature names of the device side. HelloAck.features = the client's list intersected with this set, plus "pairing.v1".
  static const std::vector<std::string>& device_features();

  // The message table (a TOTAL table, no default): every ClientMessage body is classified. Unlisted bodies are INVALID_COMMAND.
  enum class Route : std::uint8_t { kLocal, kEngine, kConfig, kLibrary, kDenied, kUpdate };
  struct MessageRule {
    int body_case;          // ClientMessage::BodyCase (= the field number)
    const char* name;
    const char* feature;    // negotiated feature required; nullptr for local and denied rows
    Route route;
    bool unpaired_allowed;  // an unpaired client may send it (the allow-list): only Hello and PairRequest
  };
  static const std::vector<MessageRule>& message_rules();
  static const MessageRule* rule_for(int body_case);
  // The library messages a Studio client may send (everything else is refused before the librarian sees it). Total over LibraryClientMessage's body.
  struct LibraryRule {
    int body_case;
    const char* name;
    bool allowed;
  };
  static const std::vector<LibraryRule>& library_rules();

  BridgeServer(ConfigService& config, PairingWindow& window, PairedStore& paired, BridgeOptions options);
  ~BridgeServer();
  BridgeServer(const BridgeServer&) = delete;
  BridgeServer& operator=(const BridgeServer&) = delete;

  bool start(std::string& err);
  void stop();
  int port() const noexcept { return port_; }
  const BridgeOptions& options() const noexcept { return opt_; }

  // ConfigService outputs addressed to connections of the Unix socket server go here (set before start()); post() is the reverse direction.
  void set_foreign_sink(std::function<void(std::vector<Outbound>)> sink) { foreign_ = std::move(sink); }
  void post(std::vector<Outbound> out);

  std::uint64_t accepted() const noexcept { return accepted_.load(); }
  std::uint64_t rejected_over_capacity() const noexcept { return over_capacity_.load(); }
  std::uint64_t malformed() const noexcept { return malformed_.load(); }   // bad protobuf or oversize frames, each closed
  std::uint64_t oversize() const noexcept { return oversize_.load(); }
  std::uint64_t slow_dropped() const noexcept { return slow_dropped_.load(); }  // clients dropped for an output queue above max_queued_bytes
  std::uint64_t paired_sessions() const noexcept { return paired_sessions_.load(); }
  std::size_t sessions() const noexcept { return session_count_.load(); }

 private:
  struct Upstream {
    int fd = -1;
    std::string rx, tx;
    std::uint64_t next_id = 2;  // 1 is the bridge's own Hello
    struct Pending {
      std::uint64_t orig;
      std::int64_t t_ms;
    };
    std::map<std::uint64_t, Pending> pending;  // upstream id -> the client's request_id and when it was sent
    bool hello_done = false;
  };
  struct Session {
    int fd = -1;
    ConnId id = 0;
    std::string rx, tx;
    bool handshaken = false, paired = false, closing = false, dead = false, token_retry_used = false;
    std::set<std::string> features;
    std::string client_id, client_name;
    std::int64_t accepted_ms = 0, last_rx_ms = 0, window_start_ms = 0;
    int msgs_in_window = 0;
    Upstream eng, lib;
    std::string update_transfer_id;
    std::string update_bundle_version;
    std::string update_staged_path;
    std::uint64_t update_total_bytes = 0;
    std::uint64_t update_bytes_received = 0;
    bool update_ready = false;
  };

  void run();
  void accept_clients();
  void read_session(Session& s);
  void handle_frame(Session& s, const std::string& payload);
  void handle_hello(Session& s, const ws::v1::ClientMessage& m);
  void handle_hello_again(Session& s, const ws::v1::ClientMessage& m);
  void send_hello_ack(Session& s, std::uint64_t request_id, std::uint32_t version);
  void handle_pair(Session& s, const ws::v1::ClientMessage& m);
  void sweep_pending(Session& s, std::int64_t t);
  void forward_engine(Session& s, ws::v1::ClientMessage m);
  void forward_library(Session& s, const ws::v1::ClientMessage& m);
  void handle_config(Session& s, const std::string& payload);
  void handle_update(Session& s, const ws::v1::ClientMessage& m);
  void cleanup_update_staging(Session& s, bool force = false);
  bool ensure_upstream(Session& s, Upstream& up, bool library);
  void read_upstream(Session& s, Upstream& up, bool library);
  void on_engine_frame(Session& s, const std::string& frame);
  void on_library_frame(Session& s, const std::string& frame);
  void upstream_lost(Session& s, const char* which);
  void send_error(Session& s, std::uint64_t request_id, ws::v1::ErrorCode code, const std::string& msg, const std::map<std::string, std::string>& details = {},
                  bool close_after = false);
  void send(Session& s, const ws::v1::ServerMessage& m, bool close_after = false);
  void deliver(const std::vector<Outbound>& out);
  void flush_fd(int fd, std::string& tx, bool& dead);
  void drop(Session& s);
  Session* find(ConnId id);
  std::int64_t now() const { return opt_.now_ms ? opt_.now_ms() : steady_ms(); }
  static std::int64_t steady_ms();
  void log(const std::string& s) const {
    if (opt_.log) opt_.log(s);
  }

  ConfigService& svc_;
  PairingWindow& window_;
  PairedStore& paired_;
  BridgeOptions opt_;
  int listen_fd_ = -1;
  int wake_[2] = {-1, -1};
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::vector<std::unique_ptr<Session>> sessions_;
  ConnId next_id_ = kBridgeConnBase;
  std::function<void(std::vector<Outbound>)> foreign_;
  std::mutex inbox_mu_;  // guards inbox_ and post_closed_ (and the wake write against stop() closing the pipe)
  std::vector<Outbound> inbox_;
  bool post_closed_ = false;
  std::atomic<std::uint64_t> accepted_{0}, over_capacity_{0}, malformed_{0}, oversize_{0}, paired_sessions_{0}, slow_dropped_{0};
  std::atomic<std::size_t> session_count_{0};
};

}  // namespace velora::sysd

#pragma once
// The ws.v1 protocol of sysd's config service as a pure state machine (Foundation: sysd config service), separate from the sockets so it is tested
// without them (and with a fake loop: every reply and every push is produced synchronously inside handle(), so "pushed within 100 ms" is "pushed
// before handle() returns"; nothing sleeps or reads a clock).
//
// A connection first sends Hello (version N or N-1 of ws.v1, as the engine's control server), then GetConfig / SetConfig / SubscribeConfig. SetConfig is
// serialised (one mutex, one writer): revisions strictly increase and two concurrent SetConfig never interleave. After a change the new full Config is
// pushed (request_id 0) to every subscriber, the requester included when it subscribed, after the requester's own reply.
// Delivery order: handle() returns its outputs in the order they were produced under the mutex; ConfigServer is the single caller (one thread), so
// each subscriber sees revisions in order. Several threads calling handle() directly (tests) each get their own outputs and must order them themselves.
// Everything else (any engine command) is ERROR_CODE_INVALID_COMMAND: this server does not speak it, as the engine does not speak the config messages.
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "velora/sysd/config_store.hpp"

namespace velora::sysd {

using ConnId = std::uint64_t;
// The TCP bridge (bridge_server.hpp) shares one ConfigService with the Unix socket server: its connection ids start here so the two never collide.
constexpr ConnId kBridgeConnBase = ConnId{1} << 40;

struct Outbound {
  ConnId to = 0;
  ws::v1::ServerMessage msg;
  bool close_after = false;  // the connection closes once this message is written
};

class ConfigService {
 public:
  explicit ConfigService(ConfigStore& store) : store_(store) {}

  void open(ConnId c);
  void close(ConnId c);
  // One frame payload (a serialised ClientMessage) from `c`. Replies and pushes are appended to `out` in the order they must be written.
  void handle(ConnId c, const std::string& payload, std::vector<Outbound>& out);

  std::size_t subscribers() const;
  std::uint64_t revision() const;
  ws::v1::Config config() const;

 private:
  struct Conn {
    bool handshaken = false, subscribed = false;
  };
  ws::v1::ServerMessage config_message(std::uint64_t request_id, bool not_durable) const;  // callers hold mu_
  void error(ConnId c, std::uint64_t request_id, ws::v1::ErrorCode code, const std::string& msg, const std::map<std::string, std::string>& details,
             std::vector<Outbound>& out, bool close_after = false) const;

  ConfigStore& store_;
  mutable std::mutex mu_;
  std::map<ConnId, Conn> conns_;
};

}  // namespace velora::sysd

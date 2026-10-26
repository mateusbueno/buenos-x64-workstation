#include "velora/sysd/config_service.hpp"

#include "velora/host/control_plane.hpp"  // check_protocol_version, min_protocol_version

namespace velora::sysd {

namespace {
constexpr std::uint32_t kProtocolVersion = 1;
}

void ConfigService::open(ConnId c) {
  std::lock_guard<std::mutex> lk(mu_);
  conns_[c] = Conn{};
}

void ConfigService::close(ConnId c) {
  std::lock_guard<std::mutex> lk(mu_);
  conns_.erase(c);
}

std::size_t ConfigService::subscribers() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t n = 0;
  for (const auto& kv : conns_) n += kv.second.subscribed ? 1 : 0;
  return n;
}

std::uint64_t ConfigService::revision() const {
  std::lock_guard<std::mutex> lk(mu_);
  return store_.revision();
}

ws::v1::Config ConfigService::config() const {
  std::lock_guard<std::mutex> lk(mu_);
  return store_.config();
}

ws::v1::ServerMessage ConfigService::config_message(std::uint64_t request_id, bool not_durable) const {
  ws::v1::ServerMessage m;
  m.set_request_id(request_id);
  m.set_revision(store_.revision());
  ws::v1::ConfigReply* r = m.mutable_config_reply();
  *r->mutable_config() = store_.config();
  r->set_revision(store_.revision());
  r->set_not_durable(not_durable);
  return m;
}

void ConfigService::error(ConnId c, std::uint64_t request_id, ws::v1::ErrorCode code, const std::string& msg,
                          const std::map<std::string, std::string>& details, std::vector<Outbound>& out, bool close_after) const {
  Outbound o;
  o.to = c;
  o.close_after = close_after;
  o.msg.set_request_id(request_id);
  o.msg.set_revision(store_.revision());
  ws::v1::Error* e = o.msg.mutable_error();
  e->set_code(code);
  e->set_message(msg);
  for (const auto& kv : details) (*e->mutable_details())[kv.first] = kv.second;
  out.push_back(std::move(o));
}

void ConfigService::handle(ConnId c, const std::string& payload, std::vector<Outbound>& out) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = conns_.find(c);
  if (it == conns_.end()) return;
  Conn& conn = it->second;
  ws::v1::ClientMessage msg;
  if (!msg.ParseFromString(payload)) {
    error(c, 0, ws::v1::ERROR_CODE_MALFORMED_MESSAGE, "frame is not a valid ClientMessage", {}, out, !conn.handshaken);
    return;
  }
  using B = ws::v1::ClientMessage;
  if (!conn.handshaken) {
    if (msg.body_case() != B::kHello) {
      error(c, msg.request_id(), ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED, "the first message on a connection must be Hello", {}, out, true);
      return;
    }
    const host::ProtocolCheck pc = host::check_protocol_version(msg.hello().version(), kProtocolVersion);
    if (pc != host::ProtocolCheck::kOk) {
      error(c, msg.request_id(), ws::v1::ERROR_CODE_UNSUPPORTED_VERSION, "unsupported protocol version",
            {{"supported", std::to_string(kProtocolVersion)},
             {"supported_min", std::to_string(host::min_protocol_version(kProtocolVersion))},
             {"reason", pc == host::ProtocolCheck::kTooOld ? "too_old" : "too_new"},
             {"requested", std::to_string(msg.hello().version())}},
            out, true);
      return;
    }
    conn.handshaken = true;
    Outbound o;
    o.to = c;
    o.msg.set_request_id(msg.request_id());
    o.msg.set_revision(store_.revision());
    ws::v1::HelloAck* h = o.msg.mutable_hello_ack();
    h->set_version(msg.hello().version());
    h->add_features("config.v1");
    h->set_revision(store_.revision());
    out.push_back(std::move(o));
    return;
  }
  switch (msg.body_case()) {
    case B::kGetConfig: {
      Outbound o;
      o.to = c;
      o.msg = config_message(msg.request_id(), false);
      out.push_back(std::move(o));
      return;
    }
    case B::kSubscribeConfig: {
      conn.subscribed = msg.subscribe_config().enable();
      Outbound ack;
      ack.to = c;
      ack.msg.set_request_id(msg.request_id());
      ack.msg.set_revision(store_.revision());
      ack.msg.mutable_ack();
      out.push_back(std::move(ack));
      if (conn.subscribed) {  // the current config right away
        Outbound push;
        push.to = c;
        push.msg = config_message(0, false);
        out.push_back(std::move(push));
      }
      return;
    }
    case B::kSetConfig: {
      const ApplyResult r = store_.apply(msg.set_config().patch());
      if (r.status == ApplyStatus::kRejected) {
        error(c, msg.request_id(), r.rejection.code, r.rejection.message, r.rejection.details, out);
        return;
      }
      Outbound reply;
      reply.to = c;
      reply.msg = config_message(msg.request_id(), r.status == ApplyStatus::kNotDurable);
      out.push_back(std::move(reply));
      if (r.changed()) {
        for (const auto& kv : conns_) {
          if (!kv.second.subscribed) continue;
          Outbound push;
          push.to = kv.first;
          push.msg = config_message(0, r.status == ApplyStatus::kNotDurable);
          out.push_back(std::move(push));
        }
      }
      return;
    }
    case B::kHello:
      error(c, msg.request_id(), ws::v1::ERROR_CODE_INVALID_COMMAND, "handshake already done on this connection", {}, out);
      return;
    case B::BODY_NOT_SET:
      error(c, msg.request_id(), ws::v1::ERROR_CODE_INVALID_COMMAND, "no command set", {}, out);
      return;
    default:
      error(c, msg.request_id(), ws::v1::ERROR_CODE_INVALID_COMMAND, "not a sysd command: sysd serves GetConfig, SetConfig and SubscribeConfig", {}, out);
      return;
  }
}

}  // namespace velora::sysd

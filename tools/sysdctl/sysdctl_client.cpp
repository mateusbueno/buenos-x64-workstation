#include "sysdctl_client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace velora::sysdctl {

Connection::~Connection() { close(); }

void Connection::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  rx_.clear();
  queue_.clear();
}

bool Connection::open(const std::string& path, std::string& err, int timeout_ms, std::uint32_t hello_version) {
  close();
  timeout_ms_ = timeout_ms;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) {
    err = "socket path too long: " + path;
    return false;
  }
  std::memcpy(a.sun_path, path.c_str(), path.size());
  fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
  if (fd_ >= 0) {
    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  }
#endif
  if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    err = "cannot connect to " + path + ": " + std::strerror(errno);
    close();
    return false;
  }
  ws::v1::ClientMessage h;
  h.mutable_hello()->set_version(hello_version);
  h.mutable_hello()->set_client_name("velora-sysdctl");
  h.mutable_hello()->add_features("config.v1");
  ws::v1::ServerMessage rep;
  if (!call(h, rep, err)) {
    close();
    return false;
  }
  if (!rep.has_hello_ack()) {
    err = rep.has_error() ? "handshake refused: " + rep.error().message() : "handshake failed";
    close();
    return false;
  }
  return true;
}

bool Connection::read_frame(ws::v1::ServerMessage& m, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (rx_.size() >= 4) {
      const std::uint32_t len = (std::uint8_t(rx_[0]) << 24) | (std::uint8_t(rx_[1]) << 16) | (std::uint8_t(rx_[2]) << 8) | std::uint8_t(rx_[3]);
      if (len > (16u << 20)) return false;
      if (rx_.size() >= 4 + len) {
        const bool ok = m.ParseFromArray(rx_.data() + 4, static_cast<int>(len));
        rx_.erase(0, 4 + len);
        return ok;
      }
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0 || fd_ < 0) return false;
    pollfd pf{fd_, POLLIN, 0};
    const int pr = ::poll(&pf, 1, static_cast<int>(left));
    if (pr < 0 && errno == EINTR) continue;
    if (pr <= 0) return false;
    char buf[4096];
    const ssize_t k = ::recv(fd_, buf, sizeof buf, 0);
    if (k <= 0) return false;
    rx_.append(buf, static_cast<std::size_t>(k));
  }
}

bool Connection::call(ws::v1::ClientMessage m, ws::v1::ServerMessage& reply, std::string& err) {
  if (fd_ < 0) {
    err = "not connected";
    return false;
  }
  m.set_request_id(next_id_++);
  std::string b;
  (void)m.SerializeToString(&b);
  const std::uint32_t n = static_cast<std::uint32_t>(b.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  const std::string out = std::string(h, 4) + b;
  std::size_t off = 0;
  while (off < out.size()) {
    const ssize_t k = ::send(fd_, out.data() + off, out.size() - off, MSG_NOSIGNAL);
    if (k <= 0) {
      err = "send failed";
      return false;
    }
    off += static_cast<std::size_t>(k);
  }
  for (;;) {
    if (!read_frame(reply, timeout_ms_)) {
      err = "no reply";
      return false;
    }
    if (reply.request_id() == m.request_id()) return true;
    if (reply.request_id() == 0) {  // a push that overtook the reply: keep it for next_push()
      std::string pb;
      (void)reply.SerializeToString(&pb);
      const std::uint32_t pn = static_cast<std::uint32_t>(pb.size());
      const char ph[4] = {char(pn >> 24), char(pn >> 16), char(pn >> 8), char(pn)};
      queue_.append(ph, 4);
      queue_.append(pb);
    }
  }
}

bool Connection::get(ws::v1::ConfigReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_get_config();
  ws::v1::ServerMessage rep;
  if (!call(m, rep, err)) return false;
  if (!rep.has_config_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.config_reply();
  return true;
}

bool Connection::set(const ws::v1::ConfigPatch& patch, ws::v1::ConfigReply& out, std::string& err, ws::v1::Error* error) {
  ws::v1::ClientMessage m;
  *m.mutable_set_config()->mutable_patch() = patch;
  ws::v1::ServerMessage rep;
  if (!call(m, rep, err)) return false;
  if (!rep.has_config_reply()) {
    if (rep.has_error()) {
      if (error) *error = rep.error();
      err = ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message();
    } else {
      err = "unexpected reply";
    }
    return false;
  }
  out = rep.config_reply();
  return true;
}

namespace {
bool pedal_reply(Connection& c, ws::v1::ClientMessage m, ws::v1::PedalCalibrationReply& out, std::string& err) {
  ws::v1::ServerMessage rep;
  if (!c.call(std::move(m), rep, err)) return false;
  if (!rep.has_pedal_calibration_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.pedal_calibration_reply();
  return true;
}
}  // namespace

bool Connection::start_pedal_calibration(ws::v1::PedalKind pedal, ws::v1::PedalCalibrationReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_start_pedal_calibration()->set_pedal(pedal);
  return pedal_reply(*this, std::move(m), out, err);
}

bool Connection::finish_pedal_calibration(std::uint64_t run_token, ws::v1::PedalCalibrationReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_finish_pedal_calibration()->set_run_token(run_token);
  return pedal_reply(*this, std::move(m), out, err);
}

bool Connection::cancel_pedal_calibration(std::uint64_t run_token, ws::v1::PedalCalibrationReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_cancel_pedal_calibration()->set_run_token(run_token);
  return pedal_reply(*this, std::move(m), out, err);
}

bool Connection::start_pairing_window(ws::v1::PairingWindowReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_start_pairing_window();
  ws::v1::ServerMessage rep;
  if (!call(std::move(m), rep, err)) return false;
  if (!rep.has_pairing_window_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.pairing_window_reply();
  return true;
}

bool Connection::cancel_pairing_window(ws::v1::PairingWindowReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_cancel_pairing_window();
  ws::v1::ServerMessage rep;
  if (!call(std::move(m), rep, err)) return false;
  if (!rep.has_pairing_window_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.pairing_window_reply();
  return true;
}

bool Connection::get_pairing_status(ws::v1::PairingWindowReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_get_pairing_status();
  ws::v1::ServerMessage rep;
  if (!call(std::move(m), rep, err)) return false;
  if (!rep.has_pairing_window_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.pairing_window_reply();
  return true;
}

bool Connection::list_paired_computers(ws::v1::PairedComputersReply& out, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_list_paired_computers();
  ws::v1::ServerMessage rep;
  if (!call(std::move(m), rep, err)) return false;
  if (!rep.has_paired_computers_reply()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  out = rep.paired_computers_reply();
  return true;
}

bool Connection::forget_paired_computer(const std::string& id, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_forget_paired_computer()->set_id(id);
  ws::v1::ServerMessage rep;
  if (!call(std::move(m), rep, err)) return false;
  if (!rep.has_ack()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  return true;
}

std::string format_pedal_result(const ws::v1::PedalCalibrationReply& r) {
  using R = ws::v1::PedalCalibrationResult;
  const std::string range = " (measured min " + std::to_string(r.min()) + ", max " + std::to_string(r.max()) + ")";
  switch (r.result()) {
    case R::PEDAL_CALIBRATION_RESULT_COLLECTING: return "measuring";
    case R::PEDAL_CALIBRATION_RESULT_STORED:
      return "stored: min " + std::to_string(r.min()) + " max " + std::to_string(r.max()) + " (config revision " + std::to_string(r.config_revision()) + ")" +
             (r.not_durable() ? " [not durable: sysd's directory sync failed]" : "");
    case R::PEDAL_CALIBRATION_RESULT_CANCELLED: return "cancelled: nothing stored";
    case R::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT: return "no movement: the pedal did not move, nothing stored" + (r.min() || r.max() ? range : std::string());
    case R::PEDAL_CALIBRATION_RESULT_TOO_SMALL_SPAN: return "refused: the travel is under 256 counts, nothing stored" + range;
    case R::PEDAL_CALIBRATION_RESULT_TIMED_OUT: return "timed out: the calibration window (60 s) elapsed, nothing stored";
    case R::PEDAL_CALIBRATION_RESULT_NOT_RUNNING: return "not running: no calibration to finish";
    case R::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE: return "sysd unavailable: nothing stored" + (r.detail().empty() ? std::string() : " (" + r.detail() + ")") + range;
    case R::PEDAL_CALIBRATION_RESULT_SYSD_REFUSED: return "sysd refused: nothing stored (" + r.detail() + ")" + range;
    default: return "unspecified result";
  }
}

bool Connection::subscribe(bool enable, std::string& err) {
  ws::v1::ClientMessage m;
  m.mutable_subscribe_config()->set_enable(enable);
  ws::v1::ServerMessage rep;
  if (!call(m, rep, err)) return false;
  if (!rep.has_ack()) {
    err = rep.has_error() ? ws::v1::ErrorCode_Name(rep.error().code()) + ": " + rep.error().message() : "unexpected reply";
    return false;
  }
  return true;
}

bool Connection::next_push(ws::v1::ServerMessage& out, int timeout_ms) {
  if (queue_.size() >= 4) {
    const std::uint32_t len = (std::uint8_t(queue_[0]) << 24) | (std::uint8_t(queue_[1]) << 16) | (std::uint8_t(queue_[2]) << 8) | std::uint8_t(queue_[3]);
    const bool ok = out.ParseFromArray(queue_.data() + 4, static_cast<int>(len));
    queue_.erase(0, 4 + len);
    return ok;
  }
  return read_frame(out, timeout_ms);
}

std::string format(const ws::v1::Config& c, std::uint64_t revision) {
  auto pedal = [](const ws::v1::PedalCalibration& p) {
    return "min=" + std::to_string(p.min()) + " max=" + std::to_string(p.max()) + " polarity=" + ws::v1::PedalPolarity_Name(p.polarity());
  };
  std::string s;
  s += "revision: " + std::to_string(revision) + "\n";
  s += "clock_source: " + ws::v1::ClockSource_Name(c.clock_source()) + "\n";
  s += "velocity_curve: " + ws::v1::VelocityCurve_Name(c.velocity_curve()) + "\n";
  s += "sustain: " + pedal(c.sustain()) + "\n";
  s += "expression: " + pedal(c.expression()) + "\n";
  s += "boot_into: " + ws::v1::BootIntoMode_Name(c.boot_into().mode()) + " default_live_set_id=" + c.boot_into().default_live_set_id() + "\n";
  return s;
}

}  // namespace velora::sysdctl

// Foundation: sysd config service. A blocking ws.v1 client of velora-sysd for the dev CLI and the tests: Hello, GetConfig, SetConfig, SubscribeConfig.
#pragma once

#include <cstdint>
#include <string>

#include "ws_v1.pb.h"

namespace velora::sysdctl {

class Connection {
 public:
  Connection() = default;
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Connects and completes Hello (version `hello_version`). False with `err` set on failure.
  bool open(const std::string& socket_path, std::string& err, int timeout_ms = 3000, std::uint32_t hello_version = 1);
  void close();
  int fd() const noexcept { return fd_; }

  // Sends `m` (request_id is set here) and waits for the reply with that id; pushed messages (request_id 0) that arrive first are queued for next_push().
  bool call(ws::v1::ClientMessage m, ws::v1::ServerMessage& reply, std::string& err);
  bool get(ws::v1::ConfigReply& out, std::string& err);
  // False with `err` when the server answers an error (err = "CODE: message"), `error` then holds it.
  bool set(const ws::v1::ConfigPatch& patch, ws::v1::ConfigReply& out, std::string& err, ws::v1::Error* error = nullptr);
  bool subscribe(bool enable, std::string& err);
  // Story 3.11 (against the ENGINE's socket): StartPedalCalibration / FinishPedalCalibration / CancelPedalCalibration. False with `err` for a typed error reply.
  bool start_pedal_calibration(ws::v1::PedalKind pedal, ws::v1::PedalCalibrationReply& out, std::string& err);
  bool finish_pedal_calibration(std::uint64_t run_token, ws::v1::PedalCalibrationReply& out, std::string& err);
  bool cancel_pedal_calibration(std::uint64_t run_token, ws::v1::PedalCalibrationReply& out, std::string& err);
  // Story 9.6: device-side pairing management (sysd UDS only)
  bool start_pairing_window(ws::v1::PairingWindowReply& out, std::string& err);
  bool cancel_pairing_window(ws::v1::PairingWindowReply& out, std::string& err);
  bool get_pairing_status(ws::v1::PairingWindowReply& out, std::string& err);
  bool list_paired_computers(ws::v1::PairedComputersReply& out, std::string& err);
  bool forget_paired_computer(const std::string& id, std::string& err);
  // Story 8.1 / 8.3: trigger firmware install on the unit (sysd UDS only)
  bool install_update(const std::string& bundle_path, std::string& err, ws::v1::Error* error = nullptr);
  // The next pushed message (request_id 0), from the queue or the socket, waiting up to timeout_ms. False on timeout / loss.
  bool next_push(ws::v1::ServerMessage& out, int timeout_ms);

 private:
  bool read_frame(ws::v1::ServerMessage& m, int timeout_ms);
  int fd_ = -1;
  std::uint64_t next_id_ = 1;
  std::string rx_;
  std::string queue_;  // frames already read as pushes, serialised back to back (length-prefixed)
  int timeout_ms_ = 3000;
};

std::string format(const ws::v1::Config& c, std::uint64_t revision);
// One line for a calibration result ("stored: min 120 max 3890 (config revision 4)", "refused: too small a span ...").
std::string format_pedal_result(const ws::v1::PedalCalibrationReply& r);

}  // namespace velora::sysdctl

#include "velora/host/pedal_calibrator.hpp"

#include <string>

namespace velora::host {

bool pedal_id_from_kind(ws::v1::PedalKind k, PedalId& out) noexcept {
  switch (k) {
    case ws::v1::PEDAL_KIND_SUSTAIN: out = PedalId::kSustain; return true;
    case ws::v1::PEDAL_KIND_EXPRESSION: out = PedalId::kExpression; return true;
    default: return false;
  }
}

namespace {
PedalCal cal_from_config(const ws::v1::PedalCalibration& p) {
  PedalCal c;
  if (p.min() <= kPedalRawMax && p.max() <= kPedalRawMax) {
    c.min = static_cast<std::uint16_t>(p.min());
    c.max = static_cast<std::uint16_t>(p.max());
  }  // else uncalibrated (0 / 0): sysd validates, but the engine does not trust what it reads
  c.direction = p.polarity() == ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED ? PedalDirection::kInverted : PedalDirection::kNormal;
  return c;
}
}  // namespace

void apply_pedal_config(const ws::v1::Config& c, PedalSettings& settings, PedalDirection switch_direction) noexcept {
  settings.set(PedalId::kSustain, cal_from_config(c.sustain()));
  settings.set(PedalId::kExpression, cal_from_config(c.expression()));
  PedalCal sw;
  sw.direction = switch_direction;
  settings.set(PedalId::kSwitch, sw);
}

ws::v1::PedalCalibrationReply PedalCalibrator::start(ws::v1::PedalKind pedal) {
  ws::v1::PedalCalibrationReply r;
  r.set_pedal(pedal);
  PedalId id;
  if (!pedal_id_from_kind(pedal, id)) {
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_UNSPECIFIED);
    return r;
  }
  probe_.begin(id);  // a new generation: anything measured by a run it replaces is ignored
  collecting_ = true;
  pedal_ = pedal;
  started_ns_ = now_();
  token_ = next_token_++;
  r.set_run_token(token_);
  r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_COLLECTING);
  return r;
}

ws::v1::PedalCalibrationReply PedalCalibrator::cancel() {
  ws::v1::PedalCalibrationReply r;
  r.set_pedal(pedal_);
  r.set_result(collecting_ ? ws::v1::PEDAL_CALIBRATION_RESULT_CANCELLED : ws::v1::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
  probe_.cancel();
  collecting_ = false;
  return r;
}

ws::v1::PedalCalibrationReply PedalCalibrator::finish() {
  ws::v1::PedalCalibrationReply r;
  r.set_pedal(pedal_);
  if (!collecting_) {
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_NOT_RUNNING);
    return r;
  }
  const PedalMeasurement m = probe_.read();
  probe_.cancel();
  collecting_ = false;
  if (now_() - started_ns_ > window_ns_) {
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_TIMED_OUT);
    return r;
  }
  if (!m.valid || m.samples == 0 || m.min == m.max) {
    if (m.valid) {
      r.set_min(m.min);
      r.set_max(m.max);
    }
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_NO_MOVEMENT);
    return r;
  }
  r.set_min(m.min);
  r.set_max(m.max);
  if (static_cast<unsigned>(m.max - m.min) < kPedalMinSpan) {
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_TOO_SMALL_SPAN);
    return r;
  }
  if (!writer_) {
    r.set_detail("no sysd link");
    r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE);
    return r;
  }
  ws::v1::ConfigPatch patch;
  ws::v1::PedalPatch* pp = pedal_ == ws::v1::PEDAL_KIND_SUSTAIN ? patch.mutable_sustain() : patch.mutable_expression();
  pp->set_min(m.min);  // only the range: the stored polarity is kept
  pp->set_max(m.max);
  const SysdSetResult w = writer_(patch);
  switch (w.status) {
    case SysdSetResult::Status::kOk:
      r.set_config_revision(w.revision);
      r.set_not_durable(w.not_durable);
      r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_STORED);
      break;
    case SysdSetResult::Status::kRefused:
      r.set_detail(ws::v1::ErrorCode_Name(w.code) + (w.reason.empty() ? "" : ": " + w.reason));
      r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_REFUSED);
      break;
    case SysdSetResult::Status::kNotConnected: r.set_detail("sysd is not connected"); r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE); break;
    case SysdSetResult::Status::kBusy: r.set_detail("another config write is in flight"); r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE); break;
    case SysdSetResult::Status::kTimeout: r.set_detail("sysd did not answer in time"); r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE); break;
    case SysdSetResult::Status::kLost: r.set_detail("the sysd connection was lost"); r.set_result(ws::v1::PEDAL_CALIBRATION_RESULT_SYSD_UNAVAILABLE); break;
  }
  return r;
}

bool PedalCalibrator::run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err) {
  using B = ws::v1::ClientMessage;
  switch (msg.body_case()) {
    case B::kStartPedalCalibration: {
      const ws::v1::PedalKind k = msg.start_pedal_calibration().pedal();
      PedalId id;
      if (!pedal_id_from_kind(k, id)) {
        err.code = ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE;
        err.message = "StartPedalCalibration needs SUSTAIN or EXPRESSION";
        err.details["field"] = "pedal";
        err.details["value"] = std::to_string(static_cast<int>(k));
        return false;
      }
      *reply.mutable_pedal_calibration_reply() = start(k);  // a Start while a run is active replaces it (new token: the old owner can no longer finish it)
      return true;
    }
    case B::kFinishPedalCalibration:
    case B::kCancelPedalCalibration: {
      const bool fin = msg.body_case() == B::kFinishPedalCalibration;
      const std::uint64_t tok = fin ? msg.finish_pedal_calibration().run_token() : msg.cancel_pedal_calibration().run_token();
      if (collecting_ && tok != token_) {  // another client's run: it is not yours to end
        err.code = ws::v1::ERROR_CODE_INVALID_COMMAND;
        err.message = "the calibration that is running belongs to another Start";
        err.details["reason"] = "wrong_run_token";
        return false;
      }
      *reply.mutable_pedal_calibration_reply() = fin ? finish() : cancel();
      return true;
    }
    default:
      err.code = ws::v1::ERROR_CODE_INVALID_COMMAND;
      err.message = "not a pedal calibration command";
      return false;
  }
}

}  // namespace velora::host

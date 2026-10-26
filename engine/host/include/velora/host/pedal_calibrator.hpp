// Pedal calibration routine (Story 3.11, FR42): a command-driven measurement. Design (chosen as the simpler one that keeps the raw measurement on the engine):
//
//   ws.v1 StartPedalCalibration{pedal} -> the engine arms the panel reader's PedalCalibrationProbe (the input thread keeps the raw 12-bit min / max in
//     atomics, no lock on that thread); the player presses the pedal fully and releases it;
//   FinishPedalCalibration -> the control thread reads the measurement, refuses a pedal that did not move or a span below 256 counts (typed, config unchanged),
//     and otherwise writes {min, max} of that pedal to sysd with SysdConfigClient::set_config() (retry-free, typed result; the polarity is kept). sysd validates
//     again, bumps the revision and pushes the config back, which re-applies the calibration from the next frame (no restart);
//   CancelPedalCalibration, a second Start, or no Finish within the window (60 s, checked when the next command arrives) end the run with nothing stored.
//
// The CLI is `velora-sysdctl --engine-socket PATH calibrate-pedal sustain|expression`: prompts, Start, waits for Enter, Finish, prints the typed result.
// Control thread only (the ControlPlane calls run()). The measured values are RAW (before polarity), so a calibration survives a polarity change.
#pragma once

#include <cstdint>
#include <functional>

#include "velora/host/control_plane.hpp"
#include "velora/host/pedal_processor.hpp"
#include "velora/host/sysd_config_client.hpp"
#include "ws_v1.pb.h"

namespace velora::host {

class PedalCalibrator : public PedalCalibrationHandler {
 public:
  using Clock = std::function<std::uint64_t()>;                                // host ns
  using Writer = std::function<SysdSetResult(const ws::v1::ConfigPatch&)>;     // empty: no sysd link
  static constexpr std::uint64_t kWindowNs = 60ull * 1'000'000'000ull;

  PedalCalibrator(PedalCalibrationProbe& probe, Writer writer, Clock now, std::uint64_t window_ns = kWindowNs)
      : probe_(probe), writer_(std::move(writer)), now_(std::move(now)), window_ns_(window_ns) {}

  bool run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err) override;

  // The same operations without the protocol (tests, other front ends).
  ws::v1::PedalCalibrationReply start(ws::v1::PedalKind pedal);
  ws::v1::PedalCalibrationReply finish();
  ws::v1::PedalCalibrationReply cancel();
  bool collecting() const noexcept { return collecting_; }
  std::uint64_t run_token() const noexcept { return token_; }

 private:
  PedalCalibrationProbe& probe_;
  Writer writer_;
  Clock now_;
  std::uint64_t window_ns_;
  bool collecting_ = false;
  ws::v1::PedalKind pedal_ = ws::v1::PEDAL_KIND_UNSPECIFIED;
  std::uint64_t started_ns_ = 0;
  std::uint64_t token_ = 0, next_token_ = 1;  // run token: Start returns it, the protocol's Finish / Cancel must present it
};

// The engine's pedal id for a ws.v1 PedalKind (false for UNSPECIFIED / unknown).
bool pedal_id_from_kind(ws::v1::PedalKind k, PedalId& out) noexcept;

// Applies the sysd config's pedal settings (sustain / expression calibration and polarity) to the reader's settings. Control thread. Values the engine cannot
// trust (min / max above 4095) are treated as uncalibrated; an unspecified polarity is the raw direction. `switch_direction` is the footswitch's wiring (sysd has
// no field for it; Story 3.8's detection will supply it).
void apply_pedal_config(const ws::v1::Config& c, PedalSettings& settings, PedalDirection switch_direction = PedalDirection::kNormal) noexcept;

}  // namespace velora::host

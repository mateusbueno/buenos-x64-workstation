#pragma once
// Story 7.6: External MIDI clock follower running on the RT plane.
//
// Estimates tempo and phase from incoming MIDI clock ticks (0xF8), Start (0xFA), Continue (0xFB), and Stop (0xFC)
// using a critically damped 2nd-order Delay-Locked Loop (DLL) filter.
// - Handles incoming ticks at 24 ppqn (40 ticks in 960 PPQN).
// - Tracks tempo changes within 2 beats (48 ticks) with critical damping to avoid overshoot.
// - Drives TransportClock tempo and phase.
// - Detects clock lost when ticks stop for > 3 ticks at last tempo (minimum 150 ms / 7200 frames at 48 kHz) without a preceding Stop.
// - Resumes clock and smoothly re-locks without double triggers.
// - Transport Stop leaves TransportClock running at last tempo without raising clock_lost.
// - Transport Start/Continue aligns the arpeggiator step grid to the next clock tick.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "velora/core/rt.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

enum class ClockSource : std::uint8_t {
  kInternal = 0,
  kDin = 1,
  kUsb = 2,
};

class ClockFollower {
 public:
  static constexpr float kMinBpm = 20.0f;
  static constexpr float kMaxBpm = 300.0f;
  static constexpr std::int32_t kMinTenths = 200;
  static constexpr std::int32_t kMaxTenths = 3000;
  static constexpr std::int32_t kDefaultTenths = 1200;
  static constexpr float kDefaultBpm = 120.0f;
  static constexpr std::uint32_t kPpqnMidi = 24;
  static constexpr std::uint64_t kMinTimeoutFrames = 7200;  // 150 ms at 48 kHz

  // 2nd-order DLL critically damped filter constants:
  // gamma = 0.18 gives critical damping with double pole at (1 - gamma) = 0.82.
  // Settles within 48 ticks (2 beats at 24 ppqn) to < 0.02 BPM error.
  static constexpr double kGamma = 0.18;
  static constexpr double kAlpha = 2.0 * kGamma - kGamma * kGamma;  // 0.3276
  static constexpr double kBeta = kGamma * kGamma;                  // 0.0324

  ClockFollower() noexcept { reset(); }

  void reset() noexcept VELORA_NONBLOCKING;

  // Process incoming realtime UMP (0xF8 Timing Clock, 0xFA Start, 0xFB Continue, 0xFC Stop).
  void process_ump(const Ump& ump, std::uint64_t frame, TransportClock& clock) noexcept VELORA_NONBLOCKING;

  void on_clock_tick(std::uint64_t frame, TransportClock& clock) noexcept VELORA_NONBLOCKING;
  void on_start(std::uint64_t frame, TransportClock& clock) noexcept VELORA_NONBLOCKING;
  void on_continue(std::uint64_t frame, TransportClock& clock) noexcept VELORA_NONBLOCKING;
  void on_stop(std::uint64_t frame, TransportClock& clock) noexcept VELORA_NONBLOCKING;

  // Advances time by `frames` on the RT plane (evaluates clock-lost timeout).
  void process_block(std::uint32_t frames, TransportClock& clock) noexcept VELORA_NONBLOCKING;

  bool clock_lost() const noexcept VELORA_NONBLOCKING { return clock_lost_; }
  bool locked() const noexcept VELORA_NONBLOCKING { return locked_; }
  bool is_running() const noexcept VELORA_NONBLOCKING { return is_running_; }

  float tempo_bpm() const noexcept VELORA_NONBLOCKING;
  std::int32_t tempo_tenths() const noexcept VELORA_NONBLOCKING { return tempo_tenths_; }
  void set_tempo_tenths(std::int32_t tenths) noexcept VELORA_NONBLOCKING;

  // Estimated period tau in audio frames (clamped to 400..6000 frames)
  double tau() const noexcept VELORA_NONBLOCKING { return tau_; }
  // Estimated phase theta in audio frames
  double theta() const noexcept VELORA_NONBLOCKING { return theta_; }

  // Timeout threshold in frames for clock loss: max(3 * tau, 7200)
  std::uint64_t timeout_frames() const noexcept VELORA_NONBLOCKING;

 private:
  double tau_ = 1000.0;
  double theta_ = 0.0;
  std::int32_t tempo_tenths_ = kDefaultTenths;
  std::int64_t tick_count_ = 0;
  std::uint64_t frames_since_last_tick_ = 0;
  std::uint64_t start_frame_ = 0;
  bool locked_ = false;
  bool first_tick_ = true;
  bool second_tick_ = false;
  bool clock_lost_ = false;
  bool is_running_ = false;
  bool stopped_ = false;
  bool has_start_ = false;
};

}  // namespace velora::core

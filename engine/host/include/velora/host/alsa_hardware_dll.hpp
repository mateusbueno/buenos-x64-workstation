#pragma once

#include <cmath>
#include <cstdint>
#include "velora/core/rt.hpp"
#include "velora/host/time_mapper.hpp"

namespace velora::host {

// Story 1.20: Delay-Locked Loop (DLL) filter tracking the audio hardware pointer.
//
// Converts device time (CLOCK_MONOTONIC_RAW nanoseconds) to audio frame position
// with sub-frame precision and zero drift, mapping events to an exact in-block offset
// after a fixed scheduling delay (default 1 period = 64 frames = 1.333 ms at 48 kHz).
class AlsaHardwareDll {
 public:
  static constexpr std::uint32_t kDefaultSampleRate = 48000;
  static constexpr std::uint32_t kDefaultPeriodFrames = 64;
  static constexpr std::uint32_t kDefaultDelayFrames = 64;     // 1 period fixed scheduling delay
  static constexpr std::uint32_t kDefaultHorizonFrames = 4800; // 100 ms

  // 2nd-order critically damped DLL constants
  static constexpr double kGamma = 0.05;
  static constexpr double kAlpha = 2.0 * kGamma - kGamma * kGamma; // ~0.0975
  static constexpr double kBeta = kGamma * kGamma;                 // ~0.0025

  explicit AlsaHardwareDll(std::uint32_t sample_rate = kDefaultSampleRate,
                           std::uint32_t period_frames = kDefaultPeriodFrames,
                           std::uint32_t delay_frames = kDefaultDelayFrames) noexcept
      : sample_rate_(sample_rate > 0 ? sample_rate : kDefaultSampleRate),
        period_frames_(period_frames > 0 ? period_frames : kDefaultPeriodFrames),
        delay_frames_(delay_frames),
        nominal_tau_(1e9 / static_cast<double>(sample_rate_)),
        tau_(nominal_tau_) {}

  void reset() noexcept {
    locked_ = false;
    first_update_ = true;
    last_hw_frame_ = 0;
    theta_ = 0.0;
    tau_ = nominal_tau_;
  }

  // Update hardware pointer on each period / block boundary from ALSA htstamp (CLOCK_MONOTONIC_RAW ns).
  void update(std::uint64_t hw_frame, std::uint64_t htstamp_ns) noexcept VELORA_NONBLOCKING {
    if (first_update_) {
      first_update_ = false;
      last_hw_frame_ = hw_frame;
      theta_ = static_cast<double>(htstamp_ns);
      tau_ = nominal_tau_;
      locked_ = true;
      return;
    }

    const double delta_frames = static_cast<double>(hw_frame - last_hw_frame_);
    if (delta_frames <= 0.0) return;

    // Predicted time for current hw_frame based on previous model
    const double pred_time = theta_ + delta_frames * tau_;
    const double error = static_cast<double>(htstamp_ns) - pred_time;

    // 2nd-order critically damped update
    theta_ = pred_time + kAlpha * error;
    tau_ += (kBeta * error) / delta_frames;

    // Clamp tau to plausible crystal tolerance (+-1000 ppm)
    const double min_tau = nominal_tau_ * 0.999;
    const double max_tau = nominal_tau_ * 1.001;
    if (tau_ < min_tau) tau_ = min_tau;
    else if (tau_ > max_tau) tau_ = max_tau;

    last_hw_frame_ = hw_frame;
  }

  // Converts CLOCK_MONOTONIC_RAW nanosecond timestamp to audio hardware frame counter.
  int64_t time_to_frame(std::uint64_t time_ns) const noexcept VELORA_NONBLOCKING {
    if (!locked_) {
      return static_cast<int64_t>(time_ns * static_cast<std::uint64_t>(sample_rate_) / 1'000'000'000ull);
    }
    const double diff_ns = static_cast<double>(time_ns) - theta_;
    return static_cast<int64_t>(last_hw_frame_) + static_cast<int64_t>(std::round(diff_ns / tau_));
  }

  // Converts audio hardware frame to estimated CLOCK_MONOTONIC_RAW time.
  std::uint64_t frame_to_time(int64_t frame) const noexcept VELORA_NONBLOCKING {
    if (!locked_) {
      return static_cast<std::uint64_t>(frame * 1'000'000'000ull / sample_rate_);
    }
    const double delta_frames = static_cast<double>(frame - static_cast<int64_t>(last_hw_frame_));
    const double t = theta_ + delta_frames * tau_;
    return t > 0.0 ? static_cast<std::uint64_t>(std::round(t)) : 0ull;
  }

  // Maps an arriving timestamped event to an in-block offset after fixed scheduling delay.
  MappedEvent map_event(std::uint64_t event_ns, std::uint64_t block_start_frame) const noexcept VELORA_NONBLOCKING {
    const int64_t event_frame = time_to_frame(event_ns);
    const int64_t due_frame = event_frame + static_cast<int64_t>(delay_frames_);
    const int64_t offset = due_frame - static_cast<int64_t>(block_start_frame);

    if (offset < 0) {
      return {0, EventTiming::kLate};
    }
    if (offset < static_cast<int64_t>(period_frames_)) {
      return {static_cast<std::uint32_t>(offset), EventTiming::kInBlock};
    }
    if (offset < static_cast<int64_t>(kDefaultHorizonFrames)) {
      return {static_cast<std::uint32_t>(offset), EventTiming::kDeferred};
    }
    return {0, EventTiming::kBeyondHorizon};
  }

  bool locked() const noexcept { return locked_; }
  std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  std::uint32_t period_frames() const noexcept { return period_frames_; }
  std::uint32_t delay_frames() const noexcept { return delay_frames_; }
  void set_delay_frames(std::uint32_t df) noexcept { delay_frames_ = df; }

  double drift_ppm() const noexcept {
    return (tau_ - nominal_tau_) / nominal_tau_ * 1e6;
  }

  double freq_drift_ppm() const noexcept {
    return (nominal_tau_ - tau_) / nominal_tau_ * 1e6;
  }

 private:
  std::uint32_t sample_rate_;
  std::uint32_t period_frames_;
  std::uint32_t delay_frames_;
  double nominal_tau_;
  double tau_;
  double theta_{0.0};
  std::uint64_t last_hw_frame_{0};
  bool locked_{false};
  bool first_update_{true};
};

} // namespace velora::host

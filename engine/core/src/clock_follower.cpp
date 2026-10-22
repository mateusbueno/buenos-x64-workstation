#include "velora/core/clock_follower.hpp"

#include <algorithm>
#include <cmath>

namespace velora::core {

void ClockFollower::reset() noexcept {
  tau_ = 1000.0;  // 120.0 BPM
  tempo_tenths_ = kDefaultTenths;
  theta_ = 0.0;
  locked_ = false;
  first_tick_ = true;
  second_tick_ = false;
  clock_lost_ = false;
  is_running_ = false;
  stopped_ = false;
  has_start_ = false;
  start_frame_ = 0;
  frames_since_last_tick_ = 0;
  tick_count_ = 0;
}

void ClockFollower::set_tempo_tenths(std::int32_t tenths) noexcept {
  tempo_tenths_ = std::clamp(tenths, kMinTenths, kMaxTenths);
  tau_ = std::clamp(1200000.0 / static_cast<double>(tempo_tenths_), 400.0, 6000.0);
}

float ClockFollower::tempo_bpm() const noexcept {
  return std::clamp(120000.0f / static_cast<float>(tau_), kMinBpm, kMaxBpm);
}

std::uint64_t ClockFollower::timeout_frames() const noexcept {
  const auto t3 = static_cast<std::uint64_t>(std::lround(3.0 * tau_));
  return std::max(t3, kMinTimeoutFrames);
}

void ClockFollower::process_block(std::uint32_t frames, TransportClock& clock) noexcept {
  (void)clock;
  frames_since_last_tick_ += frames;
  if (!stopped_ && locked_ && frames_since_last_tick_ > timeout_frames()) {
    clock_lost_ = true;
  }
}

void ClockFollower::on_start(std::uint64_t frame, TransportClock& clock) noexcept {
  is_running_ = true;
  stopped_ = false;
  clock_lost_ = false;
  frames_since_last_tick_ = 0;
  tick_count_ = 0;
  has_start_ = true;
  start_frame_ = frame;
  if (!locked_) {
    first_tick_ = true;
    second_tick_ = false;
  }
  theta_ = static_cast<double>(frame);
  const std::uint32_t in_block = static_cast<std::uint32_t>(frame >= clock.frames() ? (frame - clock.frames()) : 0);
  clock.align_to_tick(0, in_block);
}

void ClockFollower::on_continue(std::uint64_t frame, TransportClock& clock) noexcept {
  is_running_ = true;
  stopped_ = false;
  clock_lost_ = false;
  frames_since_last_tick_ = 0;
  const std::uint32_t in_block = static_cast<std::uint32_t>(frame >= clock.frames() ? (frame - clock.frames()) : 0);
  const std::int64_t cur_tick = clock.ticks_at(in_block);
  const std::int64_t next_clock_tick = ((cur_tick + 39) / 40) * 40;
  tick_count_ = (next_clock_tick / 40) - 1;
  clock.align_to_tick(next_clock_tick, in_block);
  theta_ = static_cast<double>(frame);
  first_tick_ = false;
  second_tick_ = false;
}

void ClockFollower::on_stop(std::uint64_t frame, TransportClock& clock) noexcept {
  (void)frame;
  (void)clock;
  is_running_ = false;
  stopped_ = true;
  clock_lost_ = false;
  has_start_ = false;
  frames_since_last_tick_ = 0;
}

void ClockFollower::on_clock_tick(std::uint64_t frame, TransportClock& clock) noexcept {
  frames_since_last_tick_ = 0;
  stopped_ = false;
  const bool was_lost = clock_lost_;
  clock_lost_ = false;

  if (has_start_ && frame <= start_frame_ + 24) {
    has_start_ = false;
    theta_ = static_cast<double>(frame);
    return;
  }
  has_start_ = false;

  if (first_tick_ || was_lost || !locked_) {
    theta_ = static_cast<double>(frame);
    first_tick_ = false;
    second_tick_ = !was_lost && !locked_;
    locked_ = true;
    const std::int64_t in_block = static_cast<std::int64_t>(frame) - static_cast<std::int64_t>(clock.frames());
    const std::int64_t cur_tick = clock.ticks_at(static_cast<std::uint32_t>(std::max(std::int64_t(0), in_block)));
    tick_count_ = (cur_tick + 20) / 40;
    const std::int64_t ideal_units = tick_count_ * 1200000;
    const std::int64_t target_units = static_cast<std::int64_t>(
        std::lround(static_cast<double>(ideal_units) - (theta_ - static_cast<double>(clock.frames())) * tempo_tenths_));
    if (target_units > clock.position_units()) {
      clock.set_position_units(target_units);
    }
    clock.set_tempo_tenths(tempo_tenths_);
    return;
  }

  if (second_tick_) {
    second_tick_ = false;
    const double dt = static_cast<double>(frame) - theta_;
    if (dt >= 350.0 && dt <= 6500.0) {
      tau_ = std::clamp(dt, 400.0, 6000.0);
      tempo_tenths_ = std::clamp(static_cast<std::int32_t>(std::lround(1200000.0 / tau_)), kMinTenths, kMaxTenths);
      clock.set_tempo_tenths(tempo_tenths_);
    }
    theta_ = static_cast<double>(frame);
    ++tick_count_;
    const std::int64_t ideal_units = tick_count_ * 1200000;
    const std::int64_t target_units = static_cast<std::int64_t>(
        std::lround(static_cast<double>(ideal_units) - (theta_ - static_cast<double>(clock.frames())) * tempo_tenths_));
    if (target_units > clock.position_units()) {
      clock.set_position_units(target_units);
    }
    return;
  }

  // 2nd-order critically damped DLL update
  const double pred = theta_ + tau_;
  const double e = static_cast<double>(frame) - pred;
  theta_ = pred + kAlpha * e;
  tau_ = std::clamp(tau_ + kBeta * e, 400.0, 6000.0);

  tempo_tenths_ = std::clamp(static_cast<std::int32_t>(std::lround(1200000.0 / tau_)), kMinTenths, kMaxTenths);
  clock.set_tempo_tenths(tempo_tenths_);

  ++tick_count_;
  const std::int64_t ideal_units = tick_count_ * 1200000;
  const std::int64_t target_units = static_cast<std::int64_t>(
      std::lround(static_cast<double>(ideal_units) - (theta_ - static_cast<double>(clock.frames())) * tempo_tenths_));
  if (target_units > clock.position_units()) {
    clock.set_position_units(target_units);
  }
}

void ClockFollower::process_ump(const Ump& ump, std::uint64_t frame, TransportClock& clock) noexcept {
  if (ump.is_timing_clock()) {
    on_clock_tick(frame, clock);
  } else if (ump.is_start()) {
    on_start(frame, clock);
  } else if (ump.is_continue()) {
    on_continue(frame, clock);
  } else if (ump.is_stop()) {
    on_stop(frame, clock);
  }
}

}  // namespace velora::core

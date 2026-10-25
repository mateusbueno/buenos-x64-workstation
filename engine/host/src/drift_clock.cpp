#include "velora/host/drift_clock.hpp"

#include <algorithm>
#include <cmath>

namespace velora::host {

// ---- DriftClock ---------------------------------------------------------------------------------------------------

DriftClock::DriftClock(double nominal_hz, std::size_t window) noexcept
    : nominal_ns_per_tick_(1e9 / nominal_hz), window_(window < 2 ? 2 : window), slope_(1e9 / nominal_hz) {}

void DriftClock::reset() noexcept {
  samples_.clear();
  have_ref_ = false;
  valid_ = false;
  min_rtt_ = ~0ull;
  rtt_rejects_in_row_ = 0;
  slope_ = nominal_ns_per_tick_;
  dx_ = dy_ = 0;
}

std::int64_t DriftClock::unwrap(std::uint32_t tick) noexcept {
  if (!have_ref_) {
    ref_ = (std::int64_t{1} << 32) + tick;  // keep room below for slightly earlier ticks
    have_ref_ = true;
    return ref_;
  }
  const std::int32_t d = static_cast<std::int32_t>(tick - static_cast<std::uint32_t>(ref_));
  const std::int64_t u = ref_ + d;
  if (d > 0) ref_ = u;
  return u;
}

void DriftClock::bootstrap(std::uint32_t tick, std::uint64_t host_ns) noexcept {
  if (valid_) return;
  base_tick_ = unwrap(tick);
  base_ns_ = static_cast<std::int64_t>(host_ns);
  dx_ = dy_ = 0;
  slope_ = nominal_ns_per_tick_;
  valid_ = true;
}

bool DriftClock::add_sample(std::uint32_t tick, std::uint64_t mid_ns, std::uint64_t rtt_ns) noexcept {
  const std::int64_t u = unwrap(tick);  // advances the reference even when the sample is rejected
  if (rtt_ns < min_rtt_) min_rtt_ = rtt_ns;
  if (rtt_ns > 3 * min_rtt_ + 50'000) {  // a delayed round trip says little about the midpoint
    ++rejected_;
    if (++rtt_rejects_in_row_ >= 8) {      // the link got slower for good: re-learn the best round trip
      min_rtt_ = rtt_ns;
      rtt_rejects_in_row_ = 0;
    } else {
      return false;
    }
  } else {
    rtt_rejects_in_row_ = 0;
  }
  if (!samples_.empty() && u <= samples_.back().utick) return false;  // not newer: unusable for a fit
  samples_.push_back({u, mid_ns});
  while (samples_.size() > window_) samples_.pop_front();
  refit();
  valid_ = true;
  return true;
}

bool DriftClock::went_backwards(std::uint32_t tick, std::uint32_t tolerance_ticks) const noexcept {
  if (!have_ref_) return false;
  const std::int32_t d = static_cast<std::int32_t>(tick - static_cast<std::uint32_t>(ref_));
  return d < 0 && static_cast<std::int64_t>(-static_cast<std::int64_t>(d)) > static_cast<std::int64_t>(tolerance_ticks);
}

void DriftClock::refit() noexcept {
  const Sample last = samples_.back();
  const double n = static_cast<double>(samples_.size());
  double sx = 0, sy = 0;
  for (const Sample& s : samples_) {
    sx += static_cast<double>(s.utick - last.utick);
    sy += static_cast<double>(static_cast<std::int64_t>(s.ns) - static_cast<std::int64_t>(last.ns));
  }
  const double mx = sx / n, my = sy / n;
  double sxx = 0, sxy = 0;
  for (const Sample& s : samples_) {
    const double x = static_cast<double>(s.utick - last.utick) - mx;
    const double y = static_cast<double>(static_cast<std::int64_t>(s.ns) - static_cast<std::int64_t>(last.ns)) - my;
    sxx += x * x;
    sxy += x * y;
  }
  const double span_ns = -static_cast<double>(samples_.front().utick - last.utick) * nominal_ns_per_tick_;
  double slope = nominal_ns_per_tick_;
  if (samples_.size() >= 4 && span_ns >= 1e9 && sxx > 0) slope = sxy / sxx;  // enough baseline to measure drift
  slope = std::clamp(slope, nominal_ns_per_tick_ * 0.99, nominal_ns_per_tick_ * 1.01);
  slope_ = slope;
  base_tick_ = last.utick;
  base_ns_ = static_cast<std::int64_t>(last.ns);
  dx_ = mx;
  dy_ = my;
}

std::uint64_t DriftClock::to_host_ns(std::int64_t utick) const noexcept {
  const double ns = static_cast<double>(base_ns_) + dy_ + slope_ * (static_cast<double>(utick - base_tick_) - dx_);
  return ns <= 0 ? 0 : static_cast<std::uint64_t>(std::llround(ns));
}

double DriftClock::drift_ppm() const noexcept { return (nominal_ns_per_tick_ / slope_ - 1.0) * 1e6; }

// ---- TimeSyncLink -------------------------------------------------------------------------------------------------

TimeSyncLink::TimeSyncLink(TimeSyncConfig cfg) : cfg_(cfg), clock_((cfg.tick_hz > 0.0 && std::isfinite(cfg.tick_hz)) ? cfg.tick_hz : 1'000'000.0) {
  if (!(cfg_.tick_hz > 0.0) || !std::isfinite(cfg_.tick_hz)) cfg_.tick_hz = 1'000'000.0;
  // a 32-bit tick spans 2^32 / tick_hz seconds; beyond half of it the unwrap is ambiguous (90% for margin)
  max_gap_ns_ = static_cast<std::uint64_t>(2147483648.0 / cfg_.tick_hz * 1e9 * 0.9);
}

void TimeSyncLink::note_rx(std::uint64_t rx_ns) noexcept {
  if (have_rx_ && clock_.valid() && rx_ns > last_rx_ns_ && rx_ns - last_rx_ns_ > max_gap_ns_) {
    clock_.reset();  // silent for so long that the 32-bit tick cannot be unwrapped: start over from this frame
    outstanding_ = false;
  }
  have_rx_ = true;
  last_rx_ns_ = rx_ns;
}

std::uint64_t TimeSyncLink::convert(std::uint32_t tick, std::uint64_t rx_ns) {
  if (!clock_.valid()) clock_.bootstrap(tick, rx_ns);
  if (clock_.samples() == 0) ++c_.unsynced_events;
  return clock_.to_host_ns(clock_.unwrap(tick));
}

void TimeSyncLink::on_reply(const mcu::TimeSync& ts, std::uint64_t rx_ns) {
  if (!outstanding_ || ts.token != token_ || rx_ns < sent_ns_) {
    ++c_.stray;
    return;
  }
  outstanding_ = false;
  ++c_.replies;
  const std::uint64_t rtt = rx_ns - sent_ns_;
  clock_.add_sample(ts.tick, sent_ns_ + rtt / 2, rtt);
}

void TimeSyncLink::poll(std::uint64_t now_ns) {
  if (outstanding_ && now_ns >= sent_ns_ && now_ns - sent_ns_ >= cfg_.sync_timeout_ns) {
    ++c_.missed;  // keep the last offset and drift
    outstanding_ = false;
  }
  if (!tx_ || outstanding_ || now_ns < next_sync_ns_) return;
  ++token_;
  const auto wire = writer_.write(mcu::TimeSync{false, token_, 0}, static_cast<std::uint32_t>(now_ns / 1000));
  outstanding_ = true;
  sent_ns_ = now_ns;
  ++c_.requests;
  next_sync_ns_ = now_ns + (clock_.samples() < cfg_.sync_fast_samples ? cfg_.sync_fast_interval_ns : cfg_.sync_interval_ns);
  tx_(wire.data(), wire.size());
}

bool TimeSyncLink::transmit(const mcu::Message& m, std::uint32_t tick) {
  if (!tx_) return false;
  const auto wire = writer_.write(m, tick);
  if (wire.empty()) return false;
  tx_(wire.data(), wire.size());
  return true;
}

void TimeSyncLink::restart() noexcept {
  clock_.reset();
  outstanding_ = false;
  next_sync_ns_ = 0;
}

}  // namespace velora::host

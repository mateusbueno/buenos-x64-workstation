// Shared MCU link clock (Stories 3.5 and 3.10, AR16): MCU tick -> host ns by TIME_SYNC round trips, used by the keybed reader and the
// panel reader alike. Input-edge-thread code, no hardware I/O; "now" always comes from the caller so it is deterministic under test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "velora/host/mcu_frame.hpp"

namespace velora::host {

// Estimates MCU tick -> host ns from TIME_SYNC round trips: a least-squares line over a sliding window of samples
// (offset and drift), the nominal tick rate until the window is long enough to measure drift.
class DriftClock {
 public:
  explicit DriftClock(double nominal_hz = 1'000'000.0, std::size_t window = 32) noexcept;
  bool valid() const noexcept { return valid_; }
  std::size_t samples() const noexcept { return samples_.size(); }
  // Widens a 32-bit MCU tick to a 64-bit tick near the newest one seen (wrap-safe, tolerates small reordering).
  std::int64_t unwrap(std::uint32_t tick) noexcept;
  // First contact with the MCU: tick `tick` happened about `host_ns` (used until a TIME_SYNC reply arrives).
  void bootstrap(std::uint32_t tick, std::uint64_t host_ns) noexcept;
  // A TIME_SYNC round trip: the MCU built its reply at `tick`, which is taken as the midpoint `mid_ns` of the round trip of
  // length `rtt_ns`. Returns false when the sample is rejected (round trip far above the best seen).
  bool add_sample(std::uint32_t tick, std::uint64_t mid_ns, std::uint64_t rtt_ns) noexcept;
  // Pure conversion with the current estimate.
  std::uint64_t to_host_ns(std::int64_t utick) const noexcept;
  double drift_ppm() const noexcept;  // estimated MCU clock error vs nominal
  std::uint64_t rejected() const noexcept { return rejected_; }
  // True when `tick` is earlier than the newest tick seen by more than `tolerance_ticks` (an MCU restart, not reordering).
  bool went_backwards(std::uint32_t tick, std::uint32_t tolerance_ticks) const noexcept;
  void reset() noexcept;

 private:
  struct Sample { std::int64_t utick; std::uint64_t ns; };
  void refit() noexcept;
  double nominal_ns_per_tick_;
  std::size_t window_;
  std::deque<Sample> samples_;
  std::int64_t ref_ = 0;         // newest unwrapped tick seen
  bool have_ref_ = false;
  bool valid_ = false;
  std::int64_t base_tick_ = 0;   // fit anchor: host_ns(u) = base_ns_ + dy_ + slope_ * ((u - base_tick_) - dx_)
  std::int64_t base_ns_ = 0;
  double dx_ = 0, dy_ = 0, slope_;
  std::uint64_t min_rtt_ = ~0ull;
  std::uint32_t rtt_rejects_in_row_ = 0;
  std::uint64_t rejected_ = 0;
};


struct TimeSyncConfig {
  double tick_hz = 1'000'000.0;                          // nominal MCU tick rate (> 0, finite)
  std::uint64_t sync_interval_ns = 1'000'000'000;       // steady TIME_SYNC period
  std::uint64_t sync_fast_interval_ns = 100'000'000;    // period while the clock has few samples
  std::size_t sync_fast_samples = 16;
  std::uint64_t sync_timeout_ns = 500'000'000;          // a request unanswered this long counts as a missed sync
};

struct TimeSyncCounters {
  std::uint64_t requests = 0, replies = 0, missed = 0, stray = 0, unsynced_events = 0;
};

// One MCU link's clock: the DriftClock plus the TIME_SYNC request / reply bookkeeping, the silent-link reset and tick conversion.
class TimeSyncLink {
 public:
  using TxFn = std::function<void(const std::uint8_t* data, std::size_t len)>;
  explicit TimeSyncLink(TimeSyncConfig cfg = {});
  void set_tx(TxFn tx) { tx_ = std::move(tx); }
  // Every decoded frame, received at `rx_ns`: a silence so long that the 32-bit tick cannot be unwrapped restarts the clock.
  void note_rx(std::uint64_t rx_ns) noexcept;
  // A frame tick -> host ns (bootstraps the clock from this frame when it has nothing yet).
  std::uint64_t convert(std::uint32_t tick, std::uint64_t rx_ns);
  // A TIME_SYNC reply received at `rx_ns` (stray ones are counted and ignored).
  void on_reply(const mcu::TimeSync& ts, std::uint64_t rx_ns);
  // Sends requests when due, counts missed syncs.
  void poll(std::uint64_t now_ns);
  // Sends any other frame on the same link with the link's own frame numbering (Story 3.12: LED_SET). False without a tx hook or for an invalid message.
  // Not thread-safe with poll(): the owner serialises the two.
  bool transmit(const mcu::Message& m, std::uint32_t tick);
  // The MCU restarted: its ticks start over.
  void restart() noexcept;
  const DriftClock& clock() const noexcept { return clock_; }
  DriftClock& clock() noexcept { return clock_; }
  TimeSyncCounters counters() const noexcept { return c_; }
  double tick_hz() const noexcept { return cfg_.tick_hz; }

 private:
  TimeSyncConfig cfg_;
  DriftClock clock_;
  mcu::FrameWriter writer_;
  TxFn tx_;
  TimeSyncCounters c_;
  std::uint64_t last_rx_ns_ = 0;
  bool have_rx_ = false;
  std::uint64_t max_gap_ns_ = 0;  // silence after which the 32-bit tick can no longer be unwrapped
  bool outstanding_ = false;
  std::uint32_t token_ = 0;
  std::uint64_t sent_ns_ = 0;
  std::uint64_t next_sync_ns_ = 0;
};

}  // namespace velora::host

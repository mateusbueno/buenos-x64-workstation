// Keybed reader core (Story 3.5, FR5 / FR11 / FR73 / AR16): input-edge-thread logic with no hardware I/O.
//
//   MCU bytes -> StreamDecoder -> KEY_CONTACT frames -> device time (drift-tracked) -> per-key state machine
//             -> contact_k per-note controllers + Note On / Note Off UMP -> HostEventQueue (SPSC) -> RT plane
//
// The reader touches the RT plane only through the HostEventQueue (StampedUmp with host_ns, CLOCK_MONOTONIC_RAW ns).
// Every time it needs "now" the caller passes it in (rx_ns / now_ns), so tick conversion and the whole reader are
// deterministic under test. Single thread (the keybed input thread); allocates (frame decode), never runs on the RT thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include "velora/core/ump.hpp"
#include "velora/host/keybed_calibration.hpp"
#include "velora/host/mcu_frame.hpp"
#include "velora/host/midi_ingest.hpp"

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

struct KeybedReaderConfig {
  std::uint32_t group = 0;                      // UMP group (the core serves group 0)
  std::uint32_t channel = 0;                    // organ part
  double tick_hz = 1'000'000.0;                 // nominal MCU tick rate
  std::uint64_t sync_interval_ns = 1'000'000'000;       // steady TIME_SYNC period
  std::uint64_t sync_fast_interval_ns = 100'000'000;    // period while the clock has few samples
  std::size_t sync_fast_samples = 16;
  std::uint64_t sync_timeout_ns = 500'000'000;  // a request unanswered this long counts as a missed sync
  std::uint16_t release_velocity16 = 0x4000;    // fixed release velocity (3.6 / 3.7 refine it)
};

struct KeybedReaderCounters {
  std::uint64_t frames = 0, crc_errors = 0, framing_errors = 0, seq_gaps = 0, seq_lost = 0, version_rejects = 0, rejected_frames = 0;
  std::uint64_t ignored_frames = 0;       // frames of other types (panel controls, aftertouch, ...)
  std::uint64_t key_frames = 0, contact_events = 0, note_ons = 0, note_offs = 0;
  std::uint64_t unknown_key = 0, bad_contact = 0, duplicate_closes = 0, spurious_opens = 0;
  std::uint64_t recovered_opens = 0;      // open events synthesised because the real one was lost
  std::uint64_t velocity_fallbacks = 0;   // Note On whose velocity pair start was never seen (slowest velocity used)
  std::uint64_t sync_requests = 0, sync_replies = 0, missed_syncs = 0, sync_stray = 0, sync_rejected = 0;
  std::uint64_t unsynced_events = 0;      // events converted before the first TIME_SYNC reply (bootstrap clock)
  std::uint64_t clock_clamps = 0;         // converted times raised to keep events in order
  std::uint64_t queue_full = 0;
  std::uint64_t mcu_resets = 0, contact_mismatch = 0;
};

class KeybedReader {
 public:
  using TxFn = std::function<void(const std::uint8_t* data, std::size_t len)>;
  // `cfg` is sanitised: tick_hz <= 0 or NaN becomes 1 MHz, group and channel are clamped to 0..15.
  KeybedReader(HostEventQueue& out, KeybedCalibration table, KeybedReaderConfig cfg = {});
  KeybedReader(const KeybedReader&) = delete;
  KeybedReader& operator=(const KeybedReader&) = delete;
  KeybedReader(KeybedReader&&) = delete;
  KeybedReader& operator=(KeybedReader&&) = delete;

  // Where TIME_SYNC requests go (the MCU link). Without it no requests are sent.
  void set_tx(TxFn tx) { tx_ = std::move(tx); }
  // Replace the (provisional) velocity mapper; not owned, must outlive the reader. nullptr restores the linear default.
  void set_velocity_mapper(const VelocityMapper* m) noexcept { mapper_ = m ? m : &default_mapper_; }
  // Swaps the calibration table (data change, no code change). Sounding keys are released first.
  void set_calibration(KeybedCalibration table, std::uint64_t now_ns);

  // Bytes from the MCU link, received at host time `rx_ns` (CLOCK_MONOTONIC_RAW ns).
  void on_bytes(const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns);
  // Periodic housekeeping: sends TIME_SYNC requests, counts missed syncs.
  void poll(std::uint64_t now_ns);
  // Authoritative key-state snapshot (a periodic key-state check): `closed_mask` bit c = contact c closed. Contacts the
  // reader believes closed but the snapshot shows open are released (contact_k open, Note Off), so no note sticks.
  void report_key_state(std::uint8_t key, std::uint8_t closed_mask, std::uint64_t now_ns);
  // Releases every key that is down (e.g. link lost); returns how many keys were released.
  std::size_t release_all(std::uint64_t now_ns);
  // True after a CRC error, framing error or seq gap until a report_key_state / release_all: events may have been lost.
  bool recovery_pending() const noexcept { return recovery_pending_; }

  KeybedReaderCounters counters() const;
  const DriftClock& clock() const noexcept { return clock_; }
  const mcu::Hello* hello() const noexcept { return have_hello_ ? &hello_ : nullptr; }
  bool key_sounding(std::uint8_t key) const noexcept { return key < keys_.size() && keys_[key].sounding; }
  std::uint8_t key_mask(std::uint8_t key) const noexcept { return key < keys_.size() ? keys_[key].mask : 0; }
  const KeybedCalibration& calibration() const noexcept { return table_; }

 private:
  struct KeyState {
    std::uint8_t mask = 0;
    bool sounding = false;
    std::uint64_t vel_start_ns = 0;  // start of the running velocity measurement (valid when vel_start_valid)
    bool vel_start_valid = false;
    std::uint64_t last_close_ns[3] = {0, 0, 0};
  };
  void handle(const mcu::Frame& f, std::uint64_t rx_ns);
  void on_key_contact(const mcu::KeyContact& kc, std::uint64_t t);
  void on_time_sync(const mcu::TimeSync& ts, std::uint64_t rx_ns);
  void on_hello(const mcu::Hello& h, std::uint32_t tick, std::uint64_t rx_ns);
  void do_open(std::uint8_t key, std::uint8_t contact, std::uint64_t t);
  void release_key(std::uint8_t key, std::uint64_t t);
  void emit(const core::Ump& u, std::uint64_t t);
  std::uint64_t convert(std::uint32_t tick, std::uint64_t rx_ns);

  HostEventQueue& out_;
  KeybedCalibration table_;
  KeybedReaderConfig cfg_;
  mcu::StreamDecoder decoder_;
  mcu::FrameWriter writer_;
  DriftClock clock_;
  LinearVelocityMapper default_mapper_;
  const VelocityMapper* mapper_ = &default_mapper_;
  TxFn tx_;
  std::vector<KeyState> keys_;
  KeybedReaderCounters c_;
  std::uint64_t last_out_ns_ = 0;
  std::uint64_t last_rx_ns_ = 0;
  bool have_rx_ = false;
  std::uint64_t max_gap_ns_ = 0;  // silence after which the 32-bit tick can no longer be unwrapped
  bool recovery_pending_ = false;
  // time sync
  bool outstanding_ = false;
  std::uint32_t token_ = 0;
  std::uint64_t sent_ns_ = 0;
  std::uint64_t next_sync_ns_ = 0;
  mcu::Hello hello_{};
  bool have_hello_ = false;
  std::vector<mcu::Frame> frames_;  // scratch
};

}  // namespace velora::host

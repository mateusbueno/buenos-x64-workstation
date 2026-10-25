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
#include "velora/host/drift_clock.hpp"
#include "velora/host/keybed_calibration.hpp"
#include "velora/host/mcu_frame.hpp"
#include "velora/host/midi_ingest.hpp"

namespace velora::host {

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
  using TxFn = TimeSyncLink::TxFn;
  // `cfg` is sanitised: tick_hz <= 0 or NaN becomes 1 MHz, group and channel are clamped to 0..15.
  KeybedReader(HostEventQueue& out, KeybedCalibration table, KeybedReaderConfig cfg = {});
  KeybedReader(const KeybedReader&) = delete;
  KeybedReader& operator=(const KeybedReader&) = delete;
  KeybedReader(KeybedReader&&) = delete;
  KeybedReader& operator=(KeybedReader&&) = delete;

  // Where TIME_SYNC requests go (the MCU link). Without it no requests are sent.
  void set_tx(TxFn tx) { link_.set_tx(std::move(tx)); }
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
  const DriftClock& clock() const noexcept { return link_.clock(); }
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

  HostEventQueue& out_;
  KeybedCalibration table_;
  KeybedReaderConfig cfg_;
  mcu::StreamDecoder decoder_;
  TimeSyncLink link_;
  LinearVelocityMapper default_mapper_;
  const VelocityMapper* mapper_ = &default_mapper_;
  std::vector<KeyState> keys_;
  KeybedReaderCounters c_;
  std::uint64_t last_out_ns_ = 0;
  bool recovery_pending_ = false;
  mcu::Hello hello_{};
  bool have_hello_ = false;
  std::vector<mcu::Frame> frames_;  // scratch
};

}  // namespace velora::host

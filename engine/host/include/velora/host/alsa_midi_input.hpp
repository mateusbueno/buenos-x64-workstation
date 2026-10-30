#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "velora/core/spsc_queue.hpp"
#include "velora/core/ump.hpp"
#include "velora/ports/midi_input.hpp"

namespace velora::host {

// Story 1.20: ALSA USB MIDI input adapter with timestamping and hotplug/unplug note release.
//
// - Connects to ALSA UMP (MIDI 2.0) or converts legacy MIDI 1.0 byte stream to UMP.
// - Stamps every event on arrival with CLOCK_MONOTONIC_RAW nanoseconds on an input edge thread.
// - Pushes to UmpSink (HostEventQueue SPSC queue).
// - On disconnect/unplug: releases all held notes to guarantee zero stuck notes.
// - On reconnect/rescan: safely re-attaches without disturbing audio.
class AlsaMidiInput final : public ports::MidiInput {
 public:
  explicit AlsaMidiInput(std::string device_name = "default");
  ~AlsaMidiInput() override;

  AlsaMidiInput(const AlsaMidiInput&) = delete;
  AlsaMidiInput& operator=(const AlsaMidiInput&) = delete;

  bool open(ports::UmpSink& sink, std::string& error) override;
  std::size_t rescan() override;
  std::size_t connected_sources() const override { return connected_ ? 1 : 0; }
  void close() override;

  // Simulate or signal device disconnect (e.g. USB yank)
  void disconnect() noexcept;

  // Direct injection for test harness or mock ALSA driver
  void inject_raw_bytes(const std::uint8_t* bytes, std::size_t count, std::uint64_t host_ns) noexcept;
  void inject_ump_packet(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept;

  std::size_t active_held_notes() const noexcept { return held_count_.load(std::memory_order_relaxed); }
  bool is_connected() const noexcept { return connected_.load(std::memory_order_relaxed); }

  // Returns monotonic nanoseconds using CLOCK_MONOTONIC_RAW on Linux or continuous monotonic time on Darwin
  static std::uint64_t now_monotonic_raw_ns() noexcept;

 private:
  void release_all_held_notes(std::uint64_t release_time_ns) noexcept;
  void track_note_state(const core::Ump& ump, bool is_on) noexcept;

  std::string device_name_;
  ports::UmpSink* sink_{nullptr};
  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::size_t> held_count_{0};

  // Tracking for currently held notes (16 channels x 128 notes)
  std::array<std::uint8_t, 16 * 128> held_notes_{};

  // Parser state for legacy MIDI 1.0 stream conversion to UMP
  std::uint8_t running_status_{0};
  std::uint8_t pending_bytes_[3]{};
  std::size_t pending_len_{0};
};

} // namespace velora::host

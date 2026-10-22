#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "velora/core/spsc_queue.hpp"
#include "velora/core/ump.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/host/midi_ingest.hpp"

namespace velora::host {

struct SysexReceiverCounters {
  std::atomic<std::uint64_t> sysex_valid_count{0};    // valid 6-OP dumps parsed
  std::atomic<std::uint64_t> sysex_damaged_count{0};  // damaged, malformed or truncated 6-OP dumps
  std::atomic<std::uint64_t> sysex_ignored_count{0};  // non-6-OP SysEx dumps (ignored silently)
};

class SysexReceiver {
 public:
  using ValidDumpCallback = std::function<void(const format::SysexParseResult& result)>;
  using DamagedDumpCallback = std::function<void()>;

  explicit SysexReceiver(SysexPacketQueue& queue) noexcept : queue_(queue) {}

  void set_on_valid_dump(ValidDumpCallback cb) { on_valid_dump_ = std::move(cb); }
  void set_on_damaged_dump(DamagedDumpCallback cb) { on_damaged_dump_ = std::move(cb); }

  // Polls the SPSC queue, reassembles incoming packets, validates complete messages.
  // Called on the control thread (e.g. during tick() or server/UI poll loop).
  // Returns number of complete SysEx messages processed.
  std::size_t poll();

  // Reset internal reassembly state
  void reset();

  const SysexReceiverCounters& counters() const noexcept { return counters_; }

  // Packetize raw SysEx bytes into 64-bit UMP packets (MT 0x3).
  // If strip_delimiters is true, removes leading 0xF0 and trailing 0xF7 (standard UMP SysEx7 form).
  // If false, retains delimiters in payload. Both forms are supported on reception.
  static std::vector<core::Ump> packetize_sysex(std::span<const uint8_t> sysex_bytes,
                                                uint8_t group = 0,
                                                bool strip_delimiters = false);

 private:
  void handle_completed_buffer(std::vector<uint8_t>& buf);
  void report_damaged();

  SysexPacketQueue& queue_;
  SysexReceiverCounters counters_;
  ValidDumpCallback on_valid_dump_;
  DamagedDumpCallback on_damaged_dump_;

  enum class State {
    kIdle,
    kReceiving
  };

  State state_{State::kIdle};
  std::vector<uint8_t> rx_buffer_;
  bool has_6op_header_{false};
};

}  // namespace velora::host

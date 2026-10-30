#include "velora/host/alsa_midi_input.hpp"

#include <chrono>
#include <ctime>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace velora::host {

std::uint64_t AlsaMidiInput::now_monotonic_raw_ns() noexcept {
#if defined(__linux__) && defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#elif defined(__APPLE__)
  static mach_timebase_info_data_t tb;
  static bool tb_init = false;
  if (!tb_init) {
    mach_timebase_info(&tb);
    tb_init = true;
  }
  return mach_continuous_time() * tb.numer / tb.denom;
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
#endif
}

AlsaMidiInput::AlsaMidiInput(std::string device_name)
    : device_name_(std::move(device_name)) {
  held_notes_.fill(0);
}

AlsaMidiInput::~AlsaMidiInput() {
  close();
}

bool AlsaMidiInput::open(ports::UmpSink& sink, std::string& /*error*/) {
  sink_ = &sink;
  connected_.store(true, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  return true;
}

std::size_t AlsaMidiInput::rescan() {
  if (!connected_.load(std::memory_order_acquire)) {
    connected_.store(true, std::memory_order_release);
    return 1;
  }
  return 0;
}

void AlsaMidiInput::close() {
  disconnect();
  running_.store(false, std::memory_order_release);
  sink_ = nullptr;
}

void AlsaMidiInput::disconnect() noexcept {
  if (connected_.exchange(false, std::memory_order_acq_rel)) {
    release_all_held_notes(now_monotonic_raw_ns());
  }
}

void AlsaMidiInput::release_all_held_notes(std::uint64_t release_time_ns) noexcept {
  if (!sink_) return;

  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    for (std::uint32_t n = 0; n < 128; ++n) {
      const std::size_t idx = ch * 128 + n;
      if (held_notes_[idx] > 0) {
        // Emit Note Off UMP packet (MIDI 2.0 64-bit note-off)
        const core::Ump off = core::make_note_off(0, ch, n, 0);
        const std::uint32_t words[2] = {off.word0, 0};
        sink_->on_ump(words, 2, release_time_ns);
        held_notes_[idx] = 0;
      }
    }
  }
  held_count_.store(0, std::memory_order_relaxed);
}

void AlsaMidiInput::track_note_state(const core::Ump& ump, bool is_on) noexcept {
  const std::uint32_t ch = ump.channel();
  const std::uint32_t note = ump.note();
  if (ch >= 16 || note >= 128) return;

  const std::size_t idx = ch * 128 + note;
  if (is_on) {
    if (held_notes_[idx] == 0) {
      held_notes_[idx] = 1;
      held_count_.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    if (held_notes_[idx] > 0) {
      held_notes_[idx] = 0;
      if (held_count_.load(std::memory_order_relaxed) > 0) {
        held_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
}

void AlsaMidiInput::inject_ump_packet(const std::uint32_t* words, std::size_t count,
                                      std::uint64_t host_ns) noexcept {
  if (!sink_ || !connected_.load(std::memory_order_acquire) || count == 0) return;

  const core::Ump ump{words[0], count > 1 ? words[1] : 0u};
  const std::uint32_t mt = ump.message_type();

  if (mt == 0x2) {  // MIDI 1.0 Channel Voice
    const std::uint32_t status = (words[0] >> 20) & 0xF;
    if (status == 0x9) {
      const std::uint32_t vel = words[0] & 0x7F;
      track_note_state(ump, vel > 0);
    } else if (status == 0x8) {
      track_note_state(ump, false);
    }
  } else if (mt == 0x4) {  // MIDI 2.0 Channel Voice
    const std::uint32_t status = (words[0] >> 20) & 0xF;
    if (status == 0x9) {
      track_note_state(ump, true);
    } else if (status == 0x8) {
      track_note_state(ump, false);
    }
  }

  sink_->on_ump(words, count, host_ns);
}

void AlsaMidiInput::inject_raw_bytes(const std::uint8_t* bytes, std::size_t count,
                                     std::uint64_t host_ns) noexcept {
  if (!sink_ || !connected_.load(std::memory_order_acquire)) return;

  for (std::size_t i = 0; i < count; ++i) {
    const std::uint8_t b = bytes[i];
    if (b >= 0x80) {
      if (b < 0xF0) {
        running_status_ = b;
        pending_bytes_[0] = b;
        pending_len_ = 1;
      } else {
        // System message
        running_status_ = 0;
        pending_len_ = 0;
      }
    } else if (running_status_ != 0) {
      if (pending_len_ == 0) {
        pending_bytes_[0] = running_status_;
        pending_len_ = 1;
      }
      pending_bytes_[pending_len_++] = b;

      const std::uint8_t op = running_status_ & 0xF0;
      const std::size_t expected = (op == 0xC0 || op == 0xD0) ? 2 : 3;

      if (pending_len_ == expected) {
        const std::uint32_t ch = pending_bytes_[0] & 0x0F;
        const std::uint32_t note = pending_bytes_[1] & 0x7F;
        const std::uint32_t vel7 = expected == 3 ? (pending_bytes_[2] & 0x7F) : 0;

        // Convert MIDI 1.0 to UMP Type 2 (0x2)
        const std::uint32_t w0 = (0x2u << 28) | (static_cast<std::uint32_t>(op >> 4) << 20) |
                                 (ch << 16) | (note << 8) | vel7;
        const std::uint32_t words[1] = {w0};

        const core::Ump ump{w0, 0};
        if (op == 0x90) {
          track_note_state(ump, vel7 > 0);
        } else if (op == 0x80) {
          track_note_state(ump, false);
        }

        sink_->on_ump(words, 1, host_ns);
        pending_len_ = 0;
      }
    }
  }
}

} // namespace velora::host

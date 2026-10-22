#include "velora/host/sysex_receiver.hpp"

#include <algorithm>

namespace velora::host {

std::vector<core::Ump> SysexReceiver::packetize_sysex(std::span<const uint8_t> sysex_bytes,
                                                      uint8_t group,
                                                      bool strip_delimiters) {
  std::vector<core::Ump> out;
  if (sysex_bytes.empty()) return out;

  std::span<const uint8_t> payload = sysex_bytes;
  if (strip_delimiters) {
    if (payload.front() == 0xF0) payload = payload.subspan(1);
    if (!payload.empty() && payload.back() == 0xF7) payload = payload.first(payload.size() - 1);
  }
  if (payload.empty()) return out;

  const std::size_t total = payload.size();
  const std::size_t chunk_count = (total + 5) / 6;

  for (std::size_t i = 0; i < chunk_count; ++i) {
    const std::size_t offset = i * 6;
    const std::size_t count = std::min<std::size_t>(6, total - offset);
    std::uint32_t form = core::Ump::kSysex7FormContinue;
    if (chunk_count == 1) {
      form = core::Ump::kSysex7FormComplete;
    } else if (i == 0) {
      form = core::Ump::kSysex7FormStart;
    } else if (i == chunk_count - 1) {
      form = core::Ump::kSysex7FormEnd;
    }
    out.push_back(core::make_sysex7(group, form, payload.data() + offset, count));
  }

  return out;
}

void SysexReceiver::reset() {
  state_ = State::kIdle;
  rx_buffer_.clear();
  has_6op_header_ = false;
}

void SysexReceiver::report_damaged() {
  counters_.sysex_damaged_count.fetch_add(1, std::memory_order_relaxed);
  if (on_damaged_dump_) {
    on_damaged_dump_();
  }
}

std::size_t SysexReceiver::poll() {
  std::size_t messages_completed = 0;
  core::Ump pkt;

  while (queue_.pop(pkt)) {
    if (!pkt.is_sysex7()) continue;

    const std::uint32_t form = pkt.sysex7_form();
    const std::uint32_t count = std::min<std::uint32_t>(pkt.sysex7_byte_count(), 6);

    uint8_t bytes[6];
    for (std::size_t i = 0; i < count; ++i) {
      bytes[i] = pkt.sysex7_byte(i);
    }

    if (form == core::Ump::kSysex7FormComplete) {
      if (state_ == State::kReceiving && has_6op_header_) {
        report_damaged();
      }
      rx_buffer_.clear();
      has_6op_header_ = false;
      rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + count);
      handle_completed_buffer(rx_buffer_);
      rx_buffer_.clear();
      state_ = State::kIdle;
      has_6op_header_ = false;
      ++messages_completed;
    } else if (form == core::Ump::kSysex7FormStart) {
      if (state_ == State::kReceiving && has_6op_header_) {
        report_damaged();
      }
      rx_buffer_.clear();
      rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + count);
      state_ = State::kReceiving;

      // Check if start of packet looks like a Yamaha 6-OP SysEx
      has_6op_header_ = false;
      if (rx_buffer_.size() >= 2) {
        if ((rx_buffer_[0] == 0xF0 && rx_buffer_[1] == 0x43) ||
            (rx_buffer_[0] == 0x43)) {
          has_6op_header_ = true;
        }
      }
    } else if (form == core::Ump::kSysex7FormContinue) {
      if (state_ != State::kReceiving) {
        // Stray continuation packet outside an active dump
        continue;
      }
      rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + count);

      // Check 6-OP header once we have enough bytes
      if (!has_6op_header_ && rx_buffer_.size() >= 2) {
        if ((rx_buffer_[0] == 0xF0 && rx_buffer_[1] == 0x43) ||
            (rx_buffer_[0] == 0x43)) {
          has_6op_header_ = true;
        }
      }

      // Maximum allowable 6-OP bulk dump is 4104 bytes. Cap safety at 8192 bytes.
      if (rx_buffer_.size() > 8192) {
        if (has_6op_header_) {
          report_damaged();
        }
        rx_buffer_.clear();
        state_ = State::kIdle;
        has_6op_header_ = false;
      }
    } else if (form == core::Ump::kSysex7FormEnd) {
      if (state_ != State::kReceiving) {
        // Stray end packet outside an active dump
        continue;
      }
      rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + count);
      handle_completed_buffer(rx_buffer_);
      rx_buffer_.clear();
      state_ = State::kIdle;
      has_6op_header_ = false;
      ++messages_completed;
    }
  }

  return messages_completed;
}

void SysexReceiver::handle_completed_buffer(std::vector<uint8_t>& buf) {
  if (buf.empty()) return;

  // Normalize framing: if stripped of delimiters, add 0xF0 and 0xF7
  if (buf.front() != 0xF0 && buf.front() == 0x43) {
    buf.insert(buf.begin(), 0xF0);
    if (buf.back() != 0xF7) {
      buf.push_back(0xF7);
    }
  }

  // Must begin with 0xF0
  if (buf.front() != 0xF0) {
    counters_.sysex_ignored_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Must have manufacturer ID
  if (buf.size() < 2) {
    counters_.sysex_ignored_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Non-Yamaha SysEx (e.g. Roland 0x41, Korg 0x42, Universal 0x7E/0x7F) is ignored silently
  if (buf[1] != 0x43) {
    counters_.sysex_ignored_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Yamaha SysEx: substatus byte (channel) must have high nibble 0 (0x00..0x0F)
  if (buf.size() < 3 || (buf[2] & 0xF0) != 0x00) {
    counters_.sysex_ignored_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Must end with 0xF7
  if (buf.back() != 0xF7) {
    report_damaged();
    return;
  }

  // Header inspection: 6-OP dumps use format 0 (single voice) or format 9 (32-voice bulk)
  // Single header: F0 43 [0n] 00 01 1B
  // Bulk header:   F0 43 [0n] 09 20 00
  if (buf.size() < 6) {
    report_damaged();
    return;
  }

  const bool is_single_fmt = (buf[3] == 0x00 && buf[4] == 0x01 && buf[5] == 0x1B);
  const bool is_bulk_fmt = (buf[3] == 0x09 && buf[4] == 0x20 && buf[5] == 0x00);

  if (!is_single_fmt && !is_bulk_fmt) {
    // Non-6-OP Yamaha dump (e.g. 4-OP TX81Z format 04, param changes, effect settings)
    counters_.sysex_ignored_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Validated with Story 12.3 parser (checksum, byte count, header)
  format::SysexParseResult res = format::parse_sysex_memory(buf);
  if (res.ok() && !res.voices.empty()) {
    counters_.sysex_valid_count.fetch_add(1, std::memory_order_relaxed);
    if (on_valid_dump_) {
      on_valid_dump_(res);
    }
  } else {
    report_damaged();
  }
}

}  // namespace velora::host

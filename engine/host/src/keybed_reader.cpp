#include "velora/host/keybed_reader.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/contact_events.hpp"
#include "velora/core/ump.hpp"

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

// ---- KeybedReader -------------------------------------------------------------------------------------------------

namespace {
KeybedReaderConfig sanitize(KeybedReaderConfig c) {
  if (!(c.tick_hz > 0.0) || !std::isfinite(c.tick_hz)) c.tick_hz = 1'000'000.0;
  c.group = std::min<std::uint32_t>(c.group, 15);
  c.channel = std::min<std::uint32_t>(c.channel, 15);
  return c;
}
}  // namespace

KeybedReader::KeybedReader(HostEventQueue& out, KeybedCalibration table, KeybedReaderConfig cfg)
    : out_(out), table_(std::move(table)), cfg_(sanitize(cfg)), clock_(cfg_.tick_hz), keys_(table_.key_count()) {
  // a 32-bit tick spans 2^32 / tick_hz seconds; beyond half of it the unwrap is ambiguous (90% for margin)
  max_gap_ns_ = static_cast<std::uint64_t>(2147483648.0 / cfg_.tick_hz * 1e9 * 0.9);
}

void KeybedReader::set_calibration(KeybedCalibration table, std::uint64_t now_ns) {
  release_all(now_ns);
  table_ = std::move(table);
  keys_.assign(table_.key_count(), KeyState{});
}

KeybedReaderCounters KeybedReader::counters() const {
  KeybedReaderCounters c = c_;
  const mcu::DecoderCounters d = decoder_.counters();
  c.frames = d.frames_ok;
  c.crc_errors = d.crc_errors;
  c.framing_errors = d.framing_errors;
  c.seq_gaps = d.seq_gaps;
  c.seq_lost = d.seq_lost;
  c.version_rejects = d.version_mismatches;
  c.rejected_frames += d.bad_lengths + d.unknown_types;
  c.sync_rejected = clock_.rejected();
  return c;
}

void KeybedReader::emit(const core::Ump& u, std::uint64_t t) {
  if (t < last_out_ns_) {  // never reverse event order
    t = last_out_ns_;
    ++c_.clock_clamps;
  }
  last_out_ns_ = t;
  if (!out_.push(StampedUmp{u, t})) ++c_.queue_full;
}

std::uint64_t KeybedReader::convert(std::uint32_t tick, std::uint64_t rx_ns) {
  if (!clock_.valid()) clock_.bootstrap(tick, rx_ns);
  if (clock_.samples() == 0) ++c_.unsynced_events;
  return clock_.to_host_ns(clock_.unwrap(tick));
}

void KeybedReader::on_bytes(const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns) {
  const mcu::DecoderCounters before = decoder_.counters();
  frames_.clear();
  decoder_.feed(data, len, &frames_, nullptr);
  const mcu::DecoderCounters after = decoder_.counters();
  if (after.crc_errors != before.crc_errors || after.framing_errors != before.framing_errors || after.seq_gaps != before.seq_gaps ||
      after.version_mismatches != before.version_mismatches || after.bad_lengths != before.bad_lengths ||
      after.unknown_types != before.unknown_types || after.overflow_bytes != before.overflow_bytes ||
      after.dropped_bytes != before.dropped_bytes)
    recovery_pending_ = true;  // a frame may have been lost
  for (const mcu::Frame& f : frames_) handle(f, rx_ns);
}

void KeybedReader::handle(const mcu::Frame& f, std::uint64_t rx_ns) {
  if (have_rx_ && clock_.valid() && rx_ns > last_rx_ns_ && rx_ns - last_rx_ns_ > max_gap_ns_) {
    clock_.reset();  // silent for so long that the 32-bit tick cannot be unwrapped: start over from this frame
    outstanding_ = false;
  }
  have_rx_ = true;
  last_rx_ns_ = rx_ns;
  const auto msg = mcu::parse_message(f);
  if (!msg) {
    ++c_.rejected_frames;
    return;
  }
  if (const auto* kc = std::get_if<mcu::KeyContact>(&*msg)) {
    on_key_contact(*kc, convert(f.tick, rx_ns));
  } else if (const auto* ts = std::get_if<mcu::TimeSync>(&*msg)) {
    on_time_sync(*ts, rx_ns);
  } else if (const auto* h = std::get_if<mcu::Hello>(&*msg)) {
    on_hello(*h, f.tick, rx_ns);
  } else {
    ++c_.ignored_frames;
  }
}

void KeybedReader::on_hello(const mcu::Hello& h, std::uint32_t tick, std::uint64_t rx_ns) {
  if (h.board_id != 1) {  // not the keybed MCU
    ++c_.ignored_frames;
    return;
  }
  const bool restarted = have_hello_ && (!(hello_ == h) || clock_.went_backwards(tick, static_cast<std::uint32_t>(cfg_.tick_hz / 10)));
  if (restarted) {  // the MCU restarted: its ticks start over and it has forgotten every key
    ++c_.mcu_resets;
    release_all(rx_ns);
    clock_.reset();
    outstanding_ = false;
    next_sync_ns_ = 0;
  }
  hello_ = h;
  have_hello_ = true;
  if (h.keybed_contacts != 0 && h.keybed_contacts != table_.contacts()) ++c_.contact_mismatch;
}

void KeybedReader::on_time_sync(const mcu::TimeSync& ts, std::uint64_t rx_ns) {
  if (!ts.reply) {
    ++c_.ignored_frames;
    return;
  }
  if (!outstanding_ || ts.token != token_ || rx_ns < sent_ns_) {
    ++c_.sync_stray;
    return;
  }
  outstanding_ = false;
  ++c_.sync_replies;
  const std::uint64_t rtt = rx_ns - sent_ns_;
  clock_.add_sample(ts.tick, sent_ns_ + rtt / 2, rtt);
}

void KeybedReader::poll(std::uint64_t now_ns) {
  if (outstanding_ && now_ns >= sent_ns_ && now_ns - sent_ns_ >= cfg_.sync_timeout_ns) {
    ++c_.missed_syncs;  // keep the last offset and drift
    outstanding_ = false;
  }
  if (!tx_ || outstanding_ || now_ns < next_sync_ns_) return;
  ++token_;
  const auto wire = writer_.write(mcu::TimeSync{false, token_, 0}, static_cast<std::uint32_t>(now_ns / 1000));
  outstanding_ = true;
  sent_ns_ = now_ns;
  ++c_.sync_requests;
  next_sync_ns_ = now_ns + (clock_.samples() < cfg_.sync_fast_samples ? cfg_.sync_fast_interval_ns : cfg_.sync_interval_ns);
  tx_(wire.data(), wire.size());
}

void KeybedReader::do_open(std::uint8_t key, std::uint8_t contact, std::uint64_t t) {
  KeyState& ks = keys_[key];
  const int note = table_.note_of(key);
  ks.mask = static_cast<std::uint8_t>(ks.mask & ~(1u << contact));
  if (contact == table_.vel_from()) ks.vel_start_valid = false;
  if (contact == table_.vel_to()) {  // a re-strike after a partial release is timed from this open, while the pair's first contact is still closed
    ks.vel_start_ns = t;
    ks.vel_start_valid = (ks.mask & (1u << table_.vel_from())) != 0;
  }
  emit(core::make_contact_event(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(note), contact + 1u, false), t);
  ++c_.contact_events;
  if (ks.sounding && contact <= table_.release_contact()) release_key(key, t);
}

void KeybedReader::release_key(std::uint8_t key, std::uint64_t t) {
  KeyState& ks = keys_[key];
  if (!ks.sounding) return;
  ks.sounding = false;
  emit(core::make_note_off(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(table_.note_of(key)), cfg_.release_velocity16), t);
  ++c_.note_offs;
}

void KeybedReader::on_key_contact(const mcu::KeyContact& kc, std::uint64_t t) {
  ++c_.key_frames;
  if (table_.note_of(kc.key) < 0) {
    ++c_.unknown_key;
    return;
  }
  if (kc.contact >= table_.contacts()) {
    ++c_.bad_contact;
    return;
  }
  const std::uint8_t key = kc.key, c = kc.contact;
  KeyState& ks = keys_[key];
  const int note = table_.note_of(key);
  if (kc.closed) {
    // A repeat of a close already recorded (same contact, nothing above it closed, within 5 ms) is a duplicate frame, not a new strike.
    // A contact still marked closed with higher ones closed, or long ago, lost its open frame: handled as stale below.
    if ((ks.mask & (1u << c)) && (ks.mask >> (c + 1)) == 0 && t - std::min(t, ks.last_close_ns[c]) < 5'000'000) {
      ++c_.duplicate_closes;
      return;
    }
    // Contacts close in order, so any contact at or above this one still marked closed missed its open: release it first.
    for (int b = table_.contacts() - 1; b >= c; --b)
      if (ks.mask & (1u << b)) {
        ++c_.recovered_opens;
        do_open(key, static_cast<std::uint8_t>(b), t);
      }
    ks.mask = static_cast<std::uint8_t>(ks.mask | (1u << c));
    ks.last_close_ns[c] = t;
    if (c == table_.vel_from()) {
      ks.vel_start_ns = t;
      ks.vel_start_valid = true;
    }
    emit(core::make_contact_event(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(note), c + 1u, true), t);
    ++c_.contact_events;
    if (c == table_.vel_to() && !ks.sounding) {
      std::uint64_t dt;
      if (ks.vel_start_valid) {
        dt = t - std::min(t, ks.vel_start_ns);
      } else {
        ++c_.velocity_fallbacks;
        dt = ~0ull >> 1;  // unknown travel time: the slowest velocity
      }
      ks.sounding = true;
      emit(core::make_note_on(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(note), mapper_->velocity16(dt)), t);
      ++c_.note_ons;
    }
  } else {
    // Contacts above this one open first; any still marked closed lost their open frame.
    for (int b = table_.contacts() - 1; b > c; --b)
      if (ks.mask & (1u << b)) {
        ++c_.recovered_opens;
        do_open(key, static_cast<std::uint8_t>(b), t);
      }
    if (ks.mask & (1u << c)) {
      do_open(key, c, t);
    } else {
      ++c_.spurious_opens;
      if (ks.sounding && c <= table_.release_contact()) release_key(key, t);
    }
  }
}

void KeybedReader::report_key_state(std::uint8_t key, std::uint8_t closed_mask, std::uint64_t now_ns) {
  if (table_.note_of(key) < 0) {
    ++c_.unknown_key;
    return;
  }
  KeyState& ks = keys_[key];
  for (int b = table_.contacts() - 1; b >= 0; --b)
    if ((ks.mask & (1u << b)) && !(closed_mask & (1u << b))) {
      ++c_.recovered_opens;
      do_open(key, static_cast<std::uint8_t>(b), std::max(now_ns, last_out_ns_));
    }
  if (closed_mask == 0 && ks.sounding) release_key(key, std::max(now_ns, last_out_ns_));
  recovery_pending_ = false;
}

std::size_t KeybedReader::release_all(std::uint64_t now_ns) {
  std::size_t n = 0;
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    if (keys_[k].mask == 0 && !keys_[k].sounding) continue;
    ++n;
    report_key_state(static_cast<std::uint8_t>(k), 0, now_ns);
  }
  recovery_pending_ = false;
  return n;
}

}  // namespace velora::host

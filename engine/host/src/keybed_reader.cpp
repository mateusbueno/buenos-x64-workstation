#include "velora/host/keybed_reader.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/contact_events.hpp"
#include "velora/core/ump.hpp"
#include "velora/host/drift_clock.hpp"

namespace velora::host {

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
    : out_(out), table_(std::move(table)), cfg_(sanitize(cfg)), link_(TimeSyncConfig{cfg_.tick_hz, cfg_.sync_interval_ns, cfg_.sync_fast_interval_ns, cfg_.sync_fast_samples, cfg_.sync_timeout_ns}),
      attack_mapper_(table_.velocity_range()), release_mapper_(table_.release_range()), keys_(table_.key_count()) {}

void KeybedReader::set_calibration(KeybedCalibration table, std::uint64_t now_ns) {
  release_all(now_ns);
  table_ = std::move(table);
  keys_.assign(table_.key_count(), KeyState{});
  rel_pending_count_ = 0;
  attack_mapper_.set_range(table_.velocity_range());
  release_mapper_.set_range(table_.release_range());
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
  c.sync_rejected = link_.clock().rejected();
  const TimeSyncCounters t = link_.counters();
  c.sync_requests = t.requests;
  c.sync_replies = t.replies;
  c.missed_syncs = t.missed;
  c.sync_stray = t.stray;
  c.unsynced_events = t.unsynced_events;
  return c;
}

void KeybedReader::emit(const core::Ump& u, std::uint64_t t, bool release_stamp) {
  if (t < last_out_ns_) {  // never reverse event order
    t = last_out_ns_;
    if (release_stamp) ++c_.release_stamp_clamps;  // a deferred Note Off is stamped in the past by design: its own counter
    else ++c_.clock_clamps;
  }
  last_out_ns_ = t;
  if (!out_.push(StampedUmp{u, t})) ++c_.queue_full;
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
  link_.note_rx(rx_ns);
  const auto msg = mcu::parse_message(f);
  if (!msg) {
    ++c_.rejected_frames;
    return;
  }
  if (const auto* kc = std::get_if<mcu::KeyContact>(&*msg)) {
    on_key_contact(*kc, link_.convert(f.tick, rx_ns));
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
  const bool restarted = have_hello_ && (!(hello_ == h) || link_.clock().went_backwards(tick, static_cast<std::uint32_t>(cfg_.tick_hz / 10)));
  if (restarted) {  // the MCU restarted: its ticks start over and it has forgotten every key
    ++c_.mcu_resets;
    release_all(rx_ns);
    link_.restart();
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
  link_.on_reply(ts, rx_ns);
}

void KeybedReader::poll(std::uint64_t now_ns) {
  link_.poll(now_ns);
  flush_slow_releases(now_ns);
}

// A release measurement that has waited the table's slowest release time ends at the slowest release velocity.
void KeybedReader::flush_slow_releases(std::uint64_t now_ns) {
  if (rel_pending_count_ == 0) return;
  const std::uint64_t slow = table_.release_range().slow_ns;
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    KeyState& ks = keys_[k];
    if (!ks.rel_pending || now_ns < ks.rel_start_ns || now_ns - ks.rel_start_ns < slow) continue;
    ++c_.release_timeouts;
    finish_release(static_cast<std::uint8_t>(k), ks.rel_start_ns, release_mapper_.velocity16(slow));
  }
}

void KeybedReader::do_open(std::uint8_t key, std::uint8_t contact, std::uint64_t t, bool synthetic) {
  KeyState& ks = keys_[key];
  const int note = table_.note_of(key);
  // The release pair completes: the Note Off goes out first (stamped with the release contact's own time), so the order of the stream is kept.
  if (ks.sounding && ks.rel_pending && contact <= table_.rel_to()) {
    if (synthetic || contact < table_.rel_to()) {
      release_key(key, t);  // the second contact's own open was lost: no timing
    } else {
      finish_release(key, ks.rel_start_ns, release_mapper_.velocity16(t - std::min(t, ks.rel_start_ns)));
    }
  }
  ks.mask = static_cast<std::uint8_t>(ks.mask & ~(1u << contact));
  if (contact == table_.vel_from()) ks.vel_start_valid = false;
  if (contact == table_.vel_to()) {  // a re-strike after a partial release is timed from this open, while the pair's first contact is still closed
    ks.vel_start_ns = t;
    ks.vel_start_valid = (ks.mask & (1u << table_.vel_from())) != 0;
  }
  emit(core::make_contact_event(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(note), contact + 1u, false), t);
  ++c_.contact_events;
  if (ks.sounding && !ks.rel_pending && contact <= table_.release_contact()) {
    if (!synthetic && table_.release_pair() && contact == table_.release_contact() && (ks.mask & (1u << table_.rel_to())) != 0) {
      ks.rel_pending = true;  // wait for the pair's second contact: its opening time gives the release velocity
      ks.rel_start_ns = t;
      ++rel_pending_count_;
    } else {
      release_key(key, t);
    }
  }
}

void KeybedReader::finish_release(std::uint8_t key, std::uint64_t stamp_ns, std::uint16_t velocity16) {
  KeyState& ks = keys_[key];
  if (!ks.sounding) return;
  if (ks.rel_pending) {
    ks.rel_pending = false;
    if (rel_pending_count_ > 0) --rel_pending_count_;
  }
  ks.sounding = false;
  emit(core::make_note_off(cfg_.group, cfg_.channel, static_cast<std::uint32_t>(table_.note_of(key)), velocity16), stamp_ns, true);
  ++c_.note_offs;
}

void KeybedReader::release_key(std::uint8_t key, std::uint64_t t) {
  KeyState& ks = keys_[key];
  if (!ks.sounding) return;
  ++c_.release_velocity_defaults;
  finish_release(key, ks.rel_pending ? ks.rel_start_ns : t, cfg_.release_velocity16);
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
  flush_slow_releases(t);
  if (kc.closed) {
    // A repeat of a close already recorded (same contact, nothing above it closed, within 5 ms) is a duplicate frame, not a new strike.
    // A contact still marked closed with higher ones closed, or long ago, lost its open frame: handled as stale below.
    // While a release measurement waits, a close of a contact below the release contact that is still marked closed is a bounce of that contact: ignored,
    // so it does not end the measurement early (a real re-strike closes the release contact itself).
    const bool bounce = ks.rel_pending && c < table_.release_contact() && (ks.mask & (1u << c)) != 0;
    if (bounce || ((ks.mask & (1u << c)) && (ks.mask >> (c + 1)) == 0 && t - std::min(t, ks.last_close_ns[c]) < 5'000'000)) {
      ++c_.duplicate_closes;
      return;
    }
    // A real re-strike (the release contact closes again) before the release measurement completed: the Note Off goes out first, with the release
    // velocity measured so far. A close of a lower contact (a bounce) does not end the measurement.
    if (ks.rel_pending && c >= table_.release_contact()) {
      ++c_.release_restrikes;
      finish_release(key, ks.rel_start_ns, release_mapper_.velocity16(t - std::min(t, ks.rel_start_ns)));
    }
    // Contacts close in order, so any contact at or above this one still marked closed missed its open: release it first.
    for (int b = table_.contacts() - 1; b >= c; --b)
      if (ks.mask & (1u << b)) {
        ++c_.recovered_opens;
        do_open(key, static_cast<std::uint8_t>(b), t, true);
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
        do_open(key, static_cast<std::uint8_t>(b), t, true);
      }
    if (ks.mask & (1u << c)) {
      do_open(key, c, t);
    } else {
      ++c_.spurious_opens;
      if (ks.sounding && !ks.rel_pending && c <= table_.release_contact()) release_key(key, t);
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
      do_open(key, static_cast<std::uint8_t>(b), std::max(now_ns, last_out_ns_), true);
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

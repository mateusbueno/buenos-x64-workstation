#include "velora/host/panel_reader.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "velora/core/assign_events.hpp"

namespace velora::host {

namespace {
PanelReaderConfig sanitize(PanelReaderConfig c) {
  if (!(c.tick_hz > 0.0) || !std::isfinite(c.tick_hz)) c.tick_hz = 1'000'000.0;
  c.group = std::min<std::uint32_t>(c.group, 15);
  return c;
}

const mcu_ids::IdInfo* find_control(std::uint8_t id) {
  for (const mcu_ids::IdInfo& i : mcu_ids::kControls)
    if (i.id == id) return &i;
  return nullptr;
}

const mcu_ids::IdInfo* find_button(std::uint8_t id) {
  for (const mcu_ids::IdInfo& i : mcu_ids::kButtons)
    if (i.id == id) return &i;
  return nullptr;
}
}  // namespace

PanelReader::PanelReader(HostEventQueue& out, const KeyboardOnParts& parts, PanelReaderConfig cfg)
    : out_(out),
      parts_(parts),
      cfg_(sanitize(cfg)),
      link_(TimeSyncConfig{cfg_.tick_hz, cfg_.sync_interval_ns, cfg_.sync_fast_interval_ns, cfg_.sync_fast_samples, cfg_.sync_timeout_ns}) {}

std::uint32_t PanelReader::pitch_bend_value32(std::uint16_t wheel12) noexcept {
  const std::uint64_t v = std::min<std::uint16_t>(wheel12, 4095);
  if (v <= 2048) return static_cast<std::uint32_t>(v * 0x80000000ull / 2048ull);
  return static_cast<std::uint32_t>(0x80000000ull + (v - 2048) * 0x7FFFFFFFull / 2047ull);
}

std::uint32_t PanelReader::scale12_to_32(std::uint16_t v12) noexcept {
  const std::uint64_t v = std::min<std::uint16_t>(v12, 4095);
  return static_cast<std::uint32_t>(v * 0xFFFFFFFFull / 4095ull);
}

PanelReaderCounters PanelReader::counters() const {
  PanelReaderCounters c = c_;
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

std::uint64_t PanelReader::order(std::uint64_t t) {
  if (t < last_out_ns_) {  // never reverse event order
    t = last_out_ns_;
    ++c_.clock_clamps;
  }
  last_out_ns_ = t;
  return t;
}

bool PanelReader::emit(const core::Ump& u, std::uint64_t t, bool is_controller) {
  // Panel controls are the first to go when the queue is nearly full: the last kNoteHeadroom slots are for notes and the pedal.
  if (is_controller && out_.size() + kNoteHeadroom >= HostEventQueue::capacity()) {
    ++c_.controllers_dropped;
    return false;
  }
  if (!out_.push(StampedUmp{u, t})) {
    ++c_.queue_full;
    return false;
  }
  return true;
}

void PanelReader::emit_wheel(bool pitch, std::uint16_t channels, std::uint32_t value32, std::uint64_t t) {
  std::uint16_t& pending = pitch ? pending_pitch_ : pending_mod_;
  for (std::uint32_t ch = 0; ch < 16; ++ch) {
    if (!(channels & (1u << ch))) continue;
    const bool ok = emit(pitch ? core::make_pitch_bend(cfg_.group, ch, value32)
                               : core::make_control_change(cfg_.group, ch, core::Ump::kControllerModulation, value32),
                         t);
    if (ok) pending = static_cast<std::uint16_t>(pending & ~(1u << ch));
    else pending = static_cast<std::uint16_t>(pending | (1u << ch));  // re-sent with the next frame or poll
  }
}

// Called before every frame and on poll: parts that joined the keyboard take the current wheels, parts that left it get centre / zero, and wheel UMPs that
// were shed because the queue was nearly full are re-sent with the current value.
void PanelReader::sync_parts_and_flush() {
  const std::uint16_t mask = parts_.mask();
  const std::uint16_t joined = static_cast<std::uint16_t>(mask & ~last_mask_), left = static_cast<std::uint16_t>(last_mask_ & ~mask);
  last_mask_ = mask;
  // Channels owed a wheel UMP: parts that joined (the current value), parts that left (centre / zero), and the ones whose UMP was shed.
  if (have_pitch_) pending_pitch_ = static_cast<std::uint16_t>(pending_pitch_ | joined | left);
  if (have_mod_) pending_mod_ = static_cast<std::uint16_t>(pending_mod_ | joined | left);
  if (!pending_pitch_ && !pending_mod_) return;
  if (out_.size() + kNoteHeadroom >= HostEventQueue::capacity()) return;  // still no room: try again with the next frame or poll
  const std::uint64_t t = last_out_ns_;
  if (pending_pitch_) {
    const std::uint16_t cur = static_cast<std::uint16_t>(pending_pitch_ & mask), rest = static_cast<std::uint16_t>(pending_pitch_ & ~mask);
    pending_pitch_ = 0;
    if (cur) {
      emit_wheel(true, cur, pitch32_, t);
      ++c_.wheel_replays;
    }
    if (rest) {
      emit_wheel(true, rest, 0x80000000u, t);
      ++c_.wheel_centres;
    }
    pending_pitch_ = static_cast<std::uint16_t>(pending_pitch_ & mask);  // a shed centre of a part that is off the keyboard is dropped, not retried forever
  }
  if (pending_mod_) {
    const std::uint16_t cur = static_cast<std::uint16_t>(pending_mod_ & mask), rest = static_cast<std::uint16_t>(pending_mod_ & ~mask);
    pending_mod_ = 0;
    if (cur) {
      emit_wheel(false, cur, mod32_, t);
      ++c_.wheel_replays;
    }
    if (rest) emit_wheel(false, rest, 0, t);
    pending_mod_ = static_cast<std::uint16_t>(pending_mod_ & mask);
  }
}

void PanelReader::poll(std::uint64_t now_ns) {
  link_.poll(now_ns);
  sync_parts_and_flush();
}

void PanelReader::on_bytes(const std::uint8_t* data, std::size_t len, std::uint64_t rx_ns) {
  sync_parts_and_flush();
  frames_.clear();
  decoder_.feed(data, len, &frames_, nullptr);
  for (const mcu::Frame& f : frames_) handle(f, rx_ns);
}

void PanelReader::handle(const mcu::Frame& f, std::uint64_t rx_ns) {
  link_.note_rx(rx_ns);
  const auto msg = mcu::parse_message(f);
  if (!msg) {
    if (f.type == static_cast<std::uint8_t>(mcu::FrameType::kControl) && f.payload.size() == 3 && !find_control(f.payload[0]))
      ++c_.unknown_control;  // an ID the map does not have: dropped and counted
    else if (f.type == static_cast<std::uint8_t>(mcu::FrameType::kButton) && f.payload.size() == 2 && !find_button(f.payload[0]))
      ++c_.unknown_button;
    else
      ++c_.rejected_frames;
    return;
  }
  if (const auto* c = std::get_if<mcu::Control>(&*msg)) {
    on_control(*c, order(link_.convert(f.tick, rx_ns)));
  } else if (const auto* b = std::get_if<mcu::Button>(&*msg)) {
    on_button(*b, order(link_.convert(f.tick, rx_ns)));
  } else if (const auto* ts = std::get_if<mcu::TimeSync>(&*msg)) {
    if (ts->reply) link_.on_reply(*ts, rx_ns);
    else ++c_.ignored_frames;
  } else if (const auto* h = std::get_if<mcu::Hello>(&*msg)) {
    on_hello(*h, f.tick, rx_ns);
  } else {
    ++c_.ignored_frames;
  }
}

void PanelReader::on_hello(const mcu::Hello& h, std::uint32_t tick, std::uint64_t rx_ns) {
  if (h.board_id != 2) {  // not the panel MCU
    ++c_.ignored_frames;
    return;
  }
  const bool restarted = have_hello_ && (!(hello_ == h) || link_.clock().went_backwards(tick, static_cast<std::uint32_t>(cfg_.tick_hz / 10)));
  if (restarted) {  // the MCU restarted: its ticks start over
    ++c_.mcu_resets;
    link_.restart();
    // The restarted MCU has forgotten its wheels: the keyboard's parts go back to centre / zero until it reports them again.
    const std::uint64_t t = order(rx_ns);
    pitch32_ = 0x80000000u;
    mod32_ = 0;
    have_pitch_ = have_mod_ = true;
    const std::uint16_t mask = parts_.mask();
    emit_wheel(true, mask, pitch32_, t);
    emit_wheel(false, mask, mod32_, t);
    ++c_.wheel_replays;
  }
  hello_ = h;
  have_hello_ = true;
}

void PanelReader::on_button(const mcu::Button& b, std::uint64_t t) {
  if (!find_button(b.id)) {  // defensive: the frame layer already refuses unknown IDs
    ++c_.unknown_button;
    return;
  }
  ++c_.button_frames;
  if (!sink_) {
    ++c_.buttons_unsunk;
    return;
  }
  sink_(PanelButtonEvent{b.id, b.pressed, t});
  ++c_.buttons_delivered;
}

void PanelReader::on_control(const mcu::Control& c, std::uint64_t t) {
  ++c_.control_frames;
  const mcu_ids::IdInfo* info = find_control(c.id);
  if (!info) {
    ++c_.unknown_control;
    return;
  }
  const std::string_view kind = info->kind;
  if (c.id == mcu_ids::controls::WHEEL_PITCH || c.id == mcu_ids::controls::WHEEL_MOD) {
    const bool pitch = c.id == mcu_ids::controls::WHEEL_PITCH;
    const std::uint32_t value32 = pitch ? pitch_bend_value32(c.value) : scale12_to_32(c.value);
    (pitch ? pitch32_ : mod32_) = value32;  // remembered even with no keyboard part: a part that joins later takes it
    (pitch ? have_pitch_ : have_mod_) = true;
    const std::uint16_t mask = parts_.mask();
    if (mask == 0) {
      ++c_.wheel_no_parts;
      return;
    }
    emit_wheel(pitch, mask, value32, t);
    ++(pitch ? c_.pitch_bends : c_.modulations);
    return;
  }
  // Fader, encoder, pedal: a raw panel control event. Encoders 1..8 also produce the existing encoder step event of Story 4.10.
  const bool is_pedal = kind == "pedal" || kind == "pedal_switch";  // pedals use the note headroom of the queue (never shed with the controllers)
  emit(core::make_panel_control_event(c.id, c.value), t, !is_pedal);
  ++c_.raw_controls;
  if (kind == "encoder" && c.id >= mcu_ids::controls::ENCODER_1 && c.id <= mcu_ids::controls::ENCODER_8) {
    emit(core::make_encoder_step_event(c.id - mcu_ids::controls::ENCODER_1, mcu::control_delta(c)), t);
    ++c_.encoder_steps;
  }
}

}  // namespace velora::host

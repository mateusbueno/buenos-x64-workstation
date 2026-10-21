#include "velora/core/arpeggiator.hpp"

#include <algorithm>
#include <cmath>

namespace velora::core {

ArpConfig sanitize_arp_config(const ArpConfig& cfg) noexcept {
  ArpConfig out = cfg;
  if (static_cast<std::uint8_t>(out.mode) > static_cast<std::uint8_t>(ArpMode::kAsPlayed)) {
    out.mode = ArpMode::kUp;
  }
  if (static_cast<std::uint8_t>(out.rate) > static_cast<std::uint8_t>(ArpRate::k1_32T)) {
    out.rate = ArpRate::k1_16;
  }
  if (out.octaves < 1) out.octaves = 1;
  if (out.octaves > 4) out.octaves = 4;
  if (!std::isfinite(out.gate_percent)) {
    out.gate_percent = 50.0f;
  } else {
    out.gate_percent = std::clamp(out.gate_percent, 1.0f, 200.0f);
  }
  if (!std::isfinite(out.swing_percent)) {
    out.swing_percent = 50.0f;
  } else {
    out.swing_percent = std::clamp(out.swing_percent, 50.0f, 75.0f);
  }
  return out;
}

Arpeggiator::Arpeggiator(std::uint32_t part_index) noexcept
    : part_index_(part_index & 0xFu), rng_state_(initial_rng_state(part_index & 0xFu)) {}

void Arpeggiator::set_part_index(std::uint32_t part_index) noexcept {
  part_index_ = part_index & 0xFu;
  rng_state_ = initial_rng_state(part_index_);
}

void Arpeggiator::set_config(const ArpConfig& cfg) noexcept {
  const ArpConfig next = sanitize_arp_config(cfg);
  if ((config_.enabled || pattern_running_) && next.enabled && config_.rate != next.rate) {
    rate_changed_ = true;
  }
  config_ = next;
}

bool Arpeggiator::has_pending_transition() const noexcept {
  if (!config_.enabled) {
    for (const SoundingNote& s : sounding_) {
      if (s.active) return true;
    }
    for (std::size_t i = 0; i < held_count_; ++i) {
      if (!held_[i].passthrough_sounding) return true;
    }
    return false;
  }
  for (std::size_t i = 0; i < held_count_; ++i) {
    if (held_[i].passthrough_sounding) return true;
  }
  return held_count_ > 0 && !pattern_running_;
}

void Arpeggiator::track_passthrough_note(std::uint32_t note, std::uint32_t velocity16, bool on) noexcept {
  const std::uint8_t n = static_cast<std::uint8_t>(note & 0x7Fu);
  if (on) {
    add_or_update_held(n, static_cast<std::uint16_t>(velocity16 & 0xFFFFu), true);
  } else {
    remove_held(n);
  }
}

bool Arpeggiator::is_passthrough_sounding(std::uint32_t note) const noexcept {
  const std::uint8_t n = static_cast<std::uint8_t>(note & 0x7Fu);
  for (std::size_t i = 0; i < held_count_; ++i) {
    if (held_[i].note == n) return held_[i].passthrough_sounding;
  }
  return false;
}

void Arpeggiator::flush_sounding(EventBuffer& out) noexcept {
  emit_all_sounding_offs(0, out);
}

void Arpeggiator::reset() noexcept {
  held_count_ = 0;
  press_counter_ = 0;
  for (SoundingNote& s : sounding_) s = SoundingNote{};
  pattern_running_ = false;
  rate_changed_ = false;
  step_index_ = 0;
  next_grid_tick_ = 0;
  rng_state_ = initial_rng_state(part_index_);
}

std::int64_t Arpeggiator::first_grid_tick_from_frame(const TransportClock& clock, std::uint32_t frame,
                                                     std::int64_t step_ticks) noexcept {
  if (step_ticks <= 0) step_ticks = 240;
  const std::int64_t f = static_cast<std::int64_t>(frame);
  std::int64_t g = (clock.ticks_at(frame) / step_ticks) * step_ticks;
  if (g < 0) g = 0;
  while (g >= step_ticks && clock.frame_of_tick(g - step_ticks) >= f) {
    g -= step_ticks;
  }
  while (clock.frame_of_tick(g) < f) {
    g += step_ticks;
  }
  return g;
}

void Arpeggiator::add_or_update_held(std::uint8_t note, std::uint16_t velocity16, bool passthrough_sounding) noexcept {
  for (std::size_t i = 0; i < held_count_; ++i) {
    if (held_[i].note == note) {
      held_[i].velocity16 = velocity16;
      held_[i].press_order = ++press_counter_;
      held_[i].passthrough_sounding = passthrough_sounding;
      return;
    }
  }
  if (held_count_ >= kMaxHeldNotes) return;
  std::size_t pos = held_count_;
  while (pos > 0 && held_[pos - 1].note > note) {
    held_[pos] = held_[pos - 1];
    --pos;
  }
  const std::uint64_t order = ++press_counter_;
  held_[pos] = HeldNote{note, velocity16, order, order, passthrough_sounding};
  ++held_count_;
}

void Arpeggiator::remove_held(std::uint8_t note) noexcept {
  for (std::size_t i = 0; i < held_count_; ++i) {
    if (held_[i].note == note) {
      for (std::size_t j = i + 1; j < held_count_; ++j) {
        held_[j - 1] = held_[j];
      }
      --held_count_;
      return;
    }
  }
}

void Arpeggiator::emit_all_sounding_offs(std::uint32_t frame, EventBuffer& out) noexcept {
  for (SoundingNote& s : sounding_) {
    if (s.active) {
      if (out.push(make_note_off(0, part_index_, s.note, 0), frame)) {
        s.active = false;
      }
    }
  }
}

void Arpeggiator::emit_due_sounding_offs(std::uint64_t abs_frame, std::uint32_t frame, EventBuffer& out) noexcept {
  for (SoundingNote& s : sounding_) {
    if (s.active && s.off_frame <= abs_frame) {
      if (out.push(make_note_off(0, part_index_, s.note, 0), frame)) {
        s.active = false;
      }
    }
  }
}

void Arpeggiator::apply_mode_transition(std::uint32_t frame, const TransportClock& clock, EventBuffer& out) noexcept {
  if (!config_.enabled) {
    emit_all_sounding_offs(frame, out);
    for (std::size_t i = 0; i < held_count_; ++i) {
      if (!held_[i].passthrough_sounding) {
        out.push(make_note_on(0, part_index_, held_[i].note, held_[i].velocity16), frame);
        held_[i].passthrough_sounding = true;
      }
    }
    pattern_running_ = false;
    rate_changed_ = false;
    return;
  }
  for (std::size_t i = 0; i < held_count_; ++i) {
    if (held_[i].passthrough_sounding) {
      out.push(make_note_off(0, part_index_, held_[i].note, 0), frame);
      held_[i].passthrough_sounding = false;
    }
  }
  if (held_count_ > 0 && !pattern_running_) {
    step_index_ = 0;
    next_grid_tick_ = first_grid_tick_from_frame(clock, frame, arp_rate_ticks(config_.rate));
    pattern_running_ = true;
    rate_changed_ = false;
  } else if (held_count_ > 0 && pattern_running_ && rate_changed_) {
    next_grid_tick_ = first_grid_tick_from_frame(clock, frame, arp_rate_ticks(config_.rate));
    rate_changed_ = false;
  }
}

std::size_t Arpeggiator::build_expanded_pool(std::array<ExpandedNote, kMaxExpandedNotes>& pool) const noexcept {
  if (held_count_ == 0) return 0;
  std::array<std::size_t, kMaxHeldNotes> order{};
  for (std::size_t i = 0; i < held_count_; ++i) order[i] = i;
  if (config_.mode == ArpMode::kAsPlayed) {
    // Insertion sort by press_order ascending (held_count_ <= 128).
    for (std::size_t i = 1; i < held_count_; ++i) {
      const std::size_t key = order[i];
      std::size_t j = i;
      while (j > 0 && held_[order[j - 1]].press_order > held_[key].press_order) {
        order[j] = order[j - 1];
        --j;
      }
      order[j] = key;
    }
  }
  const std::uint32_t octaves = std::clamp<std::uint32_t>(config_.octaves, 1u, 4u);
  std::size_t count = 0;
  for (std::uint32_t oct = 0; oct < octaves; ++oct) {
    const std::uint32_t shift = 12u * oct;
    for (std::size_t i = 0; i < held_count_; ++i) {
      const HeldNote& h = held_[order[i]];
      const std::uint32_t shifted = static_cast<std::uint32_t>(h.note) + shift;
      if (shifted <= 127u && count < kMaxExpandedNotes) {
        pool[count++] = ExpandedNote{static_cast<std::uint8_t>(shifted), h.velocity16};
      }
    }
  }
  return count;
}

void Arpeggiator::trigger_step(std::uint32_t frame, const TransportClock& clock, EventBuffer& out) noexcept {
  std::array<ExpandedNote, kMaxExpandedNotes> pool{};
  const std::size_t m = build_expanded_pool(pool);
  if (m == 0) return;

  ExpandedNote chosen{};
  switch (config_.mode) {
    case ArpMode::kUp:
    case ArpMode::kAsPlayed:
      chosen = pool[step_index_ % m];
      break;
    case ArpMode::kDown:
      chosen = pool[m - 1 - (step_index_ % m)];
      break;
    case ArpMode::kUpDown:
      if (m == 1) {
        chosen = pool[0];
      } else {
        const std::size_t cycle = 2 * m - 2;
        const std::size_t pos = static_cast<std::size_t>(step_index_ % cycle);
        chosen = pool[pos < m ? pos : (cycle - pos)];
      }
      break;
    case ArpMode::kRandom:
      chosen = pool[static_cast<std::size_t>(step_rng(rng_state_) % m)];
      break;
  }
  ++step_index_;

  const std::int64_t step_ticks = arp_rate_ticks(config_.rate);
  const std::int64_t f_next = clock.frame_of_tick(next_grid_tick_ + step_ticks);
  const std::int64_t step_frames = std::max<std::int64_t>(1, f_next - static_cast<std::int64_t>(frame));
  const double gate_ratio = static_cast<double>(std::clamp(config_.gate_percent, 1.0f, 200.0f)) / 100.0;
  const std::int64_t gate_frames = std::max<std::int64_t>(1, std::llround(static_cast<double>(step_frames) * gate_ratio));

  // If the same pitch is still sounding (e.g. Gate >= 100%), emit Note Off before re-striking so voices and the arbiter stay balanced.
  std::size_t free_slot = kMaxSoundingNotes;
  std::size_t oldest_slot = 0;
  std::uint64_t oldest_off = UINT64_MAX;
  for (std::size_t i = 0; i < kMaxSoundingNotes; ++i) {
    if (sounding_[i].active && sounding_[i].note == chosen.note) {
      out.push(make_note_off(0, part_index_, chosen.note, 0), frame);
      sounding_[i].active = false;
    }
    if (!sounding_[i].active && free_slot == kMaxSoundingNotes) {
      free_slot = i;
    } else if (sounding_[i].active && sounding_[i].off_frame < oldest_off) {
      oldest_off = sounding_[i].off_frame;
      oldest_slot = i;
    }
  }
  if (free_slot == kMaxSoundingNotes) {
    out.push(make_note_off(0, part_index_, sounding_[oldest_slot].note, 0), frame);
    sounding_[oldest_slot].active = false;
    free_slot = oldest_slot;
  }

  out.push(make_note_on(0, part_index_, chosen.note, chosen.velocity16), frame);
  sounding_[free_slot] = SoundingNote{
      chosen.note,
      clock.frames() + static_cast<std::uint64_t>(frame) + static_cast<std::uint64_t>(gate_frames),
      true,
  };
}

void Arpeggiator::process_input(const Ump& ump, std::uint32_t frame, const TransportClock& clock,
                                EventBuffer& out) noexcept {
  apply_mode_transition(frame, clock, out);
  if (!config_.enabled) {
    if (ump.is_note_on()) {
      add_or_update_held(static_cast<std::uint8_t>(ump.note()), static_cast<std::uint16_t>(ump.velocity16()), true);
    } else if (ump.is_note_off()) {
      remove_held(static_cast<std::uint8_t>(ump.note()));
    }
    out.push(ump, frame);
    return;
  }
  const std::uint64_t abs_f = clock.frames() + frame;
  emit_due_sounding_offs(abs_f, frame, out);
  const bool had_held = held_count_ > 0;
  const std::int64_t step_ticks = arp_rate_ticks(config_.rate);
  if (ump.is_note_on()) {
    add_or_update_held(static_cast<std::uint8_t>(ump.note()), static_cast<std::uint16_t>(ump.velocity16()), false);
    if (!had_held || !pattern_running_) {
      step_index_ = 0;
      next_grid_tick_ = first_grid_tick_from_frame(clock, frame, step_ticks);
      pattern_running_ = true;
    }
    while (clock.frame_of_tick(next_grid_tick_) < static_cast<std::int64_t>(frame)) {
      next_grid_tick_ += step_ticks;
    }
    if (clock.frame_of_tick(next_grid_tick_) == static_cast<std::int64_t>(frame)) {
      trigger_step(frame, clock, out);
      next_grid_tick_ += step_ticks;
    }
  } else if (ump.is_note_off()) {
    remove_held(static_cast<std::uint8_t>(ump.note()));
    if (held_count_ == 0) {
      pattern_running_ = false;
      emit_all_sounding_offs(frame, out);
    }
  } else {
    out.push(ump, frame);
  }
}

void Arpeggiator::process_block(const EventBuffer& in, const TransportClock& clock, std::uint32_t frames,
                                EventBuffer& out) noexcept {
  apply_mode_transition(0, clock, out);
  if (!config_.enabled) {
    for (std::size_t i = 0; i < in.size(); ++i) {
      const Ump& u = in[i].ump;
      if (u.is_note_on()) {
        add_or_update_held(static_cast<std::uint8_t>(u.note()), static_cast<std::uint16_t>(u.velocity16()), true);
      } else if (u.is_note_off()) {
        remove_held(static_cast<std::uint8_t>(u.note()));
      }
      out.push(u, in[i].target_frame);
    }
    return;
  }

  std::size_t in_idx = 0;
  const std::int64_t step_ticks = arp_rate_ticks(config_.rate);
  for (std::uint32_t f = 0; f < frames; ++f) {
    const std::uint64_t abs_f = clock.frames() + f;
    emit_due_sounding_offs(abs_f, f, out);

    if (in_idx < in.size() && in[in_idx].target_frame == f) {
      const std::uint64_t prev_max_order = press_counter_;
      const bool had_held = held_count_ > 0;
      while (in_idx < in.size() && in[in_idx].target_frame == f) {
        const Ump& u = in[in_idx++].ump;
        if (u.is_note_on()) {
          add_or_update_held(static_cast<std::uint8_t>(u.note()), static_cast<std::uint16_t>(u.velocity16()), false);
        } else if (u.is_note_off()) {
          remove_held(static_cast<std::uint8_t>(u.note()));
        } else {
          out.push(u, f);
        }
      }
      if (held_count_ == 0) {
        pattern_running_ = false;
        emit_all_sounding_offs(f, out);
      } else {
        bool any_prev_survives = false;
        if (had_held) {
          for (std::size_t i = 0; i < held_count_; ++i) {
            if (held_[i].first_press_order <= prev_max_order) {
              any_prev_survives = true;
              break;
            }
          }
        }
        if (!had_held || !any_prev_survives || !pattern_running_) {
          if (had_held && !any_prev_survives) {
            emit_all_sounding_offs(f, out);
          }
          step_index_ = 0;
          next_grid_tick_ = first_grid_tick_from_frame(clock, f, step_ticks);
          pattern_running_ = true;
        }
      }
    }

    if (pattern_running_ && held_count_ > 0) {
      if (f == 0 && clock.frame_of_tick(next_grid_tick_) < 0 &&
          next_grid_tick_ * TransportClock::kUnitsPerTick >=
              clock.position_units() - static_cast<std::int64_t>(clock.tempo_tenths()) * 64) {
        trigger_step(0, clock, out);
        next_grid_tick_ += step_ticks;
      }
      while (clock.frame_of_tick(next_grid_tick_) < static_cast<std::int64_t>(f)) {
        next_grid_tick_ += step_ticks;
      }
      if (clock.frame_of_tick(next_grid_tick_) == static_cast<std::int64_t>(f)) {
        trigger_step(f, clock, out);
        next_grid_tick_ += step_ticks;
      }
    }
  }
}

}  // namespace velora::core

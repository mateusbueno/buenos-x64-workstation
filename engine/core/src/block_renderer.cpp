#include "velora/core/block_renderer.hpp"

#include <cassert>

#include "velora/core/contact_events.hpp"

namespace velora::core {

BlockRenderer::BlockRenderer(std::uint32_t test_tone_voices)
    : arena_(64 * 1024), tone_(arena_, test_tone_voices) {
  bound_ = registry_.bind(tone_);
  assert(bound_);
  [[maybe_unused]] const bool arbiter_bound = arbiter_.bind(tone_);
  assert(arbiter_bound);
  channel_engine_.fill(static_cast<std::uint32_t>(EngineTypeId::kTestTone));
}

void BlockRenderer::record(EngineStatus s) noexcept {
  ++status_counts_[static_cast<std::size_t>(s)];
  last_status_ = s;
}

void BlockRenderer::drain_queue() noexcept {
  TimedUmp ev;
  while (queue_.pop(ev)) {
    if (pending_count_ >= kPendingCapacity) {
      record(EngineStatus::kEventOverflow);  // drop, but keep the queue draining
      continue;
    }
    // Stable insertion: after the last element with target_frame <= ev.target_frame.
    std::size_t pos = pending_count_;
    while (pos > 0 && pending_[pos - 1].target_frame > ev.target_frame) {
      pending_[pos] = pending_[pos - 1];
      --pos;
    }
    pending_[pos] = ev;
    ++pending_count_;
  }
}

void BlockRenderer::release_deferred(std::uint32_t channel) noexcept {
  for (std::uint32_t note = 0; note < 128; ++note) {
    std::uint32_t type_id = 0;
    if (!arbiter_.take_deferred_release(channel, note, type_id)) continue;
    Engine* engine = nullptr;
    if (registry_.resolve(type_id, engine) != EngineStatus::kOk) {  // the engine is gone: drop the release
      ++dropped_deferred_;
      continue;
    }
    const EngineStatus st = engine->stop_voice(channel, note);
    if (st == EngineStatus::kVoiceNotFound) {  // never sounded or stolen: nothing to release, not an error
      if (arbiter_.take_stolen_flag(channel, note)) ++stolen_note_offs_;
    } else {
      arbiter_.take_stolen_flag(channel, note);
      record(st);
    }
  }
}

void BlockRenderer::adopt_generation() noexcept {
  const Snapshot* next = exchange_.rt_acquire();
  if (next == held_ || !next) return;
  // Initial performance values apply only on generation change, and only where they changed. The diff is
  // against an RT-owned copy of the last applied values: the previous generation is no longer held (the
  // hazard moved) and the control thread may free it at any moment, so it must not be read here.
  std::array<bool, params::kPerfSlotCount> changed{};
  for (std::size_t i = 0; i < params::kPerfSlotCount; ++i) {
    changed[i] = !applied_valid_ || next->perf_init[i] != applied_init_[i];
    if (changed[i]) last_.store(i, next->perf_init[i]);
    applied_init_[i] = next->perf_init[i];
  }
  for (const params::ParamDef& d : params::kParams) {  // engines follow the values that changed
    if (d.cls != params::Class::kPerformance) continue;
    const std::uint32_t parts = d.scope == params::Scope::kPart ? params::kPartCount : 1;
    for (std::uint32_t p = 0; p < parts; ++p) {
      const std::size_t slot = params::perf_slot(d, p);
      if (changed[slot]) forward_part_param(d, p, next->perf_init[slot]);
    }
  }
  applied_valid_ = true;
  held_ = next;
}

void BlockRenderer::forward_part_param(const params::ParamDef& d, std::uint32_t part, float value) noexcept {
  if (d.scope != params::Scope::kPart || part >= 16) return;  // channel = part
  Engine* engine = nullptr;
  if (registry_.resolve(channel_engine_[part], engine) == EngineStatus::kOk) engine->set_part_param(part, d.id, value);
}

void BlockRenderer::sync_part_params(std::uint32_t channel) noexcept {
  if (channel >= params::kPartCount) return;
  for (const params::ParamDef& d : params::kParams)
    if (d.cls == params::Class::kPerformance && d.scope == params::Scope::kPart)
      forward_part_param(d, channel, last_.load(params::perf_slot(d, channel)));
}

void BlockRenderer::apply_param(const ParamEvent& ev) noexcept {
  last_.store(params::perf_slot(*ev.def, ev.part), ev.value);
  forward_part_param(*ev.def, ev.part, ev.value);
  param_applied_.fetch_add(1, std::memory_order_relaxed);
}

void BlockRenderer::bind_generation(std::uint32_t channel, std::uint32_t note) noexcept {
  if (!held_) return;
  std::uint32_t& mask = gen_mask_[(channel & 15u) * 128 + (note & 127u)];
  const std::uint32_t bit = 1u << (held_->ledger_index % SnapshotExchange::kLedgerSlots);
  if (mask & bit) return;
  if (mask == 0) ++ref_keys_;
  mask |= bit;
  exchange_.rt_ref(held_->ledger_index);
}

void BlockRenderer::reap_generation_refs() noexcept {
  if (ref_keys_ == 0) return;
  present_.fill(0);
  for (std::size_t e = 0; e < registry_.size(); ++e) {
    const Engine* eng = registry_.at(e);
    VoiceInfo vi;
    const std::uint32_t cap = eng->voice_capacity();
    for (std::uint32_t slot = 0; slot < cap; ++slot)
      if (eng->voice_info(slot, vi)) present_[(vi.channel & 15u) * 128 + (vi.note & 127u)] = 1;
  }
  for (std::size_t key = 0; key < gen_mask_.size() && ref_keys_ > 0; ++key) {
    std::uint32_t& mask = gen_mask_[key];
    if (mask == 0 || present_[key]) continue;
    for (std::uint32_t i = 0; i < SnapshotExchange::kLedgerSlots; ++i)
      if (mask & (1u << i)) exchange_.rt_unref(i);
    mask = 0;
    --ref_keys_;
  }
}

void BlockRenderer::apply(const Ump& ump) noexcept {
  if (ump.group() == 0 && ump.is_assignable_controller()) {
    ParamEvent pe;
    if (decode_param_event(ump, pe)) apply_param(pe);
    else param_rejected_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (ump.group() == 0 && ump.is_control_change() && ump.controller_index() == Ump::kControllerSustain) {
    const bool down = ump.controller_value32() >= 0x80000000u;  // MIDI value 64 of 0..127 scaled to 32 bits
    arbiter_.set_sustain(ump.channel(), down);
    if (!down) release_deferred(ump.channel());
    return;
  }
  if (ump.group() == 0 && ump.is_assignable_per_note_controller()) {  // contact_k (Story 2.5)
    apply_contact(ump);
    return;
  }
  if (ump.group() != 0 || (!ump.is_note_on() && !ump.is_note_off())) {
    ++ignored_;
    return;
  }
  // The arbiter's key/pedal state follows the events even when no engine serves the channel.
  const std::uint32_t type_id = channel_engine_[ump.channel()];
  bool stop_now = true;
  if (ump.is_note_on()) arbiter_.note_on(ump.channel(), ump.note());
  else stop_now = arbiter_.note_off(ump.channel(), ump.note(), type_id);
  Engine* engine = nullptr;
  const EngineStatus rs = registry_.resolve(type_id, engine);
  if (rs != EngineStatus::kOk) {
    record(rs);  // silence: nothing is rendered for this request
    return;
  }
  if (ump.is_note_on()) {
    const EngineStatus st = engine->start_voice(ump.channel(), ump.note(), ump.velocity16());
    record(st);
    if (st == EngineStatus::kOk) bind_generation(ump.channel(), ump.note());
  } else if (stop_now) {
    const EngineStatus st = engine->stop_voice(ump.channel(), ump.note());
    const bool was_stolen = arbiter_.take_stolen_flag(ump.channel(), ump.note());
    if (st == EngineStatus::kVoiceNotFound && was_stolen) ++stolen_note_offs_;
    else record(st);
  }  // else: the pedal holds the note; it is released on pedal up
}

// contact_k: routed to the engine that serves the channel, at the event's frame (this runs between render segments). It is
// never "ignored": an engine without contact support (everything but the organ) or a note field above 127 counts as dropped,
// everything else as routed. Contact events do not touch the arbiter's key/pedal state (the organ is never stolen and has no
// sustain pedal; a sustain-pedal Note Off still defers only the Note Off) -- see ORGAN.md.
void BlockRenderer::apply_contact(const Ump& ump) noexcept {
  // Only per-note controller indices 1..kMaxWireContact are contact_k; any other per-note controller is an unsupported UMP.
  const std::uint32_t index = ump.word0 & 0xFFu;
  if (index < 1 || index > kMaxWireContact) {
    ++ignored_;
    return;
  }
  ContactEvent ce;
  if (!decode_contact_event(ump, ce)) {
    contact_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Engine* engine = nullptr;
  const EngineStatus rs = registry_.resolve(channel_engine_[ce.channel], engine);
  if (rs != EngineStatus::kOk) {
    record(rs);
    contact_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!engine->supports_contacts()) {
    contact_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Engine::ContactEffect effect = Engine::ContactEffect::kNone;
  const EngineStatus st = engine->set_contact(ce.channel, ce.note, ce.contact, ce.closed, effect);
  if (st != EngineStatus::kOk) {  // refused (part not available, no voice slot): not delivered
    record(st);
    contact_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  contact_routed_.fetch_add(1, std::memory_order_relaxed);
  if (effect == Engine::ContactEffect::kStarted) {  // a voice exists now: the key is down and the voice holds the generation
    arbiter_.contact_down(ce.channel, ce.note);
    bind_generation(ce.channel, ce.note);
  } else if (effect == Engine::ContactEffect::kReleased) {
    arbiter_.contact_up(ce.channel, ce.note);
  }
}

void BlockRenderer::render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to) noexcept {
  if (to <= from) return;
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->render(l + from, r + from, to - from);
}

void BlockRenderer::process_block(float* left, float* right) noexcept {
  if (!left || !right) return;
  for (std::uint32_t i = 0; i < kBlockFrames; ++i) left[i] = right[i] = 0.0f;
  adopt_generation();
  drain_queue();
  // Phase 2: shared per-part generators (the organ's tonewheels) produce their block before voices render.
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->begin_block(kBlockFrames);

  std::uint32_t pos = 0;
  std::size_t consumed = 0;
  while (consumed < pending_count_ && pending_[consumed].target_frame < kBlockFrames) {
    const std::uint32_t f = pending_[consumed].target_frame;
    render_segment(left, right, pos, f);
    pos = f;
    apply(pending_[consumed].ump);
    ++consumed;
  }
  render_segment(left, right, pos, kBlockFrames);

  // Drop applied events, carry the rest into the next block.
  const std::size_t remaining = pending_count_ - consumed;
  for (std::size_t i = 0; i < remaining; ++i) {
    pending_[i] = pending_[consumed + i];
    pending_[i].target_frame -= kBlockFrames;
  }
  pending_count_ = remaining;
  reap_generation_refs();
}

}  // namespace velora::core

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
  // A Live Set generation maps each part to its engine and instrument first, so the changed values below reach the right engine.
  std::array<bool, params::kPartCount> remapped{};
  if (in_live_set_ && !next->live_set) leave_live_set(remapped);
  router_.configure(*next);
  if (next->live_set) adopt_live_set_parts(*next, remapped);
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
  for (std::uint32_t p = 0; p < params::kPartCount; ++p)  // a part that moved to another engine gets the live values it never saw
    if (remapped[p]) sync_part_params(p);
  applied_valid_ = true;
  held_ = next;
  mixer_.adopt(*next, &last_);  // part outputs, master level and the insert chains (the generation is held: safe to read)
}

// The block loop goes from a Live Set generation to a plain one: the notes the router still holds are released (with the engine and pitch they
// went to), the sample engine forgets its per-part banks (they belong to generations that may be freed) and every channel gets the default
// engine again.
void BlockRenderer::leave_live_set(std::array<bool, params::kPartCount>& remapped) noexcept {
  router_.release_all([this](std::uint32_t part, std::uint32_t pitch, std::uint32_t engine) { apply_part_note(part, pitch, 0, false, engine); });
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->clear_part_sample_banks();
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    if (channel_engine_[p] != legacy_engine_[p]) remapped[p] = true;
    channel_engine_[p] = legacy_engine_[p];
  }
  in_live_set_ = false;
}

void BlockRenderer::adopt_live_set_parts(const Snapshot& s, std::array<bool, params::kPartCount>& remapped) noexcept {
  if (!in_live_set_) {
    legacy_engine_ = channel_engine_;
    in_live_set_ = true;
  }
  Engine* sample = nullptr;
  const bool has_sample = registry_.resolve(static_cast<std::uint32_t>(EngineTypeId::kSample), sample) == EngineStatus::kOk;
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    const PartCfg& c = s.parts[p];
    // A part that does not sound maps to no engine (type 0 is unknown to the registry): silence, never a substitute.
    const std::uint32_t type = c.state == PartState::kReady ? c.engine_type : 0u;
    if (channel_engine_[p] != type) {
      channel_engine_[p] = type;
      remapped[p] = true;
    }
    if (has_sample) {
      const bool is_sample = type == static_cast<std::uint32_t>(EngineTypeId::kSample);
      sample->set_part_sample_bank(p, is_sample ? c.sample_bank.get() : nullptr);
    }
  }
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
    if (router_.active()) {
      apply_keyboard_sustain(down);
      return;
    }
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
  if (router_.active()) {
    apply_keyboard_note(ump);
    return;
  }
  apply_part_note(ump.channel(), ump.note(), ump.velocity16(), ump.is_note_on(), channel_engine_[ump.channel()]);
}

// The note path of one (channel, engine): the arbiter's key/pedal state follows the events even when no engine serves the channel.
void BlockRenderer::apply_part_note(std::uint32_t channel, std::uint32_t note, std::uint32_t velocity16, bool on,
                                    std::uint32_t type_id) noexcept {
  if (on && router_.active()) {
    // A part can change engine between generations. A voice the pedal still holds on the OLD engine for this very (part, note) would lose
    // its deferred release when the new note on resets the key (one deferred slot per key): let it go now.
    std::uint32_t held_type = 0;
    if (arbiter_.deferred_release_type(channel, note, held_type) && held_type != type_id && arbiter_.take_deferred_release(channel, note, held_type)) {
      Engine* old_engine = nullptr;
      if (registry_.resolve(held_type, old_engine) == EngineStatus::kOk) {
        old_engine->stop_voice(channel, note);
        arbiter_.take_stolen_flag(channel, note);
      } else {
        ++dropped_deferred_;
      }
    }
  }
  bool stop_now = true;
  if (on) arbiter_.note_on(channel, note);
  else stop_now = arbiter_.note_off(channel, note, type_id);
  Engine* engine = nullptr;
  const EngineStatus rs = registry_.resolve(type_id, engine);
  if (rs != EngineStatus::kOk) {
    record(rs);  // silence: nothing is rendered for this request
    return;
  }
  if (on) {
    const EngineStatus st = engine->start_voice(channel, note, velocity16);
    record(st);
    // In a Live Set the voices hold their bank by raw pointer, so any voice that started (even if a later region found no slot) binds the generation.
    if (st == EngineStatus::kOk || router_.active()) bind_generation(channel, note);
  } else if (stop_now) {
    const EngineStatus st = engine->stop_voice(channel, note);
    const bool was_stolen = arbiter_.take_stolen_flag(channel, note);
    if (st == EngineStatus::kVoiceNotFound && was_stolen) ++stolen_note_offs_;
    else record(st);
    // A release-trigger voice may have started on the part's CURRENT instrument, which this key's generation does not hold: bind it too.
    if (router_.active()) bind_generation(channel, note);
  }  // else: the pedal holds the note; it is released on pedal up
}

// Live Set keyboard path: fan the note out by zone (the key as played), or release exactly the parts that got the note on.
void BlockRenderer::apply_keyboard_note(const Ump& ump) noexcept {
  PartRouter::Fanout fan;
  if (ump.is_note_on()) {
    router_.note_on(ump.channel(), ump.note(), fan);
    for (std::uint32_t i = 0; i < fan.count; ++i) {
      const PartRouter::Target& t = fan.t[i];
      if (t.release_first) apply_part_note(t.part, t.old_pitch, 0, false, t.old_engine);  // a re-struck key lets its earlier note go first
      apply_part_note(t.part, t.pitch, ump.velocity16(), true, t.engine);
    }
  } else {
    router_.note_off(ump.channel(), ump.note(), fan);
    for (std::uint32_t i = 0; i < fan.count; ++i) apply_part_note(fan.t[i].part, fan.t[i].pitch, ump.velocity16(), false, fan.t[i].engine);
  }
}

// Sustain pedal in a Live Set: down reaches the parts with the keyboard on, up reaches every part (and lets its deferred releases go).
void BlockRenderer::apply_keyboard_sustain(bool down) noexcept {
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    if (down) {
      if (router_.pedal_down_target(p)) arbiter_.set_sustain(p, true);
    } else {
      arbiter_.set_sustain(p, false);
      release_deferred(p);
    }
  }
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

void BlockRenderer::render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to, bool routed) noexcept {
  if (to <= from) return;
  if (!routed) {
    for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->render(l + from, r + from, to - from);
    return;
  }
  float* ls[Engine::kSplitChannels];
  float* rs[Engine::kSplitChannels];
  mixer_.part_sinks(from, ls, rs);  // each engine adds its parts into the part buses
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->render_split(ls, rs, to - from);
}

float BlockRenderer::tempo_bpm() const noexcept {
  const params::ParamDef* d = params::find(params::kTempoBpm);
  return d ? delaymap::tempo_bpm(last_.load(params::perf_slot(*d, 0))) : delaymap::kDefaultTempoBpm;
}

void BlockRenderer::process_block(float* left, float* right) noexcept {
  if (!left || !right) return;
  OutputBuses out;
  out.main_left = left;
  out.main_right = right;
  out.aux_left = &fold_[0];
  out.aux_right = &fold_[kBlockFrames];
  process_block(out);
  if (!mixer_.routed()) return;  // trivial routing: Aux is silent
  bool any = false;
  for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
    any = any || fold_[i] != 0.0f || fold_[kBlockFrames + i] != 0.0f;
    left[i] += fold_[i];
    right[i] += fold_[kBlockFrames + i];
  }
  if (any) aux_folded_.fetch_add(1, std::memory_order_relaxed);
}

void BlockRenderer::process_block(const OutputBuses& out) noexcept {
  float* const left = out.main_left;
  float* const right = out.main_right;
  if (!left || !right) return;
  const bool has_aux = out.aux_left && out.aux_right;
  if ((out.aux_left != nullptr) != (out.aux_right != nullptr)) {  // half an Aux bus: refuse the block
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) left[i] = right[i] = 0.0f;
    rejected_outputs_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (std::uint32_t i = 0; i < kBlockFrames; ++i) left[i] = right[i] = 0.0f;
  if (has_aux)
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) out.aux_left[i] = out.aux_right[i] = 0.0f;
  adopt_generation();
  drain_queue();
  // Phase 2: shared per-part generators (the organ's tonewheels) produce their block before voices render.
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->begin_block(kBlockFrames);
  // Phase 5 setup: trivial routing renders straight into Main (the pre-2.7 path, bit-identical); otherwise into the part buses.
  const bool routed = mixer_.begin_block(last_);

  std::uint32_t pos = 0;
  std::size_t consumed = 0;
  while (consumed < pending_count_ && pending_[consumed].target_frame < kBlockFrames) {
    const std::uint32_t f = pending_[consumed].target_frame;
    render_segment(left, right, pos, f, routed);
    pos = f;
    apply(pending_[consumed].ump);
    ++consumed;
  }
  render_segment(left, right, pos, kBlockFrames, routed);
  if (routed) mixer_.finish(out, last_);  // chains, sends, Main/Aux, line-in, master

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

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
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) arps_[p].set_part_index(p);
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
  const bool seamless_arp = held_ != nullptr;
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    arps_[p].set_config(next->parts[p].arp, seamless_arp);
    if (custom_processors_[p] == nullptr && arps_[p].enabled()) {
      arbiter_.set_sustain(p, false);
    }
  }
  // Seamless Live Set switch (Story 4.5): the reservation prepared by the control thread takes effect at this very generation. Sounding
  // voices keep their slots and costs (VoiceArbiter::commit_prepared documents the accounting); the keyboard-pedal state is not reset.
  const bool live_set_switch = next->reservation_epoch != 0 && next->reservation_epoch != applied_reservation_epoch_;
  if (live_set_switch) {
    arbiter_.commit_prepared(next->reservation);
    applied_reservation_epoch_ = next->reservation_epoch;
    // Per-part play state belongs to the Live Set (keyswitches return to the instrument's sw_default); controllers are physical.
    for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->reset_play_state();
    // The pedal is a keyboard event, not a part event: a pedal that is physically down when the new Live Set takes over must also hold the
    // new notes of the parts that listen to the keyboard (their pedal flag may have been unset under the old part layout).
    if (next->live_set && keyboard_pedal_down_)
      for (std::uint32_t p = 0; p < params::kPartCount; ++p)
        if (router_.pedal_down_target(p)) {
          if (custom_processors_[p] == nullptr && arps_[p].enabled()) arbiter_.set_sustain(p, false);
          else arbiter_.set_sustain(p, true);
        }
  }
  if (next->live_set) replay_keyboard_ccs(*next, live_set_switch, remapped);
  std::array<bool, params::kPerfSlotCount> changed{};
  // A scene recall (new program_generation) also re-applies the slots it stores, once, whatever the previous generation held.
  const bool program_change = applied_valid_ && next->program_generation != applied_program_generation_;
  for (std::size_t i = 0; i < params::kPerfSlotCount; ++i) {
    // A Live Set switch is a program change: every performance slot takes the new Live Set's initial value, so a knob the player moved live
    // under the old Live Set never leaks into the new one (scene recalls keep their own forced-slot mechanism).
    changed[i] = !applied_valid_ || live_set_switch || next->perf_init[i] != applied_init_[i] || (program_change && next->perf_force[i] != 0);
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
  applied_program_generation_ = next->program_generation;
  if (program_change || live_set_switch) {
    last_.encoder_rt().reset_all_offsets();
  }
  held_ = next;
  last_.store_adopted_revision(next->revision);  // after every slot of this generation was applied (Story 4.9)
  mixer_.adopt(*next, &last_);  // part outputs, master level and the insert chains (the generation is held: safe to read)
}

// The block loop goes from a Live Set generation to a plain one: the notes the router still holds are released (with the engine and pitch they
// went to), the sample engine forgets its per-part banks (they belong to generations that may be freed) and every channel gets the default
// engine again.
void BlockRenderer::leave_live_set(std::array<bool, params::kPartCount>& remapped) noexcept {
  current_in_block_frame_ = 0;
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    part_out_[p].clear();
    arps_[p].flush_sounding(part_out_[p]);
    if (part_out_[p].dropped() > 0) record(EngineStatus::kEventOverflow);
    for (std::size_t i = 0; i < part_out_[p].size(); ++i) {
      const Ump& u = part_out_[p][i].ump;
      if (u.is_note_off()) {
        const std::uint32_t note = u.note();
        const std::size_t key_idx = p * 128u + (note & 127u);
        const std::uint32_t eng = note_engine_[key_idx] != 0 ? note_engine_[key_idx] : channel_engine_[p];
        apply_part_note(p, note, 0, false, eng, true);
      }
    }
  }
  router_.release_all([this](std::uint32_t part, std::uint32_t pitch, std::uint32_t engine) {
    const bool passthrough = part >= params::kPartCount ||
                             (custom_processors_[part] == nullptr && arps_[part].is_passthrough_sounding(pitch));
    if (part < params::kPartCount) arps_[part].track_passthrough_note(pitch, 0, false);
    if (passthrough) apply_part_note(part, pitch, 0, false, engine);
  });
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) arps_[p].reset();
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->clear_part_sample_banks();
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    if (channel_engine_[p] != legacy_engine_[p]) remapped[p] = true;
    channel_engine_[p] = legacy_engine_[p];
  }
  in_live_set_ = false;
  keyboard_pitch_seen_ = false;
  keyboard_cc_seen_.fill(false);  // the keyboard's controllers of that Live Set must not be replayed into a later one
  replay_listening_.fill(false);
  keyboard_pedal_down_ = false;  // the plain path owns the pedal again (its own events set it)
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

void BlockRenderer::forward_part_param(const params::ParamDef& d, std::uint32_t part, float value, bool edit) noexcept {
  if (d.scope != params::Scope::kPart || part >= 16) return;  // channel = part
  Engine* engine = nullptr;
  if (registry_.resolve(channel_engine_[part], engine) != EngineStatus::kOk) return;
  if (edit) engine->set_part_param_edit(part, d.id, value);
  else engine->set_part_param(part, d.id, value);
}

void BlockRenderer::sync_part_params(std::uint32_t channel) noexcept {
  if (channel >= params::kPartCount) return;
  for (const params::ParamDef& d : params::kParams)
    if (d.cls == params::Class::kPerformance && d.scope == params::Scope::kPart)
      forward_part_param(d, channel, last_.load(params::perf_slot(d, channel)));
}

void BlockRenderer::apply_param(const ParamEvent& ev) noexcept {
  last_.store(params::perf_slot(*ev.def, ev.part), ev.value);
  forward_part_param(*ev.def, ev.part, ev.value, /*edit=*/true);
  param_applied_.fetch_add(1, std::memory_order_relaxed);
}

// The reserved channel-15 events (assign_events.hpp). Runs between render segments, at the event's frame, in the same order as parameter events.
void BlockRenderer::apply_reserved_event(std::uint32_t id, const Ump& ump) noexcept {
  EncoderState& enc = last_.encoder_rt();
  if (id == kPanelControlEventId) {  // a raw panel control (Story 3.10): kept for the Epic 4 bindings, nothing is bound here
    panel_value_[panel_control_id(ump)].store(static_cast<std::uint16_t>(panel_control_value16(ump)), std::memory_order_relaxed);
    panel_controls_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (id >= kEncoderStepFirstId && id < kEncoderStepFirstId + kEncoderCount) {
    const std::int32_t delta = static_cast<std::int32_t>(ump.word1);
    if (delta < -32768 || delta > 32767) {  // the wire carries an int16 in an int32
      enc.count_rejected();
      return;
    }
    apply_encoder_step(id - kEncoderStepFirstId, delta);
    return;
  }
  if (id == kEncoderModeEventId) {
    if (ump.word1 >= kEncoderModeCount) {
      enc.count_bad_event();
    } else {
      const EncoderMode new_mode = static_cast<EncoderMode>(ump.word1);
      if (new_mode == EncoderMode::kAll && enc.mode() != EncoderMode::kAll) {
        enc.reset_all_offsets();
      }
      enc.set_mode(new_mode);
    }
    return;
  }
  if (id == kSelectPartEventId) {
    if (ump.word1 >= params::kPartCount) enc.count_bad_event();
    else enc.set_selected_part(ump.word1);
    return;
  }
  param_rejected_.fetch_add(1, std::memory_order_relaxed);  // an unknown reserved id (the fence is handled before)
}

// Encoder routing (Story 4.10, Story 4.13): encoder `encoder` (0..7) turned by `delta` steps.
// PART: controls 8 fixed functions on selected part (no pickup).
// ALL: controls the same fixed functions across all 8 parts, tracking relative offset from mode entry.
// ASSIGN: slot n of the held generation's map names the parameter, applying to live value on selected/master part (no pickup).
void BlockRenderer::apply_encoder_step(std::uint32_t encoder, std::int32_t delta) noexcept {
  EncoderState& enc = last_.encoder_rt();
  if (encoder >= kEncoderCount) {
    enc.count_rejected();
    return;
  }
  const EncoderMode mode = enc.mode();
  if (mode == EncoderMode::kAssign) {
    const std::uint32_t id = held_ && encoder < kAssignSlotCount ? held_->assign[encoder] : 0u;
    if (id == 0) {
      enc.count_empty();
      return;
    }
    const params::ParamDef* d = find_assignable(id);
    if (!d) {  // a map from another registry: refuse, never guess (also guards perf_slot() below)
      enc.count_rejected();
      return;
    }
    ParamEvent ev;
    ev.def = d;
    ev.part = d->scope == params::Scope::kPart ? enc.selected_part() : 0u;
    ev.value = apply_relative_step(*d, last_.load(params::perf_slot(*d, ev.part)), delta);
    Engine* engine = nullptr;
    const bool has_part = d->scope != params::Scope::kPart || registry_.resolve(channel_engine_[ev.part], engine) == EngineStatus::kOk;
    apply_param(ev);
    if (has_part) enc.count_applied();
    else enc.count_no_part();
    return;
  }

  // PART and ALL modes (Story 4.13)
  const std::uint32_t param_id = kPartModeParams[encoder];
  const params::ParamDef* d = params::find(param_id);
  if (!d || !is_assignable(*d)) {
    enc.count_rejected();
    return;
  }

  if (mode == EncoderMode::kPart) {
    const std::uint32_t part = enc.selected_part();
    ParamEvent ev;
    ev.def = d;
    ev.part = part;
    ev.value = apply_relative_step(*d, last_.load(params::perf_slot(*d, part)), delta);
    Engine* engine = nullptr;
    const bool has_part = registry_.resolve(channel_engine_[part], engine) == EngineStatus::kOk;
    apply_param(ev);
    if (has_part) enc.count_applied();
    else enc.count_no_part();
    return;
  }

  if (mode == EncoderMode::kAll) {
    const float step_fraction = static_cast<float>(delta) / 128.0f;
    const float old_offset = enc.all_offset(encoder);
    const float new_offset = std::clamp(old_offset + step_fraction, -0.5f, 0.5f);
    enc.set_all_offset(encoder, new_offset);

    bool any_has_part = false;
    for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
      ParamEvent ev;
      ev.def = d;
      ev.part = p;
      ev.value = apply_relative_step(*d, last_.load(params::perf_slot(*d, p)), delta);
      Engine* engine = nullptr;
      if (registry_.resolve(channel_engine_[p], engine) == EngineStatus::kOk) {
        any_has_part = true;
      }
      apply_param(ev);
    }
    if (any_has_part) enc.count_applied();
    else enc.count_no_part();
    return;
  }

  enc.count_unrouted();
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
  if (is_fence_event(ump)) {  // Story 4.9: everything the control thread sent before the fence has been applied; one store acknowledges it
    last_.store_fence_ack(ump.word1);
    return;
  }
  if (ump.group() == 0) {  // Story 8.8: input activity for the host's idle-safe policy (relaxed counters only)
    if (ump.is_note_on() || ump.is_note_off()) {
      const std::size_t k = static_cast<std::size_t>(ump.channel() & 0xFu) * 128u + (ump.note() & 0x7Fu);
      const std::uint8_t want = (ump.is_note_on() && ump.velocity16() != 0) ? 1 : 0;  // note on with velocity 0 is a note off
      if (input_held_[k] != want) {
        input_held_[k] = want;
        if (want) ++input_held_count_;
        else --input_held_count_;
      }
    } else if (ump.is_control_change()) {
      const std::uint32_t cc = ump.controller_index();
      if (cc == Ump::kControllerSustain) {
        input_pedal_down_ = Ump::sustain_down(ump.controller_value32());
      } else if (cc == 120 || cc == 123) {  // all sound off / all notes off: that channel's keys are up (a lost note off must not stick)
        const std::size_t base = static_cast<std::size_t>(ump.channel() & 0xFu) * 128u;
        for (std::size_t e = 0; e < registry_.size(); ++e) registry_.at(e)->all_keys_released(ump.channel() & 0xFu);  // engines that track held keys (first / legato)
        for (std::size_t n = 0; n < 128; ++n) {
          if (input_held_[base + n]) {
            input_held_[base + n] = 0;
            --input_held_count_;
          }
        }
      }
    }
  }
  if (ump.group() == 0 && ump.is_assignable_controller()) {
    std::uint32_t reserved_id = 0;
    if (decode_reserved_event_id(ump, reserved_id)) {  // channel 15: the encoder / mode / select-part events (Story 4.10)
      apply_reserved_event(reserved_id, ump);
      return;
    }
    ParamEvent pe;
    if (decode_param_event(ump, pe)) apply_param(pe);
    else param_rejected_.fetch_add(1, std::memory_order_relaxed);
    if (is_midi_cc_origin(ump)) {  // a registry-mapped MIDI CC is also a raw controller (Story 5.2), whether or not the parameter took it
      const params::ParamDef* d = params::find((((ump.word0 >> 8) & 0x7Fu) << 7) | (ump.word0 & 0x7Fu));
      if (d && d->cc >= 0) route_cc(ump.channel(), static_cast<std::uint32_t>(d->cc), ump.word1, false);
    }
    return;
  }
  if (ump.group() == 0 && ump.is_control_change() && ump.controller_index() == Ump::kControllerSustain) {
    const std::uint32_t v32 = ump.controller_value32();
    const bool down = Ump::sustain_down(v32);  // MIDI value 64 of 0..127 scaled to 32 bits
    if (router_.active()) {
      apply_keyboard_sustain(down, v32);
      return;
    }
    keyboard_pedal_down_ = down;  // kept in step on every pedal path, so a switch sees the physical pedal
    const std::uint32_t ch = ump.channel();
    // Story 3.11: the continuous value reaches the engines BEFORE the release it may trigger when the pedal goes down (nothing is released), and AFTER the deferred
    // releases when it comes up: those notes were held (the engine's last value was >= the threshold, scale 1), so a normal pedal lift releases them at the
    // instrument's own rate; only a note released later, with the pedal in the half-pedal zone, is damped.
    if (down) deliver_cc(ch, Ump::kControllerSustain, v32);
    if (ch < params::kPartCount) {
      arps_[ch].set_sustain_pedal(down);
      if (custom_processors_[ch] == nullptr && arps_[ch].enabled()) {
        arbiter_.set_sustain(ch, false);
        if (!down) deliver_cc(ch, Ump::kControllerSustain, v32);
        return;
      }
    }
    arbiter_.set_sustain(ch, down);
    if (!down) {
      release_deferred(ch);
      deliver_cc(ch, Ump::kControllerSustain, v32);
    }
    return;
  }
  if (ump.group() == 0 && ump.is_pitch_bend()) {  // Story 3.10
    route_pitch_bend(ump.channel(), ump.pitch_bend_value32());
    return;
  }
  if (ump.group() == 0 && ump.is_assignable_per_note_controller()) {  // contact_k (Story 2.5)
    apply_contact(ump);
    return;
  }
  if (ump.group() == 0 && ump.is_control_change()) {  // every other controller (Story 5.2): the CcState of the part's engine
    apply_controller(ump);
    return;
  }
  if (ump.group() == 0 && ump.is_system_realtime()) {
    if (clock_source() == ClockSource::kInternal) {
      ++ignored_;
    }
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
  if (ump.channel() < params::kPartCount) {
    const std::uint32_t p = ump.channel();
    const bool suppress_off = ump.is_note_off() && custom_processors_[p] == nullptr && arps_[p].is_held_non_passthrough(ump.note());
    arps_[p].track_passthrough_note(ump.note(), ump.velocity16(), ump.is_note_on());
    if (suppress_off) return;
  }
  apply_part_note(ump.channel(), ump.note(), ump.velocity16(), ump.is_note_on(), channel_engine_[ump.channel()]);
}

// The note path of one (channel, engine): the arbiter's key/pedal state follows the events even when no engine serves the channel.
void BlockRenderer::apply_part_note(std::uint32_t channel, std::uint32_t note, std::uint32_t velocity16, bool on,
                                    std::uint32_t type_id, bool from_arp) noexcept {
  const std::size_t key_idx = (channel & 15u) * 128u + (note & 127u);
  if (on) note_engine_[key_idx] = type_id;
  if (note_observer_) {
    note_observer_(note_observer_ctx_, EmittedNoteEvent{
        clock_.frames() + current_in_block_frame_,
        static_cast<std::uint8_t>(channel & 0xFFu),
        static_cast<std::uint8_t>(note & 0x7Fu),
        static_cast<std::uint16_t>(velocity16 & 0xFFFFu),
        on,
    });
  }
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
  else if (from_arp) arbiter_.contact_up(channel, note);
  else stop_now = arbiter_.note_off(channel, note, type_id);
  Engine* engine = nullptr;
  const EngineStatus rs = registry_.resolve(type_id, engine);
  if (rs != EngineStatus::kOk) {
    record(rs);  // silence: nothing is rendered for this request
    return;
  }
  if (!on) {
    // the key is up even when the pedal keeps the voice (sw_down / sw_up follow the key; trigger=release_key fires here)
    if (engine->key_released(channel, note) && router_.active()) bind_generation(channel, note);  // a release_key voice started on the part's current instrument
  }
  if (on) {
    const EngineStatus st = engine->start_voice(channel, note, velocity16);
    record(st);
    // In a Live Set the voices hold their bank by raw pointer, so any voice that started (even if a later region found no slot) binds the generation.
    if (st == EngineStatus::kOk || router_.active()) bind_generation(channel, note);
  } else if (stop_now) {
    const EngineStatus st = engine->release_voice(channel, note, velocity16);
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
      if (t.release_first) {
        arps_[t.part].track_passthrough_note(t.old_pitch, 0, false);
        apply_part_note(t.part, t.old_pitch, 0, false, t.old_engine);  // a re-struck key lets its earlier note go first
      }
      arps_[t.part].track_passthrough_note(t.pitch, ump.velocity16(), true);
      apply_part_note(t.part, t.pitch, ump.velocity16(), true, t.engine);
    }
  } else {
    router_.note_off(ump.channel(), ump.note(), fan);
    for (std::uint32_t i = 0; i < fan.count; ++i) {
      const std::uint32_t p = fan.t[i].part;
      const bool suppress_off = p < params::kPartCount && custom_processors_[p] == nullptr && arps_[p].is_held_non_passthrough(fan.t[i].pitch);
      if (p < params::kPartCount) arps_[p].track_passthrough_note(fan.t[i].pitch, ump.velocity16(), false);
      if (!suppress_off) apply_part_note(p, fan.t[i].pitch, ump.velocity16(), false, fan.t[i].engine);
    }
  }
}

// Sustain pedal in a Live Set: down reaches the parts with the keyboard on, up reaches every part (and lets its deferred releases go).
void BlockRenderer::apply_keyboard_sustain(bool down, std::uint32_t value32) noexcept {
  keyboard_pedal_down_ = down;
  keyboard_cc_[Ump::kControllerSustain] = value32;  // replayed to a part that begins to listen (a Live Set switch)
  keyboard_cc_seen_[Ump::kControllerSustain] = true;
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    if (down && router_.pedal_down_target(p)) deliver_cc(p, Ump::kControllerSustain, value32);  // Story 3.11: before the hold; for "up" it follows the release (below)
    arps_[p].set_sustain_pedal(down);
    if (down) {
      if (router_.pedal_down_target(p)) {
        if (custom_processors_[p] == nullptr && arps_[p].enabled()) arbiter_.set_sustain(p, false);
        else arbiter_.set_sustain(p, true);
      }
    } else {
      arbiter_.set_sustain(p, false);
      release_deferred(p);
      deliver_cc(p, Ump::kControllerSustain, value32);  // after the release: the deferred notes were held, so they release at the instrument's own rate
    }
  }
}

bool BlockRenderer::deliver_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32, bool replay) noexcept {
  Engine* engine = nullptr;
  if (part >= 16 || registry_.resolve(channel_engine_[part], engine) != EngineStatus::kOk) return false;
  const bool consumed = replay ? engine->set_part_cc_replay(part, cc, value32) : engine->set_part_cc(part, cc, value32);
  std::uint32_t key = 0;
  // Story 13.7: every controller-started voice (on_locc / on_hicc) holds its bank by raw pointer like any voice: bind the generation to each key it started.
  while (consumed && engine->take_cc_started(key)) {
    if (router_.active()) bind_generation(part, key);
  }
  return consumed;
}

void BlockRenderer::apply_controller(const Ump& ump) noexcept {
  route_cc(ump.channel(), ump.controller_index(), ump.controller_value32(), true);
}

void BlockRenderer::route_cc(std::uint32_t channel, std::uint32_t cc, std::uint32_t value, bool count_ignored) noexcept {
  bool consumed = false;
  if (router_.active()) {
    keyboard_cc_[cc] = value;
    keyboard_cc_seen_[cc] = true;
    for (std::uint32_t p = 0; p < params::kPartCount; ++p)
      if (router_.pedal_down_target(p)) consumed = deliver_cc(p, cc, value) || consumed;
  } else {
    consumed = deliver_cc(channel, cc, value);
  }
  if (!consumed && count_ignored) ++ignored_;
}

bool BlockRenderer::deliver_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept {
  Engine* engine = nullptr;
  if (part >= 16 || registry_.resolve(channel_engine_[part], engine) != EngineStatus::kOk) return false;
  return engine->set_part_pitch_bend(part, value32);
}

void BlockRenderer::route_pitch_bend(std::uint32_t channel, std::uint32_t value32) noexcept {
  bool consumed = false;
  if (router_.active()) {
    keyboard_pitch_bend_ = value32;
    keyboard_pitch_seen_ = true;
    for (std::uint32_t p = 0; p < params::kPartCount; ++p)
      if (router_.pedal_down_target(p)) consumed = deliver_pitch_bend(p, value32) || consumed;
  } else {
    consumed = deliver_pitch_bend(channel, value32);
  }
  if (!consumed) ++ignored_;
}

// A Live Set switch: the keyboard's controllers are physical, so every part that sounds with the keyboard on takes their last values
// (a part the keyboard did not reach before would otherwise start from the instrument defaults).
void BlockRenderer::replay_keyboard_ccs(const Snapshot& s, bool all, const std::array<bool, params::kPartCount>& remapped) noexcept {
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    const bool listening = router_.pedal_down_target(p);
    const std::uint64_t bank = s.parts[p].sample_bank ? s.parts[p].sample_bank->id : 0;
    // Also a part that just began to listen (keyboard on, ready) or got another engine / instrument in a generation without a new epoch.
    const bool needs = listening && (all || !replay_listening_[p] || remapped[p] || bank != replay_bank_[p]);
    replay_listening_[p] = listening;
    replay_bank_[p] = bank;
    if (!needs) continue;
    for (std::uint32_t cc = 0; cc < 128; ++cc)
      if (keyboard_cc_seen_[cc]) deliver_cc(p, cc, keyboard_cc_[cc], true);  // replay: no controller moved, no trigger fires
    if (keyboard_pitch_seen_) deliver_pitch_bend(p, keyboard_pitch_bend_);
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

void BlockRenderer::detach_voice_pool() noexcept {
  if (!voice_pool_active_) return;
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->set_parallel_voice_pool(nullptr, 0);
}

BlockRenderer::~BlockRenderer() {
  // The pool dies with the renderer: no bound engine may keep a pointer to it. (Only when the parallel render was on: an engine that was
  // destroyed before the renderer is then never touched; the engines must outlive the renderer while the parallel render is active.)
  detach_voice_pool();
  voice_pool_.stop();
}

WorkerPool::StartResult BlockRenderer::set_parallel_voice_render(std::uint32_t threads, ports::ThreadAffinity* affinity,
                                                                  const std::uint32_t* cpus) {
  assert(!in_block_.load(std::memory_order_relaxed) && "set_parallel_voice_render is setup-time only");
  detach_voice_pool();  // detach before the pool changes
  voice_pool_.stop();
  voice_pool_active_ = false;
  if (threads == 0) return WorkerPool::StartResult::kOk;
  const WorkerPool::StartResult res = voice_pool_.start(threads, affinity, cpus);
  voice_pool_active_ = true;
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->set_parallel_voice_pool(&voice_pool_, kBlockFrames);
  return res;
}

float BlockRenderer::tempo_bpm() const noexcept {
  if (clock_source() != ClockSource::kInternal) {
    return follower_.tempo_bpm();
  }
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
  struct InBlock {  // debug aid for set_parallel_voice_render()'s setup-time-only rule
    std::atomic<bool>& f;
    explicit InBlock(std::atomic<bool>& x) : f(x) { f.store(true, std::memory_order_relaxed); }
    ~InBlock() { f.store(false, std::memory_order_relaxed); }
  } in_block_guard(in_block_);
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
  const std::uint32_t clock_word = clock_word_.load(std::memory_order_acquire);  // source and epoch together
  const ClockSource cs = static_cast<ClockSource>(clock_word & 0xFFu);
  if ((clock_word >> 8) != rt_clock_epoch_) {  // a source change (Story 7.7): re-lock cleanly, on the RT thread, once per change
    rt_clock_epoch_ = clock_word >> 8;
    follower_.reset();
  }
  if (cs != ClockSource::kInternal) {
    for (std::size_t i = 0; i < pending_count_ && pending_[i].target_frame < kBlockFrames; ++i) {
      const Ump& u = pending_[i].ump;
      if (u.group() == 0 && u.is_system_realtime()) {
        follower_.process_ump(u, clock_.frames() + pending_[i].target_frame, clock_);
      }
    }
    clock_.set_tempo_tenths(follower_.tempo_tenths());
    const params::ParamDef* d = params::find(params::kTempoBpm);
    if (d) {
      last_.store(params::perf_slot(*d, 0), follower_.tempo_bpm());
    }
    last_.store_clock_lost(follower_.clock_lost());
  } else {
    clock_.set_tempo_tenths(TransportClock::tenths_from_bpm(tempo_bpm()));
    last_.store_clock_lost(false);
  }
  // Phase 2: shared per-part generators (the organ's tonewheels) produce their block before voices render.
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->begin_block(kBlockFrames);
  // Phase 5 setup: trivial routing renders straight into Main (the pre-2.7 path, bit-identical); otherwise into the part buses.
  const bool routed = mixer_.begin_block(last_);

  std::array<bool, params::kPartCount> uses_proc{};
  bool any_proc = false;
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
    uses_proc[p] = (custom_processors_[p] != nullptr) || arps_[p].enabled() || arps_[p].has_pending_transition();
    any_proc = any_proc || uses_proc[p];
  }

  std::uint32_t pos = 0;
  std::size_t consumed = 0;
  if (!any_proc) {
    while (consumed < pending_count_ && pending_[consumed].target_frame < kBlockFrames) {
      const std::uint32_t f = pending_[consumed].target_frame;
      render_segment(left, right, pos, f, routed);
      pos = f;
      current_in_block_frame_ = f;
      apply(pending_[consumed].ump);
      ++consumed;
    }
    render_segment(left, right, pos, kBlockFrames, routed);
  } else {
    for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
      part_in_[p].clear();
      part_out_[p].clear();
    }
    direct_part_notes_count_ = 0;
    auto push_in = [this](std::uint32_t part, const Ump& u, std::uint32_t f) {
      if (!part_in_[part].push(u, f)) record(EngineStatus::kEventOverflow);
    };
    auto push_direct = [this](std::size_t seq, std::uint32_t f, std::uint32_t part, std::uint32_t pitch,
                              std::uint32_t vel16, bool on, std::uint32_t eng) {
      if (direct_part_notes_count_ < kDirectPartNotesCapacity) {
        direct_part_notes_[direct_part_notes_count_++] = DirectPartNote{
            seq, f, eng, static_cast<std::uint16_t>(vel16 & 0xFFFFu),
            static_cast<std::uint8_t>(part & 0xFFu), static_cast<std::uint8_t>(pitch & 0x7Fu), on};
      } else {
        record(EngineStatus::kEventOverflow);
      }
    };
    for (std::size_t i = 0; i < pending_count_ && pending_[i].target_frame < kBlockFrames; ++i) {
      const std::uint32_t f = pending_[i].target_frame;
      const Ump& u = pending_[i].ump;
      if (u.group() == 0 && u.is_control_change() && u.controller_index() == Ump::kControllerSustain) {
        const bool down = Ump::sustain_down(u.controller_value32());
        if (router_.active()) {
          for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
            if (uses_proc[p] && custom_processors_[p] == nullptr && (!down || router_.pedal_down_target(p))) {
              push_in(p, make_control_change(0, p, Ump::kControllerSustain, u.controller_value32()), f);
            }
          }
        } else if (u.channel() < params::kPartCount) {
          const std::uint32_t p = u.channel();
          if (uses_proc[p] && custom_processors_[p] == nullptr) {
            push_in(p, u, f);
          }
        }
        continue;
      }
      if (u.group() != 0 || (!u.is_note_on() && !u.is_note_off())) continue;
      if (router_.active()) {
        PartRouter::Fanout fan;
        if (u.is_note_on()) {
          router_.note_on(u.channel(), u.note(), fan);
          for (std::uint32_t k = 0; k < fan.count; ++k) {
            const PartRouter::Target& t = fan.t[k];
            if (uses_proc[t.part]) {
              if (t.release_first) push_in(t.part, make_note_off(0, t.part, t.old_pitch, 0), f);
              push_in(t.part, make_note_on(0, t.part, t.pitch, u.velocity16()), f);
            } else {
              if (t.release_first) {
                arps_[t.part].track_passthrough_note(t.old_pitch, 0, false);
                push_direct(i, f, t.part, t.old_pitch, 0, false, t.old_engine);
              }
              arps_[t.part].track_passthrough_note(t.pitch, u.velocity16(), true);
              push_direct(i, f, t.part, t.pitch, u.velocity16(), true, t.engine);
            }
          }
        } else {
          router_.note_off(u.channel(), u.note(), fan);
          for (std::uint32_t k = 0; k < fan.count; ++k) {
            const PartRouter::Target& t = fan.t[k];
            if (uses_proc[t.part]) {
              push_in(t.part, make_note_off(0, t.part, t.pitch, u.velocity16()), f);
            } else {
              const bool suppress_off = custom_processors_[t.part] == nullptr && arps_[t.part].is_held_non_passthrough(t.pitch);
              arps_[t.part].track_passthrough_note(t.pitch, u.velocity16(), false);
              if (!suppress_off) push_direct(i, f, t.part, t.pitch, u.velocity16(), false, t.engine);
            }
          }
        }
      } else if (u.channel() < params::kPartCount) {
        const std::uint32_t p = u.channel();
        if (uses_proc[p]) {
          push_in(p, u, f);
        } else {
          const bool suppress_off = u.is_note_off() && custom_processors_[p] == nullptr && arps_[p].is_held_non_passthrough(u.note());
          arps_[p].track_passthrough_note(u.note(), u.velocity16(), u.is_note_on());
          if (!suppress_off) push_direct(i, f, p, u.note(), u.velocity16(), u.is_note_on(), channel_engine_[p]);
        }
      }
    }
    for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
      if (uses_proc[p]) {
        EventProcessor* proc = custom_processors_[p] ? custom_processors_[p] : static_cast<EventProcessor*>(&arps_[p]);
        proc->process_block(part_in_[p], clock_, kBlockFrames, part_out_[p]);
        if (part_out_[p].dropped() > 0) record(EngineStatus::kEventOverflow);
      }
    }

    std::size_t direct_idx = 0;
    std::array<std::size_t, params::kPartCount> out_idx{};
    while (true) {
      std::uint32_t next_frame = kBlockFrames;
      if (consumed < pending_count_ && pending_[consumed].target_frame < kBlockFrames) {
        next_frame = std::min(next_frame, pending_[consumed].target_frame);
      }
      if (direct_idx < direct_part_notes_count_) {
        next_frame = std::min(next_frame, direct_part_notes_[direct_idx].frame);
      }
      for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
        if (out_idx[p] < part_out_[p].size()) {
          const std::uint32_t pf = std::min(part_out_[p][out_idx[p]].target_frame, kBlockFrames - 1);
          next_frame = std::min(next_frame, pf);
        }
      }
      if (next_frame >= kBlockFrames) break;

      render_segment(left, right, pos, next_frame, routed);
      pos = next_frame;
      current_in_block_frame_ = next_frame;

      bool part_out_flushed = false;
      auto flush_part_out = [&]() {
        if (part_out_flushed) return;
        part_out_flushed = true;
        for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
          while (out_idx[p] < part_out_[p].size() &&
                 std::min(part_out_[p][out_idx[p]].target_frame, kBlockFrames - 1) == next_frame) {
            const Ump& u = part_out_[p][out_idx[p]++].ump;
            if (u.group() == 0 && (u.is_note_on() || u.is_note_off())) {
              const std::uint32_t note = u.note();
              const bool on = u.is_note_on();
              const std::size_t key_idx = p * 128u + (note & 127u);
              const std::uint32_t eng = on ? channel_engine_[p]
                                           : (note_engine_[key_idx] != 0 ? note_engine_[key_idx] : channel_engine_[p]);
              const bool from_arp = custom_processors_[p] == nullptr;
              apply_part_note(p, note, u.velocity16(), on, eng, from_arp);
            } else {
              apply(u);
            }
          }
        }
      };

      std::size_t last_part_note_seq = pending_count_;
      for (std::size_t look = consumed; look < pending_count_ && pending_[look].target_frame == next_frame; ++look) {
        const Ump& lu = pending_[look].ump;
        if (lu.group() == 0 && (lu.is_note_on() || lu.is_note_off()) &&
            (router_.active() || lu.channel() < params::kPartCount)) {
          last_part_note_seq = look;
        }
      }

      while (consumed < pending_count_ && pending_[consumed].target_frame == next_frame) {
        const std::size_t seq = consumed;
        const Ump& u = pending_[consumed++].ump;
        const bool is_part_note = u.group() == 0 && (u.is_note_on() || u.is_note_off()) &&
                                  (router_.active() || u.channel() < params::kPartCount);
        if (is_part_note) {
          while (direct_idx < direct_part_notes_count_ &&
                 direct_part_notes_[direct_idx].frame == next_frame &&
                 direct_part_notes_[direct_idx].seq <= seq) {
            const DirectPartNote& dn = direct_part_notes_[direct_idx++];
            apply_part_note(dn.part, dn.note, dn.velocity16, dn.on, dn.engine);
          }
          if (seq == last_part_note_seq) {
            flush_part_out();
          }
        } else {
          if (is_fence_event(u)) {
            while (direct_idx < direct_part_notes_count_ && direct_part_notes_[direct_idx].frame == next_frame) {
              const DirectPartNote& dn = direct_part_notes_[direct_idx++];
              apply_part_note(dn.part, dn.note, dn.velocity16, dn.on, dn.engine);
            }
            flush_part_out();
          }
          apply(u);
        }
      }
      while (direct_idx < direct_part_notes_count_ && direct_part_notes_[direct_idx].frame == next_frame) {
        const DirectPartNote& dn = direct_part_notes_[direct_idx++];
        apply_part_note(dn.part, dn.note, dn.velocity16, dn.on, dn.engine);
      }
      flush_part_out();
    }
    render_segment(left, right, pos, kBlockFrames, routed);
  }
  if (routed) mixer_.finish(out, last_);  // chains, sends, Main/Aux, line-in, master
  clock_.advance(kBlockFrames);
  if (cs != ClockSource::kInternal) {
    follower_.process_block(kBlockFrames, clock_);
    last_.store_clock_lost(follower_.clock_lost());
  }

  // Drop applied events, carry the rest into the next block.
  const std::size_t remaining = pending_count_ - consumed;
  for (std::size_t i = 0; i < remaining; ++i) {
    pending_[i] = pending_[consumed + i];
    pending_[i].target_frame -= kBlockFrames;
  }
  pending_count_ = remaining;
  reap_generation_refs();
  // Story 6.5: publish the engines' content-integrity counts (voices that faded for a damaged block of a protected library) to the control side.
  std::uint64_t integrity = 0;
  for (std::size_t i = 0; i < registry_.size(); ++i) integrity += registry_.at(i)->content_integrity_events();
  last_.store_content_integrity(integrity);
  // Story 1.16: stream underruns (a voice reached the head end of a sample with a tail) -> Diagnostics, as the delta since the previous block.
  std::uint64_t underruns = 0;
  for (std::size_t i = 0; i < registry_.size(); ++i) underruns += registry_.at(i)->stream_underrun_events();
  if (underruns > published_underruns_) {
    last_.diagnostics_rt().record_stream_underruns(underruns - published_underruns_);
    published_underruns_ = underruns;
  } else {
    published_underruns_ = underruns;  // the engine total dropped (an engine was replaced): resync, so later increments are not lost
  }
  // Story 1.21: the arbiter's steal / exhausted totals for the diagnostics reply.
  last_.diagnostics_rt().store_voice_totals(arbiter_.steals(), arbiter_.exhausted());
  last_.store_input_activity(input_held_count_, input_pedal_down_);  // Story 8.8
}

}  // namespace velora::core

#include "velora/core/block_renderer.hpp"

#include <cassert>

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

void BlockRenderer::apply(const Ump& ump) noexcept {
  if (ump.group() == 0 && ump.is_control_change() && ump.controller_index() == Ump::kControllerSustain) {
    const bool down = ump.controller_value32() >= 0x80000000u;  // MIDI value 64 of 0..127 scaled to 32 bits
    arbiter_.set_sustain(ump.channel(), down);
    if (!down) release_deferred(ump.channel());
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
    record(engine->start_voice(ump.channel(), ump.note(), ump.velocity16()));
  } else if (stop_now) {
    const EngineStatus st = engine->stop_voice(ump.channel(), ump.note());
    const bool was_stolen = arbiter_.take_stolen_flag(ump.channel(), ump.note());
    if (st == EngineStatus::kVoiceNotFound && was_stolen) ++stolen_note_offs_;
    else record(st);
  }  // else: the pedal holds the note; it is released on pedal up
}

void BlockRenderer::render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to) noexcept {
  if (to <= from) return;
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->render(l + from, r + from, to - from);
}

void BlockRenderer::process_block(float* left, float* right) noexcept {
  if (!left || !right) return;
  for (std::uint32_t i = 0; i < kBlockFrames; ++i) left[i] = right[i] = 0.0f;
  drain_queue();

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
}

}  // namespace velora::core
